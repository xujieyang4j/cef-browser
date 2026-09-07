#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#include <QApplication>
#include <QAbstractButton>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

#include "app/browser_app.h"
#include "include/cef_app.h"
#include "include/cef_command_line.h"
#include "include/wrapper/cef_helpers.h"
#include "profile/browsing_data_store.h"
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

struct AuthSmokeResult {
  bool continued = false;
  bool cancelled = false;
};

class AuthSmokeCallback final : public CefAuthCallback {
 public:
  explicit AuthSmokeCallback(std::shared_ptr<AuthSmokeResult> result)
      : result_(std::move(result)) {}

  void Continue(const CefString&, const CefString&) override {
    result_->continued = true;
  }
  void Cancel() override { result_->cancelled = true; }

 private:
  std::shared_ptr<AuthSmokeResult> result_;

  IMPLEMENT_REFCOUNTING(AuthSmokeCallback);
  DISALLOW_COPY_AND_ASSIGN(AuthSmokeCallback);
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
  window->PauseDownloadForTesting(41);
  const bool paused = window->active_download_count_for_testing() == 1 &&
                      window->download_status_for_testing(41).startsWith(
                          QStringLiteral("Paused"));
  window->UpdateDownloadForTesting(41, 50, false);
  const bool resumed = window->active_download_count_for_testing() == 1 &&
                       window->download_status_for_testing(41).startsWith(
                           QStringLiteral("50%"));
  window->UpdateDownloadForTesting(41, 100, true);
  const bool completed = window->download_count_for_testing() == 1 &&
                         window->active_download_count_for_testing() == 0 &&
                         window->download_status_for_testing(41) ==
                             QStringLiteral("Complete");
  if (started && paused && resumed && completed) {
    *output << "DOWNLOAD_SMOKE_OK paused=1 resumed=1 status=Complete"
            << Qt::endl;
    window->close();
  } else {
    *output << "DOWNLOAD_SMOKE_FAILED started=" << started
            << " paused=" << paused << " resumed=" << resumed
            << " completed=" << completed << Qt::endl;
    QCoreApplication::exit(3);
  }
}

void StartExitProtectionSmokeTest(MainWindow* window) {
  auto output = std::make_shared<QTextStream>(stdout);
  window->UpdateDownloadForTesting(81, 35, false);
  window->UpdateDownloadForTesting(82, 60, false);
  window->PauseDownloadForTesting(82);
  window->close();

  auto attempts = std::make_shared<int>(0);
  auto stage = std::make_shared<int>(0);
  auto step = std::make_shared<std::function<void()>>();
  *step = [window, output, attempts, stage, step] {
    ++*attempts;
    QMessageBox* dialog = window->findChild<QMessageBox*>();
    if (*stage == 0 && dialog && dialog->isVisible() &&
        window->active_download_count_for_testing() == 2 &&
        !window->window_close_requested_for_testing()) {
      dialog->defaultButton()->click();
      *stage = 1;
    } else if (*stage == 1 && !dialog && window->isVisible() &&
               !window->window_close_requested_for_testing()) {
      window->close();
      *stage = 2;
    } else if (*stage == 2 && dialog && dialog->isVisible()) {
      for (QAbstractButton* button : dialog->buttons()) {
        if (dialog->buttonRole(button) == QMessageBox::DestructiveRole) {
          button->click();
          break;
        }
      }
      if (window->window_close_requested_for_testing()) {
        *output << "EXIT_PROTECTION_SMOKE_OK active=2 default=keep "
                   "confirmed=cancel"
                << Qt::endl;
        return;
      }
    }
    if (*attempts > 120) {
      *output << "EXIT_PROTECTION_SMOKE_FAILED stage=" << *stage
              << " active=" << window->active_download_count_for_testing()
              << " closing="
              << window->window_close_requested_for_testing() << Qt::endl;
      QCoreApplication::exit(10);
      return;
    }
    QTimer::singleShot(50, window, [step] { (*step)(); });
  };
  QTimer::singleShot(50, window, [step] { (*step)(); });
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
         HasArgument(QStringLiteral("--smoke-test-exit-protection")) ||
         HasArgument(QStringLiteral("--smoke-test-failures")) ||
         HasArgument(QStringLiteral("--smoke-test-session")) ||
         HasArgument(QStringLiteral("--smoke-test-page-tools")) ||
         HasArgument(QStringLiteral("--smoke-test-profile")) ||
         HasArgument(QStringLiteral("--smoke-test-privacy")) ||
         HasArgument(QStringLiteral("--smoke-test-favicon")) ||
         HasArgument(QStringLiteral("--smoke-test-security")) ||
         HasArgument(QStringLiteral("--smoke-test-auth"));
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
      !captured.window_geometry.isEmpty() && !captured.clean_exit &&
      captured.recently_closed_urls == QStringList{QStringLiteral(
                                               "https://example.test/closed")};
  const bool unclean_saved = window->save_session_for_testing(false);
  const auto unclean = SessionStore::Load(session_path);
  const bool unclean_ok =
      unclean && unclean->tab_urls == captured.tab_urls &&
      unclean->active_tab == captured.active_tab && !unclean->clean_exit;
  const bool clean_saved = window->save_session_for_testing(true);
  const auto clean = SessionStore::Load(session_path);
  const bool clean_ok = clean && clean->clean_exit &&
                        clean->tab_urls == captured.tab_urls &&
                        clean->active_tab == captured.active_tab &&
                        clean->recently_closed_urls ==
                            captured.recently_closed_urls;
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

void StartPageToolsSmokeTest(MainWindow* window) {
  auto output = std::make_shared<QTextStream>(stdout);
  auto attempts = std::make_shared<int>(0);
  auto stage = std::make_shared<int>(0);
  auto step = std::make_shared<std::function<void()>>();
  *step = [window, output, attempts, stage, step] {
    ++*attempts;
    if (*stage == 0 &&
        window->current_title() == QStringLiteral("Page Tools")) {
      window->FindForTesting(QStringLiteral("trail"));
      window->ZoomInForTesting();
      *stage = 1;
    } else if (*stage == 1 && window->find_bar_visible_for_testing() &&
               window->find_result_for_testing().endsWith(
                   QStringLiteral("/ 3")) &&
               window->zoom_percent_for_testing() > 100) {
      window->ResetZoomForTesting();
      window->HideFindBarForTesting();
      window->SetWebFullscreenForTesting(true);
      *stage = 2;
    } else if (*stage == 2 && window->web_fullscreen_for_testing()) {
      window->SetWebFullscreenForTesting(false);
      *stage = 3;
    } else if (*stage == 3 && !window->web_fullscreen_for_testing() &&
               !window->find_bar_visible_for_testing() &&
               window->zoom_percent_for_testing() == 100) {
      *output << "PAGE_TOOLS_SMOKE_OK matches=3 zoom=100 fullscreen=roundtrip"
              << Qt::endl;
      window->close();
      return;
    }
    if (*attempts > 160) {
      *output << "PAGE_TOOLS_SMOKE_FAILED stage=" << *stage
              << " result=" << window->find_result_for_testing()
              << " zoom=" << window->zoom_percent_for_testing() << Qt::endl;
      QCoreApplication::exit(6);
      return;
    }
    QTimer::singleShot(50, window, [step] { (*step)(); });
  };
  QTimer::singleShot(300, window, [step] { (*step)(); });
}

void StartProfileSmokeTest(MainWindow* window, const QString& data_path) {
  auto output = std::make_shared<QTextStream>(stdout);
  const QString first_url = QStringLiteral("https://example.test/first");
  const QString second_url = QStringLiteral("https://example.test/second");
  BrowsingDataStore data(data_path);
  const bool add_first = data.AddBookmark(first_url, QStringLiteral("First"));
  const bool reject_duplicate =
      !data.AddBookmark(first_url, QStringLiteral("Duplicate"));
  const bool add_second =
      data.AddBookmark(second_url, QStringLiteral("Second"));
  data.RecordVisit(first_url, QStringLiteral("First"));
  data.RecordVisit(second_url, QStringLiteral("Second"));
  data.RecordVisit(first_url, QStringLiteral("First again"));
  const bool saved = data.Save();

  BrowsingDataStore restored(data_path);
  const bool loaded = restored.Load();
  const bool bookmark_ok = loaded && restored.bookmarks().size() == 2 &&
                           restored.IsBookmarked(first_url);
  const bool history_ok =
      loaded && restored.history().size() == 2 &&
      restored.history().first().url == first_url &&
      restored.history().first().visit_count == 2;
  const bool removed = restored.RemoveBookmark(first_url) &&
                       !restored.IsBookmarked(first_url);
  window->ToggleBookmarkForTesting();
  const bool suggestions_ok =
      window->address_suggestions_for_testing().contains(window->current_url());
  window->ToggleBookmarkForTesting();
  if (add_first && reject_duplicate && add_second && saved && bookmark_ok &&
      history_ok && removed && suggestions_ok) {
    *output << "PROFILE_SMOKE_OK bookmarks=2 history=2 visits=2 suggestions=1"
            << Qt::endl;
    window->close();
  } else {
    *output << "PROFILE_SMOKE_FAILED bookmark=" << bookmark_ok
            << " history=" << history_ok << " removed=" << removed
            << " suggestions=" << suggestions_ok
            << Qt::endl;
    QCoreApplication::exit(7);
  }
}

void StartPrivacySmokeTest(MainWindow* window) {
  auto output = std::make_shared<QTextStream>(stdout);
  window->AddHistoryForTesting(QStringLiteral("https://example.test/private"),
                               QStringLiteral("Private visit"));
  const bool seeded = window->history_count_for_testing() == 1 &&
                      window->address_suggestions_for_testing().contains(
                          QStringLiteral("https://example.test/private"));
  window->ClearBrowsingDataForTesting();

  auto attempts = std::make_shared<int>(0);
  auto step = std::make_shared<std::function<void()>>();
  *step = [window, output, seeded, attempts, step] {
    ++*attempts;
    if (!window->browsing_data_clear_in_progress_for_testing()) {
      const bool cleared = window->history_count_for_testing() == 0 &&
                           !window->address_suggestions_for_testing().contains(
                               QStringLiteral("https://example.test/private"));
      const bool completed =
          window->browsing_data_clear_result_for_testing().startsWith(
              QStringLiteral("Browsing history"));
      if (seeded && cleared && completed) {
        *output << "PRIVACY_SMOKE_OK history=cleared cef=completed"
                << Qt::endl;
        window->close();
      } else {
        *output << "PRIVACY_SMOKE_FAILED seeded=" << seeded
                << " cleared=" << cleared << " completed=" << completed
                << " result="
                << window->browsing_data_clear_result_for_testing() << Qt::endl;
        QCoreApplication::exit(11);
      }
      return;
    }
    if (*attempts > 160) {
      *output << "PRIVACY_SMOKE_FAILED timeout=1" << Qt::endl;
      QCoreApplication::exit(11);
      return;
    }
    QTimer::singleShot(50, window, [step] { (*step)(); });
  };
  QTimer::singleShot(50, window, [step] { (*step)(); });
}

void StartFaviconSmokeTest(MainWindow* window) {
  auto output = std::make_shared<QTextStream>(stdout);
  window->SetCurrentFaviconForTesting();
  if (window->current_tab_has_favicon_for_testing()) {
    *output << "FAVICON_SMOKE_OK tab_icon=visible" << Qt::endl;
    window->close();
  } else {
    *output << "FAVICON_SMOKE_FAILED tab_icon=missing" << Qt::endl;
    QCoreApplication::exit(12);
  }
}

void StartSecuritySmokeTest(MainWindow* window) {
  auto output = std::make_shared<QTextStream>(stdout);
  const QString media = window->media_permission_description_for_testing(
      CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE |
      CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE);
  const QString permissions = window->permission_description_for_testing(
      CEF_PERMISSION_TYPE_GEOLOCATION | CEF_PERMISSION_TYPE_NOTIFICATIONS);
  const bool media_ok = media.contains(QStringLiteral("microphone")) &&
                        media.contains(QStringLiteral("camera"));
  const bool permissions_ok =
      permissions.contains(QStringLiteral("location")) &&
      permissions.contains(QStringLiteral("notifications"));
  const bool schemes_ok =
      window->external_scheme_allowed_for_testing(
          QStringLiteral("mailto:test@example.com")) &&
      window->external_scheme_allowed_for_testing(
          QStringLiteral("magnet:?xt=urn:test")) &&
      !window->external_scheme_allowed_for_testing(
          QStringLiteral("javascript:alert(1)")) &&
      !window->external_scheme_allowed_for_testing(
          QStringLiteral("unknown-scheme:payload"));
  if (media_ok && permissions_ok && schemes_ok) {
    *output << "SECURITY_SMOKE_OK media=2 permissions=2 schemes=guarded"
            << Qt::endl;
    window->close();
  } else {
    *output << "SECURITY_SMOKE_FAILED media=" << media_ok
            << " permissions=" << permissions_ok
            << " schemes=" << schemes_ok << Qt::endl;
    QCoreApplication::exit(8);
  }
}

void StartAuthSmokeTest(MainWindow* window) {
  auto output = std::make_shared<QTextStream>(stdout);
  auto result = std::make_shared<AuthSmokeResult>();
  auto attempts = std::make_shared<int>(0);
  auto requested = std::make_shared<bool>(false);
  auto step = std::make_shared<std::function<void()>>();
  *step = [window, output, result, attempts, requested, step] {
    ++*attempts;
    if (!*requested && window->current_title() == QStringLiteral("Auth")) {
      *requested = window->ShowAuthForTesting(new AuthSmokeCallback(result));
    } else if (*requested && !result->cancelled) {
      if (QMessageBox* dialog = window->findChild<QMessageBox*>()) {
        for (QAbstractButton* button : dialog->buttons()) {
          if (dialog->buttonRole(button) == QMessageBox::RejectRole) {
            button->click();
            break;
          }
        }
      }
    } else if (result->cancelled && !result->continued) {
      *output << "AUTH_SMOKE_OK cancelled=1 credentials=persisted-none"
              << Qt::endl;
      window->close();
      return;
    }
    if (*attempts > 160) {
      *output << "AUTH_SMOKE_FAILED requested=" << *requested
              << " cancelled=" << result->cancelled
              << " continued=" << result->continued << Qt::endl;
      QCoreApplication::exit(9);
      return;
    }
    QTimer::singleShot(50, window, [step] { (*step)(); });
  };
  QTimer::singleShot(300, window, [step] { (*step)(); });
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
  std::unique_ptr<QTemporaryDir> smoke_cef_directory;
  if (IsSmokeTest()) {
    smoke_cef_directory = std::make_unique<QTemporaryDir>();
    if (!smoke_cef_directory->isValid()) return 8;
  }
  const QString requested_data_path = smoke_cef_directory
                                          ? smoke_cef_directory->path()
                                          : QStandardPaths::writableLocation(
                                                QStandardPaths::AppDataLocation);
  const QString local_data_path = smoke_cef_directory
                                      ? smoke_cef_directory->path()
                                      : QStandardPaths::writableLocation(
                                            QStandardPaths::AppLocalDataLocation);
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
    std::unique_ptr<QTemporaryDir> smoke_profile_directory;
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
      initial_session.recently_closed_urls = {
          QStringLiteral("https://example.test/closed")};
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

    const QString browsing_data_path =
        IsSmokeTest() ? QString()
                      : QDir(data_path).filePath(
                            QStringLiteral("browsing-data.json"));
    QString active_browsing_data_path = browsing_data_path;
    if (HasArgument(QStringLiteral("--smoke-test-profile")) ||
        HasArgument(QStringLiteral("--smoke-test-privacy"))) {
      smoke_profile_directory = std::make_unique<QTemporaryDir>();
      if (!smoke_profile_directory->isValid()) {
        CefShutdown();
        return 7;
      }
      active_browsing_data_path = smoke_profile_directory->filePath(
          QStringLiteral("browsing-data.json"));
    }
    MainWindow main_window(initial_session, active_session_path,
                           active_browsing_data_path);
    main_window.show();
    message_pump.Schedule(0);
    if (HasArgument(QStringLiteral("--smoke-test-tabs"))) {
      StartTabSmokeTest(&main_window);
    } else if (HasArgument(QStringLiteral("--smoke-test-downloads"))) {
      QTimer::singleShot(300, &main_window,
                         [&main_window] { StartDownloadSmokeTest(&main_window); });
    } else if (HasArgument(
                   QStringLiteral("--smoke-test-exit-protection"))) {
      QTimer::singleShot(300, &main_window, [&main_window] {
        StartExitProtectionSmokeTest(&main_window);
      });
    } else if (HasArgument(QStringLiteral("--smoke-test-failures"))) {
      StartFailureSmokeTest(&main_window);
    } else if (HasArgument(QStringLiteral("--smoke-test-session"))) {
      QTimer::singleShot(300, &main_window, [&main_window, active_session_path] {
        StartSessionSmokeTest(&main_window, active_session_path);
      });
    } else if (HasArgument(QStringLiteral("--smoke-test-page-tools"))) {
      StartPageToolsSmokeTest(&main_window);
    } else if (HasArgument(QStringLiteral("--smoke-test-profile"))) {
      QTimer::singleShot(300, &main_window,
                         [&main_window, active_browsing_data_path] {
                           StartProfileSmokeTest(&main_window,
                                                 active_browsing_data_path);
                         });
    } else if (HasArgument(QStringLiteral("--smoke-test-privacy"))) {
      QTimer::singleShot(300, &main_window,
                         [&main_window] { StartPrivacySmokeTest(&main_window); });
    } else if (HasArgument(QStringLiteral("--smoke-test-favicon"))) {
      QTimer::singleShot(300, &main_window,
                         [&main_window] { StartFaviconSmokeTest(&main_window); });
    } else if (HasArgument(QStringLiteral("--smoke-test-security"))) {
      QTimer::singleShot(300, &main_window,
                         [&main_window] { StartSecuritySmokeTest(&main_window); });
    } else if (HasArgument(QStringLiteral("--smoke-test-auth"))) {
      StartAuthSmokeTest(&main_window);
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

