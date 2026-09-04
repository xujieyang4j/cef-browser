#include "ui/browser_view.h"

#include <algorithm>
#include <utility>

#include <QByteArray>
#include <QFocusEvent>
#include <QResizeEvent>
#include <QShowEvent>

#include "browser/browser_client.h"
#include "include/cef_browser.h"

#if defined(OS_WIN)
#include <windows.h>
#elif defined(OS_LINUX)
#include <X11/Xlib.h>
#include "include/internal/cef_linux.h"
#elif defined(OS_MAC)
#import <AppKit/AppKit.h>
#endif

BrowserView::BrowserView(QString initial_url, QWidget* parent)
    : QWidget(parent), initial_url_(std::move(initial_url)) {
  setAttribute(Qt::WA_NativeWindow);
  setFocusPolicy(Qt::StrongFocus);
}

BrowserView::~BrowserView() {
  if (client_) {
    client_->DetachOwner();
  }
  if (browser_) {
    browser_->GetHost()->CloseBrowser(true);
    browser_ = nullptr;
  }
}

void BrowserView::LoadUrl(const QString& url) {
  if (browser_) {
    const QByteArray encoded_url = url.toUtf8();
    browser_->GetMainFrame()->LoadURL(
        std::string(encoded_url.constData(), encoded_url.size()));
  } else {
    initial_url_ = url;
    CreateBrowserIfNeeded();
  }
}

void BrowserView::GoBack() {
  if (browser_ && browser_->CanGoBack()) browser_->GoBack();
}

void BrowserView::GoForward() {
  if (browser_ && browser_->CanGoForward()) browser_->GoForward();
}

void BrowserView::Reload() {
  if (browser_) browser_->Reload();
}

void BrowserView::Stop() {
  if (browser_) browser_->StopLoad();
}

void BrowserView::ShowDevTools() {
  if (!browser_) return;
  CefWindowInfo window_info;
  window_info.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
#if defined(OS_LINUX)
  CefString(&window_info.window_name) = "Trail Browser DevTools";
#else
  window_info.SetAsPopup(kNullWindowHandle, "Trail Browser DevTools");
#endif
  browser_->GetHost()->ShowDevTools(window_info, client_, CefBrowserSettings(),
                                    CefPoint());
}

bool BrowserView::RequestClose() {
  closing_ = true;
  if (browser_) {
    return browser_->GetHost()->TryCloseBrowser();
  }
  return !create_requested_;
}

void BrowserView::OnCefBrowserCreated(CefRefPtr<CefBrowser> browser) {
  // DevTools uses the same client and also triggers this callback. It is owned
  // by CEF and closes with the inspected browser.
  if (browser_) {
    return;
  }

  browser_ = std::move(browser);
  ResizeBrowser();
  if (closing_) {
    browser_->GetHost()->CloseBrowser(true);
  }
}

void BrowserView::OnCefBrowserClosed(CefRefPtr<CefBrowser> browser) {
  if (browser_ && browser_->IsSame(browser)) {
    browser_ = nullptr;
    create_requested_ = false;
    client_ = nullptr;
    if (closing_) emit BrowserClosed();
  }
}

void BrowserView::OnCefTitleChanged(CefRefPtr<CefBrowser> browser,
                                    const QString& title) {
  if (browser_ && browser_->IsSame(browser)) emit TitleChanged(title);
}

void BrowserView::OnCefAddressChanged(CefRefPtr<CefBrowser> browser,
                                      const QString& url) {
  if (browser_ && browser_->IsSame(browser)) emit AddressChanged(url);
}

void BrowserView::OnCefLoadingStateChanged(CefRefPtr<CefBrowser> browser,
                                           bool loading, bool can_go_back,
                                           bool can_go_forward) {
  if (browser_ && browser_->IsSame(browser)) {
    emit LoadingStateChanged(loading, can_go_back, can_go_forward);
  }
}

void BrowserView::OnCefPopupRequested(CefRefPtr<CefBrowser> browser,
                                      const QString& url) {
  if (browser_ && browser_->IsSame(browser)) emit PopupRequested(url);
}

void BrowserView::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  CreateBrowserIfNeeded();
}

void BrowserView::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  ResizeBrowser();
}

void BrowserView::focusInEvent(QFocusEvent* event) {
  QWidget::focusInEvent(event);
  if (browser_) browser_->GetHost()->SetFocus(true);
}

void BrowserView::CreateBrowserIfNeeded() {
  if (create_requested_ || closing_ || !isVisible()) return;

  create_requested_ = true;
  client_ = new BrowserClient(this);

  CefWindowInfo window_info;
  const CefRect bounds(0, 0, std::max(1, width()), std::max(1, height()));
#if defined(OS_LINUX)
  window_info.SetAsChild(static_cast<CefWindowHandle>(winId()), bounds);
#else
  window_info.SetAsChild(reinterpret_cast<CefWindowHandle>(winId()), bounds);
#endif
  window_info.runtime_style = CEF_RUNTIME_STYLE_ALLOY;

  CefBrowserSettings settings;
  const QByteArray encoded_url = initial_url_.toUtf8();
  if (!CefBrowserHost::CreateBrowser(window_info, client_,
                                     std::string(encoded_url.constData(),
                                                 encoded_url.size()), settings,
                                     nullptr, nullptr)) {
    create_requested_ = false;
    client_ = nullptr;
    emit TitleChanged(QStringLiteral("Unable to create CEF browser"));
  }
}

void BrowserView::ResizeBrowser() {
  if (!browser_) return;

  const auto handle = browser_->GetHost()->GetWindowHandle();
  const int browser_width = std::max(1, width());
  const int browser_height = std::max(1, height());

#if defined(OS_WIN)
  SetWindowPos(handle, nullptr, 0, 0, browser_width, browser_height,
               SWP_NOZORDER);
#elif defined(OS_LINUX)
  XResizeWindow(cef_get_xdisplay(), handle, browser_width, browser_height);
  XFlush(cef_get_xdisplay());
#elif defined(OS_MAC)
  NSView* browser_view = (__bridge NSView*)handle;
  [browser_view setFrame:NSMakeRect(0, 0, browser_width, browser_height)];
#endif
}

