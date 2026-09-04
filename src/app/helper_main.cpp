#include "app/browser_app.h"

#include "include/cef_app.h"

#if defined(OS_MAC)
#include "include/wrapper/cef_library_loader.h"
#endif

int main(int argc, char* argv[]) {
#if defined(OS_MAC)
  CefScopedLibraryLoader library_loader;
  if (!library_loader.LoadInHelper()) return 1;
#endif

  CefMainArgs main_args(argc, argv);
  CefRefPtr<BrowserApp> app(new BrowserApp);
  return CefExecuteProcess(main_args, app, nullptr);
}

