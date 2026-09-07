#pragma once

#include <optional>

#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <QPoint>
#include <QSet>
#include <QStringList>

#include "include/cef_auth_callback.h"
#include "session/session_store.h"

class BrowserView;
class BrowsingDataStore;
class DownloadManager;
class DownloadPanel;
class QCloseEvent;
class QMoveEvent;
class QResizeEvent;
class QLineEdit;
class QLabel;
class QMenu;
class QPushButton;
class QProgressBar;
class QCompleter;
class QStringListModel;
class QStackedWidget;
class QTabBar;
class QTimer;

class MainWindow final : public QMainWindow {
  Q_OBJECT

 public:
  MainWindow(const BrowserSession& initial_session, QString session_path,
             QString browsing_data_path = {}, QWidget* parent = nullptr);
  ~MainWindow() override;
  int tab_count() const;
  QString current_url() const;
  QString current_title() const;
  void OpenTabForTesting(const QString& url);
  void CloseCurrentTabForTesting();
  void ReopenClosedTabForTesting();
  void ActivateTabForTesting(int index);
  void DuplicateCurrentTabForTesting();
  void CloseOtherTabsForTesting();
  void CloseTabsToRightForTesting();
  void UpdateDownloadForTesting(quint32 id, int percent, bool complete);
  void PauseDownloadForTesting(quint32 id);
  int download_count_for_testing() const;
  int active_download_count_for_testing() const;
  QString download_status_for_testing(quint32 id) const;
  void SetCurrentFaviconForTesting();
  bool current_tab_has_favicon_for_testing() const;
  void ShowFailureForTesting(bool render_process_failed);
  bool failure_page_active_for_testing() const;
  bool render_process_failed_for_testing() const;
  BrowserSession session_for_testing(bool clean_exit) const;
  bool save_session_for_testing(bool clean_exit);
  bool find_bar_visible_for_testing() const;
  void ShowFindBarForTesting();
  void HideFindBarForTesting();
  void FindForTesting(const QString& text);
  QString find_result_for_testing() const;
  void ZoomInForTesting();
  void ResetZoomForTesting();
  int zoom_percent_for_testing() const;
  void ToggleBookmarkForTesting();
  bool current_page_bookmarked_for_testing() const;
  int history_count_for_testing() const;
  int current_url_visit_count_for_testing() const;
  QStringList address_suggestions_for_testing() const;
  void AddHistoryForTesting(const QString& url, const QString& title);
  void ClearBrowsingDataForTesting();
  bool browsing_data_clear_in_progress_for_testing() const {
    return browsing_data_clear_in_progress_;
  }
  const QString& browsing_data_clear_result_for_testing() const {
    return browsing_data_clear_result_;
  }
  QString media_permission_description_for_testing(uint32_t permissions) const;
  QString permission_description_for_testing(uint32_t permissions) const;
  bool external_scheme_allowed_for_testing(const QString& url) const;
  bool ShowAuthForTesting(CefRefPtr<CefAuthCallback> callback);
  void SetWebFullscreenForTesting(bool fullscreen);
  bool web_fullscreen_for_testing() const { return web_fullscreen_; }
  bool window_close_requested_for_testing() const {
    return window_close_requested_;
  }

 protected:
  void closeEvent(QCloseEvent* event) override;
  void moveEvent(QMoveEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

 private slots:
  void NavigateFromAddressBar();
  void AddBlankTab();
  void ReopenClosedTab();
  void CloseTab(int index);
  void ActivateTab(int index);
  void UpdateLoadingState(bool loading, bool can_go_back,
                          bool can_go_forward);
  void UpdateAddress(const QString& url);
  void ShowFindBar();
  void HideFindBar();
  void FindFromBar(bool forward, bool find_next);
  void ToggleCurrentBookmark();
  void RebuildBookmarksMenu();
  void RebuildHistoryMenu();
  void ShowClearBrowsingDataPrompt();
  void BeginClearBrowsingData(bool show_result_dialog);
  void CompleteBrowsingDataClearTask(const QString& task, bool success);
  void RecordVisit(BrowserView* browser);
  bool SaveBrowsingData();
  void RefreshAddressSuggestions();

 private:
  static QString NormalizeUrl(QString input);
  BrowserView* AddTab(const QString& url, bool activate,
                      bool focus_address = false);
  BrowserView* CurrentBrowser() const;
  int IndexOf(const BrowserView* browser) const;
  void OpenPopup(BrowserView* source, const QString& url, int disposition);
  void ShowTabContextMenu(const QPoint& position);
  void DuplicateTab(int index);
  void CloseOtherTabs(int index);
  void CloseTabsToRight(int index);
  void QueueTabCloses(const QList<BrowserView*>& browsers);
  void ContinueQueuedTabCloses();
  void BeginTabClose(BrowserView* browser, bool remember_url);
  void CompleteTabClose(BrowserView* browser);
  void CancelTabClose(BrowserView* browser);
  void ContinueWindowClose();
  void UpdateChrome();
  void UpdateTabTitle(BrowserView* browser, const QString& title);
  void HandleBrowserShortcut(int action);
  void ScheduleSessionSave();
  BrowserSession CaptureSession(bool clean_exit) const;
  bool PersistSession(const BrowserSession& session);

  QTabBar* tab_bar_ = nullptr;
  QStackedWidget* tab_stack_ = nullptr;
  QLineEdit* address_bar_ = nullptr;
  QPushButton* back_button_ = nullptr;
  QPushButton* forward_button_ = nullptr;
  QPushButton* reload_button_ = nullptr;
  QProgressBar* loading_progress_ = nullptr;
  QCompleter* address_completer_ = nullptr;
  QStringListModel* address_suggestions_ = nullptr;
  QWidget* find_bar_ = nullptr;
  QWidget* tab_strip_ = nullptr;
  QWidget* toolbar_ = nullptr;
  QLineEdit* find_edit_ = nullptr;
  QLabel* find_result_label_ = nullptr;
  QPushButton* bookmark_button_ = nullptr;
  QPushButton* bookmarks_button_ = nullptr;
  QPushButton* history_button_ = nullptr;
  QMenu* bookmarks_menu_ = nullptr;
  QMenu* history_menu_ = nullptr;
  DownloadManager* download_manager_ = nullptr;
  DownloadPanel* download_panel_ = nullptr;
  QTimer* session_save_timer_ = nullptr;
  BrowsingDataStore* browsing_data_ = nullptr;
  QSet<BrowserView*> closing_tabs_;
  QList<QPointer<BrowserView>> queued_tab_closes_;
  QPointer<BrowserView> active_queued_tab_close_;
  QHash<BrowserView*, QString> pending_closed_urls_;
  QStringList closed_tabs_;
  QString session_path_;
  std::optional<BrowserSession> closing_session_;
  bool session_persistence_ready_ = false;
  bool download_exit_prompt_open_ = false;
  bool browsing_data_clear_in_progress_ = false;
  bool browsing_data_clear_show_result_ = false;
  int browsing_data_clear_pending_ = 0;
  QStringList browsing_data_clear_failures_;
  QString browsing_data_clear_result_;
  bool window_close_requested_ = false;
  bool allow_window_close_ = false;
  bool web_fullscreen_ = false;
  bool window_was_maximized_ = false;
  bool find_bar_was_visible_ = false;

  Q_DISABLE_COPY_MOVE(MainWindow)
};
