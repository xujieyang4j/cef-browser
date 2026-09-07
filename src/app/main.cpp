#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <string>

#include <QApplication>
#include <QAbstractButton>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

#include "app/browser_app.h"
#include "app/single_instance.h"
#include "download/download_manager.h"
#include "include/cef_app.h"
#include "include/cef_command_line.h"
#include "include/wrapper/cef_helpers.h"
#include "profile/browsing_data_store.h"
#include "session/session_store.h"
#include "settings/browser_settings.h"
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

void StartTabActionsSmokeTest(MainWindow* window) {
  auto output = std::make_shared<QTextStream>(stdout);
  auto attempts = std::make_shared<int>(0);
  auto stage = std::make_shared<int>(0);
  const QString initial_url = QStringLiteral("data:text/html,<title>Smoke</title>");
  auto step = std::make_shared<std::function<void()>>();
  *step = [window, output, attempts, stage, step, initial_url] {
    ++*attempts;
    if (*stage == 0 && window->current_title() == QStringLiteral("Smoke")) {
      window->DuplicateCurrentTabForTesting();
      *stage = 1;
    } else if (*stage == 1 && window->tab_count() == 2 &&
               window->current_url() == initial_url) {
      window->OpenTabForTesting(
          QStringLiteral("data:text/html,<title>Third</title>"));
      window->OpenTabForTesting(
          QStringLiteral("data:text/html,<title>Fourth</title>"));
      window->ActivateTabForTesting(1);
      window->CloseTabsToRightForTesting();
      *stage = 2;
    } else if (*stage == 2 && window->tab_count() == 2 &&
               window->current_url() == initial_url) {
      window->OpenTabForTesting(
          QStringLiteral("data:text/html,<title>Right One</title>"));
      window->OpenTabForTesting(
          QStringLiteral("data:text/html,<title>Right Two</title>"));
      window->ActivateTabForTesting(1);
      window->CloseOtherTabsForTesting();
      *stage = 3;
    } else if (*stage == 3 && window->tab_count() == 1 &&
               window->current_url() == initial_url) {
      window->ReopenClosedTabForTesting();
      *stage = 4;
    } else if (*stage == 4 && window->tab_count() == 2) {
      *output << "TAB_ACTIONS_SMOKE_OK duplicate=1 close_right=2 "
                 "close_others=3 reopened=1"
              << Qt::endl;
      window->close();
      return;
    }
    if (*attempts > 200) {
      *output << "TAB_ACTIONS_SMOKE_FAILED stage=" << *stage
              << " count=" << window->tab_count()
              << " url=" << window->current_url() << Qt::endl;
      QCoreApplication::exit(13);
      return;
    }
    QTimer::singleShot(50, window, [step] { (*step)(); });
  };
  QTimer::singleShot(300, window, [step] { (*step)(); });
}

void StartPinnedTabsSmokeTest(MainWindow* window, const QString& session_path) {
  auto output = std::make_shared<QTextStream>(stdout);
  auto attempts = std::make_shared<int>(0);
  auto stage = std::make_shared<int>(0);
  auto step = std::make_shared<std::function<void()>>();
  *step = [window, output, attempts, stage, step, session_path] {
    ++*attempts;
    if (*stage == 0 && window->current_title() == QStringLiteral("Smoke")) {
      window->OpenTabForTesting(
          QStringLiteral("data:text/html,<title>Pinned</title>"));
      *stage = 1;
    } else if (*stage == 1 &&
               window->current_title() == QStringLiteral("Pinned")) {
      window->ToggleCurrentTabPinnedForTesting();
      window->MoveCurrentTabForTesting(1);
      *stage = 2;
    } else if (*stage == 2 && window->current_tab_pinned_for_testing() &&
               window->pinned_tab_count_for_testing() == 1 &&
               window->current_tab_index_for_testing() == 0) {
      window->OpenTabForTesting(
          QStringLiteral("data:text/html,<title>Disposable</title>"));
      window->CloseOtherTabsForTesting();
      *stage = 3;
    } else if (*stage == 3 && window->tab_count() == 2) {
      window->ActivateTabForTesting(0);
      const BrowserSession session = window->session_for_testing(true);
      BrowserSession persisted_session = session;
      persisted_session.tab_urls = {
          QStringLiteral("https://example.test/pinned"),
          QStringLiteral("about:blank")};
      const bool saved = SessionStore::Save(session_path, persisted_session);
      const auto restored = SessionStore::Load(session_path);
      const bool session_ok = session.tab_urls.size() == 2 &&
                              session.tab_pinned.size() == 2 &&
                              session.tab_pinned.first() && saved && restored &&
                              restored->tab_pinned == session.tab_pinned;
      if (!session_ok) {
        *output << "PINNED_TABS_SMOKE_FAILED session=0" << Qt::endl;
        QCoreApplication::exit(15);
        return;
      }
      window->CloseCurrentTabForTesting();
      *stage = 4;
    } else if (*stage == 4 && window->tab_count() == 1 &&
               window->pinned_tab_count_for_testing() == 0) {
      window->ReopenClosedTabForTesting();
      *stage = 5;
    } else if (*stage == 5 && window->tab_count() == 2 &&
               window->current_title() == QStringLiteral("Pinned") &&
               !window->current_tab_pinned_for_testing()) {
      *output << "PINNED_TABS_SMOKE_OK grouped=1 protected=1 persisted=1 "
                 "explicit_close=1"
              << Qt::endl;
      window->close();
      return;
    }
    if (*attempts > 220) {
      *output << "PINNED_TABS_SMOKE_FAILED stage=" << *stage
              << " tabs=" << window->tab_count()
              << " pinned=" << window->pinned_tab_count_for_testing()
              << Qt::endl;
      QCoreApplication::exit(15);
      return;
    }
    QTimer::singleShot(50, window, [step] { (*step)(); });
  };
  QTimer::singleShot(300, window, [step] { (*step)(); });
}

void StartDownloadSmokeTest(MainWindow* window,
                            const QString& download_history_path) {
  auto output = std::make_shared<QTextStream>(stdout);
  window->UpdateDownloadForTesting(41, 25, false);
  const bool started = window->download_count_for_testing() == 1 &&
                       window->active_download_count_for_testing() == 1 &&
                       window->download_status_for_testing(41).startsWith(
                           QStringLiteral("25%"));
  const bool active_protected = !window->RemoveDownloadForTesting(41);
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
  DownloadManager restored(download_history_path);
  const bool history_loaded = restored.LoadHistory();
  const QList<DownloadManager::Item> restored_items = restored.items();
  const bool persisted =
      history_loaded && restored_items.size() == 1 &&
      restored_items.first().file_name == QStringLiteral("trail-test.bin") &&
      restored_items.first().state == DownloadManager::State::Complete &&
      !restored.CanOpenDownload(restored_items.first().id) &&
      !restored.CanShowDownloadInFolder(restored_items.first().id);
  const bool removed = window->RemoveDownloadForTesting(41);
  DownloadManager cleared_history(download_history_path);
  const bool empty_after_clear =
      removed && cleared_history.LoadHistory() &&
      cleared_history.items().isEmpty();

  const QDir history_directory = QFileInfo(download_history_path).absoluteDir();
  const QString existing_path =
      history_directory.filePath(QStringLiteral("verified-download.bin"));
  const QString missing_path =
      history_directory.filePath(QStringLiteral("missing-download.bin"));
  QFile existing_file(existing_path);
  const bool existing_opened = existing_file.open(QIODevice::WriteOnly);
  const bool existing_written =
      existing_opened && existing_file.write("trail") == 5;
  existing_file.close();

  QFile tampered_history(download_history_path);
  const bool tampered_opened = tampered_history.open(QIODevice::WriteOnly);
  bool tampered_written = false;
  if (tampered_opened) {
    const auto record = [](const QString& url, const QString& path,
                           const QString& name, const QString& state) {
      return QJsonObject{
          {QStringLiteral("fileName"), name},
          {QStringLiteral("fullPath"), path},
          {QStringLiteral("url"), url},
          {QStringLiteral("receivedBytes"), 5},
          {QStringLiteral("totalBytes"), 5},
          {QStringLiteral("percent"), 100},
          {QStringLiteral("state"), state},
      };
    };
    const QJsonObject tampered_root{
        {QStringLiteral("version"), 1},
        {QStringLiteral("downloads"),
         QJsonArray{
             record(QStringLiteral("HTTPS://example.test/safe.bin"),
                    existing_path, QStringLiteral("../spoofed.app"),
                    QStringLiteral("complete")),
             record(QStringLiteral("javascript:alert(1)"), existing_path,
                    QStringLiteral("unsafe.app"),
                    QStringLiteral("complete")),
             record(QStringLiteral("https://example.test/relative.bin"),
                    QStringLiteral("../relative.bin"),
                    QStringLiteral("../display.bin"),
                    QStringLiteral("complete")),
             record(QStringLiteral("https://example.test/missing.bin"),
                    missing_path, QStringLiteral("spoofed.bin"),
                    QStringLiteral("complete")),
             record(QStringLiteral("https://example.test/active.bin"),
                    existing_path, QStringLiteral("active.bin"),
                    QStringLiteral("in_progress"))}}};
    const QByteArray bytes =
        QJsonDocument(tampered_root).toJson(QJsonDocument::Compact);
    tampered_written = tampered_history.write(bytes) == bytes.size();
    tampered_history.close();
  }

  DownloadManager untrusted(download_history_path);
  const bool untrusted_loaded = untrusted.LoadHistory();
  const QList<DownloadManager::Item> untrusted_items = untrusted.items();
  const bool tampered_filtered =
      existing_written && tampered_written && untrusted_loaded &&
      untrusted_items.size() == 3 &&
      untrusted_items.at(0).url ==
          QStringLiteral("https://example.test/safe.bin") &&
      untrusted_items.at(0).full_path == existing_path &&
      untrusted_items.at(0).file_name ==
          QStringLiteral("verified-download.bin") &&
      untrusted_items.at(1).full_path.isEmpty() &&
      untrusted_items.at(1).file_name == QStringLiteral("display.bin") &&
      untrusted_items.at(2).full_path == missing_path &&
      untrusted_items.at(2).file_name ==
          QStringLiteral("missing-download.bin");
  bool restored_actions_blocked = tampered_filtered;
  for (const DownloadManager::Item& item : untrusted_items) {
    restored_actions_blocked =
        restored_actions_blocked && !untrusted.CanOpenDownload(item.id) &&
        !untrusted.CanShowDownloadInFolder(item.id) &&
        !untrusted.OpenDownload(item.id) &&
        !untrusted.ShowDownloadInFolder(item.id);
  }

  DownloadManager live_downloads;
  DownloadManager::Item live_item;
  live_item.id = 51;
  live_item.file_name = QStringLiteral("spoofed.app");
  live_item.full_path = existing_path;
  live_item.url = QStringLiteral("https://example.test/live.bin");
  live_item.state = DownloadManager::State::Complete;
  live_downloads.UpdateForTesting(live_item);
  const auto normalized_live_item = live_downloads.item(live_item.id);
  const bool live_actions_validated =
      normalized_live_item &&
      normalized_live_item->file_name ==
          QStringLiteral("verified-download.bin") &&
      live_downloads.CanOpenDownload(live_item.id) &&
      live_downloads.CanShowDownloadInFolder(live_item.id);
  live_item.id = 52;
  live_item.full_path = missing_path;
  live_item.state = DownloadManager::State::Complete;
  live_downloads.UpdateForTesting(live_item);
  const bool missing_file_blocked =
      !live_downloads.CanOpenDownload(live_item.id);
  live_item.id = 53;
  live_item.full_path = existing_path;
  live_item.state = DownloadManager::State::Interrupted;
  live_downloads.UpdateForTesting(live_item);
  const bool incomplete_file_blocked =
      !live_downloads.CanOpenDownload(live_item.id);
  live_item.id = 54;
  live_item.full_path = QStringLiteral("../relative.bin");
  live_item.state = DownloadManager::State::Complete;
  live_downloads.UpdateForTesting(live_item);
  const bool relative_path_blocked =
      live_downloads.item(live_item.id)->full_path.isEmpty() &&
      !live_downloads.CanOpenDownload(live_item.id) &&
      !live_downloads.CanShowDownloadInFolder(live_item.id);

  if (started && paused && resumed && completed && persisted &&
      active_protected && empty_after_clear && tampered_filtered &&
      restored_actions_blocked && live_actions_validated &&
      missing_file_blocked && incomplete_file_blocked &&
      relative_path_blocked) {
    *output << "DOWNLOAD_SMOKE_OK paused=1 resumed=1 status=Complete "
               "persisted=1 removed=1 untrusted=blocked local_paths=validated"
            << Qt::endl;
    window->close();
  } else {
    *output << "DOWNLOAD_SMOKE_FAILED started=" << started
            << " paused=" << paused << " resumed=" << resumed
            << " completed=" << completed << " persisted=" << persisted
            << " protected=" << active_protected
            << " removed=" << empty_after_clear
            << " filtered=" << tampered_filtered
            << " restored_actions=" << restored_actions_blocked
            << " live_actions=" << live_actions_validated
            << " missing=" << missing_file_blocked
            << " incomplete=" << incomplete_file_blocked
            << " relative=" << relative_path_blocked << Qt::endl;
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
         HasArgument(QStringLiteral("--smoke-test-tab-actions")) ||
         HasArgument(QStringLiteral("--smoke-test-pinned-tabs")) ||
         HasArgument(QStringLiteral("--smoke-test-downloads")) ||
         HasArgument(QStringLiteral("--smoke-test-exit-protection")) ||
         HasArgument(QStringLiteral("--smoke-test-failures")) ||
         HasArgument(QStringLiteral("--smoke-test-session")) ||
         HasArgument(QStringLiteral("--smoke-test-page-tools")) ||
         HasArgument(QStringLiteral("--smoke-test-profile")) ||
         HasArgument(QStringLiteral("--smoke-test-privacy")) ||
         HasArgument(QStringLiteral("--smoke-test-favicon")) ||
         HasArgument(QStringLiteral("--smoke-test-audio")) ||
         HasArgument(QStringLiteral("--smoke-test-browser-surfaces")) ||
         HasArgument(QStringLiteral("--smoke-test-tab-navigation")) ||
         HasArgument(QStringLiteral("--smoke-test-recent-tabs")) ||
         HasArgument(QStringLiteral("--smoke-test-application-menu")) ||
         HasArgument(QStringLiteral("--smoke-test-search-settings")) ||
         HasArgument(QStringLiteral("--smoke-test-single-instance")) ||
         HasArgument(QStringLiteral("--smoke-test-sandbox")) ||
         HasArgument(QStringLiteral("--smoke-test-security")) ||
         HasArgument(QStringLiteral("--smoke-test-auth"));
}

BrowserSession DefaultSession(const QString& url) {
  BrowserSession session;
  session.tab_urls = {url};
  return session;
}

BrowserSession SelectInitialSession(
    const BrowserSettings& settings, const std::optional<QString>& explicit_url,
    const std::optional<BrowserSession>& restored) {
  if (explicit_url) {
    const auto normalized = BrowserSettings::NormalizeNavigationInput(
        settings.search_engine(), *explicit_url);
    return DefaultSession(normalized.value_or(settings.home_page()));
  }
  switch (settings.startup_behavior()) {
    case BrowserSettings::StartupBehavior::RestoreSession:
      return restored.value_or(DefaultSession(settings.home_page()));
    case BrowserSettings::StartupBehavior::HomePage:
      return DefaultSession(settings.home_page());
    case BrowserSettings::StartupBehavior::BlankPage:
      return DefaultSession(QStringLiteral("about:blank"));
  }
  return DefaultSession(settings.home_page());
}

void StartSessionSmokeTest(MainWindow* window, const QString& session_path) {
  auto output = std::make_shared<QTextStream>(stdout);
  const BrowserSession captured = window->session_for_testing(false);
  const bool captured_ok =
      captured.tab_urls.size() == 2 && captured.active_tab == 1 &&
      !captured.window_geometry.isEmpty() && !captured.clean_exit &&
      captured.recently_closed_tabs ==
          QList<RecentlyClosedTab>{
              {QStringLiteral("https://example.test/closed"),
               QStringLiteral("Closed page")}};
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
                        clean->recently_closed_tabs ==
                            captured.recently_closed_tabs;

  const QString legacy_path = session_path + QStringLiteral(".v1");
  QFile legacy_file(legacy_path);
  const bool legacy_opened = legacy_file.open(QIODevice::WriteOnly);
  bool legacy_written = false;
  if (legacy_opened) {
    const QJsonObject legacy_root{
        {QStringLiteral("version"), 1},
        {QStringLiteral("cleanExit"), true},
        {QStringLiteral("activeTab"), 0},
        {QStringLiteral("tabs"),
         QJsonArray{QJsonObject{
             {QStringLiteral("url"), QStringLiteral("about:blank")},
             {QStringLiteral("pinned"), false}}}},
        {QStringLiteral("recentlyClosed"),
         QJsonArray{QStringLiteral("https://example.test/legacy")}},
    };
    legacy_written =
        legacy_file.write(QJsonDocument(legacy_root).toJson(
            QJsonDocument::Compact)) > 0;
    legacy_file.close();
  }
  const auto legacy = SessionStore::Load(legacy_path);
  const bool legacy_ok =
      legacy_written && legacy && legacy->recently_closed_tabs.size() == 1 &&
      legacy->recently_closed_tabs.first().url ==
          QStringLiteral("https://example.test/legacy") &&
      legacy->recently_closed_tabs.first().title.isEmpty();

  const QString untrusted_path = session_path + QStringLiteral(".untrusted");
  QFile untrusted_file(untrusted_path);
  const bool untrusted_opened = untrusted_file.open(QIODevice::WriteOnly);
  bool untrusted_written = false;
  if (untrusted_opened) {
    const QJsonObject untrusted_root{
        {QStringLiteral("version"), 2},
        {QStringLiteral("cleanExit"), true},
        {QStringLiteral("activeTab"), 1},
        {QStringLiteral("tabs"),
         QJsonArray{
             QJsonObject{{QStringLiteral("url"),
                          QStringLiteral("https://example.test/before")},
                         {QStringLiteral("pinned"), true}},
             QJsonObject{{QStringLiteral("url"),
                          QStringLiteral("javascript:alert(1)")},
                         {QStringLiteral("pinned"), true}},
             QJsonObject{{QStringLiteral("url"),
                          QStringLiteral("HTTPS://example.test/after")},
                         {QStringLiteral("pinned"), false}},
             QJsonObject{{QStringLiteral("url"),
                          QStringLiteral("data:text/html,unsafe")},
                         {QStringLiteral("pinned"), false}},
             QJsonObject{{QStringLiteral("url"),
                          QStringLiteral("file:///tmp/restored%20file.html")},
                         {QStringLiteral("pinned"), false}}}},
        {QStringLiteral("recentlyClosed"),
         QJsonArray{
             QJsonObject{{QStringLiteral("url"),
                          QStringLiteral("unknown-scheme:payload")},
                         {QStringLiteral("title"),
                          QStringLiteral("Unsafe")}},
             QJsonObject{{QStringLiteral("url"),
                          QStringLiteral("https://example.test/recent")},
                         {QStringLiteral("title"),
                          QStringLiteral("Safe")}}}},
    };
    const QByteArray bytes =
        QJsonDocument(untrusted_root).toJson(QJsonDocument::Compact);
    untrusted_written = untrusted_file.write(bytes) == bytes.size();
    untrusted_file.close();
  }
  const auto untrusted = SessionStore::Load(untrusted_path);
  const bool untrusted_filtered =
      untrusted_written && untrusted &&
      untrusted->tab_urls ==
          QStringList{QStringLiteral("https://example.test/before"),
                      QStringLiteral("https://example.test/after"),
                      QStringLiteral("file:///tmp/restored%20file.html")} &&
      untrusted->tab_pinned == QList<bool>{true, false, false} &&
      untrusted->active_tab == 1 &&
      untrusted->recently_closed_tabs ==
          QList<RecentlyClosedTab>{
              {QStringLiteral("https://example.test/recent"),
               QStringLiteral("Safe")}};

  const QString rejected_path = session_path + QStringLiteral(".rejected");
  QFile rejected_file(rejected_path);
  const bool rejected_opened = rejected_file.open(QIODevice::WriteOnly);
  bool rejected_written = false;
  if (rejected_opened) {
    const QJsonObject rejected_root{
        {QStringLiteral("version"), 2},
        {QStringLiteral("tabs"),
         QJsonArray{QJsonObject{{QStringLiteral("url"),
                                 QStringLiteral("javascript:alert(1)")}}}}};
    const QByteArray bytes =
        QJsonDocument(rejected_root).toJson(QJsonDocument::Compact);
    rejected_written = rejected_file.write(bytes) == bytes.size();
    rejected_file.close();
  }
  QString rejected_error;
  const auto rejected = SessionStore::Load(rejected_path, &rejected_error);
  const bool all_unsafe_rejected =
      rejected_written && !rejected && !rejected_error.isEmpty();

  BrowserSession unsanitized;
  unsanitized.tab_urls = {QStringLiteral("https://example.test/saved"),
                          QStringLiteral("data:text/html,unsafe"),
                          QStringLiteral("file:///tmp/saved%20file.html")};
  unsanitized.tab_pinned = {true, true, false};
  unsanitized.active_tab = 1;
  unsanitized.recently_closed_tabs = {
      {QStringLiteral("javascript:alert(1)"), QStringLiteral("Unsafe")},
      {QStringLiteral("https://example.test/saved-recent"),
       QStringLiteral("Safe")}};
  const QString sanitized_path = session_path + QStringLiteral(".sanitized");
  const bool sanitized_saved = SessionStore::Save(sanitized_path, unsanitized);
  const auto sanitized = SessionStore::Load(sanitized_path);
  const bool unsafe_not_saved =
      sanitized_saved && sanitized &&
      sanitized->tab_urls ==
          QStringList{QStringLiteral("https://example.test/saved"),
                      QStringLiteral("file:///tmp/saved%20file.html")} &&
      sanitized->tab_pinned == QList<bool>{true, false} &&
      sanitized->active_tab == 1 &&
      sanitized->recently_closed_tabs ==
          QList<RecentlyClosedTab>{
              {QStringLiteral("https://example.test/saved-recent"),
               QStringLiteral("Safe")}};
  if (captured_ok && unclean_saved && unclean_ok && clean_saved && clean_ok &&
      legacy_ok && untrusted_filtered && all_unsafe_rejected &&
      unsafe_not_saved) {
    *output << "SESSION_SMOKE_OK tabs=" << clean->tab_urls.size()
            << " active=" << clean->active_tab
            << " recent_title=persisted legacy=migrated unsafe=filtered"
            << Qt::endl;
    window->close();
  } else {
    *output << "SESSION_SMOKE_FAILED captured=" << captured_ok
            << " unclean=" << unclean_ok << " clean=" << clean_ok
            << " legacy=" << legacy_ok
            << " untrusted=" << untrusted_filtered
            << " rejected=" << all_unsafe_rejected
            << " sanitized=" << unsafe_not_saved
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
  const QString imported_url =
      QStringLiteral("https://example.test/imported?a=1&b=2");
  const QString import_path = data_path + QStringLiteral(".import.html");
  QFile import_file(import_path);
  const bool import_source_opened = import_file.open(QIODevice::WriteOnly);
  const QByteArray import_html =
      "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n<DL><p>\n"
      "<DT><A HREF=\"https://example.test/first\">Duplicate</A>\n"
      "<DT><A HREF=\"https://example.test/imported?a=1&amp;b=2\">"
      "Imported &amp; &lt;Safe&gt;</A>\n"
      "<DT><A HREF=\"javascript:alert(1)\">Unsafe</A>\n"
      "</DL><p>\n";
  const bool import_source_written =
      import_source_opened && import_file.write(import_html) == import_html.size();
  import_file.close();
  BrowsingDataStore transfer(data_path + QStringLiteral(".transfer.json"));
  const bool transfer_seeded = transfer.AddBookmark(
      first_url, QStringLiteral("Existing & <First>"));
  int imported_count = 0;
  const bool imported =
      transfer.ImportBookmarksHtml(import_path, &imported_count);
  const bool import_ok =
      import_source_written && transfer_seeded && imported &&
      imported_count == 1 && transfer.bookmarks().size() == 2 &&
      transfer.bookmarks().first().url == imported_url &&
      transfer.bookmarks().first().title ==
          QStringLiteral("Imported & <Safe>");
  const QString export_path = data_path + QStringLiteral(".export.html");
  const bool exported = transfer.ExportBookmarksHtml(export_path);
  BrowsingDataStore round_trip(data_path + QStringLiteral(".roundtrip.json"));
  int round_trip_count = 0;
  const bool round_trip_ok =
      exported &&
      round_trip.ImportBookmarksHtml(export_path, &round_trip_count) &&
      round_trip_count == 2 && round_trip.bookmarks().size() == 2 &&
      round_trip.bookmarks().first().url == imported_url &&
      round_trip.bookmarks().first().title ==
          QStringLiteral("Imported & <Safe>");
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
  const QString untrusted_data_path =
      data_path + QStringLiteral(".untrusted.json");
  QFile untrusted_data_file(untrusted_data_path);
  const bool untrusted_data_opened =
      untrusted_data_file.open(QIODevice::WriteOnly);
  bool untrusted_data_written = false;
  if (untrusted_data_opened) {
    const QString oversized_title(600, QLatin1Char('x'));
    const QJsonObject untrusted_root{
        {QStringLiteral("version"), 1},
        {QStringLiteral("bookmarks"),
         QJsonArray{
             QJsonObject{{QStringLiteral("url"), first_url},
                         {QStringLiteral("title"), oversized_title},
                         {QStringLiteral("createdAt"),
                          QStringLiteral("not-a-date")}},
             QJsonObject{{QStringLiteral("url"),
                          QStringLiteral("javascript:alert(1)")},
                         {QStringLiteral("title"),
                          QStringLiteral("Unsafe")}},
             QJsonObject{{QStringLiteral("url"), first_url},
                         {QStringLiteral("title"),
                          QStringLiteral("Duplicate")}}}},
        {QStringLiteral("history"),
         QJsonArray{
             QJsonObject{{QStringLiteral("url"), second_url},
                         {QStringLiteral("title"),
                          QStringLiteral("  Safe\n\tHistory  ")},
                         {QStringLiteral("lastVisitedAt"),
                          QStringLiteral("invalid")},
                         {QStringLiteral("visitCount"),
                          std::numeric_limits<int>::max()}},
             QJsonObject{{QStringLiteral("url"),
                          QStringLiteral("data:text/html,unsafe")},
                         {QStringLiteral("title"),
                          QStringLiteral("Unsafe")},
                         {QStringLiteral("visitCount"), 1}},
             QJsonObject{{QStringLiteral("url"), second_url},
                         {QStringLiteral("title"),
                          QStringLiteral("Duplicate")},
                         {QStringLiteral("visitCount"), 1}}}},
    };
    const QByteArray bytes =
        QJsonDocument(untrusted_root).toJson(QJsonDocument::Compact);
    untrusted_data_written =
        untrusted_data_file.write(bytes) == bytes.size();
    untrusted_data_file.close();
  }
  BrowsingDataStore untrusted_data(untrusted_data_path);
  const bool untrusted_loaded = untrusted_data.Load();
  untrusted_data.RecordVisit(
      second_url, QStringLiteral("  Updated\r\nHistory  "),
      QDateTime::fromSecsSinceEpoch(123, Qt::UTC));
  const bool stored_data_sanitized =
      untrusted_data_written && untrusted_loaded &&
      untrusted_data.bookmarks().size() == 1 &&
      untrusted_data.bookmarks().first().url == first_url &&
      untrusted_data.bookmarks().first().title.size() == 512 &&
      untrusted_data.bookmarks().first().created_at ==
          QDateTime::fromSecsSinceEpoch(0, Qt::UTC) &&
      untrusted_data.history().size() == 1 &&
      untrusted_data.history().first().url == second_url &&
      untrusted_data.history().first().title ==
          QStringLiteral("Updated History") &&
      untrusted_data.history().first().visit_count ==
          std::numeric_limits<int>::max() &&
      untrusted_data.history().first().last_visited_at ==
          QDateTime::fromSecsSinceEpoch(123, Qt::UTC);
  const bool removed = restored.RemoveBookmark(first_url) &&
                       !restored.IsBookmarked(first_url);
  const QString bookmarked_url = window->current_url();
  window->ToggleBookmarkForTesting();
  window->AddHistoryForTesting(bookmarked_url,
                               QStringLiteral("Lower-ranked history title"));
  window->AddHistoryForTesting(first_url, QStringLiteral("First again"));
  const QStringList suggestion_urls =
      window->address_suggestions_for_testing();
  const bool suggestions_ok = !suggestion_urls.isEmpty() &&
                              suggestion_urls.first() == bookmarked_url &&
                              suggestion_urls.count(bookmarked_url) == 1 &&
                              suggestion_urls.contains(first_url);
  QString first_label;
  for (const QString& label :
       window->address_suggestion_labels_for_testing()) {
    if (label.startsWith(QStringLiteral("First again — "))) {
      first_label = label;
      break;
    }
  }
  const bool renamed =
      window->RenameBookmarkForTesting(bookmarked_url,
                                       QStringLiteral("  Renamed smoke  ")) &&
      window->bookmark_title_for_testing(bookmarked_url) ==
          QStringLiteral("Renamed smoke");
  BrowsingDataStore after_rename(data_path);
  const bool rename_persisted =
      after_rename.Load() && after_rename.IsBookmarked(bookmarked_url) &&
      !after_rename.bookmarks().isEmpty() &&
      after_rename.bookmarks().first().title ==
          QStringLiteral("Renamed smoke");
  const bool empty_name =
      window->RenameBookmarkForTesting(bookmarked_url, QStringLiteral("   ")) &&
      window->bookmark_title_for_testing(bookmarked_url).isEmpty() &&
      window->bookmark_label_for_testing(bookmarked_url) == bookmarked_url;
  BrowsingDataStore after_empty_name(data_path);
  const bool empty_name_persisted =
      after_empty_name.Load() && !after_empty_name.bookmarks().isEmpty() &&
      after_empty_name.bookmarks().first().title.isEmpty();
  const bool single_removed =
      window->RemoveBookmarkForTesting(bookmarked_url) &&
      window->RemoveHistoryForTesting(bookmarked_url);
  BrowsingDataStore after_single_remove(data_path);
  const bool single_persisted =
      after_single_remove.Load() &&
      !after_single_remove.IsBookmarked(bookmarked_url) &&
      after_single_remove.history().size() == 1 &&
      after_single_remove.history().first().url == first_url;
  const bool profile_ok = add_first && reject_duplicate && add_second && saved &&
                          import_ok && round_trip_ok &&
                          bookmark_ok && history_ok && stored_data_sanitized &&
                          removed && suggestions_ok && !first_label.isEmpty() &&
                          renamed &&
                          rename_persisted && empty_name &&
                          empty_name_persisted && single_removed &&
                          single_persisted;
  if (!profile_ok) {
    *output << "PROFILE_SMOKE_FAILED bookmark=" << bookmark_ok
            << " import=" << import_ok << " roundtrip=" << round_trip_ok
            << " history=" << history_ok << " removed=" << removed
            << " stored_data=" << stored_data_sanitized
            << " suggestions=" << suggestions_ok
            << " titled=" << !first_label.isEmpty()
            << " renamed=" << rename_persisted
            << " empty=" << empty_name_persisted
            << " single=" << single_persisted
            << Qt::endl;
    QCoreApplication::exit(7);
    return;
  }

  window->NavigateAddressSuggestionForTesting(first_label);
  auto attempts = std::make_shared<int>(0);
  auto step = std::make_shared<std::function<void()>>();
  *step = [window, output, attempts, step, first_url] {
    ++*attempts;
    if (window->current_url() == first_url) {
      *output << "PROFILE_SMOKE_OK bookmarks=2 history=2 visits=2 "
                 "html=roundtrip "
                 "rename=persisted empty=url single-remove=persisted "
                 "suggestions=titled-navigation"
              << Qt::endl;
      window->close();
      return;
    }
    if (*attempts > 100) {
      *output << "PROFILE_SMOKE_FAILED navigation=0 url="
              << window->current_url() << Qt::endl;
      QCoreApplication::exit(7);
      return;
    }
    QTimer::singleShot(50, window, [step] { (*step)(); });
  };
  QTimer::singleShot(50, window, [step] { (*step)(); });
}

void StartPrivacySmokeTest(MainWindow* window, const QString& session_path,
                           const QString& download_history_path) {
  auto output = std::make_shared<QTextStream>(stdout);
  auto attempts = std::make_shared<int>(0);
  auto stage = std::make_shared<int>(0);
  auto step = std::make_shared<std::function<void()>>();
  *step = [window, output, attempts, stage, step, session_path,
           download_history_path] {
    ++*attempts;
    if (*stage == 0 && window->current_title() == QStringLiteral("Smoke")) {
      window->OpenTabForTesting(
          QStringLiteral("data:text/html,<title>Private closed tab</title>"));
      *stage = 1;
    } else if (*stage == 1 &&
               window->current_title() ==
                   QStringLiteral("Private closed tab")) {
      window->CloseCurrentTabForTesting();
      *stage = 2;
    } else if (*stage == 2 && window->tab_count() == 1 &&
               window->recently_closed_tab_count_for_testing() == 1) {
      window->AddHistoryForTesting(
          QStringLiteral("https://example.test/private"),
          QStringLiteral("Private visit"));
      window->UpdateDownloadForTesting(91, 100, true);
      // Keep one persistable tab in this otherwise data:-URL-only smoke
      // session so clearing recently closed tabs can commit the session.
      window->OpenTabForTesting(QStringLiteral("about:blank"));
      const bool seeded =
          window->history_count_for_testing() == 1 &&
          window->download_count_for_testing() == 1 &&
          window->address_suggestions_for_testing().contains(
              QStringLiteral("https://example.test/private"));
      if (!seeded) {
        *output << "PRIVACY_SMOKE_FAILED seeded=0" << Qt::endl;
        QCoreApplication::exit(11);
        return;
      }
      window->ClearBrowsingDataForTesting(true, false, false, false);
      *stage = 3;
    } else if (*stage == 3 &&
               !window->browsing_data_clear_in_progress_for_testing()) {
      const bool selective = window->history_count_for_testing() == 0 &&
                             window->download_count_for_testing() == 1 &&
                             window->recently_closed_tab_count_for_testing() ==
                                 1;
      if (!selective) {
        *output << "PRIVACY_SMOKE_FAILED selective=0" << Qt::endl;
        QCoreApplication::exit(11);
        return;
      }
      window->ClearBrowsingDataForTesting(false, true, true, true);
      *stage = 4;
    } else if (*stage == 4 &&
               !window->browsing_data_clear_in_progress_for_testing()) {
      const bool cleared = window->history_count_for_testing() == 0 &&
                           window->download_count_for_testing() == 0 &&
                           !window->address_suggestions_for_testing().contains(
                               QStringLiteral("https://example.test/private")) &&
                           window->recently_closed_tab_count_for_testing() == 0;
      const auto restored = SessionStore::Load(session_path);
      const bool session_cleared =
          restored && restored->recently_closed_tabs.isEmpty();
      DownloadManager restored_downloads(download_history_path);
      const bool downloads_cleared = restored_downloads.LoadHistory() &&
                                     restored_downloads.items().isEmpty();
      const bool completed =
          window->browsing_data_clear_result_for_testing().startsWith(
              QStringLiteral("Cleared:"));
      if (cleared && session_cleared && downloads_cleared && completed) {
        *output << "PRIVACY_SMOKE_OK selective=preserved history=cleared "
                   "recent=cleared "
                   "downloads=cleared session=cleared cef=completed"
                << Qt::endl;
        window->close();
      } else {
        *output << "PRIVACY_SMOKE_FAILED cleared=" << cleared
                << " session=" << session_cleared
                << " downloads=" << downloads_cleared
                << " completed=" << completed
                << " result="
                << window->browsing_data_clear_result_for_testing() << Qt::endl;
        QCoreApplication::exit(11);
      }
      return;
    }
    if (*attempts > 200) {
      *output << "PRIVACY_SMOKE_FAILED timeout=1 stage=" << *stage
              << Qt::endl;
      QCoreApplication::exit(11);
      return;
    }
    QTimer::singleShot(50, window, [step] { (*step)(); });
  };
  QTimer::singleShot(300, window, [step] { (*step)(); });
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

void StartAudioSmokeTest(MainWindow* window) {
  auto output = std::make_shared<QTextStream>(stdout);
  auto attempts = std::make_shared<int>(0);
  auto stage = std::make_shared<int>(0);
  auto step = std::make_shared<std::function<void()>>();
  *step = [window, output, attempts, stage, step] {
    ++*attempts;
    if (*stage == 0 && window->current_title() == QStringLiteral("Smoke")) {
      window->SetCurrentAudioStateForTesting(true, false);
      *stage = 1;
    } else if (*stage == 1 &&
               window->current_tab_text_for_testing().startsWith(
                   QStringLiteral("\U0001F50A "))) {
      window->ToggleCurrentAudioMutedForTesting();
      *stage = 2;
    } else if (*stage == 2 && window->current_audio_muted_for_testing() &&
               window->current_tab_text_for_testing().startsWith(
                   QStringLiteral("\U0001F507 "))) {
      window->ToggleCurrentAudioMutedForTesting();
      *stage = 3;
    } else if (*stage == 3 && !window->current_audio_muted_for_testing() &&
               window->current_tab_text_for_testing().startsWith(
                   QStringLiteral("\U0001F50A "))) {
      window->SetCurrentAudioStateForTesting(false, false);
      *stage = 4;
    } else if (*stage == 4 &&
               !window->current_tab_text_for_testing().startsWith(
                   QStringLiteral("\U0001F50A "))) {
      *output << "AUDIO_SMOKE_OK playing=visible mute=roundtrip stopped=clean"
              << Qt::endl;
      window->close();
      return;
    }
    if (*attempts > 160) {
      *output << "AUDIO_SMOKE_FAILED stage=" << *stage
              << " muted=" << window->current_audio_muted_for_testing()
              << " text=" << window->current_tab_text_for_testing()
              << Qt::endl;
      QCoreApplication::exit(14);
      return;
    }
    QTimer::singleShot(50, window, [step] { (*step)(); });
  };
  QTimer::singleShot(300, window, [step] { (*step)(); });
}

void StartBrowserSurfacesSmokeTest(MainWindow* window) {
  auto output = std::make_shared<QTextStream>(stdout);
  auto attempts = std::make_shared<int>(0);
  auto stage = std::make_shared<int>(0);
  auto step = std::make_shared<std::function<void()>>();
  *step = [window, output, attempts, stage, step] {
    ++*attempts;
    if (*stage == 0 && window->current_title() == QStringLiteral("Smoke")) {
      window->ShowDownloadsForTesting();
      *stage = 1;
    } else if (*stage == 1 && window->downloads_visible_for_testing()) {
      window->HideBrowserSurfacesForTesting();
      window->ShowBookmarksForTesting();
      *stage = 2;
    } else if (*stage == 2 && window->bookmarks_visible_for_testing()) {
      window->HideBrowserSurfacesForTesting();
      window->ShowHistoryForTesting();
      *stage = 3;
    } else if (*stage == 3 && window->history_visible_for_testing()) {
      window->HideBrowserSurfacesForTesting();
      window->SetWebFullscreenForTesting(true);
      window->ShowHistoryForTesting();
      *stage = 4;
    } else if (*stage == 4 && window->web_fullscreen_for_testing()) {
      window->SetWebFullscreenForTesting(false);
    } else if (*stage == 4 && !window->web_fullscreen_for_testing() &&
               window->history_visible_for_testing()) {
      window->HideBrowserSurfacesForTesting();
      window->ShowClearBrowsingDataForTesting();
      *stage = 5;
    } else if (*stage == 5 &&
               window->clear_data_prompt_visible_for_testing()) {
      const QStringList expected_options{
          QStringLiteral("Browsing history"),
          QStringLiteral("Recently closed tabs"),
          QStringLiteral("Download history"),
          QStringLiteral(
              "Site data (cache, cookies, sign-ins, and security decisions)")};
      const bool choices =
          window->clear_data_options_for_testing() == expected_options &&
          window->clear_data_submit_enabled_for_testing() &&
          window->SetAllClearDataOptionsForTesting(false) &&
          !window->clear_data_submit_enabled_for_testing();
      if (!choices) {
        *output << "BROWSER_SURFACES_SMOKE_FAILED choices=0" << Qt::endl;
        QCoreApplication::exit(16);
        return;
      }
      window->DismissClearBrowsingDataForTesting();
      *output << "BROWSER_SURFACES_SMOKE_OK downloads=1 bookmarks=1 "
                 "history=1 fullscreen=exit clear=selective"
              << Qt::endl;
      window->close();
      return;
    }
    if (*attempts > 180) {
      *output << "BROWSER_SURFACES_SMOKE_FAILED stage=" << *stage
              << " downloads=" << window->downloads_visible_for_testing()
              << " bookmarks=" << window->bookmarks_visible_for_testing()
              << " history=" << window->history_visible_for_testing()
              << " fullscreen=" << window->web_fullscreen_for_testing()
              << Qt::endl;
      QCoreApplication::exit(16);
      return;
    }
    QTimer::singleShot(50, window, [step] { (*step)(); });
  };
  QTimer::singleShot(300, window, [step] { (*step)(); });
}

void StartTabNavigationSmokeTest(MainWindow* window) {
  auto output = std::make_shared<QTextStream>(stdout);
  auto attempts = std::make_shared<int>(0);
  auto stage = std::make_shared<int>(0);
  auto step = std::make_shared<std::function<void()>>();
  *step = [window, output, attempts, stage, step] {
    ++*attempts;
    if (*stage == 0 && window->current_title() == QStringLiteral("Smoke")) {
      window->OpenTabForTesting(
          QStringLiteral("data:text/html,<title>Second</title>"));
      window->OpenTabForTesting(
          QStringLiteral("data:text/html,<title>Third</title>"));
      window->ActivateTabShortcutForTesting(1);
      *stage = 1;
    } else if (*stage == 1 && window->current_tab_index_for_testing() == 0 &&
               window->current_title() == QStringLiteral("Smoke")) {
      window->ActivateTabShortcutForTesting(9);
      *stage = 2;
    } else if (*stage == 2 && window->current_tab_index_for_testing() == 2 &&
               window->current_title() == QStringLiteral("Third")) {
      window->SetWebFullscreenForTesting(true);
      window->ShowAllTabsForTesting();
      *stage = 3;
    } else if (*stage == 3 && window->web_fullscreen_for_testing()) {
      window->SetWebFullscreenForTesting(false);
    } else if (*stage == 3 && !window->web_fullscreen_for_testing() &&
               window->all_tabs_visible_for_testing() &&
               window->all_tabs_action_count_for_testing() == 3) {
      *output << "TAB_NAVIGATION_SMOKE_OK direct=first last=third "
                 "list=3 fullscreen=exit"
              << Qt::endl;
      window->HideBrowserSurfacesForTesting();
      window->close();
      return;
    }
    if (*attempts > 180) {
      *output << "TAB_NAVIGATION_SMOKE_FAILED stage=" << *stage
              << " current=" << window->current_tab_index_for_testing()
              << " list=" << window->all_tabs_action_count_for_testing()
              << Qt::endl;
      QCoreApplication::exit(17);
      return;
    }
    QTimer::singleShot(50, window, [step] { (*step)(); });
  };
  QTimer::singleShot(300, window, [step] { (*step)(); });
}

void StartRecentlyClosedTabsSmokeTest(MainWindow* window) {
  auto output = std::make_shared<QTextStream>(stdout);
  auto attempts = std::make_shared<int>(0);
  auto stage = std::make_shared<int>(0);
  const QString second_url =
      QStringLiteral("data:text/html,<title>Second</title>");
  const QString third_url =
      QStringLiteral("data:text/html,<title>Third</title>");
  auto step = std::make_shared<std::function<void()>>();
  *step = [window, output, attempts, stage, step, second_url, third_url] {
    ++*attempts;
    if (*stage == 0 && window->current_title() == QStringLiteral("Smoke")) {
      window->OpenTabForTesting(second_url);
      window->OpenTabForTesting(third_url);
      *stage = 1;
    } else if (*stage == 1 &&
               window->current_title() == QStringLiteral("Third")) {
      window->CloseCurrentTabForTesting();
      *stage = 2;
    } else if (*stage == 2 && window->tab_count() == 2 &&
               window->recently_closed_tab_count_for_testing() == 1) {
      window->CloseCurrentTabForTesting();
      *stage = 3;
    } else if (*stage == 3 && window->tab_count() == 1 &&
               window->recently_closed_tab_count_for_testing() == 2) {
      window->ShowAllTabsForTesting();
      *stage = 4;
    } else if (*stage == 4 && window->all_tabs_visible_for_testing() &&
               window->all_tabs_action_count_for_testing() == 4) {
      window->HideBrowserSurfacesForTesting();
      const bool ordered =
          window->recently_closed_tabs_for_testing() ==
          QStringList{third_url, second_url};
      const bool titled =
          window->recently_closed_menu_labels_for_testing() ==
          QStringList{QStringLiteral("Second"), QStringLiteral("Third")};
      const bool triggered = window->TriggerRecentlyClosedForTesting(1);
      const bool remaining =
          window->recently_closed_tabs_for_testing() ==
          QStringList{second_url};
      const bool persisted =
          window->session_for_testing(true).recently_closed_tabs ==
          QList<RecentlyClosedTab>{
              {second_url, QStringLiteral("Second")}};
      if (ordered && titled && triggered && remaining && persisted &&
          window->tab_count() == 2 && window->current_url() == third_url) {
        *output << "RECENT_TABS_SMOKE_OK menu=2 titles=1 restored=older "
                   "persisted=1"
                << Qt::endl;
        window->close();
        return;
      }
      *output << "RECENT_TABS_SMOKE_FAILED ordered=" << ordered
              << " titled=" << titled << " triggered=" << triggered
              << " remaining=" << remaining
              << " persisted=" << persisted
              << " current=" << window->current_url() << Qt::endl;
      QCoreApplication::exit(18);
      return;
    }
    if (*attempts > 180) {
      *output << "RECENT_TABS_SMOKE_FAILED stage=" << *stage
              << " tabs=" << window->tab_count()
              << " recent=" << window->recently_closed_tab_count_for_testing()
              << Qt::endl;
      QCoreApplication::exit(18);
      return;
    }
    QTimer::singleShot(50, window, [step] { (*step)(); });
  };
  QTimer::singleShot(300, window, [step] { (*step)(); });
}

void StartApplicationMenuSmokeTest(MainWindow* window) {
  auto output = std::make_shared<QTextStream>(stdout);
  auto attempts = std::make_shared<int>(0);
  auto stage = std::make_shared<int>(0);
  auto step = std::make_shared<std::function<void()>>();
  *step = [window, output, attempts, stage, step] {
    ++*attempts;
    if (*stage == 0 && window->current_title() == QStringLiteral("Smoke")) {
      const QStringList menus = window->application_menu_titles_for_testing();
      const QStringList file =
          window->application_menu_actions_for_testing(QStringLiteral("File"));
      const QStringList edit =
          window->application_menu_actions_for_testing(QStringLiteral("Edit"));
      const QStringList view =
          window->application_menu_actions_for_testing(QStringLiteral("View"));
      const QStringList history = window->application_menu_actions_for_testing(
          QStringLiteral("History"));
      const QStringList bookmarks =
          window->application_menu_actions_for_testing(
              QStringLiteral("Bookmarks"));
      const QStringList window_menu =
          window->application_menu_actions_for_testing(QStringLiteral("Window"));
      const bool structure_ok =
          menus == QStringList{QStringLiteral("File"), QStringLiteral("Edit"),
                               QStringLiteral("View"), QStringLiteral("History"),
                               QStringLiteral("Bookmarks"),
                               QStringLiteral("Settings"),
                               QStringLiteral("Window")} &&
          file.contains(QStringLiteral("New Tab")) &&
          file.contains(QStringLiteral("Print…")) &&
          edit.contains(QStringLiteral("Select All")) &&
          view.contains(QStringLiteral("Developer Tools")) &&
          history.contains(QStringLiteral("Clear Browsing Data…")) &&
          bookmarks.contains(QStringLiteral("Show Bookmarks")) &&
          bookmarks.contains(QStringLiteral("Import Bookmarks…")) &&
          bookmarks.contains(QStringLiteral("Export Bookmarks…")) &&
          window_menu.contains(QStringLiteral("All Tabs"));
      const bool shortcuts_ok =
          !window->application_menu_shortcut_for_testing(
                     QStringLiteral("File"), QStringLiteral("New Tab"))
               .isEmpty() &&
          !window->application_menu_shortcut_for_testing(
                     QStringLiteral("Edit"), QStringLiteral("Find in Page…"))
               .isEmpty() &&
          !window->application_menu_shortcut_for_testing(
                     QStringLiteral("Window"), QStringLiteral("All Tabs"))
               .isEmpty();
      if (!structure_ok || !shortcuts_ok ||
          !window->TriggerApplicationMenuActionForTesting(
              QStringLiteral("File"), QStringLiteral("New Tab"))) {
        *output << "APPLICATION_MENU_SMOKE_FAILED structure=" << structure_ok
                << " shortcuts=" << shortcuts_ok << Qt::endl;
        QCoreApplication::exit(19);
        return;
      }
      *stage = 1;
    } else if (*stage == 1 && window->tab_count() == 2) {
      if (!window->TriggerApplicationMenuActionForTesting(
              QStringLiteral("File"), QStringLiteral("Close Tab"))) {
        *output << "APPLICATION_MENU_SMOKE_FAILED close=0" << Qt::endl;
        QCoreApplication::exit(19);
        return;
      }
      *stage = 2;
    } else if (*stage == 2 && window->tab_count() == 1 &&
               window->current_title() == QStringLiteral("Smoke")) {
      *output << "APPLICATION_MENU_SMOKE_OK menus=7 shortcuts=visible "
                 "actions=triggered"
              << Qt::endl;
      window->close();
      return;
    }
    if (*attempts > 180) {
      *output << "APPLICATION_MENU_SMOKE_FAILED stage=" << *stage
              << " tabs=" << window->tab_count() << Qt::endl;
      QCoreApplication::exit(19);
      return;
    }
    QTimer::singleShot(50, window, [step] { (*step)(); });
  };
  QTimer::singleShot(300, window, [step] { (*step)(); });
}

void StartSearchSettingsSmokeTest(MainWindow* window,
                                  const QString& settings_path) {
  auto output = std::make_shared<QTextStream>(stdout);
  const bool default_ok =
      window->search_engine_for_testing() == QStringLiteral("Google") &&
      window->NormalizeUrlForTesting(QStringLiteral("trail browser")) ==
          QStringLiteral("https://www.google.com/search?q=trail%20browser");
  const bool selected =
      window->SelectSearchEngineForTesting(QStringLiteral("DuckDuckGo"));
  BrowserSettings restored(settings_path);
  const bool loaded = restored.Load();
  const bool persisted =
      loaded && restored.search_engine() ==
                    BrowserSettings::SearchEngine::DuckDuckGo;
  const bool encoded =
      window->NormalizeUrlForTesting(QString::fromUtf8("隐私 搜索")) ==
      QStringLiteral("https://duckduckgo.com/?q=%E9%9A%90%E7%A7%81%20%E6%90%9C%E7%B4%A2");
  const bool address_ok =
      window->NormalizeUrlForTesting(QStringLiteral("example.com")) ==
          QStringLiteral("http://example.com") &&
      window->NormalizeUrlForTesting(QStringLiteral("localhost:8080")) ==
          QStringLiteral("http://localhost:8080") &&
      window->NormalizeUrlForTesting(QStringLiteral("intranet")) ==
          QStringLiteral("https://duckduckgo.com/?q=intranet");
  const auto normalized_https =
      window->NormalizeUrlForTesting(QStringLiteral("HTTPS://example.com/a"));
  const auto normalized_file = window->NormalizeUrlForTesting(
      QStringLiteral("file:///tmp/trail%20browser.html"));
  const auto normalized_blank =
      window->NormalizeUrlForTesting(QStringLiteral("ABOUT:BLANK"));
  const bool safe_schemes =
      normalized_https == QStringLiteral("https://example.com/a") &&
      normalized_file == QStringLiteral("file:///tmp/trail%20browser.html") &&
      normalized_blank == QStringLiteral("about:blank");
  const bool unsafe_navigation_rejected =
      !window->NormalizeUrlForTesting(
          QStringLiteral("javascript:alert(1)")) &&
      !window->NormalizeUrlForTesting(QStringLiteral("javascript:123")) &&
      !window->NormalizeUrlForTesting(
          QStringLiteral("data:text/html,unsafe")) &&
      !window->NormalizeUrlForTesting(
          QStringLiteral("unknown-scheme:payload")) &&
      !window->NormalizeUrlForTesting(QStringLiteral("unknown-scheme:123")) &&
      !window->NormalizeUrlForTesting(QStringLiteral("about:settings"));
  const bool menu_ok = window->application_menu_actions_for_testing(
                                  QStringLiteral("Settings")) ==
                              QStringList{
                                  QStringLiteral("Default Search Engine"),
                                  QStringLiteral("Use Current Page as Home"),
                                  QStringLiteral("Reset Home Page"),
                                  QStringLiteral("Open Home Page in New Tabs"),
                                  QStringLiteral("On Startup")};
  const QString home_url = QStringLiteral("https://example.test/home");
  const bool home_saved = window->SetHomePageForTesting(home_url);
  BrowserSettings home_restored(settings_path);
  const bool home_loaded = home_restored.Load();
  const bool home_persisted =
      home_loaded && home_restored.home_page() == home_url;
  const bool unsafe_rejected =
      !window->SetHomePageForTesting(QStringLiteral("javascript:alert(1)")) &&
      window->home_page_for_testing() == home_url;
  const bool new_tab_setting = window->SetOpenHomeOnNewTabForTesting(true);
  BrowserSettings new_tab_restored(settings_path);
  const bool new_tab_loaded = new_tab_restored.Load();
  const bool new_tab_persisted =
      new_tab_loaded && new_tab_restored.open_home_on_new_tab() &&
      window->open_home_on_new_tab_for_testing();
  const bool startup_selected = window->SetStartupBehaviorForTesting(
      QStringLiteral("Open Home Page"));
  BrowserSettings startup_restored(settings_path);
  const bool startup_loaded = startup_restored.Load();
  BrowserSession previous_session;
  previous_session.tab_urls = {QStringLiteral("https://example.test/previous")};
  const bool startup_persisted =
      startup_loaded &&
      startup_restored.startup_behavior() ==
          BrowserSettings::StartupBehavior::HomePage &&
      window->startup_behavior_for_testing() ==
          QStringLiteral("Open Home Page");
  const bool home_startup =
      SelectInitialSession(startup_restored, std::nullopt, previous_session)
          .tab_urls == QStringList{home_url};
  startup_restored.set_startup_behavior(
      BrowserSettings::StartupBehavior::RestoreSession);
  const bool restore_startup =
      SelectInitialSession(startup_restored, std::nullopt, previous_session)
          .tab_urls == previous_session.tab_urls;
  startup_restored.set_startup_behavior(
      BrowserSettings::StartupBehavior::BlankPage);
  const bool blank_startup =
      SelectInitialSession(startup_restored, std::nullopt, previous_session)
          .tab_urls == QStringList{QStringLiteral("about:blank")};
  const bool explicit_startup =
      SelectInitialSession(startup_restored,
                           QStringLiteral("https://example.test/explicit"),
                           previous_session)
              .tab_urls ==
          QStringList{QStringLiteral("https://example.test/explicit")};
  const bool startup_normalization =
      SelectInitialSession(startup_restored, QStringLiteral("example.test/path"),
                           previous_session)
              .tab_urls ==
          QStringList{QStringLiteral("http://example.test/path")} &&
      SelectInitialSession(startup_restored, QStringLiteral("privacy query"),
                           previous_session)
              .tab_urls ==
          QStringList{QStringLiteral(
              "https://duckduckgo.com/?q=privacy%20query")} &&
      SelectInitialSession(startup_restored,
                           QStringLiteral("javascript:alert(1)"),
                           previous_session)
              .tab_urls == QStringList{home_url};
  const bool startup_decision = home_startup && restore_startup &&
                                blank_startup && explicit_startup &&
                                startup_normalization;
  const bool preliminary_ok = default_ok && selected && persisted && encoded &&
                              address_ok && safe_schemes &&
                              unsafe_navigation_rejected && menu_ok &&
                              home_saved && home_persisted && unsafe_rejected &&
                              new_tab_setting && new_tab_persisted &&
                              startup_selected && startup_persisted &&
                              startup_decision;
  if (!preliminary_ok) {
    *output << "SEARCH_SETTINGS_SMOKE_FAILED default=" << default_ok
            << " selected=" << selected << " persisted=" << persisted
            << " encoded=" << encoded << " address=" << address_ok
            << " safe_schemes=" << safe_schemes
            << " https=" << normalized_https.value_or(QStringLiteral("null"))
            << " file=" << normalized_file.value_or(QStringLiteral("null"))
            << " blank=" << normalized_blank.value_or(QStringLiteral("null"))
            << " unsafe_navigation=" << unsafe_navigation_rejected
            << " menu=" << menu_ok << " home=" << home_persisted
            << " unsafe=" << unsafe_rejected
            << " new_tab=" << new_tab_persisted
            << " startup=" << startup_persisted
            << " decision=" << startup_decision << Qt::endl;
    QCoreApplication::exit(20);
    return;
  }

  const bool new_tab_triggered =
      window->TriggerApplicationMenuActionForTesting(
          QStringLiteral("File"), QStringLiteral("New Tab"));
  if (!new_tab_triggered) {
    *output << "SEARCH_SETTINGS_SMOKE_FAILED new_tab_trigger=0" << Qt::endl;
    QCoreApplication::exit(20);
    return;
  }
  auto attempts = std::make_shared<int>(0);
  auto step = std::make_shared<std::function<void()>>();
  *step = [window, output, attempts, step, home_url] {
    ++*attempts;
    if (window->tab_count() == 2 && window->current_url() == home_url) {
      *output << "SEARCH_SETTINGS_SMOKE_OK default=google selected=duckduckgo "
                 "persisted=1 encoded=1 home=new-tab startup=home"
              << Qt::endl;
      window->close();
      return;
    }
    if (*attempts > 100) {
      *output << "SEARCH_SETTINGS_SMOKE_FAILED home_navigation=0 url="
              << window->current_url() << Qt::endl;
      QCoreApplication::exit(20);
      return;
    }
    QTimer::singleShot(50, window, [step] { (*step)(); });
  };
  QTimer::singleShot(50, window, [step] { (*step)(); });
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
  const auto normalized_mail = window->normalize_external_url_for_testing(
      QStringLiteral("  MAILTO:test@example.com?subject=Trail%20Browser  "));
  const auto normalized_magnet = window->normalize_external_url_for_testing(
      QStringLiteral("magnet:?xt=urn:btih:0123456789abcdef"));
  const bool schemes_ok =
      normalized_mail && normalized_magnet &&
      *normalized_mail ==
          QStringLiteral("mailto:test@example.com?subject=Trail%20Browser") &&
      *normalized_magnet ==
          QStringLiteral("magnet:?xt=urn:btih:0123456789abcdef") &&
      window->external_scheme_allowed_for_testing(
          QStringLiteral("tel:+123456789")) &&
      window->external_scheme_allowed_for_testing(
          QStringLiteral("webcal://calendar.example.test/events")) &&
      !window->external_scheme_allowed_for_testing(
          QStringLiteral("javascript:alert(1)")) &&
      !window->external_scheme_allowed_for_testing(
          QStringLiteral("unknown-scheme:payload")) &&
      !window->external_scheme_allowed_for_testing(QStringLiteral("mailto:")) &&
      !window->external_scheme_allowed_for_testing(QStringLiteral("magnet:")) &&
      !window->external_scheme_allowed_for_testing(
          QStringLiteral("webcal:/missing-host")) &&
      !window->external_scheme_allowed_for_testing(
          QStringLiteral("webcal://user:password@calendar.example.test")) &&
      !window->external_scheme_allowed_for_testing(
          QStringLiteral("mailto:test@example.com\r\nX-Test: injected"));
  const int tabs_before_popups = window->tab_count();
  const bool unsafe_popups_blocked =
      !window->OpenPopupForTesting(QStringLiteral("javascript:alert(1)"),
                                   CEF_WOD_NEW_FOREGROUND_TAB) &&
      !window->OpenPopupForTesting(QStringLiteral("data:text/html,unsafe"),
                                   CEF_WOD_NEW_BACKGROUND_TAB) &&
      !window->OpenPopupForTesting(QStringLiteral("file:///tmp/private"),
                                   CEF_WOD_CURRENT_TAB) &&
      !window->OpenPopupForTesting(
          QStringLiteral("https://example.test/download"),
          CEF_WOD_SAVE_TO_DISK) &&
      window->tab_count() == tabs_before_popups;
  const bool safe_popup_opened = window->OpenPopupForTesting(
      QStringLiteral(" HTTPS://example.test/popup "),
      CEF_WOD_NEW_BACKGROUND_TAB);
  const bool popup_ok = safe_popup_opened &&
                        window->tab_count() == tabs_before_popups + 1;
  if (media_ok && permissions_ok && schemes_ok && unsafe_popups_blocked &&
      popup_ok) {
    *output << "SECURITY_SMOKE_OK media=2 permissions=2 "
               "schemes=normalized popups=guarded"
            << Qt::endl;
    window->close();
  } else {
    *output << "SECURITY_SMOKE_FAILED media=" << media_ok
            << " permissions=" << permissions_ok
            << " schemes=" << schemes_ok
            << " unsafe_popups=" << unsafe_popups_blocked
            << " popup=" << popup_ok << Qt::endl;
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

void StartSingleInstanceSmokeTest(MainWindow* window,
                                  const QString& data_path) {
  auto output = std::make_shared<QTextStream>(stdout);
  const QString forwarded_url =
      QStringLiteral("http://example.test/forwarded");
  auto forward = [data_path](const QString& value, QString* error) {
    SingleInstance client(data_path);
    return client.Start(value, error) ==
           SingleInstance::StartResult::Forwarded;
  };
  QString error;
  const bool forwarded = forward(QStringLiteral("example.test/forwarded"),
                                 &error);
  if (!forwarded) {
    *output << "SINGLE_INSTANCE_SMOKE_FAILED forwarded=0 error=" << error
            << Qt::endl;
    QCoreApplication::exit(21);
    return;
  }

  auto attempts = std::make_shared<int>(0);
  auto stage = std::make_shared<int>(0);
  auto step = std::make_shared<std::function<void()>>();
  *step = [window, output, attempts, stage, step, forward, forwarded_url] {
    ++*attempts;
    if (*stage == 0 && window->tab_count() == 2 &&
        window->current_url() == forwarded_url) {
      QString error;
      const bool rejected_sent =
          forward(QStringLiteral("javascript:alert(1)"), &error);
      const bool activation_sent = forward(QString(), &error);
      const bool search_sent = forward(QStringLiteral("trail browser"), &error);
      if (!rejected_sent || !activation_sent || !search_sent) {
        *output << "SINGLE_INSTANCE_SMOKE_FAILED followup=0 error=" << error
                << Qt::endl;
        QCoreApplication::exit(21);
        return;
      }
      *stage = 1;
    } else if (*stage == 1 && window->tab_count() == 3 &&
               window->current_url() ==
                   QStringLiteral(
                       "https://www.google.com/search?q=trail%20browser")) {
      *output << "SINGLE_INSTANCE_SMOKE_OK forwarded=domain,search "
                 "activation=window unsafe=blocked tabs=3"
              << Qt::endl;
      window->close();
      return;
    }
    if (*attempts > 120) {
      *output << "SINGLE_INSTANCE_SMOKE_FAILED tabs=" << window->tab_count()
              << " url=" << window->current_url() << Qt::endl;
      QCoreApplication::exit(21);
      return;
    }
    QTimer::singleShot(50, window, [step] { (*step)(); });
  };
  QTimer::singleShot(50, window, [step] { (*step)(); });
}

#if defined(OS_LINUX)
struct LinuxProcessStatus {
  qint64 parent_pid = 0;
  int no_new_privileges = -1;
  int seccomp_mode = -1;
  QByteArray command_line;
};

QHash<qint64, LinuxProcessStatus> ReadLinuxProcessStatuses() {
  QHash<qint64, LinuxProcessStatus> processes;
  const QStringList entries =
      QDir(QStringLiteral("/proc"))
          .entryList(QDir::Dirs | QDir::NoDotAndDotDot);
  for (const QString& entry : entries) {
    bool valid_pid = false;
    const qint64 pid = entry.toLongLong(&valid_pid);
    if (!valid_pid) continue;

    QFile status_file(QStringLiteral("/proc/%1/status").arg(pid));
    QFile command_file(QStringLiteral("/proc/%1/cmdline").arg(pid));
    if (!status_file.open(QIODevice::ReadOnly) ||
        !command_file.open(QIODevice::ReadOnly)) {
      continue;
    }

    LinuxProcessStatus status;
    status.command_line = command_file.readAll();
    status.command_line.replace('\0', ' ');
    for (const QByteArray& line : status_file.readAll().split('\n')) {
      if (line.startsWith("PPid:")) {
        status.parent_pid = line.mid(5).trimmed().toLongLong();
      } else if (line.startsWith("NoNewPrivs:")) {
        status.no_new_privileges = line.mid(11).trimmed().toInt();
      } else if (line.startsWith("Seccomp:")) {
        status.seccomp_mode = line.mid(8).trimmed().toInt();
      }
    }
    processes.insert(pid, std::move(status));
  }
  return processes;
}

bool IsDescendantProcess(
    qint64 pid, qint64 ancestor,
    const QHash<qint64, LinuxProcessStatus>& processes) {
  QSet<qint64> visited;
  while (pid > 1 && !visited.contains(pid)) {
    visited.insert(pid);
    const auto process = processes.constFind(pid);
    if (process == processes.cend()) return false;
    if (process->parent_pid == ancestor) return true;
    pid = process->parent_pid;
  }
  return false;
}

void StartLinuxSandboxSmokeTest(MainWindow* window) {
  auto output = std::make_shared<QTextStream>(stdout);
  auto attempts = std::make_shared<int>(0);
  auto step = std::make_shared<std::function<void()>>();
  *step = [window, output, attempts, step] {
    ++*attempts;
    const qint64 browser_pid = QCoreApplication::applicationPid();
    const auto processes = ReadLinuxProcessStatuses();
    bool renderer_seen = false;
    bool renderer_sandboxed = false;
    bool disable_switch_seen = false;
    for (auto process = processes.cbegin(); process != processes.cend();
         ++process) {
      if (!IsDescendantProcess(process.key(), browser_pid, processes)) {
        continue;
      }
      if (process->command_line.contains("--no-sandbox")) {
        disable_switch_seen = true;
      }
      if (process->command_line.contains("--type=renderer")) {
        renderer_seen = true;
        if (process->no_new_privileges == 1 && process->seccomp_mode == 2) {
          renderer_sandboxed = true;
        }
      }
    }

    if (renderer_sandboxed && !disable_switch_seen) {
      *output << "LINUX_SANDBOX_SMOKE_OK renderer=seccomp "
                 "no_new_privileges=1"
              << Qt::endl;
      window->close();
      return;
    }
    if (*attempts > 120) {
      *output << "LINUX_SANDBOX_SMOKE_FAILED renderer=" << renderer_seen
              << " sandboxed=" << renderer_sandboxed
              << " no_sandbox_switch=" << disable_switch_seen << Qt::endl;
      QCoreApplication::exit(22);
      return;
    }
    QTimer::singleShot(50, window, [step] { (*step)(); });
  };
  QTimer::singleShot(300, window, [step] { (*step)(); });
}
#endif

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
#if defined(OS_LINUX)
  settings.no_sandbox = false;
#else
  // Windows requires CEF's bootstrap packaging flow and macOS requires signed
  // helper entitlements before the Chromium sandbox can be enabled.
  settings.no_sandbox = true;
#endif
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
  SingleInstance single_instance(data_path);
  QString single_instance_error;
  const SingleInstance::StartResult instance_result = single_instance.Start(
      ExplicitStartupUrl().value_or(QString()), &single_instance_error);
  if (instance_result == SingleInstance::StartResult::Forwarded) return 0;
  if (instance_result == SingleInstance::StartResult::Error) {
    qWarning("Unable to contact or start the primary browser instance: %s",
             qPrintable(single_instance_error));
    return 21;
  }
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
    const QString settings_path =
        QDir(data_path).filePath(QStringLiteral("settings.json"));
    const QString download_history_path =
        QDir(data_path).filePath(QStringLiteral("downloads.json"));
    std::unique_ptr<QTemporaryDir> smoke_session_directory;
    std::unique_ptr<QTemporaryDir> smoke_profile_directory;
    QString active_session_path = session_path;
    QString active_settings_path = settings_path;
    BrowserSettings startup_settings(settings_path);
    QString startup_settings_error;
    if (!startup_settings.Load(&startup_settings_error)) {
      qWarning("Unable to load browser settings: %s",
               qPrintable(startup_settings_error));
    }
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
          QStringLiteral("https://example.test/session-one"),
          QStringLiteral("file:///tmp/session%20two.html")};
      initial_session.active_tab = 1;
      initial_session.clean_exit = false;
      initial_session.recently_closed_tabs = {
          {QStringLiteral("https://example.test/closed"),
           QStringLiteral("Closed page")}};
    } else if (HasArgument(QStringLiteral("--smoke-test-pinned-tabs"))) {
      smoke_session_directory = std::make_unique<QTemporaryDir>();
      if (!smoke_session_directory->isValid()) {
        CefShutdown();
        return 5;
      }
      active_session_path =
          smoke_session_directory->filePath(QStringLiteral("session.json"));
      initial_session = DefaultSession(
          QStringLiteral("data:text/html,<title>Smoke</title>"));
    } else if (IsSmokeTest()) {
      initial_session = DefaultSession(ExplicitStartupUrl().value_or(
          QStringLiteral("data:text/html,<title>Smoke</title>")));
      active_session_path.clear();
      active_settings_path =
          QDir(data_path).filePath(QStringLiteral("settings.json"));
    } else if (const auto startup_url = ExplicitStartupUrl()) {
      // A URL explicitly supplied by the caller always wins over restoration.
      initial_session =
          SelectInitialSession(startup_settings, startup_url, std::nullopt);
    } else {
      QString session_error;
      const auto restored = SessionStore::Load(session_path, &session_error);
      initial_session =
          SelectInitialSession(startup_settings, std::nullopt, restored);
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
      if (HasArgument(QStringLiteral("--smoke-test-privacy"))) {
        active_session_path = smoke_profile_directory->filePath(
            QStringLiteral("session.json"));
      }
    }
    MainWindow main_window(initial_session, active_session_path,
                           active_browsing_data_path, active_settings_path,
                           download_history_path);
    single_instance.SetActivationHandler(
        [&main_window](const QString& url) {
          main_window.HandleExternalOpenRequest(url);
        });
    main_window.show();
    message_pump.Schedule(0);
    if (HasArgument(QStringLiteral("--smoke-test-tabs"))) {
      StartTabSmokeTest(&main_window);
    } else if (HasArgument(QStringLiteral("--smoke-test-tab-actions"))) {
      StartTabActionsSmokeTest(&main_window);
    } else if (HasArgument(QStringLiteral("--smoke-test-pinned-tabs"))) {
      StartPinnedTabsSmokeTest(&main_window, active_session_path);
    } else if (HasArgument(QStringLiteral("--smoke-test-downloads"))) {
      QTimer::singleShot(
          300, &main_window, [&main_window, download_history_path] {
            StartDownloadSmokeTest(&main_window, download_history_path);
          });
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
      QTimer::singleShot(
          300, &main_window,
          [&main_window, active_session_path, download_history_path] {
            StartPrivacySmokeTest(&main_window, active_session_path,
                                  download_history_path);
          });
    } else if (HasArgument(QStringLiteral("--smoke-test-favicon"))) {
      QTimer::singleShot(300, &main_window,
                         [&main_window] { StartFaviconSmokeTest(&main_window); });
    } else if (HasArgument(QStringLiteral("--smoke-test-audio"))) {
      StartAudioSmokeTest(&main_window);
    } else if (HasArgument(
                   QStringLiteral("--smoke-test-browser-surfaces"))) {
      StartBrowserSurfacesSmokeTest(&main_window);
    } else if (HasArgument(
                   QStringLiteral("--smoke-test-tab-navigation"))) {
      StartTabNavigationSmokeTest(&main_window);
    } else if (HasArgument(QStringLiteral("--smoke-test-recent-tabs"))) {
      StartRecentlyClosedTabsSmokeTest(&main_window);
    } else if (HasArgument(QStringLiteral("--smoke-test-application-menu"))) {
      StartApplicationMenuSmokeTest(&main_window);
    } else if (HasArgument(QStringLiteral("--smoke-test-search-settings"))) {
      QTimer::singleShot(300, &main_window,
                         [&main_window, active_settings_path] {
                           StartSearchSettingsSmokeTest(&main_window,
                                                        active_settings_path);
                         });
    } else if (HasArgument(QStringLiteral("--smoke-test-security"))) {
      QTimer::singleShot(300, &main_window,
                         [&main_window] { StartSecuritySmokeTest(&main_window); });
    } else if (HasArgument(QStringLiteral("--smoke-test-auth"))) {
      StartAuthSmokeTest(&main_window);
    } else if (HasArgument(
                   QStringLiteral("--smoke-test-single-instance"))) {
      QTimer::singleShot(300, &main_window, [&main_window, data_path] {
        StartSingleInstanceSmokeTest(&main_window, data_path);
      });
#if defined(OS_LINUX)
    } else if (HasArgument(QStringLiteral("--smoke-test-sandbox"))) {
      StartLinuxSandboxSmokeTest(&main_window);
#endif
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

