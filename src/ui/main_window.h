#pragma once

#include <QHash>
#include <QMainWindow>
#include <QSet>
#include <QStringList>

class BrowserView;
class DownloadManager;
class DownloadPanel;
class QCloseEvent;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTabBar;

class MainWindow final : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(const QString& initial_url, QWidget* parent = nullptr);
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

 protected:
  void closeEvent(QCloseEvent* event) override;

 private slots:
  void NavigateFromAddressBar();
  void AddBlankTab();
  void ReopenClosedTab();
  void CloseTab(int index);
  void ActivateTab(int index);
  void UpdateLoadingState(bool loading, bool can_go_back,
                          bool can_go_forward);
  void UpdateAddress(const QString& url);

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

  QTabBar* tab_bar_ = nullptr;
  QStackedWidget* tab_stack_ = nullptr;
  QLineEdit* address_bar_ = nullptr;
  QPushButton* back_button_ = nullptr;
  QPushButton* forward_button_ = nullptr;
  QPushButton* reload_button_ = nullptr;
  DownloadManager* download_manager_ = nullptr;
  DownloadPanel* download_panel_ = nullptr;
  QSet<BrowserView*> closing_tabs_;
  QHash<BrowserView*, QString> pending_closed_urls_;
  QStringList closed_tabs_;
  bool window_close_requested_ = false;
  bool allow_window_close_ = false;

  Q_DISABLE_COPY_MOVE(MainWindow)
};
