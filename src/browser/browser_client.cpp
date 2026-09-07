#include "browser/browser_client.h"

#include <utility>

#include <QString>

#include "include/wrapper/cef_helpers.h"
#include "ui/browser_view.h"

BrowserClient::BrowserClient(
    BrowserView* owner, CefRefPtr<CefDownloadHandler> download_handler)
    : owner_(owner), download_handler_(std::move(download_handler)) {}

void BrowserClient::OnTitleChange(CefRefPtr<CefBrowser> browser,
                                  const CefString& title) {
  CEF_REQUIRE_UI_THREAD();
  if (owner_) {
    owner_->OnCefTitleChanged(browser,
                              QString::fromStdString(title.ToString()));
  }
}

void BrowserClient::OnFindResult(CefRefPtr<CefBrowser> browser, int, int count,
                                 const CefRect&, int active_match_ordinal,
                                 bool final_update) {
  CEF_REQUIRE_UI_THREAD();
  if (owner_) {
    owner_->OnCefFindResult(browser, count, active_match_ordinal, final_update);
  }
}

void BrowserClient::OnAddressChange(CefRefPtr<CefBrowser> browser,
                                    CefRefPtr<CefFrame> frame,
                                    const CefString& url) {
  CEF_REQUIRE_UI_THREAD();
  if (owner_ && frame->IsMain()) {
    owner_->OnCefAddressChanged(browser,
                                QString::fromStdString(url.ToString()));
  }
}

void BrowserClient::OnLoadingStateChange(CefRefPtr<CefBrowser> browser,
                                         bool is_loading,
                                         bool can_go_back,
                                         bool can_go_forward) {
  CEF_REQUIRE_UI_THREAD();
  if (owner_) {
    owner_->OnCefLoadingStateChanged(browser, is_loading, can_go_back,
                                     can_go_forward);
  }
}

void BrowserClient::OnLoadError(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                ErrorCode error_code,
                                const CefString& error_text,
                                const CefString& failed_url) {
  CEF_REQUIRE_UI_THREAD();
  if (owner_ && frame->IsMain() && error_code != ERR_ABORTED) {
    owner_->OnCefLoadError(
        browser, static_cast<int>(error_code),
        QString::fromStdString(error_text.ToString()),
        QString::fromStdString(failed_url.ToString()));
  }
}

void BrowserClient::OnRenderProcessTerminated(
    CefRefPtr<CefBrowser> browser, TerminationStatus status, int error_code,
    const CefString& error_string) {
  CEF_REQUIRE_UI_THREAD();
  if (owner_) {
    owner_->OnCefRenderProcessTerminated(
        browser, static_cast<int>(status), error_code,
        QString::fromStdString(error_string.ToString()));
  }
}

void BrowserClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  if (owner_) {
    owner_->OnCefBrowserCreated(browser);
  } else {
    browser->GetHost()->CloseBrowser(true);
  }
}

bool BrowserClient::DoClose(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  return false;
}

bool BrowserClient::OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                                  const CefKeyEvent& event, CefEventHandle,
                                  bool*) {
  CEF_REQUIRE_UI_THREAD();
  return owner_ && owner_->OnCefKeyEvent(browser, event);
}

void BrowserClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  if (owner_) {
    owner_->OnCefBrowserClosed(browser);
  }
}

void BrowserClient::OnDialogClosed(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  if (owner_) owner_->OnCefDialogClosed(browser);
}

bool BrowserClient::OnBeforePopup(
    CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, int,
    const CefString& target_url, const CefString&,
    cef_window_open_disposition_t target_disposition, bool,
    const CefPopupFeatures&, CefWindowInfo&, CefRefPtr<CefClient>&,
    CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&, bool*) {
  CEF_REQUIRE_UI_THREAD();
  if (owner_) {
    owner_->OnCefPopupRequested(browser,
        QString::fromStdString(target_url.ToString()), target_disposition);
  }
  return true;
}

void BrowserClient::DetachOwner() {
  CEF_REQUIRE_UI_THREAD();
  owner_.clear();
}

