#include "ui/main_window.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLineEdit>
#include <QPushButton>
#include <QShortcut>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include "ui/browser_view.h"

MainWindow::MainWindow(const QString& initial_url, QWidget* parent)
    : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("Trail Browser"));
  resize(1280, 800);
  setMinimumSize(640, 480);

  auto* central = new QWidget(this);
  auto* page_layout = new QVBoxLayout(central);
  page_layout->setContentsMargins(0, 0, 0, 0);
  page_layout->setSpacing(0);

  auto* toolbar = new QWidget(central);
  auto* toolbar_layout = new QHBoxLayout(toolbar);
  toolbar_layout->setContentsMargins(8, 6, 8, 6);
  toolbar_layout->setSpacing(6);

  back_button_ = new QPushButton(QStringLiteral("←"), toolbar);
  forward_button_ = new QPushButton(QStringLiteral("→"), toolbar);
  reload_button_ = new QPushButton(QStringLiteral("↻"), toolbar);
  address_bar_ = new QLineEdit(initial_url, toolbar);

  back_button_->setToolTip(QStringLiteral("Back"));
  forward_button_->setToolTip(QStringLiteral("Forward"));
  reload_button_->setToolTip(QStringLiteral("Reload"));
  address_bar_->setPlaceholderText(QStringLiteral("Search or enter an address"));
  address_bar_->setClearButtonEnabled(true);
  back_button_->setEnabled(false);
  forward_button_->setEnabled(false);

  toolbar_layout->addWidget(back_button_);
  toolbar_layout->addWidget(forward_button_);
  toolbar_layout->addWidget(reload_button_);
  toolbar_layout->addWidget(address_bar_, 1);

  browser_view_ = new BrowserView(initial_url, central);
  page_layout->addWidget(toolbar);
  page_layout->addWidget(browser_view_, 1);
  setCentralWidget(central);

  connect(back_button_, &QPushButton::clicked, browser_view_,
          &BrowserView::GoBack);
  connect(forward_button_, &QPushButton::clicked, browser_view_,
          &BrowserView::GoForward);
  connect(reload_button_, &QPushButton::clicked, browser_view_,
          &BrowserView::Reload);
  connect(address_bar_, &QLineEdit::returnPressed, this,
          &MainWindow::NavigateFromAddressBar);
  connect(browser_view_, &BrowserView::TitleChanged, this,
          [this](const QString& title) {
            setWindowTitle(title.isEmpty() ? QStringLiteral("Trail Browser")
                                           : title + QStringLiteral(" — Trail Browser"));
          });
  connect(browser_view_, &BrowserView::AddressChanged, this,
          &MainWindow::UpdateAddress);
  connect(browser_view_, &BrowserView::LoadingStateChanged, this,
          &MainWindow::UpdateLoadingState);
  connect(browser_view_, &BrowserView::PopupRequested, this,
          [this](const QString& url) { browser_view_->LoadUrl(url); });
  connect(browser_view_, &BrowserView::BrowserClosed, this, [this] {
    if (close_requested_) {
      close_requested_ = false;
      QMetaObject::invokeMethod(this, &QWidget::close, Qt::QueuedConnection);
    }
  });

  auto* focus_address = new QShortcut(QKeySequence(QStringLiteral("Ctrl+L")), this);
  connect(focus_address, &QShortcut::activated, address_bar_, [this] {
    address_bar_->setFocus();
    address_bar_->selectAll();
  });
  auto* dev_tools = new QShortcut(QKeySequence(Qt::Key_F12), this);
  connect(dev_tools, &QShortcut::activated, browser_view_,
          &BrowserView::ShowDevTools);
}

void MainWindow::closeEvent(QCloseEvent* event) {
  if (browser_view_->RequestClose()) {
    event->accept();
    return;
  }

  close_requested_ = true;
  setEnabled(false);
  event->ignore();
}

void MainWindow::NavigateFromAddressBar() {
  const QString url = NormalizeUrl(address_bar_->text());
  address_bar_->setText(url);
  browser_view_->LoadUrl(url);
}

void MainWindow::UpdateLoadingState(bool loading, bool can_go_back,
                                    bool can_go_forward) {
  back_button_->setEnabled(can_go_back);
  forward_button_->setEnabled(can_go_forward);
  reload_button_->setText(loading ? QStringLiteral("×")
                                  : QStringLiteral("↻"));
  reload_button_->setToolTip(loading ? QStringLiteral("Stop")
                                     : QStringLiteral("Reload"));
  disconnect(reload_button_, &QPushButton::clicked, nullptr, nullptr);
  if (loading) {
    connect(reload_button_, &QPushButton::clicked, browser_view_,
            &BrowserView::Stop);
  } else {
    connect(reload_button_, &QPushButton::clicked, browser_view_,
            &BrowserView::Reload);
  }
}

void MainWindow::UpdateAddress(const QString& url) {
  if (!address_bar_->hasFocus()) {
    address_bar_->setText(url);
  }
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

