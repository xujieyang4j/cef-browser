#include "ui/browser_view.h"

#include <algorithm>
#include <utility>

#include <QByteArray>
#include <QFocusEvent>
#include <QHideEvent>
#include <QMetaObject>
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
    : QWidget(parent),
      initial_url_(std::move(initial_url)),
      current_url_(initial_url_) {
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

void BrowserView::FinalizeClose() {
  if (!browser_) return;
  browser_->GetHost()->CloseBrowser(true);
  browser_ = nullptr;
  hide();
  destroy(true, true);
}

bool BrowserView::RequestClose() {
  closing_ = true;
  if (browser_) {
    // A tab is not a top-level native window and cannot complete the regular
    // close handshake independently. Force the CEF child closed, then wait
    // for OnBeforeClose before deleting its Qt host.
    browser_->GetHost()->CloseBrowser(true);
    return false;
  }
  return !create_requested_ && browser_ids_.isEmpty();
}

void BrowserView::OnCefBrowserCreated(CefRefPtr<CefBrowser> browser) {
  browser_ids_.insert(browser->GetIdentifier());

  // DevTools uses the same client and also triggers this callback. Track it so
  // application shutdown cannot reach CefShutdown while DevTools still exists.
  if (browser_) {
    return;
  }

  browser_ = std::move(browser);
  primary_browser_id_ = browser_->GetIdentifier();
  ResizeBrowser();
  UpdateNativeVisibility();
  if (closing_) {
    browser_->GetHost()->CloseBrowser(true);
  }
}

void BrowserView::OnCefBrowserClosed(CefRefPtr<CefBrowser> browser) {
  const int browser_id = browser->GetIdentifier();
  browser_ids_.remove(browser_id);
  if (browser_id == primary_browser_id_) {
    browser_ = nullptr;
    primary_browser_closed_ = true;
  }

  if (primary_browser_closed_ && browser_ids_.isEmpty()) {
    create_requested_ = false;
    client_ = nullptr;
    QMetaObject::invokeMethod(
        this, [this] { emit BrowserClosed(); }, Qt::QueuedConnection);
  }
}

void BrowserView::OnCefDialogClosed(CefRefPtr<CefBrowser> browser) {
  if (!closing_ || !browser_ || !browser_->IsSame(browser)) return;
  QMetaObject::invokeMethod(
      this,
      [this] {
        if (browser_ && !browser_->GetHost()->IsReadyToBeClosed()) {
          closing_ = false;
          emit CloseCancelled();
        }
      },
      Qt::QueuedConnection);
}

bool BrowserView::OnCefKeyEvent(CefRefPtr<CefBrowser> browser,
                                const CefKeyEvent& event) {
  if (!browser_ || !browser_->IsSame(browser) || closing_) return false;
  if (event.type != KEYEVENT_RAWKEYDOWN && event.type != KEYEVENT_KEYDOWN) {
    return false;
  }

#if defined(OS_MAC)
  const bool primary_modifier = event.modifiers & EVENTFLAG_COMMAND_DOWN;
#else
  const bool primary_modifier = event.modifiers & EVENTFLAG_CONTROL_DOWN;
#endif
  const bool shift = event.modifiers & EVENTFLAG_SHIFT_DOWN;
  ShortcutAction action;
  if (primary_modifier && event.windows_key_code == 'T') {
    action = shift ? ShortcutAction::ReopenClosedTab : ShortcutAction::NewTab;
  } else if (primary_modifier && !shift && event.windows_key_code == 'W') {
    action = ShortcutAction::CloseTab;
  } else if (primary_modifier && !shift && event.windows_key_code == 'L') {
    action = ShortcutAction::FocusAddress;
  } else if ((event.modifiers & EVENTFLAG_CONTROL_DOWN) &&
             event.windows_key_code == 0x09) {
    action = shift ? ShortcutAction::PreviousTab : ShortcutAction::NextTab;
  } else {
    return false;
  }

  emit ShortcutRequested(static_cast<int>(action));
  return true;
}

void BrowserView::OnCefTitleChanged(CefRefPtr<CefBrowser> browser,
                                    const QString& title) {
  if (browser_ && browser_->IsSame(browser)) {
    page_title_ = title;
    emit TitleChanged(title);
  }
}

void BrowserView::OnCefAddressChanged(CefRefPtr<CefBrowser> browser,
                                      const QString& url) {
  if (browser_ && browser_->IsSame(browser)) {
    current_url_ = url;
    emit AddressChanged(url);
  }
}

void BrowserView::OnCefLoadingStateChanged(CefRefPtr<CefBrowser> browser,
                                           bool loading, bool can_go_back,
                                           bool can_go_forward) {
  if (browser_ && browser_->IsSame(browser)) {
    is_loading_ = loading;
    can_go_back_ = can_go_back;
    can_go_forward_ = can_go_forward;
    emit LoadingStateChanged(loading, can_go_back, can_go_forward);
  }
}

void BrowserView::OnCefPopupRequested(CefRefPtr<CefBrowser> browser,
                                      const QString& url,
                                      cef_window_open_disposition_t disposition) {
  if (browser_ && browser_->IsSame(browser)) {
    emit PopupRequested(url, static_cast<int>(disposition));
  }
}

void BrowserView::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  CreateBrowserIfNeeded();
  UpdateNativeVisibility();
}

void BrowserView::hideEvent(QHideEvent* event) {
  QWidget::hideEvent(event);
  UpdateNativeVisibility();
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

void BrowserView::UpdateNativeVisibility() {
  if (!browser_) return;
  const auto handle = browser_->GetHost()->GetWindowHandle();
#if defined(OS_WIN)
  ShowWindow(handle, isVisible() ? SW_SHOW : SW_HIDE);
#elif defined(OS_LINUX)
  if (isVisible()) {
    XMapWindow(cef_get_xdisplay(), handle);
  } else {
    XUnmapWindow(cef_get_xdisplay(), handle);
  }
  XFlush(cef_get_xdisplay());
#elif defined(OS_MAC)
  NSView* browser_view = (__bridge NSView*)handle;
  [browser_view setHidden:!isVisible()];
#endif
}

