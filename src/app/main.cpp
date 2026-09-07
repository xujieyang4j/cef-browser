#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

#include "app/browser_app.h"
#include "include/cef_app.h"
#include "include/cef_command_line.h"
#include "include/wrapper/cef_helpers.h"
#include "session/session_store.h"
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

struct TabSmokeState {
  int stage = 0;
  int attempts = 0;
};

void StartTabSmokeTest(MainWindow* window) {
  auto state = std::make_shared<TabSmokeState>();
  auto output = std::make_shared<QTextStream>(stdout);
  auto step = std::make_shared<std::function<void()>>();
  *step = [window, state, step, output] {
    ++state->attempts;
    if (state->stage == 0) {
      window->OpenTabForTesting(
          QStringLiteral("data:text/html,<title>Second</title>"));
      state->stage = 1;
    } else if (state->stage == 1 && window->tab_count() == 2 &&
               window->current_title() == QStringLiteral("Second")) {
      window->CloseCurrentTabForTesting();
      state->stage = 2;
    } else if (state->stage == 2 && window->tab_count() == 1 &&
               window->current_title() == QStringLiteral("First")) {
      window->ReopenClosedTabForTesting();
      state->stage = 3;
    } else if (state->stage == 3 && window->tab_count() == 2 &&
               window->current_title() == QStringLiteral("Second")) {
      *output << "TAB_SMOKE_OK count=" << window->tab_count()
              << " url=" << window->current_url() << Qt::endl;
      window->close();
      return;
    } else if (state->attempts > 160) {
      *output << "TAB_SMOKE_FAILED stage=" << state->stage
              << " count=" << window->tab_count() << Qt::endl;
      QCoreApplication::exit(2);
      return;
    }
    QTimer::singleShot(50, window, [step] { (*step)(); });
  };
  QTimer::singleShot(300, window, [step] { (*step)(); });
}

void StartDownloadSmokeTest(MainWindow* window) {
  auto output = std::make_shared<QTextStream>(stdout);
  window->UpdateDownloadForTesting(41, 25, false);
  const bool started = window->download_count_for_testing() == 1 &&
                       window->active_download_count_for_testing() == 1 &&
                       window->download_status_for_testing(41).startsWith(
                           QStringLiteral("25%"));
  window->UpdateDownloadForTesting(41, 100, true);
  const bool completed = window->download_count_for_testing() == 1 &&
                         window->active_download_count_for_testing() == 0 &&
                         window->download_status_for_testing(41) ==
                             QStringLiteral("Complete");
  if (started && completed) {
    *output << "DOWNLOAD_SMOKE_OK status=Complete" << Qt::endl;
    window->close();
  } else {
    *output << "DOWNLOAD_SMOKE_FAILED started=" << started
            << " completed=" << completed << Qt::endl;
    QCoreApplication::exit(3);
  }
}

void StartFailureSmokeTest(MainWindow* window) {
  auto output = std::make_shared<QTextStream>(stdout);
  auto attempts = std::make_shared<int>(0);
  auto step = std::make_shared<std::function<void()>>();
  const QString original_url =
      QStringLiteral("data:text/html,<title>Recovery</title>");
  *step = [window, output, attempts, step, original_url] {
    ++*attempts;
    if (window->current_title() != QStringLiteral("Recovery")) {
      if (*attempts > 120) {
        *output << "FAILURE_SMOKE_FAILED stage=load" << Qt::endl;
        QCoreApplication::exit(4);
        return;
      }
      QTimer::singleShot(50, window, [step] { (*step)(); });
      return;
    }

    window->ShowFailureForTesting(false);
    const bool load_error = window->failure_page_active_for_testing() &&
                            !window->render_process_failed_for_testing() &&
                            window->current_url() == original_url;
    window->ShowFailureForTesting(true);
    const bool renderer_error = window->failure_page_active_for_testing() &&
                                window->render_process_failed_for_testing() &&
                                window->current_url() == original_url;
    if (load_error && renderer_error) {
      *output << "FAILURE_SMOKE_OK url=" << window->current_url() << Qt::endl;
      window->close();
    } else {
      *output << "FAILURE_SMOKE_FAILED load=" << load_error
              << " renderer=" << renderer_error << Qt::endl;
      QCoreApplication::exit(4);
    }
  };
  QTimer::singleShot(300, window, [step] { (*step)(); });
}

std::optional<QString> ExplicitStartupUrl() {
  const QStringList arguments = QCoreApplication::arguments();
  for (int index = 1; index < arguments.size(); ++index) {
    if (!arguments.at(index).startsWith(QLatin1Char('-'))) {
      return arguments.at(index);
    }
  }
  return std::nullopt;
}

bool HasArgument(const QString& argument) {
  return QCoreApplication::arguments().contains(argument);
}

bool IsSmokeTest() {
  return HasArgument(QStringLiteral("--smoke-test-tabs")) ||
         HasArgument(QStringLiteral("--smoke-test-downloads")) ||
         HasArgument(QStringLiteral("--smoke-test-failures")) ||
         HasArgument(QStringLiteral("--smoke-test-session"));
}

BrowserSession DefaultSession(const QString& url) {
  BrowserSession session;
  session.tab_urls = {url};
  return session;
}

void StartSessionSmokeTest(MainWindow* window, const QString& session_path) {
  auto output = std::make_shared<QTextStream>(stdout);
  const BrowserSession captured = window->session_for_testing(false);
  const bool captured_ok =
      captured.tab_urls.size() == 2 && captured.active_tab == 1 &&
      !captured.window_geometry.isEmpty() && !captured.clean_exit;
  const bool unclean_saved = window->save_session_for_testing(false);
  const auto unclean = SessionStore::Load(session_path);
  const bool unclean_ok =
      unclean && unclean->tab_urls == captured.tab_urls &&
      unclean->active_tab == captured.active_tab && !unclean->clean_exit;
  const bool clean_saved = window->save_session_for_testing(true);
  const auto clean = SessionStore::Load(session_path);
  const bool clean_ok = clean && clean->clean_exit &&
                        clean->tab_urls == captured.tab_urls &&
                        clean->active_tab == captured.active_tab;
  if (captured_ok && unclean_saved && unclean_ok && clean_saved && clean_ok) {
    *output << "SESSION_SMOKE_OK tabs=" << clean->tab_urls.size()
            << " active=" << clean->active_tab << Qt::endl;
    window->close();
  } else {
    *output << "SESSION_SMOKE_FAILED captured=" << captured_ok
            << " unclean=" << unclean_ok << " clean=" << clean_ok
            << Qt::endl;
    QCoreApplication::exit(5);
  }
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
  const QString requested_data_path =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  const QString local_data_path =
      QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  QDir().mkpath(requested_data_path);
  QDir().mkpath(local_data_path);
  const QString canonical_data_path = QDir(requested_data_path).canonicalPath();
  const QString data_path = canonical_data_path.isEmpty()
                                ? QDir(requested_data_path).absolutePath()
                                : canonical_data_path;
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
    const QString session_path =
        QDir(data_path).filePath(QStringLiteral("session.json"));
    std::unique_ptr<QTemporaryDir> smoke_session_directory;
    QString active_session_path = session_path;
    BrowserSession initial_session;
    if (HasArgument(QStringLiteral("--smoke-test-session"))) {
      smoke_session_directory = std::make_unique<QTemporaryDir>();
      if (!smoke_session_directory->isValid()) {
        CefShutdown();
        return 5;
      }
      active_session_path =
          smoke_session_directory->filePath(QStringLiteral("session.json"));
      initial_session.tab_urls = {
          QStringLiteral("data:text/html,<title>Session One</title>"),
          QStringLiteral("data:text/html,<title>Session Two</title>")};
      initial_session.active_tab = 1;
      initial_session.clean_exit = false;
    } else if (IsSmokeTest()) {
      initial_session = DefaultSession(ExplicitStartupUrl().value_or(
          QStringLiteral("data:text/html,<title>Smoke</title>")));
      active_session_path.clear();
    } else if (const auto startup_url = ExplicitStartupUrl()) {
      // A URL explicitly supplied by the caller always wins over restoration.
      initial_session = DefaultSession(*startup_url);
    } else {
      QString session_error;
      const auto restored = SessionStore::Load(session_path, &session_error);
      initial_session = restored.value_or(
          DefaultSession(QStringLiteral("https://www.example.com")));
      if (!session_error.isEmpty()) {
        qWarning("Unable to restore browser session: %s",
                 qPrintable(session_error));
      }
    }

    MainWindow main_window(initial_session, active_session_path);
    main_window.show();
    message_pump.Schedule(0);
    if (HasArgument(QStringLiteral("--smoke-test-tabs"))) {
      StartTabSmokeTest(&main_window);
    } else if (HasArgument(QStringLiteral("--smoke-test-downloads"))) {
      QTimer::singleShot(300, &main_window,
                         [&main_window] { StartDownloadSmokeTest(&main_window); });
    } else if (HasArgument(QStringLiteral("--smoke-test-failures"))) {
      StartFailureSmokeTest(&main_window);
    } else if (HasArgument(QStringLiteral("--smoke-test-session"))) {
      QTimer::singleShot(300, &main_window, [&main_window, active_session_path] {
        StartSessionSmokeTest(&main_window, active_session_path);
      });
    }
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

