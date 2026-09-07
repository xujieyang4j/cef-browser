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
class BrowserSettings;
class BrowsingDataStore;
class DownloadManager;
class DownloadPanel;
class QAction;
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
             QString browsing_data_path = {}, QString settings_path = {},
             QString download_history_path = {},
             QWidget* parent = nullptr);
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
  void ToggleCurrentTabPinnedForTesting();
  bool current_tab_pinned_for_testing() const;
  int pinned_tab_count_for_testing() const;
  void MoveCurrentTabForTesting(int to);
  int current_tab_index_for_testing() const;
  void UpdateDownloadForTesting(quint32 id, int percent, bool complete);
  void PauseDownloadForTesting(quint32 id);
  int download_count_for_testing() const;
  int active_download_count_for_testing() const;
  QString download_status_for_testing(quint32 id) const;
  bool RemoveDownloadForTesting(quint32 id);
  bool ClearFinishedDownloadsForTesting();
  void SetCurrentFaviconForTesting();
  bool current_tab_has_favicon_for_testing() const;
  void SetCurrentAudioStateForTesting(bool playing, bool muted);
  void ToggleCurrentAudioMutedForTesting();
  bool current_audio_muted_for_testing() const;
  QString current_tab_text_for_testing() const;
  void ShowDownloadsForTesting();
  void ShowBookmarksForTesting();
  void ShowHistoryForTesting();
  void ShowClearBrowsingDataForTesting();
  void HideBrowserSurfacesForTesting();
  bool downloads_visible_for_testing() const;
  bool bookmarks_visible_for_testing() const;
  bool history_visible_for_testing() const;
  bool clear_data_prompt_visible_for_testing() const;
  QStringList clear_data_options_for_testing() const;
  bool SetAllClearDataOptionsForTesting(bool checked);
  bool clear_data_submit_enabled_for_testing() const;
  void ShowAllTabsForTesting();
  bool all_tabs_visible_for_testing() const;
  int all_tabs_action_count_for_testing() const;
  int recently_closed_tab_count_for_testing() const;
  QStringList recently_closed_tabs_for_testing() const;
  QStringList recently_closed_menu_labels_for_testing();
  bool TriggerRecentlyClosedForTesting(int recent_index);
  QStringList application_menu_titles_for_testing() const;
  QStringList application_menu_actions_for_testing(
      const QString& menu_title) const;
  QString application_menu_shortcut_for_testing(
      const QString& menu_title, const QString& action_text) const;
  bool TriggerApplicationMenuActionForTesting(const QString& menu_title,
                                              const QString& action_text);
  QString search_engine_for_testing() const;
  bool SelectSearchEngineForTesting(const QString& name);
  QString home_page_for_testing() const;
  bool SetHomePageForTesting(const QString& value);
  void GoHomeForTesting();
  bool SetOpenHomeOnNewTabForTesting(bool enabled);
  bool open_home_on_new_tab_for_testing() const;
  bool SetStartupBehaviorForTesting(const QString& name);
  QString startup_behavior_for_testing() const;
  QString NormalizeUrlForTesting(const QString& input) const;
  void ActivateTabShortcutForTesting(int number);
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
  QStringList address_suggestion_labels_for_testing() const;
  void NavigateAddressSuggestionForTesting(const QString& label);
  void AddHistoryForTesting(const QString& url, const QString& title);
  bool RemoveBookmarkForTesting(const QString& url);
  bool RemoveHistoryForTesting(const QString& url);
  void ClearBrowsingDataForTesting();
  void ClearBrowsingDataForTesting(bool history, bool recently_closed,
                                   bool downloads, bool site_data);
  void DismissClearBrowsingDataForTesting();
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
  void NavigateFromAddressSuggestion(const QString& label);
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
  void ShowBookmarkContextMenu(const QPoint& position);
  void ShowHistoryContextMenu(const QPoint& position);
  void RebuildAllTabsMenu();
  void ShowClearBrowsingDataPrompt();
  void CompleteBrowsingDataClearTask(const QString& task, bool success);
  void RecordVisit(BrowserView* browser);
  bool SaveBrowsingData();
  void RefreshAddressSuggestions();

 private:
  struct BrowsingDataSelection {
    bool history = false;
    bool recently_closed = false;
    bool downloads = false;
    bool site_data = false;

    bool Any() const {
      return history || recently_closed || downloads || site_data;
    }
    int TaskCount() const {
      return static_cast<int>(history) + static_cast<int>(recently_closed) +
             static_cast<int>(downloads) + (site_data ? 4 : 0);
    }
  };

  enum class BrowserUiSurface {
    Downloads,
    Bookmarks,
    History,
    AllTabs,
    ClearData
  };

  QString NormalizeUrl(QString input) const;
  BrowserView* AddTab(const QString& url, bool activate,
                      bool focus_address = false);
  BrowserView* CurrentBrowser() const;
  int IndexOf(const BrowserView* browser) const;
  void OpenPopup(BrowserView* source, const QString& url, int disposition);
  void ShowTabContextMenu(const QPoint& position);
  void DuplicateTab(int index);
  void SetTabPinned(int index, bool pinned);
  bool IsTabPinned(BrowserView* browser) const;
  int PinnedTabCount() const;
  void ConstrainMovedTab(int from, int to);
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
  void ActivateTabByShortcut(int index);
  void CreateApplicationMenus();
  QAction* FindApplicationMenuAction(const QString& menu_title,
                                     const QString& action_text) const;
  void ShowBrowserUiSurface(BrowserUiSurface surface);
  void PerformBrowserUiSurface(BrowserUiSurface surface);
  bool ReopenClosedTabAt(int recent_index);
  void ScheduleSessionSave();
  BrowserSession CaptureSession(bool clean_exit) const;
  bool PersistSession(const BrowserSession& session);
  void SetSearchEngine(int engine_value);
  bool SetHomePage(const QString& value);
  void GoHome();
  bool SetOpenHomeOnNewTab(bool enabled);
  bool SetStartupBehavior(int behavior_value);
  bool RemoveBookmark(const QString& url);
  bool RemoveHistory(const QString& url);
  void BeginClearBrowsingData(bool show_result_dialog,
                              const BrowsingDataSelection& selection);

  QTabBar* tab_bar_ = nullptr;
  QStackedWidget* tab_stack_ = nullptr;
  QLineEdit* address_bar_ = nullptr;
  QPushButton* back_button_ = nullptr;
  QPushButton* forward_button_ = nullptr;
  QPushButton* reload_button_ = nullptr;
  QPushButton* home_button_ = nullptr;
  QPushButton* downloads_button_ = nullptr;
  QProgressBar* loading_progress_ = nullptr;
  QAction* close_tab_action_ = nullptr;
  QAction* reopen_closed_tab_action_ = nullptr;
  QAction* back_action_ = nullptr;
  QAction* forward_action_ = nullptr;
  QAction* reload_action_ = nullptr;
  QAction* toggle_bookmark_action_ = nullptr;
  QCompleter* address_completer_ = nullptr;
  QStringListModel* address_suggestions_ = nullptr;
  QHash<QString, QString> address_suggestion_urls_;
  QStringList address_suggestion_url_order_;
  QWidget* find_bar_ = nullptr;
  QWidget* tab_strip_ = nullptr;
  QWidget* toolbar_ = nullptr;
  QLineEdit* find_edit_ = nullptr;
  QLabel* find_result_label_ = nullptr;
  QPushButton* bookmark_button_ = nullptr;
  QPushButton* bookmarks_button_ = nullptr;
  QPushButton* history_button_ = nullptr;
  QPushButton* all_tabs_button_ = nullptr;
  QMenu* bookmarks_menu_ = nullptr;
  QMenu* history_menu_ = nullptr;
  QMenu* all_tabs_menu_ = nullptr;
  DownloadManager* download_manager_ = nullptr;
  DownloadPanel* download_panel_ = nullptr;
  QTimer* session_save_timer_ = nullptr;
  BrowsingDataStore* browsing_data_ = nullptr;
  BrowserSettings* browser_settings_ = nullptr;
  QSet<BrowserView*> closing_tabs_;
  QSet<BrowserView*> forgotten_closing_tabs_;
  QSet<BrowserView*> pinned_tabs_;
  QList<QPointer<BrowserView>> queued_tab_closes_;
  QPointer<BrowserView> active_queued_tab_close_;
  QHash<BrowserView*, RecentlyClosedTab> pending_closed_tabs_;
  QList<RecentlyClosedTab> closed_tabs_;
  QString session_path_;
  std::optional<BrowserSession> closing_session_;
  bool session_persistence_ready_ = false;
  bool constraining_tab_move_ = false;
  bool download_exit_prompt_open_ = false;
  bool browsing_data_clear_in_progress_ = false;
  bool browsing_data_clear_show_result_ = false;
  int browsing_data_clear_pending_ = 0;
  QStringList browsing_data_clear_failures_;
  QStringList browsing_data_clear_completed_;
  QString browsing_data_clear_result_;
  bool window_close_requested_ = false;
  bool allow_window_close_ = false;
  bool web_fullscreen_ = false;
  std::optional<BrowserUiSurface> pending_browser_ui_surface_;
  bool window_was_maximized_ = false;
  bool find_bar_was_visible_ = false;

  Q_DISABLE_COPY_MOVE(MainWindow)
};
