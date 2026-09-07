#include "ui/browser_view.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QByteArray>
#include <QDesktopServices>
#include <QFocusEvent>
#include <QHideEvent>
#include <QMetaObject>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QtMath>
#include <QShowEvent>
#include <QUrl>

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

BrowserView::BrowserView(QString initial_url,
                         CefRefPtr<CefDownloadHandler> download_handler,
                         QWidget* parent)
    : QWidget(parent),
      initial_url_(std::move(initial_url)),
      current_url_(initial_url_),
      download_handler_(std::move(download_handler)) {
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
  failure_page_active_ = false;
  render_process_failed_ = false;
  failure_page_url_.clear();
  if (browser_) {
    const QByteArray encoded_url = url.toUtf8();
    browser_->GetMainFrame()->LoadURL(
        std::string(encoded_url.constData(), encoded_url.size()));
  } else {
    initial_url_ = url;
    CreateBrowserIfNeeded();
  }
}

void BrowserView::ShowFailureForTesting(bool render_process_failed) {
  if (render_process_failed) {
    ShowFailurePage(QStringLiteral("This page crashed"),
                    QStringLiteral("The page renderer stopped unexpectedly."),
                    QStringLiteral("Test renderer termination"), current_url_,
                    true);
  } else {
    ShowFailurePage(QStringLiteral("Page unavailable"),
                    QStringLiteral("Trail Browser could not load this page."),
                    QStringLiteral("ERR_TEST_FAILURE (-999)"), current_url_,
                    false);
  }
}

void BrowserView::GoBack() {
  if (browser_ && browser_->CanGoBack()) browser_->GoBack();
}

void BrowserView::GoForward() {
  if (browser_ && browser_->CanGoForward()) browser_->GoForward();
}

void BrowserView::Reload() {
  if (!browser_) return;
  if (failure_page_active_) {
    LoadUrl(current_url_);
  } else {
    browser_->Reload();
  }
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

void BrowserView::Find(const QString& text, bool forward, bool find_next) {
  if (!browser_) return;
  const QByteArray encoded = text.toUtf8();
  browser_->GetHost()->Find(
      std::string(encoded.constData(), encoded.size()), forward, false,
      find_next);
}

void BrowserView::StopFinding(bool clear_selection) {
  if (browser_) browser_->GetHost()->StopFinding(clear_selection);
}

void BrowserView::ZoomIn() {
  if (!browser_) return;
  browser_->GetHost()->Zoom(CEF_ZOOM_COMMAND_IN);
  emit ZoomChanged(
      qRound(100.0 * std::pow(1.2, browser_->GetHost()->GetZoomLevel())));
}

void BrowserView::ZoomOut() {
  if (!browser_) return;
  browser_->GetHost()->Zoom(CEF_ZOOM_COMMAND_OUT);
  emit ZoomChanged(
      qRound(100.0 * std::pow(1.2, browser_->GetHost()->GetZoomLevel())));
}

void BrowserView::ResetZoom() {
  if (!browser_) return;
  browser_->GetHost()->Zoom(CEF_ZOOM_COMMAND_RESET);
  emit ZoomChanged(100);
}

int BrowserView::zoom_percent() const {
  return browser_
             ? qRound(100.0 *
                      std::pow(1.2, browser_->GetHost()->GetZoomLevel()))
             : 100;
}

QString BrowserView::MediaPermissionDescription(uint32_t permissions) {
  QStringList names;
  if (permissions & CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE) {
    names.append(QStringLiteral("microphone"));
  }
  if (permissions & CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE) {
    names.append(QStringLiteral("camera"));
  }
  if (permissions & CEF_MEDIA_PERMISSION_DESKTOP_AUDIO_CAPTURE) {
    names.append(QStringLiteral("system audio"));
  }
  if (permissions & CEF_MEDIA_PERMISSION_DESKTOP_VIDEO_CAPTURE) {
    names.append(QStringLiteral("screen"));
  }
  return names.isEmpty() ? QStringLiteral("media devices")
                         : names.join(QStringLiteral(", "));
}

QString BrowserView::PermissionDescription(uint32_t permissions) {
  QStringList names;
  const struct {
    uint32_t flag;
    const char* name;
  } known[] = {
      {CEF_PERMISSION_TYPE_CAMERA_STREAM, "camera"},
      {CEF_PERMISSION_TYPE_CLIPBOARD, "clipboard"},
      {CEF_PERMISSION_TYPE_GEOLOCATION, "location"},
      {CEF_PERMISSION_TYPE_IDLE_DETECTION, "activity status"},
      {CEF_PERMISSION_TYPE_MIC_STREAM, "microphone"},
      {CEF_PERMISSION_TYPE_MIDI_SYSEX, "MIDI devices"},
      {CEF_PERMISSION_TYPE_MULTIPLE_DOWNLOADS, "multiple downloads"},
      {CEF_PERMISSION_TYPE_NOTIFICATIONS, "notifications"},
      {CEF_PERMISSION_TYPE_POINTER_LOCK, "pointer lock"},
      {CEF_PERMISSION_TYPE_FILE_SYSTEM_ACCESS, "files"},
  };
  for (const auto& permission : known) {
    if (permissions & permission.flag) {
      names.append(QString::fromLatin1(permission.name));
    }
  }
  return names.isEmpty() ? QStringLiteral("additional capabilities")
                         : names.join(QStringLiteral(", "));
}

bool BrowserView::IsAllowedExternalScheme(const QString& url) {
  const QString scheme = QUrl(url).scheme().toLower();
  return scheme == QStringLiteral("mailto") ||
         scheme == QStringLiteral("tel") ||
         scheme == QStringLiteral("sms") ||
         scheme == QStringLiteral("webcal") ||
         scheme == QStringLiteral("magnet");
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
  if (event.windows_key_code == 0x72) {
    action = shift ? ShortcutAction::FindPrevious : ShortcutAction::FindNext;
  } else if (primary_modifier && event.windows_key_code == 'T') {
    action = shift ? ShortcutAction::ReopenClosedTab : ShortcutAction::NewTab;
  } else if (primary_modifier && !shift && event.windows_key_code == 'W') {
    action = ShortcutAction::CloseTab;
  } else if (primary_modifier && !shift && event.windows_key_code == 'L') {
    action = ShortcutAction::FocusAddress;
  } else if (primary_modifier && !shift && event.windows_key_code == 'F') {
    action = ShortcutAction::FindInPage;
  } else if (primary_modifier &&
             (event.windows_key_code == '+' ||
              event.windows_key_code == '=' ||
              event.windows_key_code == 0xBB)) {
    action = ShortcutAction::ZoomIn;
  } else if (primary_modifier &&
             (event.windows_key_code == '-' ||
              event.windows_key_code == 0xBD)) {
    action = ShortcutAction::ZoomOut;
  } else if (primary_modifier && event.windows_key_code == '0') {
    action = ShortcutAction::ResetZoom;
  } else if (primary_modifier && !shift && event.windows_key_code == 'D') {
    action = ShortcutAction::ToggleBookmark;
  } else if ((event.modifiers & EVENTFLAG_CONTROL_DOWN) &&
             event.windows_key_code == 0x09) {
    action = shift ? ShortcutAction::PreviousTab : ShortcutAction::NextTab;
  } else {
    return false;
  }

  emit ShortcutRequested(static_cast<int>(action));
  return true;
}

void BrowserView::OnCefFindResult(CefRefPtr<CefBrowser> browser, int count,
                                  int active_match_ordinal,
                                  bool final_update) {
  if (browser_ && browser_->IsSame(browser)) {
    emit FindResultChanged(count, active_match_ordinal, final_update);
  }
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
    if (failure_page_active_ && url == failure_page_url_) {
      // The data URL is only an implementation detail. Keep the attempted URL
      // visible and available for session restore and manual retry.
      emit AddressChanged(current_url_);
      return;
    }
    failure_page_active_ = false;
    render_process_failed_ = false;
    failure_page_url_.clear();
    current_url_ = url;
    emit AddressChanged(url);
  }
}

void BrowserView::OnCefLoadingStateChanged(CefRefPtr<CefBrowser> browser,
                                           bool loading, bool can_go_back,
                                           bool can_go_forward) {
  if (browser_ && browser_->IsSame(browser)) {
    const bool completed_navigation = is_loading_ && !loading &&
                                      !failure_page_active_;
    is_loading_ = loading;
    can_go_back_ = can_go_back;
    can_go_forward_ = can_go_forward;
    emit LoadingStateChanged(loading, can_go_back, can_go_forward);
    if (completed_navigation) emit NavigationCompleted();
  }
}

void BrowserView::OnCefLoadError(CefRefPtr<CefBrowser> browser, int error_code,
                                 const QString& error_text,
                                 const QString& failed_url) {
  if (!browser_ || !browser_->IsSame(browser) || closing_ ||
      failed_url == failure_page_url_) {
    return;
  }
  const QString url = failed_url.isEmpty() ? current_url_ : failed_url;
  ShowFailurePage(
      QStringLiteral("Page unavailable"),
      QStringLiteral("Trail Browser could not load this page."),
      QStringLiteral("%1 (%2)").arg(error_text).arg(error_code), url, false);
}

void BrowserView::OnCefRenderProcessTerminated(
    CefRefPtr<CefBrowser> browser, int status, int error_code,
    const QString& error_string) {
  if (!browser_ || !browser_->IsSame(browser) || closing_ ||
      render_process_failed_) {
    return;
  }

  QString reason;
  switch (static_cast<cef_termination_status_t>(status)) {
    case TS_PROCESS_WAS_KILLED:
      reason = QStringLiteral("Renderer was terminated");
      break;
    case TS_PROCESS_CRASHED:
      reason = QStringLiteral("Renderer crashed");
      break;
    case TS_PROCESS_OOM:
      reason = QStringLiteral("Renderer ran out of memory");
      break;
    case TS_LAUNCH_FAILED:
      reason = QStringLiteral("Renderer could not start");
      break;
    case TS_INTEGRITY_FAILURE:
      reason = QStringLiteral("Renderer integrity check failed");
      break;
    case TS_ABNORMAL_TERMINATION:
    default:
      reason = QStringLiteral("Renderer stopped unexpectedly");
      break;
  }
  if (!error_string.isEmpty()) reason += QStringLiteral(": %1").arg(error_string);
  if (error_code != 0) reason += QStringLiteral(" (%1)").arg(error_code);
  ShowFailurePage(QStringLiteral("This page crashed"),
                  QStringLiteral("Your other tabs are still available."),
                  reason, current_url_, true);
}

void BrowserView::OnCefCertificateError(CefRefPtr<CefBrowser> browser,
                                        int error_code,
                                        const QString& request_url,
                                        CefRefPtr<CefCallback> callback) {
  if (!browser_ || !browser_->IsSame(browser) || closing_) {
    callback->Cancel();
    return;
  }
  callback->Cancel();
  QMetaObject::invokeMethod(
      this,
      [this, request_url, error_code] {
        ShowFailurePage(
            QStringLiteral("Your connection is not private"),
            QStringLiteral("Trail Browser blocked this connection because "
                           "the site's certificate is invalid."),
            QStringLiteral("Certificate error %1").arg(error_code),
            request_url, false);
        emit SecurityMessage(QStringLiteral("Unsafe HTTPS connection blocked"));
      },
      Qt::QueuedConnection);
}

void BrowserView::OnCefMediaPermissionRequest(
    CefRefPtr<CefBrowser> browser, const QString& requesting_origin,
    uint32_t requested_permissions,
    CefRefPtr<CefMediaAccessCallback> callback) {
  if (!browser_ || !browser_->IsSame(browser) || closing_) {
    callback->Cancel();
    return;
  }
  auto* dialog = new QMessageBox(
      QMessageBox::Question, QStringLiteral("Site permission"),
      QStringLiteral("%1 wants to use your %2.")
          .arg(requesting_origin.toHtmlEscaped(),
               MediaPermissionDescription(requested_permissions)),
      QMessageBox::NoButton, this);
  dialog->setInformativeText(
      QStringLiteral("Allow access for this request only?"));
  dialog->addButton(QStringLiteral("Block"), QMessageBox::RejectRole);
  QAbstractButton* allow_button =
      dialog->addButton(QStringLiteral("Allow"), QMessageBox::AcceptRole);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  connect(dialog, &QMessageBox::finished, this,
          [dialog, allow_button, callback, requested_permissions](int) {
            if (dialog->clickedButton() == allow_button) {
              callback->Continue(requested_permissions);
            } else {
              callback->Cancel();
            }
          });
  dialog->open();
}

void BrowserView::OnCefPermissionRequest(
    CefRefPtr<CefBrowser> browser, quint64 prompt_id,
    const QString& requesting_origin, uint32_t requested_permissions,
    CefRefPtr<CefPermissionPromptCallback> callback) {
  if (!browser_ || !browser_->IsSame(browser) || closing_) {
    callback->Continue(CEF_PERMISSION_RESULT_DENY);
    return;
  }
  auto* dialog = new QMessageBox(
      QMessageBox::Question, QStringLiteral("Site permission"),
      QStringLiteral("%1 wants to use %2.")
          .arg(requesting_origin.toHtmlEscaped(),
               PermissionDescription(requested_permissions)),
      QMessageBox::NoButton, this);
  dialog->setInformativeText(
      QStringLiteral("Allow access for this request only?"));
  dialog->addButton(QStringLiteral("Block"), QMessageBox::RejectRole);
  QAbstractButton* allow_button =
      dialog->addButton(QStringLiteral("Allow"), QMessageBox::AcceptRole);
  permission_dialogs_.insert(prompt_id, dialog);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  connect(dialog, &QMessageBox::finished, this,
          [this, dialog, allow_button, callback, prompt_id](int) {
    permission_dialogs_.remove(prompt_id);
    if (dialog->property("cefDismissed").toBool()) return;
    callback->Continue(dialog->clickedButton() == allow_button
                           ? CEF_PERMISSION_RESULT_ACCEPT
                           : CEF_PERMISSION_RESULT_DENY);
  });
  dialog->open();
}

void BrowserView::OnCefPermissionDismissed(CefRefPtr<CefBrowser> browser,
                                           quint64 prompt_id) {
  if (!browser_ || !browser_->IsSame(browser)) return;
  QPointer<QMessageBox> dialog = permission_dialogs_.take(prompt_id);
  if (dialog) {
    dialog->setProperty("cefDismissed", true);
    dialog->reject();
  }
}

void BrowserView::OnCefExternalProtocol(const QString& url) {
  if (closing_ || !IsAllowedExternalScheme(url)) {
    emit SecurityMessage(QStringLiteral("External link was blocked"));
    return;
  }
  auto* dialog = new QMessageBox(
      QMessageBox::Question, QStringLiteral("Open external application?"),
      QStringLiteral("This link wants to open another application."),
      QMessageBox::NoButton, this);
  dialog->setInformativeText(url);
  dialog->addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
  QAbstractButton* open_button =
      dialog->addButton(QStringLiteral("Open link"), QMessageBox::AcceptRole);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  connect(dialog, &QMessageBox::finished, this,
          [dialog, open_button, url](int) {
    if (dialog->clickedButton() == open_button) {
      QDesktopServices::openUrl(QUrl(url));
    }
  });
  dialog->open();
}

void BrowserView::OnCefPopupRequested(CefRefPtr<CefBrowser> browser,
                                      const QString& url,
                                      cef_window_open_disposition_t disposition) {
  if (browser_ && browser_->IsSame(browser)) {
    emit PopupRequested(url, static_cast<int>(disposition));
  }
}

void BrowserView::ShowFailurePage(const QString& heading,
                                  const QString& summary,
                                  const QString& detail,
                                  const QString& failed_url,
                                  bool render_process_failed) {
  if (!browser_ || failed_url.isEmpty()) return;
  current_url_ = failed_url;
  failure_page_active_ = true;
  render_process_failed_ = render_process_failed;
  failure_page_url_ = FailurePageUrl(heading, summary, detail, failed_url);
  emit AddressChanged(current_url_);
  const QByteArray encoded = failure_page_url_.toUtf8();
  browser_->GetMainFrame()->LoadURL(
      std::string(encoded.constData(), encoded.size()));
}

QString BrowserView::FailurePageUrl(const QString& heading,
                                    const QString& summary,
                                    const QString& detail,
                                    const QString& retry_url) {
  const QString html = QStringLiteral(
      "<!doctype html><meta charset=utf-8><meta name=viewport "
      "content='width=device-width'><title>%1</title><style>"
      ":root{color-scheme:light dark}body{font-family:system-ui,sans-serif;"
      "margin:0;display:grid;min-height:100vh;place-items:center;background:#f5f6f8;"
      "color:#202124}.card{max-width:620px;margin:32px;padding:36px;border-radius:18px;"
      "background:white;box-shadow:0 12px 40px #00000018}h1{margin-top:0;font-size:30px}"
      "p{line-height:1.55}.url{word-break:break-all;color:#5f6368}.detail{font-family:ui-monospace,"
      "monospace;font-size:13px;color:#6b7280}a{display:inline-block;margin-top:14px;padding:10px 18px;"
      "border-radius:9px;background:#2563eb;color:white;text-decoration:none}"
      "@media(prefers-color-scheme:dark){body{background:#202124;color:#e8eaed}.card{background:#292a2d}"
      ".url,.detail{color:#bdc1c6}}</style><main class=card><h1>%1</h1><p>%2</p>"
      "<p class=url>%3</p><p class=detail>%4</p><a href=\"%5\">Try again</a></main>")
                           .arg(heading.toHtmlEscaped(), summary.toHtmlEscaped(),
                                retry_url.toHtmlEscaped(), detail.toHtmlEscaped(),
                                retry_url.toHtmlEscaped());
  return QStringLiteral("data:text/html;charset=utf-8,%1")
      .arg(QString::fromLatin1(QUrl::toPercentEncoding(html)));
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
  client_ = new BrowserClient(this, download_handler_);

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

