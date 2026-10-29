#include "app.h"

#include "include/wrapper/cef_helpers.h"
#include "shell.h"

void BrowserApp::OnBeforeCommandLineProcessing(const CefString& process_type,
                                               CefRefPtr<CefCommandLine> cmd) {
  if (!process_type.empty()) return;  // solo el proceso del navegador

  // --- Rendimiento ---
  cmd->AppendSwitch("enable-gpu-rasterization");  // raster en GPU
  cmd->AppendSwitch("enable-zero-copy");          // menos copias de memoria

  // --- Menos trabajo y menos red en segundo plano ---
  cmd->AppendSwitch("disable-background-networking");
  cmd->AppendSwitch("disable-sync");
  cmd->AppendSwitch("no-pings");
  cmd->AppendSwitchWithValue("disable-features",
                             "MediaRouter,OptimizationHints,InterestFeedContentSuggestions");

  // Las pestañas ocultas (SetVisible(false)) quedan limitadas por Chromium
  // automáticamente: timers throttled, sin pintar, sin vsync.
}

void BrowserApp::OnContextInitialized() {
  CEF_REQUIRE_UI_THREAD();
  StartShell();
}

void BrowserApp::OnWebKitInitialized() {
  CefMessageRouterConfig config;  // expone window.cefQuery / cefQueryCancel
  router_ = CefMessageRouterRendererSide::Create(config);
}

void BrowserApp::OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                  CefRefPtr<CefV8Context> context) {
  router_->OnContextCreated(browser, frame, context);
}

void BrowserApp::OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                   CefRefPtr<CefV8Context> context) {
  router_->OnContextReleased(browser, frame, context);
}

bool BrowserApp::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                          CefRefPtr<CefFrame> frame,
                                          CefProcessId source_process,
                                          CefRefPtr<CefProcessMessage> message) {
  return router_->OnProcessMessageReceived(browser, frame, source_process, message);
}
