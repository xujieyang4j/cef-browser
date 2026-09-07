#pragma once

#include <optional>

#include <QHash>
#include <QMainWindow>
#include <QSet>
#include <QStringList>

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
  void UpdateDownloadForTesting(quint32 id, int percent, bool complete);
  int download_count_for_testing() const;
  int active_download_count_for_testing() const;
  QString download_status_for_testing(quint32 id) const;
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
  void RecordVisit(BrowserView* browser);
  bool SaveBrowsingData();

 private:
  static QString NormalizeUrl(QString input);
  BrowserView* AddTab(const QString& url, bool activate,
                      bool focus_address = false);
  BrowserView* CurrentBrowser() const;
  int IndexOf(const BrowserView* browser) const;
  void OpenPopup(BrowserView* source, const QString& url, int disposition);
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
  QWidget* find_bar_ = nullptr;
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
  QHash<BrowserView*, QString> pending_closed_urls_;
  QStringList closed_tabs_;
  QString session_path_;
  std::optional<BrowserSession> closing_session_;
  bool session_persistence_ready_ = false;
  bool window_close_requested_ = false;
  bool allow_window_close_ = false;

  Q_DISABLE_COPY_MOVE(MainWindow)
};
