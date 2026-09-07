#include "ui/main_window.h"

#include <algorithm>
#include <utility>

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QShortcut>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QToolButton>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>

#include "download/download_manager.h"
#include "profile/browsing_data_store.h"
#include "ui/browser_view.h"
#include "ui/download_panel.h"

namespace {

constexpr int kMaxClosedTabs = 20;

class BrowserTabBar final : public QTabBar {
 public:
  using QTabBar::QTabBar;

 protected:
  void mouseReleaseEvent(QMouseEvent* event) override {
    if (event->button() == Qt::MiddleButton) {
      const int index = tabAt(event->position().toPoint());
      if (index >= 0) emit tabCloseRequested(index);
      event->accept();
      return;
    }
    QTabBar::mouseReleaseEvent(event);
  }
};

QString TabText(const QString& title, const QString& url) {
  if (!title.trimmed().isEmpty()) return title.trimmed();
  if (url.isEmpty() || url == QStringLiteral("about:blank")) {
    return QStringLiteral("New Tab");
  }
  const QUrl parsed(url);
  return parsed.host().isEmpty() ? url : parsed.host();
}

}  // namespace

MainWindow::MainWindow(const BrowserSession& initial_session,
                       QString session_path, QString browsing_data_path,
                       QWidget* parent)
    : QMainWindow(parent), session_path_(std::move(session_path)) {
  setWindowTitle(QStringLiteral("Trail Browser"));
  resize(1280, 800);
  setMinimumSize(640, 480);

  auto* central = new QWidget(this);
  auto* page_layout = new QVBoxLayout(central);
  page_layout->setContentsMargins(0, 0, 0, 0);
  page_layout->setSpacing(0);

  tab_strip_ = new QWidget(central);
  auto* tab_layout = new QHBoxLayout(tab_strip_);
  tab_layout->setContentsMargins(6, 4, 6, 0);
  tab_layout->setSpacing(4);

  tab_bar_ = new BrowserTabBar(tab_strip_);
  tab_bar_->setDocumentMode(true);
  tab_bar_->setDrawBase(false);
  tab_bar_->setElideMode(Qt::ElideRight);
  tab_bar_->setExpanding(false);
  tab_bar_->setMovable(true);
  tab_bar_->setTabsClosable(true);
  tab_bar_->setUsesScrollButtons(true);

  auto* add_tab_button = new QToolButton(tab_strip_);
  add_tab_button->setText(QStringLiteral("+"));
  add_tab_button->setToolTip(QStringLiteral("New tab (Ctrl+T)"));
  tab_layout->addWidget(tab_bar_, 1);
  tab_layout->addWidget(add_tab_button);

  toolbar_ = new QWidget(central);
  auto* toolbar_layout = new QHBoxLayout(toolbar_);
  toolbar_layout->setContentsMargins(8, 6, 8, 6);
  toolbar_layout->setSpacing(6);

  back_button_ = new QPushButton(QStringLiteral("←"), toolbar_);
  forward_button_ = new QPushButton(QStringLiteral("→"), toolbar_);
  reload_button_ = new QPushButton(QStringLiteral("↻"), toolbar_);
  address_bar_ = new QLineEdit(toolbar_);
  auto* downloads_button =
      new QPushButton(QStringLiteral("Downloads"), toolbar_);
  bookmark_button_ = new QPushButton(QStringLiteral("☆"), toolbar_);
  bookmarks_button_ = new QPushButton(QStringLiteral("Bookmarks"), toolbar_);
  history_button_ = new QPushButton(QStringLiteral("History"), toolbar_);
  bookmarks_menu_ = new QMenu(bookmarks_button_);
  history_menu_ = new QMenu(history_button_);
  bookmarks_button_->setMenu(bookmarks_menu_);
  history_button_->setMenu(history_menu_);

  back_button_->setToolTip(QStringLiteral("Back"));
  forward_button_->setToolTip(QStringLiteral("Forward"));
  reload_button_->setToolTip(QStringLiteral("Reload"));
  address_bar_->setPlaceholderText(
      QStringLiteral("Search or enter an address"));
  address_bar_->setClearButtonEnabled(true);
  downloads_button->setToolTip(QStringLiteral("Show downloads"));
  bookmark_button_->setToolTip(QStringLiteral("Bookmark this page"));
  back_button_->setEnabled(false);
  forward_button_->setEnabled(false);

  toolbar_layout->addWidget(back_button_);
  toolbar_layout->addWidget(forward_button_);
  toolbar_layout->addWidget(reload_button_);
  toolbar_layout->addWidget(address_bar_, 1);
  toolbar_layout->addWidget(bookmark_button_);
  toolbar_layout->addWidget(bookmarks_button_);
  toolbar_layout->addWidget(history_button_);
  toolbar_layout->addWidget(downloads_button);

  find_bar_ = new QWidget(central);
  auto* find_layout = new QHBoxLayout(find_bar_);
  find_layout->setContentsMargins(8, 4, 8, 6);
  find_layout->setSpacing(5);
  find_edit_ = new QLineEdit(find_bar_);
  find_result_label_ = new QLabel(find_bar_);
  auto* previous_match = new QPushButton(QStringLiteral("↑"), find_bar_);
  auto* next_match = new QPushButton(QStringLiteral("↓"), find_bar_);
  auto* close_find = new QPushButton(QStringLiteral("×"), find_bar_);
  find_edit_->setPlaceholderText(QStringLiteral("Find in page"));
  find_result_label_->setMinimumWidth(70);
  find_result_label_->setAlignment(Qt::AlignCenter);
  previous_match->setToolTip(QStringLiteral("Previous match (Shift+Enter)"));
  next_match->setToolTip(QStringLiteral("Next match (Enter)"));
  close_find->setToolTip(QStringLiteral("Close (Esc)"));
  find_layout->addStretch();
  find_layout->addWidget(find_edit_);
  find_layout->addWidget(find_result_label_);
  find_layout->addWidget(previous_match);
  find_layout->addWidget(next_match);
  find_layout->addWidget(close_find);
  find_bar_->hide();

  tab_stack_ = new QStackedWidget(central);
  download_manager_ = new DownloadManager(this);
  browsing_data_ = new BrowsingDataStore(std::move(browsing_data_path));
  QString browsing_data_error;
  if (!browsing_data_->Load(&browsing_data_error)) {
    qWarning("Unable to load browsing data: %s",
             qPrintable(browsing_data_error));
  }
  download_panel_ = new DownloadPanel(download_manager_, central);
  session_save_timer_ = new QTimer(this);
  session_save_timer_->setSingleShot(true);
  connect(session_save_timer_, &QTimer::timeout, this, [this] {
    if (session_persistence_ready_ && !window_close_requested_) {
      PersistSession(CaptureSession(false));
    }
  });
  page_layout->addWidget(tab_strip_);
  page_layout->addWidget(toolbar_);
  page_layout->addWidget(find_bar_);
  page_layout->addWidget(tab_stack_, 1);
  page_layout->addWidget(download_panel_);
  setCentralWidget(central);

  connect(add_tab_button, &QToolButton::clicked, this, &MainWindow::AddBlankTab);
  connect(tab_bar_, &QTabBar::currentChanged, this, &MainWindow::ActivateTab);
  connect(tab_bar_, &QTabBar::tabCloseRequested, this, &MainWindow::CloseTab);
  connect(tab_bar_, &QTabBar::tabMoved, this, [this](int, int) {
    // The tab-to-page relationship is stored in tabData and therefore moves
    // with the visual tab. Keep the stacked page pointed at that object.
    ActivateTab(tab_bar_->currentIndex());
    ScheduleSessionSave();
  });
  connect(back_button_, &QPushButton::clicked, this, [this] {
    if (BrowserView* browser = CurrentBrowser()) browser->GoBack();
  });
  connect(forward_button_, &QPushButton::clicked, this, [this] {
    if (BrowserView* browser = CurrentBrowser()) browser->GoForward();
  });
  connect(reload_button_, &QPushButton::clicked, this, [this] {
    if (BrowserView* browser = CurrentBrowser()) {
      browser->is_loading() ? browser->Stop() : browser->Reload();
    }
  });
  connect(address_bar_, &QLineEdit::returnPressed, this,
          &MainWindow::NavigateFromAddressBar);
  connect(find_edit_, &QLineEdit::textChanged, this, [this] {
    FindFromBar(true, false);
  });
  connect(find_edit_, &QLineEdit::returnPressed, this,
          [this] { FindFromBar(true, true); });
  connect(previous_match, &QPushButton::clicked, this,
          [this] { FindFromBar(false, true); });
  connect(next_match, &QPushButton::clicked, this,
          [this] { FindFromBar(true, true); });
  connect(close_find, &QPushButton::clicked, this, &MainWindow::HideFindBar);
  connect(downloads_button, &QPushButton::clicked, download_panel_,
          &DownloadPanel::ToggleVisibility);
  connect(bookmark_button_, &QPushButton::clicked, this,
          &MainWindow::ToggleCurrentBookmark);
  connect(bookmarks_menu_, &QMenu::aboutToShow, this,
          &MainWindow::RebuildBookmarksMenu);
  connect(history_menu_, &QMenu::aboutToShow, this,
          &MainWindow::RebuildHistoryMenu);
  connect(download_manager_, &DownloadManager::ActiveCountChanged, this,
          [downloads_button](int count) {
            downloads_button->setText(
                count > 0 ? QStringLiteral("Downloads (%1)").arg(count)
                          : QStringLiteral("Downloads"));
          });

  auto* focus_address =
      new QShortcut(QKeySequence(QStringLiteral("Ctrl+L")), this);
  connect(focus_address, &QShortcut::activated, address_bar_, [this] {
    address_bar_->setFocus();
    address_bar_->selectAll();
  });
  auto* new_tab = new QShortcut(QKeySequence::AddTab, this);
  connect(new_tab, &QShortcut::activated, this, &MainWindow::AddBlankTab);
  auto* close_tab = new QShortcut(QKeySequence::Close, this);
  connect(close_tab, &QShortcut::activated, this,
          [this] { CloseTab(tab_bar_->currentIndex()); });
#if !defined(OS_MAC)
  auto* close_tab_alternate =
      new QShortcut(QKeySequence(QStringLiteral("Ctrl+W")), this);
  connect(close_tab_alternate, &QShortcut::activated, this,
          [this] { CloseTab(tab_bar_->currentIndex()); });
#endif
  auto* next_tab = new QShortcut(QKeySequence::NextChild, this);
  connect(next_tab, &QShortcut::activated, this, [this] {
    if (tab_bar_->count() > 1) {
      tab_bar_->setCurrentIndex((tab_bar_->currentIndex() + 1) %
                                tab_bar_->count());
    }
  });
  auto* previous_tab = new QShortcut(QKeySequence::PreviousChild, this);
  connect(previous_tab, &QShortcut::activated, this, [this] {
    if (tab_bar_->count() > 1) {
      tab_bar_->setCurrentIndex((tab_bar_->currentIndex() - 1 +
                                 tab_bar_->count()) %
                                tab_bar_->count());
    }
  });
  auto* reopen_tab =
      new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+T")), this);
  connect(reopen_tab, &QShortcut::activated, this,
          &MainWindow::ReopenClosedTab);
  auto* dev_tools = new QShortcut(QKeySequence(Qt::Key_F12), this);
  connect(dev_tools, &QShortcut::activated, this, [this] {
    if (BrowserView* browser = CurrentBrowser()) browser->ShowDevTools();
  });
  auto* print_page = new QShortcut(QKeySequence::Print, this);
  connect(print_page, &QShortcut::activated, this, [this] {
    if (BrowserView* browser = CurrentBrowser()) browser->Print();
  });
  auto* find_in_page = new QShortcut(QKeySequence::Find, this);
  connect(find_in_page, &QShortcut::activated, this, &MainWindow::ShowFindBar);
  auto* find_next = new QShortcut(QKeySequence::FindNext, this);
  connect(find_next, &QShortcut::activated, this,
          [this] { FindFromBar(true, true); });
  auto* find_previous = new QShortcut(QKeySequence::FindPrevious, this);
  connect(find_previous, &QShortcut::activated, this,
          [this] { FindFromBar(false, true); });
  auto* close_find_shortcut =
      new QShortcut(QKeySequence(Qt::Key_Escape), find_bar_);
  close_find_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
  connect(close_find_shortcut, &QShortcut::activated, this,
          &MainWindow::HideFindBar);
  auto* exit_fullscreen =
      new QShortcut(QKeySequence(Qt::Key_Escape), this);
  connect(exit_fullscreen, &QShortcut::activated, this, [this] {
    if (web_fullscreen_) {
      if (BrowserView* browser = CurrentBrowser()) browser->ExitFullscreen();
    }
  });
  auto* previous_match_shortcut = new QShortcut(
      QKeySequence(Qt::SHIFT | Qt::Key_Return), find_bar_);
  previous_match_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
  connect(previous_match_shortcut, &QShortcut::activated, this,
          [this] { FindFromBar(false, true); });
  auto* zoom_in = new QShortcut(QKeySequence::ZoomIn, this);
  connect(zoom_in, &QShortcut::activated, this, [this] {
    if (BrowserView* browser = CurrentBrowser()) browser->ZoomIn();
  });
  auto* zoom_out = new QShortcut(QKeySequence::ZoomOut, this);
  connect(zoom_out, &QShortcut::activated, this, [this] {
    if (BrowserView* browser = CurrentBrowser()) browser->ZoomOut();
  });
#if defined(OS_MAC)
  auto* reset_zoom =
      new QShortcut(QKeySequence(Qt::META | Qt::Key_0), this);
#else
  auto* reset_zoom =
      new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_0), this);
#endif
  connect(reset_zoom, &QShortcut::activated, this, [this] {
    if (BrowserView* browser = CurrentBrowser()) browser->ResetZoom();
  });
#if defined(OS_MAC)
  auto* toggle_bookmark =
      new QShortcut(QKeySequence(Qt::META | Qt::Key_D), this);
#else
  auto* toggle_bookmark =
      new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_D), this);
#endif
  connect(toggle_bookmark, &QShortcut::activated, this,
          &MainWindow::ToggleCurrentBookmark);

  const QStringList initial_urls = initial_session.tab_urls.isEmpty()
                                       ? QStringList{QStringLiteral("https://www.example.com")}
                                       : initial_session.tab_urls;
  for (const QString& url : initial_urls) AddTab(url, false);
  tab_bar_->setCurrentIndex(
      std::clamp(initial_session.active_tab, 0, tab_bar_->count() - 1));
  ActivateTab(tab_bar_->currentIndex());
  if (!initial_session.window_geometry.isEmpty()) {
    restoreGeometry(initial_session.window_geometry);
  }
  if (!initial_session.clean_exit) {
    statusBar()->showMessage(
        QStringLiteral("Restored tabs after an unexpected shutdown"), 8000);
  }
  RebuildBookmarksMenu();
  RebuildHistoryMenu();

  // Do not overwrite a known-good previous session until the window and CEF
  // event loop have had time to start successfully. After this gate, every
  // saved live snapshot is marked unclean until orderly shutdown completes.
  QTimer::singleShot(1500, this, [this] {
    if (window_close_requested_) return;
    session_persistence_ready_ = true;
    PersistSession(CaptureSession(false));
  });
}

MainWindow::~MainWindow() {
  delete browsing_data_;
}

int MainWindow::tab_count() const {
  return tab_bar_->count();
}

QString MainWindow::current_url() const {
  BrowserView* browser = CurrentBrowser();
  return browser ? browser->current_url() : QString();
}

QString MainWindow::current_title() const {
  BrowserView* browser = CurrentBrowser();
  return browser ? browser->page_title() : QString();
}

void MainWindow::OpenTabForTesting(const QString& url) {
  AddTab(url, true);
}

void MainWindow::CloseCurrentTabForTesting() {
  CloseTab(tab_bar_->currentIndex());
}

void MainWindow::ReopenClosedTabForTesting() {
  ReopenClosedTab();
}

void MainWindow::UpdateDownloadForTesting(quint32 id, int percent,
                                          bool complete) {
  DownloadManager::Item item;
  item.id = id;
  item.file_name = QStringLiteral("trail-test.bin");
  item.url = QStringLiteral("https://example.test/trail-test.bin");
  item.total_bytes = 1024;
  item.received_bytes = complete ? item.total_bytes
                                 : item.total_bytes * percent / 100;
  item.bytes_per_second = complete ? 0 : 256;
  item.percent = complete ? 100 : percent;
  item.state = complete ? DownloadManager::State::Complete
                        : DownloadManager::State::InProgress;
  download_manager_->UpdateForTesting(item);
}

void MainWindow::PauseDownloadForTesting(quint32 id) {
  const auto current = download_manager_->item(id);
  if (!current) return;
  DownloadManager::Item paused = *current;
  paused.state = DownloadManager::State::Paused;
  paused.bytes_per_second = 0;
  download_manager_->UpdateForTesting(paused);
}

int MainWindow::download_count_for_testing() const {
  return download_manager_->items().size();
}

int MainWindow::active_download_count_for_testing() const {
  return download_manager_->active_count();
}

QString MainWindow::download_status_for_testing(quint32 id) const {
  const auto item = download_manager_->item(id);
  return item ? DownloadManager::StatusText(*item) : QString();
}

void MainWindow::ShowFailureForTesting(bool render_process_failed) {
  if (BrowserView* browser = CurrentBrowser()) {
    browser->ShowFailureForTesting(render_process_failed);
  }
}

bool MainWindow::failure_page_active_for_testing() const {
  BrowserView* browser = CurrentBrowser();
  return browser && browser->failure_page_active();
}

bool MainWindow::render_process_failed_for_testing() const {
  BrowserView* browser = CurrentBrowser();
  return browser && browser->render_process_failed();
}

BrowserSession MainWindow::session_for_testing(bool clean_exit) const {
  return CaptureSession(clean_exit);
}

bool MainWindow::save_session_for_testing(bool clean_exit) {
  return PersistSession(CaptureSession(clean_exit));
}

bool MainWindow::find_bar_visible_for_testing() const {
  return find_bar_->isVisible();
}

void MainWindow::ShowFindBarForTesting() {
  ShowFindBar();
}

void MainWindow::HideFindBarForTesting() {
  HideFindBar();
}

void MainWindow::FindForTesting(const QString& text) {
  ShowFindBar();
  find_edit_->setText(text);
}

QString MainWindow::find_result_for_testing() const {
  return find_result_label_->text();
}

void MainWindow::ZoomInForTesting() {
  if (BrowserView* browser = CurrentBrowser()) browser->ZoomIn();
}

void MainWindow::ResetZoomForTesting() {
  if (BrowserView* browser = CurrentBrowser()) browser->ResetZoom();
}

int MainWindow::zoom_percent_for_testing() const {
  BrowserView* browser = CurrentBrowser();
  return browser ? browser->zoom_percent() : 100;
}

void MainWindow::ToggleBookmarkForTesting() {
  ToggleCurrentBookmark();
}

bool MainWindow::current_page_bookmarked_for_testing() const {
  BrowserView* browser = CurrentBrowser();
  return browser && browsing_data_->IsBookmarked(browser->current_url());
}

int MainWindow::history_count_for_testing() const {
  return browsing_data_->history().size();
}

int MainWindow::current_url_visit_count_for_testing() const {
  BrowserView* browser = CurrentBrowser();
  if (!browser) return 0;
  for (const BrowsingDataStore::HistoryEntry& entry : browsing_data_->history()) {
    if (entry.url == browser->current_url()) return entry.visit_count;
  }
  return 0;
}

QString MainWindow::media_permission_description_for_testing(
    uint32_t permissions) const {
  return BrowserView::MediaPermissionDescription(permissions);
}

QString MainWindow::permission_description_for_testing(
    uint32_t permissions) const {
  return BrowserView::PermissionDescription(permissions);
}

bool MainWindow::external_scheme_allowed_for_testing(const QString& url) const {
  return BrowserView::IsAllowedExternalScheme(url);
}

void MainWindow::SetWebFullscreenForTesting(bool fullscreen) {
  if (BrowserView* browser = CurrentBrowser()) {
    browser->FullscreenChanged(fullscreen);
  }
}

void MainWindow::closeEvent(QCloseEvent* event) {
  if (allow_window_close_) {
    if (closing_session_) PersistSession(*closing_session_);
    event->accept();
    return;
  }

  event->ignore();
  if (window_close_requested_) return;
  window_close_requested_ = true;
  closing_session_ = CaptureSession(true);
  ContinueWindowClose();
}

void MainWindow::moveEvent(QMoveEvent* event) {
  QMainWindow::moveEvent(event);
  ScheduleSessionSave();
}

void MainWindow::resizeEvent(QResizeEvent* event) {
  QMainWindow::resizeEvent(event);
  ScheduleSessionSave();
}

void MainWindow::NavigateFromAddressBar() {
  if (BrowserView* browser = CurrentBrowser()) {
    const QString url = NormalizeUrl(address_bar_->text());
    address_bar_->setText(url == QStringLiteral("about:blank") ? QString()
                                                                 : url);
    browser->LoadUrl(url);
  }
}

void MainWindow::AddBlankTab() {
  AddTab(QStringLiteral("about:blank"), true, true);
}

void MainWindow::ReopenClosedTab() {
  if (closed_tabs_.isEmpty()) return;
  AddTab(closed_tabs_.takeLast(), true);
}

void MainWindow::CloseTab(int index) {
  auto* browser = qvariant_cast<BrowserView*>(tab_bar_->tabData(index));
  if (!browser || closing_tabs_.contains(browser)) return;

  if (tab_bar_->count() == 1) {
    close();
    return;
  }
  BeginTabClose(browser, true);
}

void MainWindow::ActivateTab(int index) {
  if (index >= 0 && index < tab_bar_->count()) {
    auto* browser = qvariant_cast<BrowserView*>(tab_bar_->tabData(index));
    if (browser) tab_stack_->setCurrentWidget(browser);
  }
  UpdateChrome();
  ScheduleSessionSave();
}

void MainWindow::UpdateLoadingState(bool loading, bool can_go_back,
                                    bool can_go_forward) {
  if (sender() != CurrentBrowser()) return;
  back_button_->setEnabled(can_go_back);
  forward_button_->setEnabled(can_go_forward);
  reload_button_->setText(loading ? QStringLiteral("×")
                                  : QStringLiteral("↻"));
  reload_button_->setToolTip(loading ? QStringLiteral("Stop")
                                     : QStringLiteral("Reload"));
}

void MainWindow::UpdateAddress(const QString& url) {
  if (sender() != CurrentBrowser() || address_bar_->hasFocus()) return;
  address_bar_->setText(url == QStringLiteral("about:blank") ? QString()
                                                               : url);
}

void MainWindow::ShowFindBar() {
  find_bar_->show();
  find_edit_->setFocus();
  find_edit_->selectAll();
}

void MainWindow::HideFindBar() {
  find_bar_->hide();
  find_result_label_->clear();
  if (BrowserView* browser = CurrentBrowser()) {
    browser->StopFinding(true);
    browser->setFocus();
  }
}

void MainWindow::FindFromBar(bool forward, bool find_next) {
  BrowserView* browser = CurrentBrowser();
  if (!browser) return;
  const QString text = find_edit_->text();
  if (text.isEmpty()) {
    browser->StopFinding(true);
    find_result_label_->clear();
    return;
  }
  browser->Find(text, forward, find_next);
}

BrowserView* MainWindow::AddTab(const QString& url, bool activate,
                                bool focus_address) {
  auto* browser = new BrowserView(url, download_manager_->handler(), tab_stack_);
  tab_stack_->addWidget(browser);
  const int tab_index = tab_bar_->addTab(TabText({}, url));
  tab_bar_->setTabData(tab_index, QVariant::fromValue(browser));

  connect(browser, &BrowserView::TitleChanged, this,
          [this, browser](const QString& title) {
            UpdateTabTitle(browser, title);
          });
  connect(browser, &BrowserView::AddressChanged, this,
          [this, browser](const QString& address) {
            const int index = IndexOf(browser);
            if (index >= 0) tab_bar_->setTabToolTip(index, address);
            if (browser == CurrentBrowser()) UpdateAddress(address);
            ScheduleSessionSave();
          });
  connect(browser, &BrowserView::NavigationCompleted, this,
          [this, browser] { RecordVisit(browser); });
  connect(browser, &BrowserView::SecurityMessage, this,
          [this](const QString& message) {
            statusBar()->showMessage(message, 5000);
          });
  connect(browser, &BrowserView::StatusMessageChanged, this,
          [this, browser](const QString& message) {
            if (browser != CurrentBrowser()) return;
            message.isEmpty() ? statusBar()->clearMessage()
                              : statusBar()->showMessage(message);
          });
  connect(browser, &BrowserView::FullscreenChanged, this,
          [this, browser](bool fullscreen) {
            if (browser != CurrentBrowser()) return;
            if (fullscreen == web_fullscreen_) return;
            if (fullscreen) {
              window_was_maximized_ = isMaximized();
              find_bar_was_visible_ = find_bar_->isVisible();
            }
            web_fullscreen_ = fullscreen;
            tab_strip_->setVisible(!fullscreen);
            toolbar_->setVisible(!fullscreen);
            find_bar_->setVisible(!fullscreen && find_bar_was_visible_);
            if (fullscreen) {
              showFullScreen();
            } else if (window_was_maximized_) {
              showMaximized();
            } else {
              showNormal();
            }
            if (fullscreen) {
              statusBar()->showMessage(
                  QStringLiteral("Press Esc to exit full screen"), 3000);
            }
          });
  connect(browser, &BrowserView::LoadingStateChanged, this,
          [this, browser](bool loading, bool can_go_back, bool can_go_forward) {
            if (browser == CurrentBrowser()) {
              UpdateLoadingState(loading, can_go_back, can_go_forward);
            }
          });
  connect(browser, &BrowserView::FindResultChanged, this,
          [this, browser](int count, int active_match, bool final_update) {
            if (browser != CurrentBrowser() || !find_bar_->isVisible()) return;
            if (count <= 0 && final_update) {
              find_result_label_->setText(QStringLiteral("No matches"));
            } else if (count > 0) {
              find_result_label_->setText(
                  QStringLiteral("%1 / %2").arg(active_match).arg(count));
            }
          });
  connect(browser, &BrowserView::ZoomChanged, this,
          [this, browser](int percent) {
            if (browser == CurrentBrowser()) {
              statusBar()->showMessage(QStringLiteral("Zoom: %1%").arg(percent),
                                       2000);
            }
          });
  connect(browser, &BrowserView::PopupRequested, this,
          [this, browser](const QString& target, int disposition) {
            OpenPopup(browser, target, disposition);
          });
  connect(browser, &BrowserView::ShortcutRequested, this,
          [this, browser](int action) {
            if (browser == CurrentBrowser()) HandleBrowserShortcut(action);
          });
  connect(browser, &BrowserView::BrowserClosed, this,
          [this, browser] { CompleteTabClose(browser); });
  connect(browser, &BrowserView::CloseCancelled, this,
          [this, browser] { CancelTabClose(browser); });

  tab_bar_->setTabToolTip(tab_index, url);
  if (activate || tab_bar_->count() == 1) {
    tab_bar_->setCurrentIndex(tab_index);
    tab_stack_->setCurrentWidget(browser);
    UpdateChrome();
    if (focus_address) {
      address_bar_->setFocus();
      address_bar_->selectAll();
    } else {
      browser->setFocus();
    }
  }
  ScheduleSessionSave();
  return browser;
}

BrowserView* MainWindow::CurrentBrowser() const {
  return qvariant_cast<BrowserView*>(
      tab_bar_->tabData(tab_bar_->currentIndex()));
}

int MainWindow::IndexOf(const BrowserView* browser) const {
  for (int index = 0; index < tab_bar_->count(); ++index) {
    if (qvariant_cast<BrowserView*>(tab_bar_->tabData(index)) == browser) {
      return index;
    }
  }
  return -1;
}

void MainWindow::OpenPopup(BrowserView* source, const QString& url,
                           int disposition_value) {
  if (!source || closing_tabs_.contains(source)) return;
  const QString target = url.isEmpty() ? QStringLiteral("about:blank") : url;
  const auto disposition =
      static_cast<cef_window_open_disposition_t>(disposition_value);

  if (disposition == CEF_WOD_CURRENT_TAB) {
    source->LoadUrl(target);
    return;
  }
  if (disposition == CEF_WOD_NEW_BACKGROUND_TAB) {
    AddTab(target, false);
    return;
  }
  if (disposition == CEF_WOD_SINGLETON_TAB ||
      disposition == CEF_WOD_SWITCH_TO_TAB) {
    for (int index = 0; index < tab_bar_->count(); ++index) {
      auto* candidate =
          qvariant_cast<BrowserView*>(tab_bar_->tabData(index));
      if (candidate && candidate->current_url() == target) {
        tab_bar_->setCurrentIndex(index);
        return;
      }
    }
  }
  if (disposition != CEF_WOD_IGNORE_ACTION &&
      disposition != CEF_WOD_SAVE_TO_DISK) {
    AddTab(target, true);
  }
}

void MainWindow::BeginTabClose(BrowserView* browser, bool remember_url) {
  if (!browser || closing_tabs_.contains(browser)) return;
  closing_tabs_.insert(browser);
  if (remember_url && !browser->current_url().isEmpty()) {
    pending_closed_urls_.insert(browser, browser->current_url());
  }
  const int index = IndexOf(browser);
  if (index >= 0) tab_bar_->setTabEnabled(index, false);

  if (browser->RequestClose()) CompleteTabClose(browser);
}

void MainWindow::CompleteTabClose(BrowserView* browser) {
  if (!browser || !closing_tabs_.remove(browser)) return;
  browser->FinalizeClose();

  if (pending_closed_urls_.contains(browser)) {
    closed_tabs_.append(pending_closed_urls_.take(browser));
    while (closed_tabs_.size() > kMaxClosedTabs) closed_tabs_.removeFirst();
  }

  const int index = IndexOf(browser);
  if (index >= 0) {
    tab_stack_->removeWidget(browser);
    tab_bar_->removeTab(index);
  }
  browser->deleteLater();

  if (window_close_requested_) {
    ContinueWindowClose();
  } else {
    UpdateChrome();
    ScheduleSessionSave();
  }
}

void MainWindow::CancelTabClose(BrowserView* browser) {
  if (!browser || !closing_tabs_.remove(browser)) return;
  pending_closed_urls_.remove(browser);
  const int index = IndexOf(browser);
  if (index >= 0) {
    tab_bar_->setTabEnabled(index, true);
    tab_bar_->setCurrentIndex(index);
  }
  window_close_requested_ = false;
  closing_session_.reset();
  UpdateChrome();
  ScheduleSessionSave();
}

void MainWindow::ContinueWindowClose() {
  if (!window_close_requested_ || !closing_tabs_.isEmpty()) return;
  if (tab_stack_->count() == 0) {
    allow_window_close_ = true;
    QMetaObject::invokeMethod(this, &QWidget::close, Qt::QueuedConnection);
    return;
  }

  BrowserView* browser = CurrentBrowser();
  if (!browser) {
    browser = qvariant_cast<BrowserView*>(tab_bar_->tabData(0));
  }
  BeginTabClose(browser, false);
}

void MainWindow::UpdateChrome() {
  BrowserView* browser = CurrentBrowser();
  const bool available = browser && !closing_tabs_.contains(browser);
  address_bar_->setEnabled(available);
  back_button_->setEnabled(available && browser->can_go_back());
  forward_button_->setEnabled(available && browser->can_go_forward());
  reload_button_->setEnabled(available);
  bookmark_button_->setEnabled(available && !browser->current_url().isEmpty() &&
                               browser->current_url() != QStringLiteral("about:blank"));

  if (!browser) {
    address_bar_->clear();
    setWindowTitle(QStringLiteral("Trail Browser"));
    return;
  }

  const QString url = browser->current_url();
  bookmark_button_->setText(browsing_data_->IsBookmarked(url)
                                ? QStringLiteral("★")
                                : QStringLiteral("☆"));
  address_bar_->setText(url == QStringLiteral("about:blank") ? QString()
                                                               : url);
  reload_button_->setText(browser->is_loading() ? QStringLiteral("×")
                                                : QStringLiteral("↻"));
  reload_button_->setToolTip(browser->is_loading() ? QStringLiteral("Stop")
                                                   : QStringLiteral("Reload"));
  const QString title = TabText(browser->page_title(), url);
  setWindowTitle(title == QStringLiteral("New Tab")
                     ? QStringLiteral("Trail Browser")
                     : title + QStringLiteral(" — Trail Browser"));
}

void MainWindow::UpdateTabTitle(BrowserView* browser, const QString& title) {
  const int index = IndexOf(browser);
  if (index < 0) return;
  tab_bar_->setTabText(index, TabText(title, browser->current_url()));
  if (browser == CurrentBrowser()) UpdateChrome();
}

void MainWindow::HandleBrowserShortcut(int action_value) {
  const auto action = static_cast<BrowserView::ShortcutAction>(action_value);
  switch (action) {
    case BrowserView::ShortcutAction::NewTab:
      AddBlankTab();
      break;
    case BrowserView::ShortcutAction::CloseTab:
      CloseTab(tab_bar_->currentIndex());
      break;
    case BrowserView::ShortcutAction::ReopenClosedTab:
      ReopenClosedTab();
      break;
    case BrowserView::ShortcutAction::FocusAddress:
      address_bar_->setFocus();
      address_bar_->selectAll();
      break;
    case BrowserView::ShortcutAction::NextTab:
      if (tab_bar_->count() > 1) {
        tab_bar_->setCurrentIndex((tab_bar_->currentIndex() + 1) %
                                  tab_bar_->count());
      }
      break;
    case BrowserView::ShortcutAction::PreviousTab:
      if (tab_bar_->count() > 1) {
        tab_bar_->setCurrentIndex((tab_bar_->currentIndex() - 1 +
                                   tab_bar_->count()) %
                                  tab_bar_->count());
      }
      break;
    case BrowserView::ShortcutAction::FindInPage:
      ShowFindBar();
      break;
    case BrowserView::ShortcutAction::FindNext:
      FindFromBar(true, true);
      break;
    case BrowserView::ShortcutAction::FindPrevious:
      FindFromBar(false, true);
      break;
    case BrowserView::ShortcutAction::ZoomIn:
      if (BrowserView* browser = CurrentBrowser()) browser->ZoomIn();
      break;
    case BrowserView::ShortcutAction::ZoomOut:
      if (BrowserView* browser = CurrentBrowser()) browser->ZoomOut();
      break;
    case BrowserView::ShortcutAction::ResetZoom:
      if (BrowserView* browser = CurrentBrowser()) browser->ResetZoom();
      break;
    case BrowserView::ShortcutAction::ToggleBookmark:
      ToggleCurrentBookmark();
      break;
    case BrowserView::ShortcutAction::ExitFullscreen:
      if (web_fullscreen_) {
        if (BrowserView* browser = CurrentBrowser()) browser->ExitFullscreen();
      }
      break;
  }
}

void MainWindow::ToggleCurrentBookmark() {
  BrowserView* browser = CurrentBrowser();
  if (!browser) return;
  const QString url = browser->current_url();
  if (browsing_data_->IsBookmarked(url)) {
    browsing_data_->RemoveBookmark(url);
    statusBar()->showMessage(QStringLiteral("Bookmark removed"), 2000);
  } else if (browsing_data_->AddBookmark(url, browser->page_title())) {
    statusBar()->showMessage(QStringLiteral("Bookmark added"), 2000);
  } else {
    return;
  }
  SaveBrowsingData();
  RebuildBookmarksMenu();
  UpdateChrome();
}

void MainWindow::RebuildBookmarksMenu() {
  bookmarks_menu_->clear();
  if (browsing_data_->bookmarks().isEmpty()) {
    QAction* empty = bookmarks_menu_->addAction(QStringLiteral("No bookmarks yet"));
    empty->setEnabled(false);
    return;
  }
  for (const BrowsingDataStore::Bookmark& bookmark :
       browsing_data_->bookmarks()) {
    const QString label = bookmark.title.isEmpty() ? bookmark.url
                                                    : bookmark.title;
    QAction* action = bookmarks_menu_->addAction(label);
    action->setToolTip(bookmark.url);
    connect(action, &QAction::triggered, this,
            [this, url = bookmark.url] { AddTab(url, true); });
  }
  bookmarks_menu_->addSeparator();
  QAction* remove = bookmarks_menu_->addAction(
      QStringLiteral("Remove bookmark for current page"));
  BrowserView* browser = CurrentBrowser();
  remove->setEnabled(browser &&
                     browsing_data_->IsBookmarked(browser->current_url()));
  connect(remove, &QAction::triggered, this,
          &MainWindow::ToggleCurrentBookmark);
}

void MainWindow::RebuildHistoryMenu() {
  history_menu_->clear();
  if (browsing_data_->history().isEmpty()) {
    QAction* empty = history_menu_->addAction(QStringLiteral("No history yet"));
    empty->setEnabled(false);
    return;
  }
  constexpr int kVisibleHistoryEntries = 25;
  const auto& history = browsing_data_->history();
  const int count = std::min(static_cast<int>(history.size()),
                             kVisibleHistoryEntries);
  for (int index = 0; index < count; ++index) {
    const auto& entry = history.at(index);
    const QString label = entry.title.isEmpty() ? entry.url : entry.title;
    QAction* action = history_menu_->addAction(label);
    action->setToolTip(entry.url);
    connect(action, &QAction::triggered, this,
            [this, url = entry.url] { AddTab(url, true); });
  }
  history_menu_->addSeparator();
  QAction* clear = history_menu_->addAction(QStringLiteral("Clear history"));
  connect(clear, &QAction::triggered, this, [this] {
    browsing_data_->ClearHistory();
    SaveBrowsingData();
    RebuildHistoryMenu();
  });
}

void MainWindow::RecordVisit(BrowserView* browser) {
  if (!browser || browser->is_loading()) return;
  const QString url = browser->current_url();
  if (url.isEmpty()) return;
  browsing_data_->RecordVisit(url, browser->page_title());
  SaveBrowsingData();
}

bool MainWindow::SaveBrowsingData() {
  QString error;
  const bool saved = browsing_data_->Save(&error);
  if (!saved && !error.isEmpty()) {
    statusBar()->showMessage(
        QStringLiteral("Unable to save browsing data: %1").arg(error), 8000);
  }
  return saved;
}

void MainWindow::ScheduleSessionSave() {
  if (!session_persistence_ready_ || window_close_requested_ ||
      session_path_.isEmpty()) {
    return;
  }
  constexpr int kSaveDelayMs = 500;
  session_save_timer_->start(kSaveDelayMs);
}

BrowserSession MainWindow::CaptureSession(bool clean_exit) const {
  BrowserSession session;
  session.clean_exit = clean_exit;
  session.active_tab = std::max(0, tab_bar_->currentIndex());
  session.window_geometry = saveGeometry();
  for (int index = 0; index < tab_bar_->count(); ++index) {
    BrowserView* browser =
        qvariant_cast<BrowserView*>(tab_bar_->tabData(index));
    if (browser && !closing_tabs_.contains(browser)) {
      const QString url = browser->current_url().trimmed();
      session.tab_urls.append(url.isEmpty() ? QStringLiteral("about:blank")
                                            : url);
    }
  }
  if (session.tab_urls.isEmpty()) {
    session.tab_urls.append(QStringLiteral("about:blank"));
    session.active_tab = 0;
  } else {
    session.active_tab =
        std::clamp(session.active_tab, 0,
                   static_cast<int>(session.tab_urls.size()) - 1);
  }
  return session;
}

bool MainWindow::PersistSession(const BrowserSession& session) {
  if (session_path_.isEmpty()) return false;
  QString error;
  const bool saved = SessionStore::Save(session_path_, session, &error);
  if (!saved) {
    statusBar()->showMessage(
        QStringLiteral("Unable to save browser session: %1").arg(error),
        8000);
  }
  return saved;
}

QString MainWindow::NormalizeUrl(QString input) {
  input = input.trimmed();
  if (input.isEmpty()) return QStringLiteral("about:blank");

  if (input.contains(QLatin1Char(' '))) {
    return QStringLiteral("https://www.google.com/search?q=%1")
        .arg(QString::fromLatin1(QUrl::toPercentEncoding(input)));
  }

  const QUrl parsed = QUrl::fromUserInput(input);
  return parsed.isValid() ? parsed.toString() : QStringLiteral("about:blank");
}
