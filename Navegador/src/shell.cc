#include "shell.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_parser.h"
#include "include/cef_task.h"
#include "include/views/cef_box_layout.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_browser_view_delegate.h"
#include "include/views/cef_panel.h"
#include "include/views/cef_window.h"
#include "include/views/cef_window_delegate.h"
#include "include/wrapper/cef_helpers.h"
#include "include/wrapper/cef_message_router.h"

#include "adblock.h"

namespace {

constexpr int kBaseUiHeight = 92;  // alto inicial de la barra superior (px)

// ---------------------------------------------------------------- utilidades

class FnTask : public CefTask {
 public:
  explicit FnTask(std::function<void()> fn) : fn_(std::move(fn)) {}
  void Execute() override { fn_(); }

 private:
  std::function<void()> fn_;
  IMPLEMENT_REFCOUNTING(FnTask);
};

std::wstring ExeDirW() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring p(buf, n);
  return p.substr(0, p.find_last_of(L"\\/"));
}

std::string FileUrl(const std::string& path) {
  std::string out = "file:///";
  for (char c : path) {
    if (c == '\\') out += '/';
    else if (c == ' ') out += "%20";
    else if (c == '#') out += "%23";
    else if (c == '%') out += "%25";
    else out += c;
  }
  return out;
}

std::vector<std::string> Split(const std::string& s, char d, size_t max_parts) {
  std::vector<std::string> out;
  size_t start = 0;
  while (out.size() + 1 < max_parts) {
    size_t p = s.find(d, start);
    if (p == std::string::npos) break;
    out.push_back(s.substr(start, p - start));
    start = p + 1;
  }
  out.push_back(s.substr(start));
  return out;
}

bool IsAllowedUrl(const std::string& u) {
  return u.rfind("https://", 0) == 0 || u.rfind("http://", 0) == 0 ||
         u.rfind("file:///", 0) == 0;
}

bool ValidProxy(const std::string& s) {
  static const char* schemes[] = {"socks5://", "socks4://", "http://", "https://"};
  bool ok = false;
  for (const char* p : schemes) {
    if (s.rfind(p, 0) == 0 && s.size() > std::strlen(p)) ok = true;
  }
  if (!ok) return false;
  for (char c : s) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || std::strchr(".:-_[]/", c))) return false;
  }
  return true;
}

// ------------------------------------------------------------------- modelo

struct Tab {
  int id = 0;
  CefRefPtr<CefBrowserView> view;
  CefRefPtr<CefBrowser> browser;
  std::string title = "Nueva pestaña";
  std::string url;
  bool loading = false;
  bool can_back = false;
  bool can_forward = false;
};

// Delegate de las vistas de navegador. La barra de interfaz usa un alto fijo;
// las pestañas rellenan el panel (alto 0 = sin preferencia).
class ViewDelegate : public CefBrowserViewDelegate {
 public:
  explicit ViewDelegate(int height) : height_(height) {}
  void set_height(int h) { height_ = h; }

  CefSize GetPreferredSize(CefRefPtr<CefView>) override { return CefSize(0, height_); }
  cef_runtime_style_t GetBrowserRuntimeStyle() override { return CEF_RUNTIME_STYLE_ALLOY; }

 private:
  int height_;
  IMPLEMENT_REFCOUNTING(ViewDelegate);
};

struct Shell {
  static Shell& Get() {
    static Shell s;
    return s;
  }

  void Start();
  void BuildWindow(CefRefPtr<CefWindow> w);
  void Reset();
  void OnAccelerator(int id);
  bool Handle(const std::string& req, std::string* err);

  int NewTab(const std::string& url);
  void CloseTab(int id);
  void Activate(int id);
  Tab* Find(int id);

  bool SetProxy(bool on, const std::string& server, std::string* err);
  void SetUiHeight(int h);
  void FocusUrl();
  void PushState();
  void RunUiJs(const std::string& js);
  std::string NewTabUrl() const {
    return newtab_base + (theme_hash.empty() ? "" : "#" + theme_hash);
  }

  // Avisos desde los clients de cada pestaña (hilo UI)
  void OnTabCreated(int id, CefRefPtr<CefBrowser> b) {
    if (Tab* t = Find(id)) t->browser = b;
  }
  void OnTabTitle(int id, const std::string& s) {
    if (Tab* t = Find(id)) {
      t->title = s;
      PushState();
    }
  }
  void OnTabUrl(int id, const std::string& u) {
    if (Tab* t = Find(id)) {
      t->url = u;
      PushState();
    }
  }
  void OnTabLoading(int id, bool loading, bool back, bool fwd) {
    if (Tab* t = Find(id)) {
      t->loading = loading;
      t->can_back = back;
      t->can_forward = fwd;
      PushState();
    }
  }

  CefRefPtr<CefWindow> window;
  CefRefPtr<CefBrowserView> ui_view;
  CefRefPtr<CefPanel> content;
  CefRefPtr<CefBrowser> ui_browser;
  CefRefPtr<ViewDelegate> ui_delegate;
  std::vector<Tab> tabs;
  int active = -1;
  int next_id = 1;

  std::shared_ptr<AdBlock> adblock;  // se asigna antes de crear ventanas; luego solo lectura
  std::atomic<bool> adblock_on{true};
  bool proxy_on = false;

  std::string ui_url;
  std::string newtab_base;
  std::string theme_hash;
};

// ------------------------------------------------------ clients de CEF

// Cliente de cada pestaña web: título, URL, carga, popups y bloqueo de anuncios.
class TabClient : public CefClient,
                  public CefLifeSpanHandler,
                  public CefDisplayHandler,
                  public CefLoadHandler,
                  public CefRequestHandler,
                  public CefResourceRequestHandler {
 public:
  explicit TabClient(int id) : id_(id) {}

  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

  // CefLifeSpanHandler
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    Shell::Get().OnTabCreated(id_, browser);
  }
  // NOTA: la firma de OnBeforePopup cambia entre versiones de CEF (p. ej. el
  // parámetro popup_id). Si no compila, copia la de include/cef_life_span_handler.h.
  bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int /*popup_id*/,
                     const CefString& target_url, const CefString&,
                     cef_window_open_disposition_t, bool, const CefPopupFeatures&,
                     CefWindowInfo&, CefRefPtr<CefClient>&, CefBrowserSettings&,
                     CefRefPtr<CefDictionaryValue>&, bool*) override {
    std::string url = target_url.ToString();
    if (IsAllowedUrl(url)) {
      CefPostTask(TID_UI, new FnTask([url] { Shell::Get().NewTab(url); }));
    }
    return true;  // cancela la ventana emergente: se abre como pestaña
  }

  // CefDisplayHandler
  void OnTitleChange(CefRefPtr<CefBrowser>, const CefString& title) override {
    Shell::Get().OnTabTitle(id_, title.ToString());
  }
  void OnAddressChange(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                       const CefString& url) override {
    if (frame->IsMain()) Shell::Get().OnTabUrl(id_, url.ToString());
  }

  // CefLoadHandler
  void OnLoadingStateChange(CefRefPtr<CefBrowser>, bool loading, bool back,
                            bool fwd) override {
    Shell::Get().OnTabLoading(id_, loading, back, fwd);
  }

  // CefRequestHandler / CefResourceRequestHandler: aquí vive el adblock
  CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(
      CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>, bool, bool,
      const CefString&, bool&) override {
    return this;
  }
  cef_return_value_t OnBeforeResourceLoad(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                          CefRefPtr<CefRequest> request,
                                          CefRefPtr<CefCallback>) override {
    Shell& s = Shell::Get();
    if (s.adblock_on.load() && s.adblock &&
        request->GetResourceType() != RT_MAIN_FRAME &&
        s.adblock->ShouldBlock(request->GetURL().ToString())) {
      return RV_CANCEL;
    }
    request->SetHeaderByName("Sec-GPC", "1", true);  // Global Privacy Control
    return RV_CONTINUE;
  }

 private:
  int id_;
  IMPLEMENT_REFCOUNTING(TabClient);
};

// Cliente de la barra de interfaz. Es el ÚNICO con acceso al puente cefQuery:
// las páginas web (TabClient) no tienen router en el lado del navegador.
class ShellClient : public CefClient,
                    public CefLifeSpanHandler,
                    public CefRequestHandler,
                    public CefMessageRouterBrowserSide::Handler {
 public:
  ShellClient() {
    CefMessageRouterConfig config;
    router_ = CefMessageRouterBrowserSide::Create(config);
    router_->AddHandler(this, false);
  }

  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                CefProcessId source, CefRefPtr<CefProcessMessage> msg) override {
    return router_->OnProcessMessageReceived(browser, frame, source, msg);
  }

  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override { Shell::Get().ui_browser = browser; }
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    router_->OnBeforeClose(browser);
    Shell::Get().ui_browser = nullptr;
  }
  bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int, const CefString&,
                     const CefString&, cef_window_open_disposition_t, bool,
                     const CefPopupFeatures&, CefWindowInfo&, CefRefPtr<CefClient>&,
                     CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&, bool*) override {
    return true;  // la interfaz nunca abre ventanas
  }

  // La interfaz jamás debe navegar a la web.
  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefRequest> request, bool, bool) override {
    router_->OnBeforeBrowse(browser, frame);
    return request->GetURL().ToString().rfind("file:", 0) != 0;
  }

  bool OnQuery(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int64_t, const CefString& request,
               bool, CefRefPtr<Callback> callback) override {
    if (!frame->IsMain()) return false;
    std::string err;
    if (Shell::Get().Handle(request.ToString(), &err)) callback->Success("");
    else callback->Failure(1, err);
    return true;
  }

 private:
  CefRefPtr<CefMessageRouterBrowserSide> router_;
  IMPLEMENT_REFCOUNTING(ShellClient);
};

class WindowDelegate : public CefWindowDelegate {
 public:
  void OnWindowCreated(CefRefPtr<CefWindow> window) override { Shell::Get().BuildWindow(window); }
  void OnWindowDestroyed(CefRefPtr<CefWindow>) override {
    Shell::Get().Reset();
    CefQuitMessageLoop();
  }
  bool CanClose(CefRefPtr<CefWindow>) override { return true; }
  CefSize GetPreferredSize(CefRefPtr<CefView>) override { return CefSize(1360, 860); }
  bool OnAccelerator(CefRefPtr<CefWindow>, int command_id) override {
    Shell::Get().OnAccelerator(command_id);
    return true;
  }
  cef_runtime_style_t GetWindowRuntimeStyle() override { return CEF_RUNTIME_STYLE_ALLOY; }

 private:
  IMPLEMENT_REFCOUNTING(WindowDelegate);
};

// ------------------------------------------------------ métodos de Shell

void Shell::Start() {
  const std::wstring dir = ExeDirW();
  const std::string dir8 = CefString(dir).ToString();
  ui_url = FileUrl(dir8 + "\\ui\\index.html");
  newtab_base = FileUrl(dir8 + "\\ui\\newtab.html");

  adblock = std::make_shared<AdBlock>();
  adblock->LoadDirectory(std::filesystem::path(dir) / L"ui" / L"filters");

  CefWindow::CreateTopLevelWindow(new WindowDelegate);
}

void Shell::BuildWindow(CefRefPtr<CefWindow> w) {
  window = w;
  w->SetTitle("Kairo");

  CefBoxLayoutSettings ls;
  ls.horizontal = false;  // apila: barra de interfaz arriba, contenido debajo
  CefRefPtr<CefBoxLayout> layout = w->SetToBoxLayout(ls);

  CefBrowserSettings bs;
  ui_delegate = new ViewDelegate(kBaseUiHeight);
  ui_view = CefBrowserView::CreateBrowserView(new ShellClient, ui_url, bs, nullptr, nullptr,
                                              ui_delegate);
  content = CefPanel::CreatePanel(nullptr);
  content->SetToFillLayout();

  w->AddChildView(ui_view);
  w->AddChildView(content);
  layout->SetFlexForView(content, 1);  // el contenido ocupa todo el resto

  // Atajos: Ctrl+T, Ctrl+W, Ctrl+L, Ctrl+R, Ctrl+Tab
  w->SetAccelerator(1, 'T', false, true, false);
  w->SetAccelerator(2, 'W', false, true, false);
  w->SetAccelerator(3, 'L', false, true, false);
  w->SetAccelerator(4, 'R', false, true, false);
  w->SetAccelerator(5, VK_TAB, false, true, false);

  w->Show();
  // La primera pestaña se crea cuando la interfaz avisa "ready" (ya con el tema).
}

void Shell::Reset() {
  tabs.clear();
  content = nullptr;
  ui_view = nullptr;
  ui_delegate = nullptr;
  window = nullptr;
}

Tab* Shell::Find(int id) {
  for (auto& t : tabs)
    if (t.id == id) return &t;
  return nullptr;
}

int Shell::NewTab(const std::string& url) {
  if (!content) return -1;
  Tab t;
  t.id = next_id++;
  t.url = url;
  CefBrowserSettings bs;
  t.view = CefBrowserView::CreateBrowserView(new TabClient(t.id), url, bs, nullptr, nullptr,
                                             new ViewDelegate(0));
  content->AddChildView(t.view);
  const int id = t.id;
  tabs.push_back(t);
  Activate(id);
  return id;
}

void Shell::CloseTab(int id) {
  auto it = std::find_if(tabs.begin(), tabs.end(), [id](const Tab& t) { return t.id == id; });
  if (it == tabs.end()) return;
  const size_t idx = static_cast<size_t>(it - tabs.begin());
  Tab t = *it;
  tabs.erase(it);
  if (t.browser) t.browser->GetHost()->CloseBrowser(true);
  content->RemoveChildView(t.view);

  if (tabs.empty()) {
    if (window) window->Close();  // cerrar la última pestaña cierra el navegador
    return;
  }
  if (active == id) Activate(tabs[std::min(idx, tabs.size() - 1)].id);
  else PushState();
}

void Shell::Activate(int id) {
  active = id;
  for (auto& t : tabs) t.view->SetVisible(t.id == id);  // las ocultas quedan throttled
  if (Tab* t = Find(id)) t->view->RequestFocus();
  PushState();
}

void Shell::SetUiHeight(int h) {
  if (!ui_delegate || !ui_view) return;
  ui_delegate->set_height(h);
  ui_view->InvalidateLayout();
}

void Shell::FocusUrl() {
  if (ui_view) ui_view->RequestFocus();
  RunUiJs("window.kairo&&kairo.focusUrl()");
}

void Shell::RunUiJs(const std::string& js) {
  if (ui_browser) ui_browser->GetMainFrame()->ExecuteJavaScript(js, "", 0);
}

void Shell::PushState() {
  if (!ui_browser) return;
  // Se serializa con CefWriteJSON: los títulos de páginas web son datos no confiables.
  auto list = CefListValue::Create();
  for (size_t i = 0; i < tabs.size(); ++i) {
    const Tab& t = tabs[i];
    auto d = CefDictionaryValue::Create();
    d->SetInt("id", t.id);
    d->SetString("title", t.title);
    d->SetString("url", t.url);
    d->SetBool("loading", t.loading);
    d->SetBool("back", t.can_back);
    d->SetBool("fwd", t.can_forward);
    list->SetDictionary(i, d);
  }
  auto root = CefDictionaryValue::Create();
  root->SetList("tabs", list);
  root->SetInt("active", active);
  root->SetInt("blocked", static_cast<int>(adblock ? adblock->blocked() : 0));
  root->SetBool("adblock", adblock_on.load());
  root->SetBool("proxy", proxy_on);
  auto v = CefValue::Create();
  v->SetDictionary(root);
  RunUiJs("window.kairo&&kairo.state(" + CefWriteJSON(v, JSON_WRITER_DEFAULT).ToString() + ")");
}

bool Shell::SetProxy(bool on, const std::string& server, std::string* err) {
  if (on && !ValidProxy(server)) {
    *err = "Dirección no válida. Ejemplo: socks5://127.0.0.1:1080";
    return false;
  }
  CefRefPtr<CefRequestContext> ctx = CefRequestContext::GetGlobalContext();

  auto proxy = CefDictionaryValue::Create();
  if (on) {
    proxy->SetString("mode", "fixed_servers");  // sin fallback: si el proxy cae, no hay tráfico
    proxy->SetString("server", server);
  } else {
    proxy->SetString("mode", "system");
  }
  auto pv = CefValue::Create();
  pv->SetDictionary(proxy);
  CefString e;
  if (!ctx->SetPreference("proxy", pv, e)) {
    *err = e.ToString();
    return false;
  }

  // Con proxy activo, WebRTC no debe filtrar la IP real por UDP directo.
  auto rtc = CefValue::Create();
  rtc->SetString(on ? "disable_non_proxied_udp" : "default");
  ctx->SetPreference("webrtc.ip_handling_policy", rtc, e);

  ctx->CloseAllConnections(nullptr);  // las conexiones abiertas no deben seguir por la ruta vieja
  proxy_on = on;
  return true;
}

void Shell::OnAccelerator(int id) {
  switch (id) {
    case 1:
      NewTab(NewTabUrl());
      FocusUrl();
      break;
    case 2:
      CloseTab(active);
      break;
    case 3:
      FocusUrl();
      break;
    case 4:
      if (Tab* t = Find(active)) {
        if (t->browser) t->browser->Reload();
      }
      break;
    case 5:
      if (!tabs.empty()) {
        size_t i = 0;
        for (; i < tabs.size(); ++i)
          if (tabs[i].id == active) break;
        Activate(tabs[(i + 1) % tabs.size()].id);
      }
      break;
  }
}

// Comandos que envía la interfaz con window.cefQuery({request: "cmd|arg|arg"}).
bool Shell::Handle(const std::string& req, std::string* err) {
  const auto p = Split(req, '|', 3);
  const std::string& c = p[0];
  auto arg = [&](size_t i) -> std::string { return i < p.size() ? p[i] : std::string(); };
  Tab* cur = Find(active);

  if (c == "ready") {
    if (tabs.empty()) NewTab(NewTabUrl());
    else PushState();
    return true;
  }
  if (c == "new") {
    NewTab(IsAllowedUrl(arg(1)) ? arg(1) : NewTabUrl());
    return true;
  }
  if (c == "close") {
    CloseTab(std::atoi(arg(1).c_str()));
    return true;
  }
  if (c == "activate") {
    Activate(std::atoi(arg(1).c_str()));
    return true;
  }
  if (c == "go") {
    Tab* t = Find(std::atoi(arg(1).c_str()));
    if (!t || !t->browser || !IsAllowedUrl(arg(2))) {
      *err = "No se puede abrir esa dirección";
      return false;
    }
    t->browser->GetMainFrame()->LoadURL(arg(2));
    return true;
  }
  if (c == "back" || c == "forward" || c == "reload" || c == "stop") {
    if (cur && cur->browser) {
      if (c == "back") cur->browser->GoBack();
      else if (c == "forward") cur->browser->GoForward();
      else if (c == "reload") cur->browser->Reload();
      else cur->browser->StopLoad();
    }
    return true;
  }
  if (c == "adblock") {
    adblock_on = (arg(1) == "on");
    PushState();
    return true;
  }
  if (c == "proxy") {
    const bool ok = SetProxy(arg(1) == "on", arg(2), err);
    PushState();
    return ok;
  }
  if (c == "uiheight") {
    SetUiHeight(std::clamp(std::atoi(arg(1).c_str()), 60, 600));
    return true;
  }
  if (c == "theme") {
    theme_hash = arg(1);
    return true;
  }
  *err = "Comando desconocido";
  return false;
}

}  // namespace

void StartShell() { Shell::Get().Start(); }
