#include "browser/browser_client.h"

#include <utility>

#include <QString>

#include "include/wrapper/cef_helpers.h"
#include "include/cef_task.h"
#include "ui/browser_view.h"

namespace {

class ExternalProtocolTask final : public CefTask {
 public:
  ExternalProtocolTask(CefRefPtr<BrowserClient> client, QString url)
      : client_(std::move(client)), url_(std::move(url)) {}

  void Execute() override {
    CEF_REQUIRE_UI_THREAD();
    client_->NotifyExternalProtocol(url_);
  }

 private:
  CefRefPtr<BrowserClient> client_;
  QString url_;

  IMPLEMENT_REFCOUNTING(ExternalProtocolTask);
  DISALLOW_COPY_AND_ASSIGN(ExternalProtocolTask);
};

class AuthRequestTask final : public CefTask {
 public:
  AuthRequestTask(CefRefPtr<BrowserClient> client,
                  CefRefPtr<CefBrowser> browser, QString origin_url,
                  bool is_proxy, QString host, int port, QString realm,
                  QString scheme, CefRefPtr<CefAuthCallback> callback)
      : client_(std::move(client)),
        browser_(std::move(browser)),
        origin_url_(std::move(origin_url)),
        is_proxy_(is_proxy),
        host_(std::move(host)),
        port_(port),
        realm_(std::move(realm)),
        scheme_(std::move(scheme)),
        callback_(std::move(callback)) {}

  void Execute() override {
    CEF_REQUIRE_UI_THREAD();
    client_->NotifyAuthRequest(browser_, std::move(origin_url_), is_proxy_,
                               std::move(host_), port_, std::move(realm_),
                               std::move(scheme_), std::move(callback_));
  }

 private:
  CefRefPtr<BrowserClient> client_;
  CefRefPtr<CefBrowser> browser_;
  QString origin_url_;
  bool is_proxy_;
  QString host_;
  int port_;
  QString realm_;
  QString scheme_;
  CefRefPtr<CefAuthCallback> callback_;

  IMPLEMENT_REFCOUNTING(AuthRequestTask);
  DISALLOW_COPY_AND_ASSIGN(AuthRequestTask);
};

}  // namespace

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

void BrowserClient::OnFullscreenModeChange(CefRefPtr<CefBrowser> browser,
                                           bool fullscreen) {
  CEF_REQUIRE_UI_THREAD();
  if (owner_) owner_->OnCefFullscreenChanged(browser, fullscreen);
}

void BrowserClient::OnStatusMessage(CefRefPtr<CefBrowser> browser,
                                    const CefString& value) {
  CEF_REQUIRE_UI_THREAD();
  if (owner_) {
    owner_->OnCefStatusMessage(
        browser, QString::fromStdString(value.ToString()));
  }
}

void BrowserClient::OnLoadingProgressChange(CefRefPtr<CefBrowser> browser,
                                            double progress) {
  CEF_REQUIRE_UI_THREAD();
  if (owner_) owner_->OnCefLoadingProgressChanged(browser, progress);
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

bool BrowserClient::OnCertificateError(CefRefPtr<CefBrowser> browser,
                                       cef_errorcode_t cert_error,
                                       const CefString& request_url,
                                       CefRefPtr<CefSSLInfo>,
                                       CefRefPtr<CefCallback> callback) {
  CEF_REQUIRE_UI_THREAD();
  if (!owner_) {
    callback->Cancel();
    return true;
  }
  owner_->OnCefCertificateError(
      browser, static_cast<int>(cert_error),
      QString::fromStdString(request_url.ToString()), std::move(callback));
  return true;
}

bool BrowserClient::GetAuthCredentials(
    CefRefPtr<CefBrowser> browser, const CefString& origin_url, bool is_proxy,
    const CefString& host, int port, const CefString& realm,
    const CefString& scheme, CefRefPtr<CefAuthCallback> callback) {
  CEF_REQUIRE_IO_THREAD();
  CefRefPtr<CefAuthCallback> pending_callback = callback;
  if (!CefPostTask(
          TID_UI, new AuthRequestTask(
                      this, std::move(browser),
                      QString::fromStdString(origin_url.ToString()), is_proxy,
                      QString::fromStdString(host.ToString()), port,
                      QString::fromStdString(realm.ToString()),
                      QString::fromStdString(scheme.ToString()),
                      std::move(callback)))) {
    pending_callback->Cancel();
  }
  return true;
}

CefRefPtr<CefResourceRequestHandler>
BrowserClient::GetResourceRequestHandler(
    CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>, bool,
    bool, const CefString&, bool&) {
  return this;
}

void BrowserClient::OnProtocolExecution(CefRefPtr<CefBrowser>,
                                        CefRefPtr<CefFrame>,
                                        CefRefPtr<CefRequest> request,
                                        bool& allow_os_execution) {
  CEF_REQUIRE_IO_THREAD();
  allow_os_execution = false;
  const QString url = QString::fromStdString(request->GetURL().ToString());
  CefPostTask(TID_UI, new ExternalProtocolTask(this, url));
}

bool BrowserClient::OnRequestMediaAccessPermission(
    CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>,
    const CefString& requesting_origin, uint32_t requested_permissions,
    CefRefPtr<CefMediaAccessCallback> callback) {
  CEF_REQUIRE_UI_THREAD();
  if (!owner_) {
    callback->Cancel();
    return true;
  }
  owner_->OnCefMediaPermissionRequest(
      browser, QString::fromStdString(requesting_origin.ToString()),
      requested_permissions, std::move(callback));
  return true;
}

bool BrowserClient::OnShowPermissionPrompt(
    CefRefPtr<CefBrowser> browser, uint64_t prompt_id,
    const CefString& requesting_origin, uint32_t requested_permissions,
    CefRefPtr<CefPermissionPromptCallback> callback) {
  CEF_REQUIRE_UI_THREAD();
  if (!owner_) {
    callback->Continue(CEF_PERMISSION_RESULT_DENY);
    return true;
  }
  owner_->OnCefPermissionRequest(
      browser, prompt_id,
      QString::fromStdString(requesting_origin.ToString()),
      requested_permissions, std::move(callback));
  return true;
}

void BrowserClient::OnDismissPermissionPrompt(
    CefRefPtr<CefBrowser> browser, uint64_t prompt_id,
    cef_permission_request_result_t) {
  CEF_REQUIRE_UI_THREAD();
  if (owner_) owner_->OnCefPermissionDismissed(browser, prompt_id);
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

void BrowserClient::NotifyExternalProtocol(const QString& url) {
  CEF_REQUIRE_UI_THREAD();
  if (owner_) owner_->OnCefExternalProtocol(url);
}

void BrowserClient::NotifyAuthRequest(
    CefRefPtr<CefBrowser> browser, QString origin_url, bool is_proxy,
    QString host, int port, QString realm, QString scheme,
    CefRefPtr<CefAuthCallback> callback) {
  CEF_REQUIRE_UI_THREAD();
  if (owner_) {
    owner_->OnCefAuthRequest(browser, origin_url, is_proxy, host, port, realm,
                             scheme, std::move(callback));
  } else {
    callback->Cancel();
  }
}

