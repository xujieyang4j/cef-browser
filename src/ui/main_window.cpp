#include "ui/main_window.h"

#include <algorithm>
#include <utility>

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLineEdit>
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
                       QString session_path, QWidget* parent)
    : QMainWindow(parent), session_path_(std::move(session_path)) {
  setWindowTitle(QStringLiteral("Trail Browser"));
  resize(1280, 800);
  setMinimumSize(640, 480);

  auto* central = new QWidget(this);
  auto* page_layout = new QVBoxLayout(central);
  page_layout->setContentsMargins(0, 0, 0, 0);
  page_layout->setSpacing(0);

  auto* tab_strip = new QWidget(central);
  auto* tab_layout = new QHBoxLayout(tab_strip);
  tab_layout->setContentsMargins(6, 4, 6, 0);
  tab_layout->setSpacing(4);

  tab_bar_ = new BrowserTabBar(tab_strip);
  tab_bar_->setDocumentMode(true);
  tab_bar_->setDrawBase(false);
  tab_bar_->setElideMode(Qt::ElideRight);
  tab_bar_->setExpanding(false);
  tab_bar_->setMovable(true);
  tab_bar_->setTabsClosable(true);
  tab_bar_->setUsesScrollButtons(true);

  auto* add_tab_button = new QToolButton(tab_strip);
  add_tab_button->setText(QStringLiteral("+"));
  add_tab_button->setToolTip(QStringLiteral("New tab (Ctrl+T)"));
  tab_layout->addWidget(tab_bar_, 1);
  tab_layout->addWidget(add_tab_button);

  auto* toolbar = new QWidget(central);
  auto* toolbar_layout = new QHBoxLayout(toolbar);
  toolbar_layout->setContentsMargins(8, 6, 8, 6);
  toolbar_layout->setSpacing(6);

  back_button_ = new QPushButton(QStringLiteral("←"), toolbar);
  forward_button_ = new QPushButton(QStringLiteral("→"), toolbar);
  reload_button_ = new QPushButton(QStringLiteral("↻"), toolbar);
  address_bar_ = new QLineEdit(toolbar);
  auto* downloads_button = new QPushButton(QStringLiteral("Downloads"), toolbar);

  back_button_->setToolTip(QStringLiteral("Back"));
  forward_button_->setToolTip(QStringLiteral("Forward"));
  reload_button_->setToolTip(QStringLiteral("Reload"));
  address_bar_->setPlaceholderText(
      QStringLiteral("Search or enter an address"));
  address_bar_->setClearButtonEnabled(true);
  downloads_button->setToolTip(QStringLiteral("Show downloads"));
  back_button_->setEnabled(false);
  forward_button_->setEnabled(false);

  toolbar_layout->addWidget(back_button_);
  toolbar_layout->addWidget(forward_button_);
  toolbar_layout->addWidget(reload_button_);
  toolbar_layout->addWidget(address_bar_, 1);
  toolbar_layout->addWidget(downloads_button);

  tab_stack_ = new QStackedWidget(central);
  download_manager_ = new DownloadManager(this);
  download_panel_ = new DownloadPanel(download_manager_, central);
  session_save_timer_ = new QTimer(this);
  session_save_timer_->setSingleShot(true);
  connect(session_save_timer_, &QTimer::timeout, this, [this] {
    if (session_persistence_ready_ && !window_close_requested_) {
      PersistSession(CaptureSession(false));
    }
  });
  page_layout->addWidget(tab_strip);
  page_layout->addWidget(toolbar);
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
  connect(downloads_button, &QPushButton::clicked, download_panel_,
          &DownloadPanel::ToggleVisibility);
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

  // Do not overwrite a known-good previous session until the window and CEF
  // event loop have had time to start successfully. After this gate, every
  // saved live snapshot is marked unclean until orderly shutdown completes.
  QTimer::singleShot(1500, this, [this] {
    if (window_close_requested_) return;
    session_persistence_ready_ = true;
    PersistSession(CaptureSession(false));
  });
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
  connect(browser, &BrowserView::LoadingStateChanged, this,
          [this, browser](bool loading, bool can_go_back, bool can_go_forward) {
            if (browser == CurrentBrowser()) {
              UpdateLoadingState(loading, can_go_back, can_go_forward);
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

  if (!browser) {
    address_bar_->clear();
    setWindowTitle(QStringLiteral("Trail Browser"));
    return;
  }

  const QString url = browser->current_url();
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
  }
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
