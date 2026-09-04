#include <algorithm>
#include <cstdint>
#include <string>

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QTimer>

#include "app/browser_app.h"
#include "include/cef_app.h"
#include "include/cef_command_line.h"
#include "include/wrapper/cef_helpers.h"
#include "ui/main_window.h"

#if defined(OS_WIN)
#include <windows.h>
#elif defined(OS_MAC)
#include "include/wrapper/cef_library_loader.h"
#include "platform/cef_application_mac.h"
#endif

namespace {

constexpr int kMaxMessagePumpDelayMs = 1000 / 30;

class CefMessagePump final : public QObject {
 public:
  explicit CefMessagePump(QObject* parent = nullptr) : QObject(parent) {
    timer_.setSingleShot(true);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, [this] {
      CefDoMessageLoopWork();
      // Keep a bounded fallback because not every platform event is guaranteed
      // to result in another OnScheduleMessagePumpWork callback.
      if (!timer_.isActive()) {
        timer_.start(kMaxMessagePumpDelayMs);
      }
    });
  }

  void Schedule(std::int64_t delay_ms) {
    QMetaObject::invokeMethod(
        this,
        [this, delay_ms] {
          const int delay = static_cast<int>(
              std::clamp<std::int64_t>(delay_ms, 0, kMaxMessagePumpDelayMs));
          // Each CEF callback replaces any previously scheduled work, even
          // when the new deadline is later.
          timer_.start(delay);
        },
        Qt::QueuedConnection);
  }

 private:
  QTimer timer_;
};

QString InitialUrl() {
  const QStringList arguments = QCoreApplication::arguments();
  return arguments.size() > 1 ? arguments.at(1)
                              : QStringLiteral("https://www.example.com");
}

int RunBrowser(int argc, char* argv[]) {
#if defined(OS_MAC)
  CefScopedLibraryLoader library_loader;
  if (!library_loader.LoadInMain()) return 1;
#endif

#if defined(OS_WIN)
  CefMainArgs main_args(GetModuleHandle(nullptr));
#else
  CefMainArgs main_args(argc, argv);
#endif
  CefRefPtr<BrowserApp> cef_app(new BrowserApp);

#if !defined(OS_MAC)
  const int subprocess_code = CefExecuteProcess(main_args, cef_app, nullptr);
  if (subprocess_code >= 0) return subprocess_code;
#endif

#if defined(OS_LINUX)
  // CEF windowed rendering currently requires X11. Qt can still default to
  // Wayland, so force xcb unless the caller has selected a platform.
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "xcb");
  }
#endif

  QApplication application(argc, argv);
#if defined(OS_MAC)
  // Qt creates its private NSApplication subclass. Add the event-state hooks
  // required by CEF after that instance exists and before CefInitialize.
  if (!InstallCefMacApplicationHooks()) return 1;
#endif
  QCoreApplication::setOrganizationName(QStringLiteral("Trail"));
  QCoreApplication::setApplicationName(QStringLiteral("Trail Browser"));

  CefSettings settings;
  settings.no_sandbox = true;  // Development default; see README security note.
  settings.external_message_pump = true;
  settings.multi_threaded_message_loop = false;
  const QString data_path =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  const QString local_data_path =
      QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  QDir().mkpath(data_path);
  QDir().mkpath(local_data_path);
  const QByteArray cache_path =
      QDir::toNativeSeparators(data_path + QStringLiteral("/cef-cache"))
          .toUtf8();
  const QByteArray root_cache_path = QDir::toNativeSeparators(data_path).toUtf8();
  const QByteArray log_path =
      QDir::toNativeSeparators(local_data_path + QStringLiteral("/cef.log"))
          .toUtf8();
  CefString(&settings.cache_path) =
      std::string(cache_path.constData(), cache_path.size());
  CefString(&settings.root_cache_path) =
      std::string(root_cache_path.constData(), root_cache_path.size());
  CefString(&settings.log_file) =
      std::string(log_path.constData(), log_path.size());

  CefMessagePump message_pump;
  cef_app->SetSchedulePump(
      [&message_pump](std::int64_t delay_ms) { message_pump.Schedule(delay_ms); });

  if (!CefInitialize(main_args, settings, cef_app, nullptr)) {
    return CefGetExitCode();
  }

  int exit_code = 0;
  {
    MainWindow main_window(InitialUrl());
    main_window.show();
    message_pump.Schedule(0);
    exit_code = application.exec();
  }

  cef_app->SetSchedulePump({});
  CefShutdown();
  return exit_code;
}

}  // namespace

#if defined(OS_WIN)
int APIENTRY wWinMain(HINSTANCE, HINSTANCE, wchar_t*, int) {
  return RunBrowser(__argc, __argv);
}
#else
#if defined(OS_LINUX)
NO_STACK_PROTECTOR
#endif
int main(int argc, char* argv[]) {
  return RunBrowser(argc, argv);
}
#endif

