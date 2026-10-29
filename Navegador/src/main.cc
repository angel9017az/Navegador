#include <windows.h>

#include <filesystem>
#include <string>

#include "include/cef_app.h"
#include "app.h"

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
  CefEnableHighDPISupport();

  CefMainArgs main_args(hInstance);
  CefRefPtr<BrowserApp> app(new BrowserApp);

  // Los procesos hijo (renderer, GPU...) terminan aquí.
  int exit_code = CefExecuteProcess(main_args, app.get(), nullptr);
  if (exit_code >= 0) return exit_code;

  CefSettings settings;
  settings.no_sandbox = true;  // el sandbox exige un build especial de CEF
  settings.log_severity = LOGSEVERITY_WARNING;
  settings.persist_session_cookies = true;

  // Perfil persistente: %LOCALAPPDATA%\Kairo\UserData
  wchar_t buf[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  std::filesystem::path data =
      std::filesystem::path(std::wstring(buf, n)) / L"Kairo" / L"UserData";
  std::filesystem::create_directories(data);
  CefString(&settings.root_cache_path).FromWString(data.wstring());
  CefString(&settings.cache_path).FromWString(data.wstring());

  if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
    return CefGetExitCode();
  }
  CefRunMessageLoop();
  CefShutdown();
  return 0;
}
