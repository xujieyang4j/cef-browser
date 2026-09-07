#include "ui/main_window.h"

#include <algorithm>
#include <functional>
#include <utility>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QClipboard>
#include <QCheckBox>
#include <QCompleter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QMoveEvent>
#include <QPushButton>
#include <QProgressBar>
#include <QPixmap>
#include <QResizeEvent>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStringListModel>
#include <QTabBar>
#include <QToolButton>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>

#include "download/download_manager.h"
#include "include/cef_cookie.h"
#include "include/cef_request_context.h"
#include "profile/browsing_data_store.h"
#include "settings/browser_settings.h"
#include "ui/browser_view.h"
#include "ui/download_panel.h"

namespace {

constexpr int kMaxClosedTabs = 20;
constexpr int kMaxAddressSuggestions = 200;

class CompletionCallback final : public CefCompletionCallback {
 public:
  explicit CompletionCallback(std::function<void()> completion)
      : completion_(std::move(completion)) {}

  void OnComplete() override {
    if (completion_) completion_();
  }

 private:
  std::function<void()> completion_;

  IMPLEMENT_REFCOUNTING(CompletionCallback);
  DISALLOW_COPY_AND_ASSIGN(CompletionCallback);
};

class DeleteCookiesCallback final : public CefDeleteCookiesCallback {
 public:
  explicit DeleteCookiesCallback(std::function<void(int)> completion)
      : completion_(std::move(completion)) {}

  void OnComplete(int num_deleted) override {
    if (completion_) completion_(num_deleted);
  }

 private:
  std::function<void(int)> completion_;

  IMPLEMENT_REFCOUNTING(DeleteCookiesCallback);
  DISALLOW_COPY_AND_ASSIGN(DeleteCookiesCallback);
};

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

QKeySequence PrimaryShortcut(const QString& keys) {
#if defined(OS_MAC)
  return QKeySequence(QStringLiteral("Meta+") + keys);
#else
  return QKeySequence(QStringLiteral("Ctrl+") + keys);
#endif
}

}  // namespace

MainWindow::MainWindow(const BrowserSession& initial_session,
                       QString session_path, QString browsing_data_path,
                       QString settings_path, QString download_history_path,
                       QWidget* parent)
    : QMainWindow(parent), session_path_(std::move(session_path)) {
  browser_settings_ = new BrowserSettings(std::move(settings_path));
  QString settings_error;
  if (!browser_settings_->Load(&settings_error)) {
    qWarning("Unable to load browser settings: %s",
             qPrintable(settings_error));
  }
  setWindowTitle(QStringLiteral("Trail Browser"));
  resize(1280, 800);
  setMinimumSize(640, 480);
  CreateApplicationMenus();

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
  tab_bar_->setContextMenuPolicy(Qt::CustomContextMenu);

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
  home_button_ = new QPushButton(QStringLiteral("⌂"), toolbar_);
  address_bar_ = new QLineEdit(toolbar_);
  downloads_button_ = new QPushButton(QStringLiteral("Downloads"), toolbar_);
  bookmark_button_ = new QPushButton(QStringLiteral("☆"), toolbar_);
  bookmarks_button_ = new QPushButton(QStringLiteral("Bookmarks"), toolbar_);
  history_button_ = new QPushButton(QStringLiteral("History"), toolbar_);
  all_tabs_button_ = new QPushButton(QStringLiteral("⌄"), toolbar_);
  bookmarks_menu_ = new QMenu(bookmarks_button_);
  history_menu_ = new QMenu(history_button_);
  all_tabs_menu_ = new QMenu(all_tabs_button_);
  bookmarks_button_->setMenu(bookmarks_menu_);
  history_button_->setMenu(history_menu_);
  all_tabs_button_->setMenu(all_tabs_menu_);
  bookmarks_menu_->setContextMenuPolicy(Qt::CustomContextMenu);
  history_menu_->setContextMenuPolicy(Qt::CustomContextMenu);

  back_button_->setToolTip(QStringLiteral("Back"));
  forward_button_->setToolTip(QStringLiteral("Forward"));
  reload_button_->setToolTip(QStringLiteral("Reload"));
  home_button_->setToolTip(QStringLiteral("Home"));
  address_bar_->setPlaceholderText(
      QStringLiteral("Search or enter an address"));
  address_bar_->setClearButtonEnabled(true);
  address_suggestions_ = new QStringListModel(this);
  address_completer_ = new QCompleter(address_suggestions_, this);
  address_completer_->setCaseSensitivity(Qt::CaseInsensitive);
  address_completer_->setCompletionMode(QCompleter::PopupCompletion);
  address_completer_->setFilterMode(Qt::MatchContains);
  address_completer_->setMaxVisibleItems(12);
  address_bar_->setCompleter(address_completer_);
  downloads_button_->setToolTip(QStringLiteral("Show downloads"));
  bookmark_button_->setToolTip(QStringLiteral("Bookmark this page"));
  all_tabs_button_->setToolTip(
      QStringLiteral("Open and recently closed tabs (Ctrl+Shift+A)"));
  back_button_->setEnabled(false);
  forward_button_->setEnabled(false);

  toolbar_layout->addWidget(back_button_);
  toolbar_layout->addWidget(forward_button_);
  toolbar_layout->addWidget(reload_button_);
  toolbar_layout->addWidget(home_button_);
  toolbar_layout->addWidget(address_bar_, 1);
  toolbar_layout->addWidget(bookmark_button_);
  toolbar_layout->addWidget(bookmarks_button_);
  toolbar_layout->addWidget(history_button_);
  toolbar_layout->addWidget(all_tabs_button_);
  toolbar_layout->addWidget(downloads_button_);

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
  loading_progress_ = new QProgressBar(central);
  loading_progress_->setRange(0, 1000);
  loading_progress_->setTextVisible(false);
  loading_progress_->setFixedHeight(3);
  loading_progress_->hide();

  tab_stack_ = new QStackedWidget(central);
  download_manager_ =
      new DownloadManager(std::move(download_history_path), this);
  QString download_history_error;
  if (!download_manager_->LoadHistory(&download_history_error)) {
    qWarning("Unable to load download history: %s",
             qPrintable(download_history_error));
  }
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
  page_layout->addWidget(loading_progress_);
  page_layout->addWidget(find_bar_);
  page_layout->addWidget(tab_stack_, 1);
  page_layout->addWidget(download_panel_);
  setCentralWidget(central);

  connect(add_tab_button, &QToolButton::clicked, this, &MainWindow::AddBlankTab);
  connect(tab_bar_, &QTabBar::currentChanged, this, &MainWindow::ActivateTab);
  connect(tab_bar_, &QTabBar::tabCloseRequested, this, &MainWindow::CloseTab);
  connect(tab_bar_, &QTabBar::customContextMenuRequested, this,
          &MainWindow::ShowTabContextMenu);
  connect(tab_bar_, &QTabBar::tabMoved, this, [this](int from, int to) {
    ConstrainMovedTab(from, to);
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
  connect(home_button_, &QPushButton::clicked, this, &MainWindow::GoHome);
  connect(address_bar_, &QLineEdit::returnPressed, this,
          &MainWindow::NavigateFromAddressBar);
  connect(address_completer_,
          QOverload<const QString&>::of(&QCompleter::activated), this,
          &MainWindow::NavigateFromAddressSuggestion);
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
  connect(downloads_button_, &QPushButton::clicked, this, [this] {
    download_panel_->ToggleVisibility();
  });
  connect(bookmark_button_, &QPushButton::clicked, this,
          &MainWindow::ToggleCurrentBookmark);
  connect(bookmarks_menu_, &QMenu::aboutToShow, this,
          &MainWindow::RebuildBookmarksMenu);
  connect(history_menu_, &QMenu::aboutToShow, this,
          &MainWindow::RebuildHistoryMenu);
  connect(bookmarks_menu_, &QMenu::customContextMenuRequested, this,
          &MainWindow::ShowBookmarkContextMenu);
  connect(history_menu_, &QMenu::customContextMenuRequested, this,
          &MainWindow::ShowHistoryContextMenu);
  connect(all_tabs_menu_, &QMenu::aboutToShow, this,
          &MainWindow::RebuildAllTabsMenu);
  connect(download_manager_, &DownloadManager::ActiveCountChanged, this,
          [this](int count) {
            downloads_button_->setText(
                count > 0 ? QStringLiteral("Downloads (%1)").arg(count)
                          : QStringLiteral("Downloads"));
          });
  connect(download_manager_, &DownloadManager::PersistenceError, this,
          [this](const QString& error) {
            statusBar()->showMessage(
                QStringLiteral("Unable to save download history: %1")
                    .arg(error),
                8000);
          });

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
  for (int number = 1; number <= 9; ++number) {
    auto* activate_tab =
        new QShortcut(PrimaryShortcut(QString::number(number)), this);
    connect(activate_tab, &QShortcut::activated, this, [this, number] {
      ActivateTabByShortcut(number == 9 ? tab_bar_->count() - 1
                                        : number - 1);
    });
  }

  const QStringList initial_urls = initial_session.tab_urls.isEmpty()
                                       ? QStringList{QStringLiteral("https://www.example.com")}
                                       : initial_session.tab_urls;
  const int closed_start = std::max(
      0, static_cast<int>(initial_session.recently_closed_tabs.size()) -
             kMaxClosedTabs);
  closed_tabs_ = initial_session.recently_closed_tabs.mid(closed_start);
  QList<BrowserView*> restored_tabs;
  for (const QString& url : initial_urls) {
    restored_tabs.append(AddTab(url, false));
  }
  const int initial_active =
      std::clamp(initial_session.active_tab, 0,
                 static_cast<int>(restored_tabs.size()) - 1);
  BrowserView* active_restored_tab = restored_tabs.at(initial_active);
  for (int index = 0; index < restored_tabs.size(); ++index) {
    if (index < initial_session.tab_pinned.size() &&
        initial_session.tab_pinned.at(index)) {
      SetTabPinned(IndexOf(restored_tabs.at(index)), true);
    }
  }
  tab_bar_->setCurrentIndex(IndexOf(active_restored_tab));
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
  RefreshAddressSuggestions();

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
  delete browser_settings_;
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

void MainWindow::HandleExternalOpenRequest(const QString& url) {
  if (!url.trimmed().isEmpty()) {
    const auto normalized = NormalizeUrl(url);
    if (normalized) {
      AddTab(*normalized, true);
    } else {
      statusBar()->showMessage(
          QStringLiteral("Blocked an unsupported external URL"), 5000);
    }
  }
  if (isMinimized()) showNormal();
  show();
  raise();
  activateWindow();
}

void MainWindow::CloseCurrentTabForTesting() {
  CloseTab(tab_bar_->currentIndex());
}

void MainWindow::ReopenClosedTabForTesting() {
  ReopenClosedTab();
}

void MainWindow::ActivateTabForTesting(int index) {
  if (index >= 0 && index < tab_bar_->count()) {
    tab_bar_->setCurrentIndex(index);
  }
}

void MainWindow::DuplicateCurrentTabForTesting() {
  DuplicateTab(tab_bar_->currentIndex());
}

void MainWindow::CloseOtherTabsForTesting() {
  CloseOtherTabs(tab_bar_->currentIndex());
}

void MainWindow::CloseTabsToRightForTesting() {
  CloseTabsToRight(tab_bar_->currentIndex());
}

void MainWindow::ToggleCurrentTabPinnedForTesting() {
  BrowserView* browser = CurrentBrowser();
  if (browser) SetTabPinned(IndexOf(browser), !IsTabPinned(browser));
}

bool MainWindow::current_tab_pinned_for_testing() const {
  return IsTabPinned(CurrentBrowser());
}

int MainWindow::pinned_tab_count_for_testing() const {
  return PinnedTabCount();
}

void MainWindow::MoveCurrentTabForTesting(int to) {
  const int from = tab_bar_->currentIndex();
  if (from >= 0 && to >= 0 && to < tab_bar_->count()) {
    tab_bar_->moveTab(from, to);
  }
}

int MainWindow::current_tab_index_for_testing() const {
  return tab_bar_->currentIndex();
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

bool MainWindow::RemoveDownloadForTesting(quint32 id) {
  return download_manager_->RemoveDownload(id);
}

bool MainWindow::ClearFinishedDownloadsForTesting() {
  return download_manager_->ClearFinished();
}

void MainWindow::SetCurrentFaviconForTesting() {
  BrowserView* browser = CurrentBrowser();
  if (!browser) return;
  QPixmap image(16, 16);
  image.fill(Qt::darkCyan);
  browser->SetFaviconForTesting(QIcon(image));
}

bool MainWindow::current_tab_has_favicon_for_testing() const {
  return !tab_bar_->tabIcon(tab_bar_->currentIndex()).isNull();
}

void MainWindow::SetCurrentAudioStateForTesting(bool playing, bool muted) {
  if (BrowserView* browser = CurrentBrowser()) {
    browser->SetAudioStateForTesting(playing, muted);
  }
}

void MainWindow::ToggleCurrentAudioMutedForTesting() {
  if (BrowserView* browser = CurrentBrowser()) browser->ToggleAudioMuted();
}

bool MainWindow::current_audio_muted_for_testing() const {
  BrowserView* browser = CurrentBrowser();
  return browser && browser->audio_muted();
}

QString MainWindow::current_tab_text_for_testing() const {
  return tab_bar_->tabText(tab_bar_->currentIndex());
}

void MainWindow::ShowDownloadsForTesting() {
  ShowBrowserUiSurface(BrowserUiSurface::Downloads);
}

void MainWindow::ShowBookmarksForTesting() {
  ShowBrowserUiSurface(BrowserUiSurface::Bookmarks);
}

void MainWindow::ShowHistoryForTesting() {
  ShowBrowserUiSurface(BrowserUiSurface::History);
}

void MainWindow::ShowClearBrowsingDataForTesting() {
  ShowBrowserUiSurface(BrowserUiSurface::ClearData);
}

void MainWindow::HideBrowserSurfacesForTesting() {
  download_panel_->hide();
  bookmarks_menu_->close();
  history_menu_->close();
  all_tabs_menu_->close();
}

bool MainWindow::downloads_visible_for_testing() const {
  return download_panel_->isVisible();
}

bool MainWindow::bookmarks_visible_for_testing() const {
  return bookmarks_menu_->isVisible();
}

bool MainWindow::history_visible_for_testing() const {
  return history_menu_->isVisible();
}

bool MainWindow::clear_data_prompt_visible_for_testing() const {
  for (QDialog* dialog : findChildren<QDialog*>()) {
    if (dialog &&
        dialog->windowTitle() == QStringLiteral("Clear browsing data?") &&
        dialog->isVisible()) {
      return true;
    }
  }
  return false;
}

QStringList MainWindow::clear_data_options_for_testing() const {
  for (QDialog* dialog : findChildren<QDialog*>()) {
    if (!dialog ||
        dialog->windowTitle() != QStringLiteral("Clear browsing data?")) {
      continue;
    }
    QStringList options;
    for (QCheckBox* choice : dialog->findChildren<QCheckBox*>()) {
      options.append(choice->text());
    }
    return options;
  }
  return {};
}

bool MainWindow::SetAllClearDataOptionsForTesting(bool checked) {
  for (QDialog* dialog : findChildren<QDialog*>()) {
    if (!dialog ||
        dialog->windowTitle() != QStringLiteral("Clear browsing data?")) {
      continue;
    }
    const QList<QCheckBox*> choices = dialog->findChildren<QCheckBox*>();
    for (QCheckBox* choice : choices) choice->setChecked(checked);
    return !choices.isEmpty();
  }
  return false;
}

bool MainWindow::clear_data_submit_enabled_for_testing() const {
  if (QPushButton* submit =
          findChild<QPushButton*>(QStringLiteral("clearSelectedData"))) {
    return submit->isEnabled();
  }
  return false;
}

void MainWindow::ShowAllTabsForTesting() {
  ShowBrowserUiSurface(BrowserUiSurface::AllTabs);
}

bool MainWindow::all_tabs_visible_for_testing() const {
  return all_tabs_menu_->isVisible();
}

int MainWindow::all_tabs_action_count_for_testing() const {
  return all_tabs_menu_->actions().size();
}

int MainWindow::recently_closed_tab_count_for_testing() const {
  return closed_tabs_.size();
}

QStringList MainWindow::recently_closed_tabs_for_testing() const {
  QStringList urls;
  for (const RecentlyClosedTab& tab : closed_tabs_) urls.append(tab.url);
  return urls;
}

QStringList MainWindow::recently_closed_menu_labels_for_testing() {
  RebuildAllTabsMenu();
  QStringList labels;
  const QList<QAction*> actions = all_tabs_menu_->actions();
  const int first_recent_action = tab_bar_->count() + 1;
  for (int index = first_recent_action; index < actions.size(); ++index) {
    labels.append(actions.at(index)->text());
  }
  return labels;
}

bool MainWindow::TriggerRecentlyClosedForTesting(int recent_index) {
  RebuildAllTabsMenu();
  const int action_index = tab_bar_->count() + 1 + recent_index;
  const QList<QAction*> actions = all_tabs_menu_->actions();
  if (action_index < 0 || action_index >= actions.size()) return false;
  actions.at(action_index)->trigger();
  return true;
}

QStringList MainWindow::application_menu_titles_for_testing() const {
  QStringList titles;
  for (QAction* action : menuBar()->actions()) titles.append(action->text());
  return titles;
}

QStringList MainWindow::application_menu_actions_for_testing(
    const QString& menu_title) const {
  for (QAction* menu_action : menuBar()->actions()) {
    if (menu_action->text() != menu_title || !menu_action->menu()) continue;
    QStringList actions;
    for (QAction* action : menu_action->menu()->actions()) {
      if (!action->isSeparator()) actions.append(action->text());
    }
    return actions;
  }
  return {};
}

QString MainWindow::application_menu_shortcut_for_testing(
    const QString& menu_title, const QString& action_text) const {
  QAction* action = FindApplicationMenuAction(menu_title, action_text);
  return action ? action->shortcut().toString(QKeySequence::PortableText)
                : QString();
}

bool MainWindow::TriggerApplicationMenuActionForTesting(
    const QString& menu_title, const QString& action_text) {
  QAction* action = FindApplicationMenuAction(menu_title, action_text);
  if (!action || !action->isEnabled()) return false;
  action->trigger();
  return true;
}

QString MainWindow::search_engine_for_testing() const {
  return BrowserSettings::SearchEngineName(browser_settings_->search_engine());
}

bool MainWindow::SelectSearchEngineForTesting(const QString& name) {
  const BrowserSettings::SearchEngine engines[] = {
      BrowserSettings::SearchEngine::Google,
      BrowserSettings::SearchEngine::DuckDuckGo,
      BrowserSettings::SearchEngine::Bing};
  for (BrowserSettings::SearchEngine engine : engines) {
    if (BrowserSettings::SearchEngineName(engine) == name) {
      SetSearchEngine(static_cast<int>(engine));
      return browser_settings_->search_engine() == engine;
    }
  }
  return false;
}

std::optional<QString> MainWindow::NormalizeUrlForTesting(
    const QString& input) const {
  return NormalizeUrl(input);
}

bool MainWindow::OpenPopupForTesting(const QString& url, int disposition) {
  return OpenPopup(CurrentBrowser(), url, disposition);
}

QString MainWindow::home_page_for_testing() const {
  return browser_settings_->home_page();
}

bool MainWindow::SetHomePageForTesting(const QString& value) {
  return SetHomePage(value);
}

void MainWindow::GoHomeForTesting() {
  GoHome();
}

bool MainWindow::SetOpenHomeOnNewTabForTesting(bool enabled) {
  return SetOpenHomeOnNewTab(enabled);
}

bool MainWindow::open_home_on_new_tab_for_testing() const {
  return browser_settings_->open_home_on_new_tab();
}

bool MainWindow::SetStartupBehaviorForTesting(const QString& name) {
  const BrowserSettings::StartupBehavior behaviors[] = {
      BrowserSettings::StartupBehavior::RestoreSession,
      BrowserSettings::StartupBehavior::HomePage,
      BrowserSettings::StartupBehavior::BlankPage};
  for (BrowserSettings::StartupBehavior behavior : behaviors) {
    if (BrowserSettings::StartupBehaviorName(behavior) == name) {
      return SetStartupBehavior(static_cast<int>(behavior));
    }
  }
  return false;
}

QString MainWindow::startup_behavior_for_testing() const {
  return BrowserSettings::StartupBehaviorName(
      browser_settings_->startup_behavior());
}

void MainWindow::ActivateTabShortcutForTesting(int number) {
  ActivateTabByShortcut(number == 9 ? tab_bar_->count() - 1 : number - 1);
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

QStringList MainWindow::address_suggestions_for_testing() const {
  return address_suggestion_url_order_;
}

QStringList MainWindow::address_suggestion_labels_for_testing() const {
  return address_suggestions_->stringList();
}

void MainWindow::NavigateAddressSuggestionForTesting(const QString& label) {
  NavigateFromAddressSuggestion(label);
}

void MainWindow::AddHistoryForTesting(const QString& url,
                                      const QString& title) {
  browsing_data_->RecordVisit(url, title);
  SaveBrowsingData();
  RefreshAddressSuggestions();
}

bool MainWindow::RenameBookmarkForTesting(const QString& url,
                                          const QString& title) {
  return RenameBookmark(url, title);
}

QString MainWindow::bookmark_title_for_testing(const QString& url) const {
  for (const BrowsingDataStore::Bookmark& bookmark :
       browsing_data_->bookmarks()) {
    if (bookmark.url == url) return bookmark.title;
  }
  return {};
}

QString MainWindow::bookmark_label_for_testing(const QString& url) {
  RebuildBookmarksMenu();
  for (QAction* action : bookmarks_menu_->actions()) {
    if (action->property("bookmarkUrl").toString() == url) {
      return action->text();
    }
  }
  return {};
}

bool MainWindow::RemoveBookmarkForTesting(const QString& url) {
  return RemoveBookmark(url);
}

bool MainWindow::RemoveHistoryForTesting(const QString& url) {
  return RemoveHistory(url);
}

void MainWindow::ClearBrowsingDataForTesting() {
  BeginClearBrowsingData(
      false, BrowsingDataSelection{true, true, true, true});
}

void MainWindow::ClearBrowsingDataForTesting(bool history,
                                             bool recently_closed,
                                             bool downloads,
                                             bool site_data) {
  BeginClearBrowsingData(
      false,
      BrowsingDataSelection{history, recently_closed, downloads, site_data});
}

void MainWindow::DismissClearBrowsingDataForTesting() {
  for (QDialog* dialog : findChildren<QDialog*>()) {
    if (dialog &&
        dialog->windowTitle() == QStringLiteral("Clear browsing data?")) {
      dialog->reject();
    }
  }
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

std::optional<QString> MainWindow::normalize_external_url_for_testing(
    const QString& url) const {
  return BrowserView::NormalizeExternalUrl(url);
}

bool MainWindow::ShowAuthForTesting(CefRefPtr<CefAuthCallback> callback) {
  BrowserView* browser = CurrentBrowser();
  return browser && browser->ShowAuthForTesting(std::move(callback));
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
  if (download_manager_->active_count() > 0) {
    if (download_exit_prompt_open_) return;
    download_exit_prompt_open_ = true;

    const int active_downloads = download_manager_->active_count();
    auto* dialog = new QMessageBox(
        QMessageBox::Warning, QStringLiteral("Downloads in progress"),
        QStringLiteral(
            "%1 download(s) are still active. Keep the browser open to "
            "allow them to finish.")
            .arg(active_downloads),
        QMessageBox::NoButton, this);
    auto* keep_downloading = dialog->addButton(
        QStringLiteral("Continue downloading"), QMessageBox::RejectRole);
    auto* quit_and_cancel = dialog->addButton(
        QStringLiteral("Quit and cancel downloads"),
        QMessageBox::DestructiveRole);
    dialog->setDefaultButton(keep_downloading);
    dialog->setEscapeButton(keep_downloading);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QMessageBox::finished, this,
            [this, dialog, quit_and_cancel](int) {
              download_exit_prompt_open_ = false;
              if (dialog->clickedButton() != quit_and_cancel) return;
              download_manager_->CancelAllActive();
              queued_tab_closes_.clear();
              active_queued_tab_close_.clear();
              window_close_requested_ = true;
              closing_session_ = CaptureSession(true);
              ContinueWindowClose();
            });
    dialog->open();
    return;
  }

  queued_tab_closes_.clear();
  active_queued_tab_close_.clear();
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
    const QString entered = address_bar_->text();
    const QString suggested_url = address_suggestion_urls_.value(entered);
    const auto url = NormalizeUrl(suggested_url.isEmpty() ? entered
                                                           : suggested_url);
    if (!url) {
      statusBar()->showMessage(
          QStringLiteral("This URL scheme is not supported"), 5000);
      address_bar_->selectAll();
      return;
    }
    address_bar_->setText(*url == QStringLiteral("about:blank") ? QString()
                                                                   : *url);
    browser->LoadUrl(*url);
  }
}

void MainWindow::NavigateFromAddressSuggestion(const QString& label) {
  const QString url = address_suggestion_urls_.value(label);
  if (url.isEmpty()) return;
  address_bar_->setText(url);
  if (BrowserView* browser = CurrentBrowser()) browser->LoadUrl(url);
}

void MainWindow::AddBlankTab() {
  const bool use_home = browser_settings_->open_home_on_new_tab();
  AddTab(use_home ? browser_settings_->home_page()
                  : QStringLiteral("about:blank"),
         true, !use_home);
}

void MainWindow::ReopenClosedTab() {
  ReopenClosedTabAt(0);
}

bool MainWindow::ReopenClosedTabAt(int recent_index) {
  const int stored_index = closed_tabs_.size() - 1 - recent_index;
  if (stored_index < 0 || stored_index >= closed_tabs_.size()) return false;
  const RecentlyClosedTab tab = closed_tabs_.takeAt(stored_index);
  AddTab(tab.url, true);
  return true;
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
  if (back_action_) back_action_->setEnabled(can_go_back);
  if (forward_action_) forward_action_->setEnabled(can_go_forward);
  reload_button_->setText(loading ? QStringLiteral("×")
                                  : QStringLiteral("↻"));
  reload_button_->setToolTip(loading ? QStringLiteral("Stop")
                                     : QStringLiteral("Reload"));
  if (loading) {
    if (loading_progress_->value() >= loading_progress_->maximum()) {
      loading_progress_->setValue(0);
    }
    loading_progress_->show();
  } else {
    loading_progress_->setValue(loading_progress_->maximum());
    loading_progress_->hide();
  }
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
  connect(browser, &BrowserView::FaviconChanged, this,
          [this, browser](const QIcon& icon) {
            const int index = IndexOf(browser);
            if (index >= 0) tab_bar_->setTabIcon(index, icon);
          });
  connect(browser, &BrowserView::AudioStateChanged, this,
          [this, browser](bool, bool) {
            UpdateTabTitle(browser, browser->page_title());
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
  connect(browser, &BrowserView::LoadingProgressChanged, this,
          [this, browser](double progress) {
            if (browser != CurrentBrowser()) return;
            const int value = std::clamp(static_cast<int>(progress * 1000),
                                         0, 1000);
            loading_progress_->setValue(value);
            loading_progress_->setVisible(value < 1000 && browser->is_loading());
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
            } else if (pending_browser_ui_surface_) {
              const BrowserUiSurface surface = *pending_browser_ui_surface_;
              pending_browser_ui_surface_.reset();
              QMetaObject::invokeMethod(
                  this, [this, surface] { PerformBrowserUiSurface(surface); },
                  Qt::QueuedConnection);
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

void MainWindow::ShowTabContextMenu(const QPoint& position) {
  const int index = tab_bar_->tabAt(position);
  auto* browser = index >= 0
                      ? qvariant_cast<BrowserView*>(tab_bar_->tabData(index))
                      : nullptr;
  if (!browser || closing_tabs_.contains(browser)) return;
  const QPointer<BrowserView> target(browser);

  auto* menu = new QMenu(tab_bar_);
  menu->setAttribute(Qt::WA_DeleteOnClose);
  QAction* new_tab = menu->addAction(QStringLiteral("New tab"));
  QAction* duplicate = menu->addAction(QStringLiteral("Duplicate tab"));
  QAction* copy_address =
      menu->addAction(QStringLiteral("Copy page address"));
  QAction* mute = menu->addAction(browser->audio_muted()
                                      ? QStringLiteral("Unmute tab")
                                      : QStringLiteral("Mute tab"));
  QAction* pin = menu->addAction(IsTabPinned(browser)
                                     ? QStringLiteral("Unpin tab")
                                     : QStringLiteral("Pin tab"));
  menu->addSeparator();
  QAction* close_tab = menu->addAction(QStringLiteral("Close tab"));
  QAction* close_others =
      menu->addAction(QStringLiteral("Close other tabs"));
  QAction* close_right =
      menu->addAction(QStringLiteral("Close tabs to the right"));
  close_others->setEnabled(tab_bar_->count() > 1);
  close_right->setEnabled(index + 1 < tab_bar_->count());

  connect(new_tab, &QAction::triggered, this, &MainWindow::AddBlankTab);
  connect(duplicate, &QAction::triggered, this,
          [this, target] {
            if (target) DuplicateTab(IndexOf(target));
          });
  connect(copy_address, &QAction::triggered, this, [target] {
    if (target) QApplication::clipboard()->setText(target->current_url());
  });
  connect(mute, &QAction::triggered, this, [target] {
    if (target) target->ToggleAudioMuted();
  });
  connect(pin, &QAction::triggered, this, [this, target] {
    if (target) SetTabPinned(IndexOf(target), !IsTabPinned(target));
  });
  connect(close_tab, &QAction::triggered, this,
          [this, target] {
            if (target) CloseTab(IndexOf(target));
          });
  connect(close_others, &QAction::triggered, this,
          [this, target] {
            if (target) CloseOtherTabs(IndexOf(target));
          });
  connect(close_right, &QAction::triggered, this,
          [this, target] {
            if (target) CloseTabsToRight(IndexOf(target));
          });
  menu->popup(tab_bar_->mapToGlobal(position));
}

void MainWindow::DuplicateTab(int index) {
  auto* source = index >= 0
                     ? qvariant_cast<BrowserView*>(tab_bar_->tabData(index))
                     : nullptr;
  if (!source || closing_tabs_.contains(source)) return;
  const QString url = source->current_url().isEmpty()
                          ? QStringLiteral("about:blank")
                          : source->current_url();
  BrowserView* duplicate = AddTab(url, true);
  const int duplicate_index = IndexOf(duplicate);
  const int target_index = std::max(index + 1, PinnedTabCount());
  if (duplicate_index >= 0 && duplicate_index != target_index) {
    tab_bar_->moveTab(duplicate_index, target_index);
  }
}

void MainWindow::SetTabPinned(int index, bool pinned) {
  auto* browser = index >= 0
                      ? qvariant_cast<BrowserView*>(tab_bar_->tabData(index))
                      : nullptr;
  if (!browser || closing_tabs_.contains(browser) ||
      IsTabPinned(browser) == pinned) {
    return;
  }

  if (pinned) {
    pinned_tabs_.insert(browser);
    const int target = PinnedTabCount() - 1;
    if (index != target) tab_bar_->moveTab(index, target);
  } else {
    pinned_tabs_.remove(browser);
    const int target = PinnedTabCount();
    const int current = IndexOf(browser);
    if (current != target) tab_bar_->moveTab(current, target);
  }
  UpdateTabTitle(browser, browser->page_title());
  ScheduleSessionSave();
}

bool MainWindow::IsTabPinned(BrowserView* browser) const {
  return browser && pinned_tabs_.contains(browser);
}

int MainWindow::PinnedTabCount() const {
  return pinned_tabs_.size();
}

void MainWindow::ConstrainMovedTab(int, int to) {
  if (constraining_tab_move_ || to < 0 || to >= tab_bar_->count()) return;
  auto* browser = qvariant_cast<BrowserView*>(tab_bar_->tabData(to));
  if (!browser) return;
  const int pinned_count = PinnedTabCount();
  int target = to;
  if (IsTabPinned(browser) && to >= pinned_count) {
    target = std::max(0, pinned_count - 1);
  } else if (!IsTabPinned(browser) && to < pinned_count) {
    target = pinned_count;
  }
  if (target == to) return;
  constraining_tab_move_ = true;
  tab_bar_->moveTab(to, target);
  constraining_tab_move_ = false;
}

void MainWindow::CloseOtherTabs(int index) {
  if (index < 0 || index >= tab_bar_->count()) return;
  QList<BrowserView*> targets;
  for (int candidate = tab_bar_->count() - 1; candidate >= 0; --candidate) {
    if (candidate == index) continue;
    if (auto* browser = qvariant_cast<BrowserView*>(
            tab_bar_->tabData(candidate))) {
      if (!IsTabPinned(browser)) targets.append(browser);
    }
  }
  QueueTabCloses(targets);
}

void MainWindow::CloseTabsToRight(int index) {
  if (index < 0 || index >= tab_bar_->count()) return;
  QList<BrowserView*> targets;
  for (int candidate = tab_bar_->count() - 1; candidate > index; --candidate) {
    if (auto* browser = qvariant_cast<BrowserView*>(
            tab_bar_->tabData(candidate))) {
      if (!IsTabPinned(browser)) targets.append(browser);
    }
  }
  QueueTabCloses(targets);
}

void MainWindow::QueueTabCloses(const QList<BrowserView*>& browsers) {
  if (window_close_requested_) return;
  for (BrowserView* browser : browsers) {
    if (!browser || closing_tabs_.contains(browser) ||
        active_queued_tab_close_ == browser) {
      continue;
    }
    const bool already_queued = std::any_of(
        queued_tab_closes_.cbegin(), queued_tab_closes_.cend(),
        [browser](const QPointer<BrowserView>& queued) {
          return queued == browser;
        });
    if (!already_queued) queued_tab_closes_.append(browser);
  }
  ContinueQueuedTabCloses();
}

void MainWindow::ContinueQueuedTabCloses() {
  if (window_close_requested_ || active_queued_tab_close_) return;
  while (!queued_tab_closes_.isEmpty()) {
    QPointer<BrowserView> next = queued_tab_closes_.takeFirst();
    if (!next || IndexOf(next) < 0 || closing_tabs_.contains(next)) continue;
    active_queued_tab_close_ = next;
    BeginTabClose(next, true);
    return;
  }
}

bool MainWindow::OpenPopup(BrowserView* source, const QString& url,
                           int disposition_value) {
  if (!source || IndexOf(source) < 0 || closing_tabs_.contains(source)) {
    return false;
  }
  const auto normalized = BrowserSettings::NormalizeStoredUrl(
      url.trimmed().isEmpty() ? QStringLiteral("about:blank") : url);
  if (!normalized ||
      (*normalized != QStringLiteral("about:blank") &&
       QUrl(*normalized).scheme() != QStringLiteral("http") &&
       QUrl(*normalized).scheme() != QStringLiteral("https"))) {
    statusBar()->showMessage(QStringLiteral("Blocked an unsafe pop-up URL"),
                             5000);
    return false;
  }
  const QString& target = *normalized;
  const auto disposition =
      static_cast<cef_window_open_disposition_t>(disposition_value);

  if (disposition == CEF_WOD_CURRENT_TAB) {
    source->LoadUrl(target);
    return true;
  }
  if (disposition == CEF_WOD_NEW_BACKGROUND_TAB) {
    AddTab(target, false);
    return true;
  }
  if (disposition == CEF_WOD_SINGLETON_TAB ||
      disposition == CEF_WOD_SWITCH_TO_TAB) {
    for (int index = 0; index < tab_bar_->count(); ++index) {
      auto* candidate =
          qvariant_cast<BrowserView*>(tab_bar_->tabData(index));
      if (candidate && candidate->current_url() == target) {
        tab_bar_->setCurrentIndex(index);
        return true;
      }
    }
  }
  switch (disposition) {
    case CEF_WOD_SINGLETON_TAB:
    case CEF_WOD_NEW_FOREGROUND_TAB:
    case CEF_WOD_NEW_POPUP:
    case CEF_WOD_NEW_WINDOW:
    case CEF_WOD_OFF_THE_RECORD:
    case CEF_WOD_SWITCH_TO_TAB:
    case CEF_WOD_NEW_SPLIT_VIEW:
      AddTab(target, true);
      return true;
    case CEF_WOD_UNKNOWN:
    case CEF_WOD_CURRENT_TAB:
    case CEF_WOD_NEW_BACKGROUND_TAB:
    case CEF_WOD_SAVE_TO_DISK:
    case CEF_WOD_IGNORE_ACTION:
    case CEF_WOD_NEW_PICTURE_IN_PICTURE:
    case CEF_WOD_NUM_VALUES:
      statusBar()->showMessage(
          QStringLiteral("Blocked an unsupported pop-up action"), 5000);
      return false;
  }
  return false;
}

void MainWindow::BeginTabClose(BrowserView* browser, bool remember_url) {
  if (!browser || closing_tabs_.contains(browser)) return;
  closing_tabs_.insert(browser);
  if (remember_url && !forgotten_closing_tabs_.contains(browser) &&
      !browser->current_url().isEmpty()) {
    pending_closed_tabs_.insert(
        browser, RecentlyClosedTab{browser->current_url(),
                                   browser->page_title().trimmed()});
  }
  const int index = IndexOf(browser);
  if (index >= 0) tab_bar_->setTabEnabled(index, false);

  if (browser->RequestClose()) CompleteTabClose(browser);
}

void MainWindow::CompleteTabClose(BrowserView* browser) {
  if (!browser || !closing_tabs_.remove(browser)) return;
  const bool queued_close = active_queued_tab_close_ == browser;
  browser->FinalizeClose();
  pinned_tabs_.remove(browser);

  const bool forget_closed_tab = forgotten_closing_tabs_.remove(browser);
  if (!forget_closed_tab && pending_closed_tabs_.contains(browser)) {
    closed_tabs_.append(pending_closed_tabs_.take(browser));
    while (closed_tabs_.size() > kMaxClosedTabs) closed_tabs_.removeFirst();
  }
  pending_closed_tabs_.remove(browser);

  const int index = IndexOf(browser);
  if (index >= 0) {
    tab_stack_->removeWidget(browser);
    tab_bar_->removeTab(index);
  }
  browser->deleteLater();
  if (queued_close) active_queued_tab_close_.clear();

  if (window_close_requested_) {
    ContinueWindowClose();
  } else {
    UpdateChrome();
    ScheduleSessionSave();
    if (queued_close) {
      QMetaObject::invokeMethod(
          this, &MainWindow::ContinueQueuedTabCloses, Qt::QueuedConnection);
    }
  }
}

void MainWindow::CancelTabClose(BrowserView* browser) {
  if (!browser || !closing_tabs_.remove(browser)) return;
  const bool queued_close = active_queued_tab_close_ == browser;
  if (queued_close) active_queued_tab_close_.clear();
  pending_closed_tabs_.remove(browser);
  forgotten_closing_tabs_.remove(browser);
  const int index = IndexOf(browser);
  if (index >= 0) {
    tab_bar_->setTabEnabled(index, true);
    tab_bar_->setCurrentIndex(index);
  }
  window_close_requested_ = false;
  closing_session_.reset();
  UpdateChrome();
  ScheduleSessionSave();
  if (queued_close) {
    QMetaObject::invokeMethod(
        this, &MainWindow::ContinueQueuedTabCloses, Qt::QueuedConnection);
  }
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
  if (close_tab_action_) close_tab_action_->setEnabled(available);
  if (reopen_closed_tab_action_) {
    reopen_closed_tab_action_->setEnabled(!closed_tabs_.isEmpty());
  }
  if (back_action_) back_action_->setEnabled(available && browser->can_go_back());
  if (forward_action_) {
    forward_action_->setEnabled(available && browser->can_go_forward());
  }
  if (reload_action_) reload_action_->setEnabled(available);
  if (toggle_bookmark_action_) {
    toggle_bookmark_action_->setEnabled(
        available && !browser->current_url().isEmpty() &&
        browser->current_url() != QStringLiteral("about:blank"));
  }

  if (!browser) {
    address_bar_->clear();
    setWindowTitle(QStringLiteral("Trail Browser"));
    return;
  }

  const QString url = browser->current_url();
  bookmark_button_->setText(browsing_data_->IsBookmarked(url)
                                ? QStringLiteral("★")
                                : QStringLiteral("☆"));
  if (toggle_bookmark_action_) {
    toggle_bookmark_action_->setText(
        browsing_data_->IsBookmarked(url)
            ? QStringLiteral("Remove Bookmark for This Page")
            : QStringLiteral("Bookmark This Page"));
  }
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
  QString tab_text = TabText(title, browser->current_url());
  if (IsTabPinned(browser)) tab_text.prepend(QStringLiteral("\U0001F4CC "));
  if (browser->audio_muted()) {
    tab_text.prepend(QStringLiteral("\U0001F507 "));
  } else if (browser->audio_playing()) {
    tab_text.prepend(QStringLiteral("\U0001F50A "));
  }
  tab_bar_->setTabText(index, tab_text);
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
    case BrowserView::ShortcutAction::ShowDownloads:
      ShowBrowserUiSurface(BrowserUiSurface::Downloads);
      break;
    case BrowserView::ShortcutAction::ShowBookmarks:
      ShowBrowserUiSurface(BrowserUiSurface::Bookmarks);
      break;
    case BrowserView::ShortcutAction::ShowHistory:
      ShowBrowserUiSurface(BrowserUiSurface::History);
      break;
    case BrowserView::ShortcutAction::ShowAllTabs:
      ShowBrowserUiSurface(BrowserUiSurface::AllTabs);
      break;
    case BrowserView::ShortcutAction::ActivateTab1:
    case BrowserView::ShortcutAction::ActivateTab2:
    case BrowserView::ShortcutAction::ActivateTab3:
    case BrowserView::ShortcutAction::ActivateTab4:
    case BrowserView::ShortcutAction::ActivateTab5:
    case BrowserView::ShortcutAction::ActivateTab6:
    case BrowserView::ShortcutAction::ActivateTab7:
    case BrowserView::ShortcutAction::ActivateTab8:
      ActivateTabByShortcut(
          static_cast<int>(action) -
          static_cast<int>(BrowserView::ShortcutAction::ActivateTab1));
      break;
    case BrowserView::ShortcutAction::ActivateLastTab:
      ActivateTabByShortcut(tab_bar_->count() - 1);
      break;
    case BrowserView::ShortcutAction::ClearBrowsingData:
      ShowBrowserUiSurface(BrowserUiSurface::ClearData);
      break;
    case BrowserView::ShortcutAction::ExitFullscreen:
      if (web_fullscreen_) {
        if (BrowserView* browser = CurrentBrowser()) browser->ExitFullscreen();
      }
      break;
    case BrowserView::ShortcutAction::GoBack:
      if (BrowserView* browser = CurrentBrowser()) browser->GoBack();
      break;
    case BrowserView::ShortcutAction::GoForward:
      if (BrowserView* browser = CurrentBrowser()) browser->GoForward();
      break;
    case BrowserView::ShortcutAction::Reload:
      if (BrowserView* browser = CurrentBrowser()) browser->Reload();
      break;
    case BrowserView::ShortcutAction::GoHome:
      GoHome();
      break;
  }
}

void MainWindow::ActivateTabByShortcut(int index) {
  if (index >= 0 && index < tab_bar_->count()) {
    tab_bar_->setCurrentIndex(index);
  }
}

QAction* MainWindow::FindApplicationMenuAction(
    const QString& menu_title, const QString& action_text) const {
  for (QAction* menu_action : menuBar()->actions()) {
    if (menu_action->text() != menu_title || !menu_action->menu()) continue;
    for (QAction* action : menu_action->menu()->actions()) {
      if (action->text() == action_text) return action;
    }
  }
  return nullptr;
}

void MainWindow::CreateApplicationMenus() {
  const auto add_action = [this](QMenu* menu, const QString& text,
                                 const QKeySequence& shortcut,
                                 std::function<void()> callback) {
    QAction* action = menu->addAction(text);
    if (!shortcut.isEmpty()) action->setShortcut(shortcut);
    connect(action, &QAction::triggered, this, std::move(callback));
    return action;
  };
  const auto edit_target = [this](auto line_edit_action,
                                  auto browser_action) {
    if (auto* edit = qobject_cast<QLineEdit*>(QApplication::focusWidget())) {
      (edit->*line_edit_action)();
    } else if (BrowserView* browser = CurrentBrowser()) {
      (browser->*browser_action)();
    }
  };

  QMenu* file = menuBar()->addMenu(QStringLiteral("File"));
  add_action(file, QStringLiteral("New Tab"), QKeySequence::AddTab,
             [this] { AddBlankTab(); });
  close_tab_action_ = add_action(
      file, QStringLiteral("Close Tab"), QKeySequence::Close,
      [this] { CloseTab(tab_bar_->currentIndex()); });
  file->addSeparator();
  add_action(file, QStringLiteral("Print…"), QKeySequence::Print, [this] {
    if (BrowserView* browser = CurrentBrowser()) browser->Print();
  });
  file->addSeparator();
  QAction* quit = add_action(file, QStringLiteral("Quit Trail Browser"),
                             QKeySequence::Quit, [this] { close(); });
  quit->setMenuRole(QAction::QuitRole);

  QMenu* edit = menuBar()->addMenu(QStringLiteral("Edit"));
  add_action(edit, QStringLiteral("Undo"), QKeySequence::Undo,
             [edit_target] {
               edit_target(&QLineEdit::undo, &BrowserView::Undo);
             });
  add_action(edit, QStringLiteral("Redo"), QKeySequence::Redo,
             [edit_target] {
               edit_target(&QLineEdit::redo, &BrowserView::Redo);
             });
  edit->addSeparator();
  add_action(edit, QStringLiteral("Cut"), QKeySequence::Cut,
             [edit_target] {
               edit_target(&QLineEdit::cut, &BrowserView::Cut);
             });
  add_action(edit, QStringLiteral("Copy"), QKeySequence::Copy,
             [edit_target] {
               edit_target(&QLineEdit::copy, &BrowserView::Copy);
             });
  add_action(edit, QStringLiteral("Paste"), QKeySequence::Paste,
             [edit_target] {
               edit_target(&QLineEdit::paste, &BrowserView::Paste);
             });
  add_action(edit, QStringLiteral("Select All"), QKeySequence::SelectAll,
             [edit_target] {
               edit_target(&QLineEdit::selectAll, &BrowserView::SelectAll);
             });
  edit->addSeparator();
  add_action(edit, QStringLiteral("Find in Page…"), QKeySequence::Find,
             [this] { ShowFindBar(); });
  add_action(edit, QStringLiteral("Find Next"), QKeySequence::FindNext,
             [this] { FindFromBar(true, true); });
  add_action(edit, QStringLiteral("Find Previous"),
             QKeySequence::FindPrevious,
             [this] { FindFromBar(false, true); });

  QMenu* view = menuBar()->addMenu(QStringLiteral("View"));
  add_action(view, QStringLiteral("Focus Address Bar"),
             PrimaryShortcut(QStringLiteral("L")), [this] {
               address_bar_->setFocus();
               address_bar_->selectAll();
             });
  add_action(view, QStringLiteral("Home"),
#if defined(OS_MAC)
             QKeySequence(QStringLiteral("Meta+Shift+H")),
#else
             QKeySequence(QStringLiteral("Alt+Home")),
#endif
             [this] { GoHome(); });
  view->addSeparator();
  back_action_ = add_action(view, QStringLiteral("Back"), QKeySequence::Back,
                            [this] {
                              if (BrowserView* browser = CurrentBrowser())
                                browser->GoBack();
                            });
  forward_action_ = add_action(
      view, QStringLiteral("Forward"), QKeySequence::Forward, [this] {
        if (BrowserView* browser = CurrentBrowser()) browser->GoForward();
      });
  reload_action_ = add_action(
      view, QStringLiteral("Reload"), QKeySequence::Refresh, [this] {
        if (BrowserView* browser = CurrentBrowser()) browser->Reload();
      });
  view->addSeparator();
  add_action(view, QStringLiteral("Zoom In"), QKeySequence::ZoomIn, [this] {
    if (BrowserView* browser = CurrentBrowser()) browser->ZoomIn();
  });
  add_action(view, QStringLiteral("Zoom Out"), QKeySequence::ZoomOut,
             [this] {
               if (BrowserView* browser = CurrentBrowser()) browser->ZoomOut();
             });
  add_action(view, QStringLiteral("Actual Size"),
             PrimaryShortcut(QStringLiteral("0")), [this] {
               if (BrowserView* browser = CurrentBrowser())
                 browser->ResetZoom();
             });
  view->addSeparator();
  add_action(view, QStringLiteral("Developer Tools"),
             QKeySequence(Qt::Key_F12), [this] {
               if (BrowserView* browser = CurrentBrowser())
                 browser->ShowDevTools();
             });

  QMenu* history = menuBar()->addMenu(QStringLiteral("History"));
  reopen_closed_tab_action_ = add_action(
      history, QStringLiteral("Reopen Closed Tab"),
      PrimaryShortcut(QStringLiteral("Shift+T")),
      [this] { ReopenClosedTab(); });
#if defined(OS_MAC)
  const QKeySequence history_shortcut(QStringLiteral("Meta+Y"));
  const QKeySequence clear_shortcut(QStringLiteral("Meta+Shift+Backspace"));
#else
  const QKeySequence history_shortcut(QStringLiteral("Ctrl+H"));
  const QKeySequence clear_shortcut(QStringLiteral("Ctrl+Shift+Delete"));
#endif
  add_action(history, QStringLiteral("Show History"), history_shortcut,
             [this] { ShowBrowserUiSurface(BrowserUiSurface::History); });
  add_action(history, QStringLiteral("Downloads"),
#if defined(OS_MAC)
             QKeySequence(QStringLiteral("Meta+Shift+J")),
#else
             QKeySequence(QStringLiteral("Ctrl+J")),
#endif
             [this] { ShowBrowserUiSurface(BrowserUiSurface::Downloads); });
  history->addSeparator();
  add_action(history, QStringLiteral("Clear Browsing Data…"), clear_shortcut,
             [this] { ShowBrowserUiSurface(BrowserUiSurface::ClearData); });

  QMenu* bookmarks = menuBar()->addMenu(QStringLiteral("Bookmarks"));
  toggle_bookmark_action_ = add_action(
      bookmarks, QStringLiteral("Bookmark This Page"),
      PrimaryShortcut(QStringLiteral("D")),
      [this] { ToggleCurrentBookmark(); });
  add_action(bookmarks, QStringLiteral("Show Bookmarks"),
             PrimaryShortcut(QStringLiteral("Shift+B")),
             [this] { ShowBrowserUiSurface(BrowserUiSurface::Bookmarks); });
  bookmarks->addSeparator();
  add_action(bookmarks, QStringLiteral("Import Bookmarks…"), {},
             [this] { ImportBookmarks(); });
  add_action(bookmarks, QStringLiteral("Export Bookmarks…"), {},
             [this] { ExportBookmarks(); });

  QMenu* settings = menuBar()->addMenu(QStringLiteral("Settings"));
  QMenu* search_engine =
      settings->addMenu(QStringLiteral("Default Search Engine"));
  QActionGroup* search_group = new QActionGroup(search_engine);
  search_group->setExclusive(true);
  const BrowserSettings::SearchEngine engines[] = {
      BrowserSettings::SearchEngine::Google,
      BrowserSettings::SearchEngine::DuckDuckGo,
      BrowserSettings::SearchEngine::Bing};
  for (BrowserSettings::SearchEngine engine : engines) {
    QAction* action = search_engine->addAction(
        BrowserSettings::SearchEngineName(engine));
    action->setCheckable(true);
    action->setChecked(browser_settings_->search_engine() == engine);
    action->setProperty("searchEngineAction", true);
    action->setData(static_cast<int>(engine));
    search_group->addAction(action);
    connect(action, &QAction::triggered, this,
            [this, engine] { SetSearchEngine(static_cast<int>(engine)); });
  }
  settings->addSeparator();
  add_action(settings, QStringLiteral("Use Current Page as Home"), {},
             [this] {
               if (BrowserView* browser = CurrentBrowser()) {
                 SetHomePage(browser->current_url());
               }
             });
  add_action(settings, QStringLiteral("Reset Home Page"), {}, [this] {
    SetHomePage(QStringLiteral("https://www.example.com"));
  });
  QAction* open_home =
      settings->addAction(QStringLiteral("Open Home Page in New Tabs"));
  open_home->setCheckable(true);
  open_home->setChecked(browser_settings_->open_home_on_new_tab());
  open_home->setProperty("openHomeOnNewTabAction", true);
  connect(open_home, &QAction::toggled, this,
          [this](bool enabled) { SetOpenHomeOnNewTab(enabled); });
  settings->addSeparator();
  QMenu* startup = settings->addMenu(QStringLiteral("On Startup"));
  QActionGroup* startup_group = new QActionGroup(startup);
  startup_group->setExclusive(true);
  const BrowserSettings::StartupBehavior behaviors[] = {
      BrowserSettings::StartupBehavior::RestoreSession,
      BrowserSettings::StartupBehavior::HomePage,
      BrowserSettings::StartupBehavior::BlankPage};
  for (BrowserSettings::StartupBehavior behavior : behaviors) {
    QAction* action =
        startup->addAction(BrowserSettings::StartupBehaviorName(behavior));
    action->setCheckable(true);
    action->setChecked(browser_settings_->startup_behavior() == behavior);
    action->setProperty("startupBehaviorAction", true);
    action->setData(static_cast<int>(behavior));
    startup_group->addAction(action);
    connect(action, &QAction::triggered, this, [this, behavior] {
      SetStartupBehavior(static_cast<int>(behavior));
    });
  }

  QMenu* window = menuBar()->addMenu(QStringLiteral("Window"));
  add_action(window, QStringLiteral("Next Tab"), QKeySequence::NextChild,
             [this] {
               if (tab_bar_->count() > 1) {
                 tab_bar_->setCurrentIndex((tab_bar_->currentIndex() + 1) %
                                           tab_bar_->count());
               }
             });
  add_action(window, QStringLiteral("Previous Tab"),
             QKeySequence::PreviousChild, [this] {
               if (tab_bar_->count() > 1) {
                 tab_bar_->setCurrentIndex((tab_bar_->currentIndex() - 1 +
                                            tab_bar_->count()) %
                                           tab_bar_->count());
               }
             });
  add_action(window, QStringLiteral("All Tabs"),
             PrimaryShortcut(QStringLiteral("Shift+A")),
             [this] { ShowBrowserUiSurface(BrowserUiSurface::AllTabs); });
}

void MainWindow::SetSearchEngine(int engine_value) {
  const auto engine =
      static_cast<BrowserSettings::SearchEngine>(engine_value);
  const BrowserSettings::SearchEngine previous =
      browser_settings_->search_engine();
  browser_settings_->set_search_engine(engine);
  for (QAction* action : findChildren<QAction*>()) {
    if (action->property("searchEngineAction").toBool()) {
      action->setChecked(action->data().toInt() == engine_value);
    }
  }
  QString error;
  if (!browser_settings_->Save(&error)) {
    browser_settings_->set_search_engine(previous);
    for (QAction* action : findChildren<QAction*>()) {
      if (action->property("searchEngineAction").toBool()) {
        const QSignalBlocker blocker(action);
        action->setChecked(
            action->data().toInt() == static_cast<int>(previous));
      }
    }
    statusBar()->showMessage(
        QStringLiteral("Unable to save browser settings: %1").arg(error),
        8000);
  } else {
    statusBar()->showMessage(
        QStringLiteral("Default search engine: %1")
            .arg(BrowserSettings::SearchEngineName(engine)),
        2500);
  }
}

bool MainWindow::SetHomePage(const QString& value) {
  const auto normalized = NormalizeUrl(value);
  const QString previous = browser_settings_->home_page();
  if (!normalized || !browser_settings_->set_home_page(*normalized)) {
    statusBar()->showMessage(QStringLiteral("This URL cannot be used as home"),
                             5000);
    return false;
  }
  QString error;
  if (!browser_settings_->Save(&error)) {
    browser_settings_->set_home_page(previous);
    statusBar()->showMessage(
        QStringLiteral("Unable to save browser settings: %1").arg(error),
        8000);
    return false;
  }
  statusBar()->showMessage(QStringLiteral("Home page updated"), 2500);
  return true;
}

void MainWindow::GoHome() {
  if (BrowserView* browser = CurrentBrowser()) {
    browser->LoadUrl(browser_settings_->home_page());
  }
}

bool MainWindow::SetOpenHomeOnNewTab(bool enabled) {
  const bool previous = browser_settings_->open_home_on_new_tab();
  browser_settings_->set_open_home_on_new_tab(enabled);
  QString error;
  if (!browser_settings_->Save(&error)) {
    browser_settings_->set_open_home_on_new_tab(previous);
    for (QAction* action : findChildren<QAction*>()) {
      if (action->property("openHomeOnNewTabAction").toBool()) {
        const QSignalBlocker blocker(action);
        action->setChecked(previous);
      }
    }
    statusBar()->showMessage(
        QStringLiteral("Unable to save browser settings: %1").arg(error),
        8000);
    return false;
  }
  statusBar()->showMessage(
      enabled ? QStringLiteral("New tabs will open the home page")
              : QStringLiteral("New tabs will open a blank page"),
      2500);
  return true;
}

bool MainWindow::SetStartupBehavior(int behavior_value) {
  const auto behavior =
      static_cast<BrowserSettings::StartupBehavior>(behavior_value);
  const BrowserSettings::StartupBehavior previous =
      browser_settings_->startup_behavior();
  browser_settings_->set_startup_behavior(behavior);
  QString error;
  if (!browser_settings_->Save(&error)) {
    browser_settings_->set_startup_behavior(previous);
    for (QAction* action : findChildren<QAction*>()) {
      if (action->property("startupBehaviorAction").toBool()) {
        const QSignalBlocker blocker(action);
        action->setChecked(
            action->data().toInt() == static_cast<int>(previous));
      }
    }
    statusBar()->showMessage(
        QStringLiteral("Unable to save browser settings: %1").arg(error),
        8000);
    return false;
  }
  for (QAction* action : findChildren<QAction*>()) {
    if (action->property("startupBehaviorAction").toBool()) {
      const QSignalBlocker blocker(action);
      action->setChecked(action->data().toInt() == behavior_value);
    }
  }
  statusBar()->showMessage(
      QStringLiteral("On startup: %1")
          .arg(BrowserSettings::StartupBehaviorName(behavior)),
      2500);
  return true;
}

void MainWindow::ShowBrowserUiSurface(BrowserUiSurface surface) {
  if (web_fullscreen_) {
    pending_browser_ui_surface_ = surface;
    if (BrowserView* browser = CurrentBrowser()) browser->ExitFullscreen();
    return;
  }
  PerformBrowserUiSurface(surface);
}

void MainWindow::PerformBrowserUiSurface(BrowserUiSurface surface) {
  switch (surface) {
    case BrowserUiSurface::Downloads:
      download_panel_->show();
      download_panel_->raise();
      downloads_button_->setFocus(Qt::ShortcutFocusReason);
      break;
    case BrowserUiSurface::Bookmarks:
      RebuildBookmarksMenu();
      bookmarks_menu_->popup(bookmarks_button_->mapToGlobal(
          QPoint(0, bookmarks_button_->height())));
      break;
    case BrowserUiSurface::History:
      RebuildHistoryMenu();
      history_menu_->popup(history_button_->mapToGlobal(
          QPoint(0, history_button_->height())));
      break;
    case BrowserUiSurface::AllTabs:
      RebuildAllTabsMenu();
      all_tabs_menu_->popup(all_tabs_button_->mapToGlobal(
          QPoint(0, all_tabs_button_->height())));
      break;
    case BrowserUiSurface::ClearData:
      ShowClearBrowsingDataPrompt();
      break;
  }
}

void MainWindow::ToggleCurrentBookmark() {
  BrowserView* browser = CurrentBrowser();
  if (!browser) return;
  const QString url = browser->current_url();
  if (browsing_data_->IsBookmarked(url)) {
    RemoveBookmark(url);
    return;
  }
  const BrowsingDataStore previous = *browsing_data_;
  if (!browsing_data_->AddBookmark(url, browser->page_title())) return;
  if (!SaveBrowsingData()) {
    *browsing_data_ = previous;
    return;
  }
  statusBar()->showMessage(QStringLiteral("Bookmark added"), 2000);
  RebuildBookmarksMenu();
  RefreshAddressSuggestions();
  UpdateChrome();
}

bool MainWindow::RemoveBookmark(const QString& url) {
  const BrowsingDataStore previous = *browsing_data_;
  if (!browsing_data_->RemoveBookmark(url)) return false;
  if (!SaveBrowsingData()) {
    *browsing_data_ = previous;
    return false;
  }
  RebuildBookmarksMenu();
  RefreshAddressSuggestions();
  UpdateChrome();
  statusBar()->showMessage(QStringLiteral("Bookmark removed"), 2000);
  return true;
}

bool MainWindow::RenameBookmark(const QString& url, const QString& title) {
  const BrowsingDataStore previous = *browsing_data_;
  if (!browsing_data_->RenameBookmark(url, title)) return false;
  if (!SaveBrowsingData()) {
    *browsing_data_ = previous;
    return false;
  }
  RebuildBookmarksMenu();
  RefreshAddressSuggestions();
  statusBar()->showMessage(
      title.trimmed().isEmpty() ? QStringLiteral("Bookmark name cleared")
                                : QStringLiteral("Bookmark renamed"),
      2000);
  return true;
}

bool MainWindow::RemoveHistory(const QString& url) {
  const BrowsingDataStore previous = *browsing_data_;
  if (!browsing_data_->RemoveHistory(url)) return false;
  if (!SaveBrowsingData()) {
    *browsing_data_ = previous;
    return false;
  }
  RebuildHistoryMenu();
  RefreshAddressSuggestions();
  statusBar()->showMessage(QStringLiteral("History entry removed"), 2000);
  return true;
}

void MainWindow::ImportBookmarks() {
  const QString path = QFileDialog::getOpenFileName(
      this, QStringLiteral("Import Bookmarks"), QString(),
      QStringLiteral("Bookmark HTML (*.html *.htm);;All Files (*)"));
  if (path.isEmpty()) return;

  const BrowsingDataStore previous = *browsing_data_;
  int imported = 0;
  QString error;
  if (!browsing_data_->ImportBookmarksHtml(path, &imported, &error)) {
    statusBar()->showMessage(
        QStringLiteral("Unable to import bookmarks: %1").arg(error), 8000);
    return;
  }
  if (imported > 0 && !SaveBrowsingData()) {
    *browsing_data_ = previous;
    return;
  }
  RebuildBookmarksMenu();
  RefreshAddressSuggestions();
  UpdateChrome();
  statusBar()->showMessage(
      imported == 0
          ? QStringLiteral("No new bookmarks found")
          : QStringLiteral("Imported %1 bookmark(s)").arg(imported),
      4000);
}

void MainWindow::ExportBookmarks() {
  QString path = QFileDialog::getSaveFileName(
      this, QStringLiteral("Export Bookmarks"),
      QStringLiteral("trail-browser-bookmarks.html"),
      QStringLiteral("Bookmark HTML (*.html)"));
  if (path.isEmpty()) return;
  if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".html");

  QString error;
  if (!browsing_data_->ExportBookmarksHtml(path, &error)) {
    statusBar()->showMessage(
        QStringLiteral("Unable to export bookmarks: %1").arg(error), 8000);
    return;
  }
  statusBar()->showMessage(QStringLiteral("Bookmarks exported"), 4000);
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
    action->setProperty("bookmarkUrl", bookmark.url);
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

void MainWindow::ShowBookmarkContextMenu(const QPoint& position) {
  QAction* bookmark = bookmarks_menu_->actionAt(position);
  const QString url =
      bookmark ? bookmark->property("bookmarkUrl").toString() : QString();
  if (url.isEmpty()) return;
  QMenu context(bookmarks_menu_);
  QAction* edit = context.addAction(QStringLiteral("Edit bookmark…"));
  QAction* remove = context.addAction(QStringLiteral("Remove bookmark"));
  QAction* selected = context.exec(bookmarks_menu_->mapToGlobal(position));
  if (selected == edit) {
    QString current_title;
    for (const BrowsingDataStore::Bookmark& item :
         browsing_data_->bookmarks()) {
      if (item.url == url) {
        current_title = item.title;
        break;
      }
    }
    bool accepted = false;
    const QString title = QInputDialog::getText(
        this, QStringLiteral("Edit bookmark"), QStringLiteral("Name:"),
        QLineEdit::Normal, current_title, &accepted);
    if (accepted) RenameBookmark(url, title);
  } else if (selected == remove) {
    RemoveBookmark(url);
  }
}

void MainWindow::RebuildHistoryMenu() {
  history_menu_->clear();
  if (browsing_data_->history().isEmpty()) {
    QAction* empty = history_menu_->addAction(QStringLiteral("No history yet"));
    empty->setEnabled(false);
  } else {
    constexpr int kVisibleHistoryEntries = 25;
    const auto& history = browsing_data_->history();
    const int count = std::min(static_cast<int>(history.size()),
                               kVisibleHistoryEntries);
    for (int index = 0; index < count; ++index) {
      const auto& entry = history.at(index);
      const QString label = entry.title.isEmpty() ? entry.url : entry.title;
      QAction* action = history_menu_->addAction(label);
      action->setToolTip(entry.url);
      action->setProperty("historyUrl", entry.url);
      connect(action, &QAction::triggered, this,
              [this, url = entry.url] { AddTab(url, true); });
    }
  }
  history_menu_->addSeparator();
  QAction* clear =
      history_menu_->addAction(QStringLiteral("Clear browsing data…"));
  clear->setEnabled(!browsing_data_clear_in_progress_);
  connect(clear, &QAction::triggered, this,
          &MainWindow::ShowClearBrowsingDataPrompt);
}

void MainWindow::ShowHistoryContextMenu(const QPoint& position) {
  QAction* history = history_menu_->actionAt(position);
  const QString url =
      history ? history->property("historyUrl").toString() : QString();
  if (url.isEmpty()) return;
  QMenu context(history_menu_);
  QAction* remove =
      context.addAction(QStringLiteral("Remove from history"));
  if (context.exec(history_menu_->mapToGlobal(position)) == remove) {
    RemoveHistory(url);
  }
}

void MainWindow::RebuildAllTabsMenu() {
  all_tabs_menu_->clear();
  for (int index = 0; index < tab_bar_->count(); ++index) {
    auto* browser =
        qvariant_cast<BrowserView*>(tab_bar_->tabData(index));
    if (!browser || closing_tabs_.contains(browser)) continue;
    QString label = TabText(browser->page_title(), browser->current_url());
    if (IsTabPinned(browser)) label.prepend(QStringLiteral("\U0001F4CC "));
    if (browser->audio_muted()) {
      label.prepend(QStringLiteral("\U0001F507 "));
    } else if (browser->audio_playing()) {
      label.prepend(QStringLiteral("\U0001F50A "));
    }
    QAction* action = all_tabs_menu_->addAction(label);
    action->setCheckable(true);
    action->setChecked(index == tab_bar_->currentIndex());
    action->setToolTip(browser->current_url());
    const QPointer<BrowserView> target(browser);
    connect(action, &QAction::triggered, this, [this, target] {
      if (target) ActivateTabByShortcut(IndexOf(target));
    });
  }

  if (closed_tabs_.isEmpty()) return;
  all_tabs_menu_->addSection(QStringLiteral("Recently closed"));
  constexpr int kVisibleRecentlyClosedTabs = 10;
  const int count = std::min(static_cast<int>(closed_tabs_.size()),
                             kVisibleRecentlyClosedTabs);
  for (int recent_index = 0; recent_index < count; ++recent_index) {
    const RecentlyClosedTab& tab =
        closed_tabs_.at(closed_tabs_.size() - 1 - recent_index);
    QAction* action = all_tabs_menu_->addAction(
        tab.title.isEmpty() ? tab.url : tab.title);
    action->setToolTip(tab.url);
    connect(action, &QAction::triggered, this, [this, recent_index] {
      ReopenClosedTabAt(recent_index);
    });
  }
}

void MainWindow::ShowClearBrowsingDataPrompt() {
  if (browsing_data_clear_in_progress_) return;
  auto* dialog = new QDialog(this);
  dialog->setWindowTitle(QStringLiteral("Clear browsing data?"));
  auto* layout = new QVBoxLayout(dialog);
  auto* explanation = new QLabel(
      QStringLiteral("Choose which browser data to remove. Bookmarks and "
                     "downloaded files are kept."),
      dialog);
  explanation->setWordWrap(true);
  layout->addWidget(explanation);

  auto* history = new QCheckBox(QStringLiteral("Browsing history"), dialog);
  auto* recently_closed =
      new QCheckBox(QStringLiteral("Recently closed tabs"), dialog);
  auto* downloads =
      new QCheckBox(QStringLiteral("Download history"), dialog);
  auto* site_data = new QCheckBox(
      QStringLiteral(
          "Site data (cache, cookies, sign-ins, and security decisions)"),
      dialog);
  const QList<QCheckBox*> choices{history, recently_closed, downloads,
                                  site_data};
  for (QCheckBox* choice : choices) {
    choice->setChecked(true);
    layout->addWidget(choice);
  }

  auto* buttons = new QDialogButtonBox(dialog);
  auto* cancel = buttons->addButton(QDialogButtonBox::Cancel);
  auto* clear = buttons->addButton(QStringLiteral("Clear selected data"),
                                   QDialogButtonBox::DestructiveRole);
  clear->setObjectName(QStringLiteral("clearSelectedData"));
  clear->setDefault(true);
  layout->addWidget(buttons);
  const auto update_clear_enabled = [choices, clear] {
    clear->setEnabled(std::any_of(choices.cbegin(), choices.cend(),
                                  [](const QCheckBox* choice) {
                                    return choice->isChecked();
                                  }));
  };
  for (QCheckBox* choice : choices) {
    connect(choice, &QCheckBox::toggled, dialog, update_clear_enabled);
  }
  connect(cancel, &QPushButton::clicked, dialog, &QDialog::reject);
  connect(clear, &QPushButton::clicked, dialog,
          [this, dialog, history, recently_closed, downloads, site_data] {
            const BrowsingDataSelection selection{
                history->isChecked(), recently_closed->isChecked(),
                downloads->isChecked(), site_data->isChecked()};
            dialog->accept();
            BeginClearBrowsingData(true, selection);
          });
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->open();
}

void MainWindow::BeginClearBrowsingData(
    bool show_result_dialog, const BrowsingDataSelection& selection) {
  if (browsing_data_clear_in_progress_ || !selection.Any()) return;
  browsing_data_clear_in_progress_ = true;
  browsing_data_clear_show_result_ = show_result_dialog;
  browsing_data_clear_pending_ = selection.TaskCount();
  browsing_data_clear_failures_.clear();
  browsing_data_clear_completed_.clear();
  browsing_data_clear_result_.clear();
  RebuildHistoryMenu();
  statusBar()->showMessage(QStringLiteral("Clearing browsing data…"));

  if (selection.history) {
    browsing_data_->ClearHistory();
    CompleteBrowsingDataClearTask(QStringLiteral("browsing history"),
                                  SaveBrowsingData());
    RebuildHistoryMenu();
    RefreshAddressSuggestions();
  }
  if (selection.recently_closed) {
    closed_tabs_.clear();
    for (BrowserView* browser : closing_tabs_) {
      forgotten_closing_tabs_.insert(browser);
    }
    for (const QPointer<BrowserView>& browser : queued_tab_closes_) {
      if (browser) forgotten_closing_tabs_.insert(browser);
    }
    pending_closed_tabs_.clear();
    RebuildHistoryMenu();
    RebuildAllTabsMenu();
    UpdateChrome();
    CompleteBrowsingDataClearTask(
        QStringLiteral("recently closed tabs"),
        session_path_.isEmpty() || PersistSession(CaptureSession(false)));
  }
  if (selection.downloads) {
    CompleteBrowsingDataClearTask(QStringLiteral("download history"),
                                  download_manager_->ClearFinished());
  }
  if (!selection.site_data) return;

  QPointer<MainWindow> owner(this);
  CefRefPtr<CefRequestContext> context = CefRequestContext::GetGlobalContext();
  if (!context) {
    CompleteBrowsingDataClearTask(QStringLiteral("cache"), false);
    CompleteBrowsingDataClearTask(QStringLiteral("site credentials"), false);
    CompleteBrowsingDataClearTask(QStringLiteral("certificate exceptions"),
                                  false);
    CompleteBrowsingDataClearTask(QStringLiteral("cookies"), false);
    return;
  }

  context->ClearHttpCache(new CompletionCallback([owner] {
    if (owner) owner->CompleteBrowsingDataClearTask(QStringLiteral("cache"),
                                                     true);
  }));
  context->ClearHttpAuthCredentials(new CompletionCallback([owner] {
    if (owner) {
      owner->CompleteBrowsingDataClearTask(
          QStringLiteral("site credentials"), true);
    }
  }));
  context->ClearCertificateExceptions(new CompletionCallback([owner] {
    if (owner) {
      owner->CompleteBrowsingDataClearTask(
          QStringLiteral("certificate exceptions"), true);
    }
  }));

  CefRefPtr<CefCookieManager> cookie_manager =
      context->GetCookieManager(nullptr);
  if (!cookie_manager ||
      !cookie_manager->DeleteCookies(
          CefString(), CefString(),
          new DeleteCookiesCallback([owner](int deleted) {
            if (owner) {
              owner->CompleteBrowsingDataClearTask(QStringLiteral("cookies"),
                                                    deleted >= 0);
            }
          }))) {
    CompleteBrowsingDataClearTask(QStringLiteral("cookies"), false);
  }
}

void MainWindow::CompleteBrowsingDataClearTask(const QString& task,
                                               bool success) {
  if (!browsing_data_clear_in_progress_ || browsing_data_clear_pending_ <= 0) {
    return;
  }
  if (!success) browsing_data_clear_failures_.append(task);
  if (success) browsing_data_clear_completed_.append(task);
  --browsing_data_clear_pending_;
  if (browsing_data_clear_pending_ > 0) return;

  browsing_data_clear_in_progress_ = false;
  if (!browsing_data_clear_completed_.isEmpty()) {
    browsing_data_clear_result_ =
        QStringLiteral("Cleared: %1.")
            .arg(browsing_data_clear_completed_.join(QStringLiteral(", ")));
  }
  if (!browsing_data_clear_failures_.isEmpty()) {
    if (!browsing_data_clear_result_.isEmpty()) {
      browsing_data_clear_result_ += QLatin1Char(' ');
    }
    browsing_data_clear_result_ +=
        QStringLiteral("Could not clear: %1.")
            .arg(browsing_data_clear_failures_.join(QStringLiteral(", ")));
  }
  statusBar()->showMessage(browsing_data_clear_result_, 8000);
  RebuildHistoryMenu();

  if (browsing_data_clear_show_result_) {
    auto* result = new QMessageBox(
        browsing_data_clear_failures_.isEmpty() ? QMessageBox::Information
                                                 : QMessageBox::Warning,
        browsing_data_clear_failures_.isEmpty()
            ? QStringLiteral("Browsing data cleared")
            : QStringLiteral("Browsing data partially cleared"),
        browsing_data_clear_result_, QMessageBox::Ok, this);
    result->setAttribute(Qt::WA_DeleteOnClose);
    result->open();
  }
}

void MainWindow::RecordVisit(BrowserView* browser) {
  if (!browser || browser->is_loading()) return;
  const QString url = browser->current_url();
  if (url.isEmpty()) return;
  browsing_data_->RecordVisit(url, browser->page_title());
  SaveBrowsingData();
  RefreshAddressSuggestions();
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

void MainWindow::RefreshAddressSuggestions() {
  QStringList labels;
  QStringList urls;
  QHash<QString, QString> label_urls;
  QSet<QString> seen;
  const auto add = [&labels, &urls, &label_urls, &seen](
                       const QString& url, const QString& title) {
    if (!url.isEmpty() && !seen.contains(url)) {
      seen.insert(url);
      QString label = title.trimmed().isEmpty()
                          ? url
                          : QStringLiteral("%1 — %2").arg(title.trimmed(), url);
      if (label_urls.contains(label)) label = url;
      labels.append(label);
      urls.append(url);
      label_urls.insert(label, url);
    }
  };
  for (const BrowsingDataStore::Bookmark& bookmark :
       browsing_data_->bookmarks()) {
    if (labels.size() >= kMaxAddressSuggestions) break;
    add(bookmark.url, bookmark.title);
  }
  for (const BrowsingDataStore::HistoryEntry& entry : browsing_data_->history()) {
    if (labels.size() >= kMaxAddressSuggestions) break;
    add(entry.url, entry.title);
  }
  address_suggestion_urls_ = std::move(label_urls);
  address_suggestion_url_order_ = std::move(urls);
  address_suggestions_->setStringList(labels);
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
  session.recently_closed_tabs = closed_tabs_;
  for (int index = 0; index < tab_bar_->count(); ++index) {
    BrowserView* browser =
        qvariant_cast<BrowserView*>(tab_bar_->tabData(index));
    if (browser && !closing_tabs_.contains(browser)) {
      const QString url = browser->current_url().trimmed();
      session.tab_urls.append(url.isEmpty() ? QStringLiteral("about:blank")
                                            : url);
      session.tab_pinned.append(IsTabPinned(browser));
    }
  }
  if (session.tab_urls.isEmpty()) {
    session.tab_urls.append(QStringLiteral("about:blank"));
    session.tab_pinned.append(false);
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

std::optional<QString> MainWindow::NormalizeUrl(QString input) const {
  return BrowserSettings::NormalizeNavigationInput(
      browser_settings_->search_engine(), std::move(input));
}
