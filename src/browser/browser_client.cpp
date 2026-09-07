#include "browser/browser_client.h"

#include <algorithm>
#include <utility>

#include <QString>

#include "include/cef_parser.h"
#include "include/wrapper/cef_helpers.h"
#include "include/cef_task.h"
#include "ui/browser_view.h"

namespace {

constexpr size_t kMaxPageTitleCharacters = 512;
constexpr size_t kMaxStatusMessageCharacters = 2048;
constexpr size_t kMaxFaviconCandidates = 16;
constexpr size_t kMaxFaviconUrlCharacters = 64 * 1024;
constexpr size_t kMaxActionUrlCharacters = 64 * 1024;
constexpr size_t kMaxSecurityOriginCharacters = 8 * 1024;
constexpr size_t kMaxPromptFieldCharacters = 512;
constexpr size_t kMaxJavaScriptMessageCharacters = 4 * 1024;
constexpr size_t kMaxJavaScriptPromptCharacters = 1024;

QString BoundedString(const CefString& value, size_t max_characters) {
  size_t length = std::min(value.length(), max_characters);
  if (length == 0) return {};
  if (length < value.length() &&
      QChar::isHighSurrogate(
          static_cast<char32_t>(value.c_str()[length - 1]))) {
    --length;
  }
  return QString::fromUtf16(value.c_str(), static_cast<qsizetype>(length));
}

std::optional<QString> CheckedString(const CefString& value,
                                     size_t max_characters) {
  if (value.length() > max_characters) return std::nullopt;
  return BoundedString(value, max_characters);
}

std::optional<QString> CheckedUtf8String(const CefString& value,
                                         size_t max_bytes) {
  if (value.length() > max_bytes) return std::nullopt;
  QString converted = BoundedString(value, max_bytes);
  if (converted.toUtf8().size() > static_cast<qsizetype>(max_bytes)) {
    return std::nullopt;
  }
  return converted;
}

class ExternalProtocolTask final : public CefTask {
 public:
  ExternalProtocolTask(CefRefPtr<BrowserClient> client,
                       CefRefPtr<CefBrowser> browser, QString url)
      : client_(std::move(client)),
        browser_(std::move(browser)),
        url_(std::move(url)) {}

  void Execute() override {
    CEF_REQUIRE_UI_THREAD();
    client_->NotifyExternalProtocol(browser_, url_);
  }

 private:
  CefRefPtr<BrowserClient> client_;
  CefRefPtr<CefBrowser> browser_;
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

class AudioStateTask final : public CefTask {
 public:
  AudioStateTask(CefRefPtr<BrowserClient> client,
                 CefRefPtr<CefBrowser> browser, bool playing)
      : client_(std::move(client)),
        browser_(std::move(browser)),
        playing_(playing) {}

  void Execute() override {
    CEF_REQUIRE_UI_THREAD();
    client_->NotifyAudioState(browser_, playing_);
  }

 private:
  CefRefPtr<BrowserClient> client_;
  CefRefPtr<CefBrowser> browser_;
  bool playing_;

  IMPLEMENT_REFCOUNTING(AudioStateTask);
  DISALLOW_COPY_AND_ASSIGN(AudioStateTask);
};

}  // namespace

BrowserClient::BrowserClient(
    BrowserView* owner, CefRefPtr<CefDownloadHandler> download_handler)
    : owner_(owner), download_handler_(std::move(download_handler)) {}

void BrowserClient::OnTitleChange(CefRefPtr<CefBrowser> browser,
                                  const CefString& title) {
  CEF_REQUIRE_UI_THREAD();
  if (owner_) {
    owner_->OnCefTitleChanged(
        browser, BoundedString(title, kMaxPageTitleCharacters));
  }
}

void BrowserClient::OnFaviconURLChange(
    CefRefPtr<CefBrowser> browser,
    const std::vector<CefString>& icon_urls) {
  CEF_REQUIRE_UI_THREAD();
  if (!owner_) return;
  QStringList urls;
  const size_t count = std::min(icon_urls.size(), kMaxFaviconCandidates);
  urls.reserve(static_cast<qsizetype>(count));
  for (size_t index = 0; index < count; ++index) {
    if (icon_urls.at(index).length() <= kMaxFaviconUrlCharacters) {
      urls.append(
          BoundedString(icon_urls.at(index), kMaxFaviconUrlCharacters));
    }
  }
  owner_->OnCefFaviconURLChanged(browser, urls);
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
        browser, BoundedString(value, kMaxStatusMessageCharacters));
  }
}

void BrowserClient::OnLoadingProgressChange(CefRefPtr<CefBrowser> browser,
                                            double progress) {
  CEF_REQUIRE_UI_THREAD();
  if (owner_) owner_->OnCefLoadingProgressChanged(browser, progress);
}

void BrowserClient::OnAudioStreamStarted(CefRefPtr<CefBrowser> browser,
                                         const CefAudioParameters&, int) {
  CefPostTask(TID_UI, new AudioStateTask(this, std::move(browser), true));
}

void BrowserClient::OnAudioStreamPacket(CefRefPtr<CefBrowser>, const float**,
                                        int, int64_t) {}

void BrowserClient::OnAudioStreamStopped(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  CefPostTask(TID_UI, new AudioStateTask(this, std::move(browser), false));
}

void BrowserClient::OnAudioStreamError(CefRefPtr<CefBrowser> browser,
                                       const CefString&) {
  CefPostTask(TID_UI, new AudioStateTask(this, std::move(browser), false));
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
    const auto safe_url = CheckedUtf8String(url, kMaxActionUrlCharacters);
    if (safe_url) owner_->OnCefAddressChanged(browser, *safe_url);
  }
}

bool BrowserClient::OnJSDialog(
    CefRefPtr<CefBrowser> browser, const CefString& origin_url,
    JSDialogType dialog_type, const CefString& message_text,
    const CefString& default_prompt_text,
    CefRefPtr<CefJSDialogCallback> callback, bool& suppress_message) {
  CEF_REQUIRE_UI_THREAD();
  suppress_message = false;
  if (!owner_) {
    callback->Continue(false, CefString());
    return true;
  }
  if (origin_url.length() > kMaxSecurityOriginCharacters) {
    suppress_message = true;
    return false;
  }

  const QString origin = BoundedString(
      CefFormatUrlForSecurityDisplay(origin_url), kMaxPromptFieldCharacters);
  const bool shown = owner_->OnCefJavaScriptDialog(
      browser, origin, dialog_type,
      BoundedString(message_text, kMaxJavaScriptMessageCharacters),
      BoundedString(default_prompt_text, kMaxJavaScriptPromptCharacters),
      std::move(callback));
  if (!shown) suppress_message = true;
  return shown;
}

bool BrowserClient::OnBeforeUnloadDialog(
    CefRefPtr<CefBrowser> browser, const CefString&, bool is_reload,
    CefRefPtr<CefJSDialogCallback> callback) {
  CEF_REQUIRE_UI_THREAD();
  if (owner_) {
    owner_->OnCefBeforeUnloadDialog(browser, is_reload, std::move(callback));
  } else {
    callback->Continue(false, CefString());
  }
  return true;
}

void BrowserClient::OnResetDialogState(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  if (owner_) owner_->OnCefResetJavaScriptDialog(browser);
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
    const auto safe_url =
        CheckedUtf8String(failed_url, kMaxActionUrlCharacters);
    owner_->OnCefLoadError(
        browser, static_cast<int>(error_code),
        BoundedString(error_text, kMaxPromptFieldCharacters),
        safe_url.value_or(QString()));
  }
}

void BrowserClient::OnRenderProcessTerminated(
    CefRefPtr<CefBrowser> browser, TerminationStatus status, int error_code,
    const CefString& error_string) {
  CEF_REQUIRE_UI_THREAD();
  if (owner_) {
    owner_->OnCefRenderProcessTerminated(
        browser, static_cast<int>(status), error_code,
        BoundedString(error_string, kMaxPromptFieldCharacters));
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
  const auto safe_url =
      CheckedUtf8String(request_url, kMaxActionUrlCharacters);
  owner_->OnCefCertificateError(browser, static_cast<int>(cert_error),
                                safe_url.value_or(QString()),
                                std::move(callback));
  return true;
}

bool BrowserClient::GetAuthCredentials(
    CefRefPtr<CefBrowser> browser, const CefString& origin_url, bool is_proxy,
    const CefString& host, int port, const CefString& realm,
    const CefString& scheme, CefRefPtr<CefAuthCallback> callback) {
  CEF_REQUIRE_IO_THREAD();
  CefRefPtr<CefAuthCallback> pending_callback = callback;
  const auto safe_origin =
      CheckedString(origin_url, kMaxSecurityOriginCharacters);
  const auto safe_host = CheckedString(host, kMaxPromptFieldCharacters);
  if ((!is_proxy && !safe_origin) || (is_proxy && !safe_host)) {
    callback->Cancel();
    return true;
  }
  if (!CefPostTask(
          TID_UI, new AuthRequestTask(
                      this, std::move(browser),
                      safe_origin.value_or(QString()), is_proxy,
                      safe_host.value_or(QString()), port,
                      BoundedString(realm, kMaxPromptFieldCharacters),
                      BoundedString(scheme, kMaxPromptFieldCharacters),
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

void BrowserClient::OnProtocolExecution(CefRefPtr<CefBrowser> browser,
                                        CefRefPtr<CefFrame>,
                                        CefRefPtr<CefRequest> request,
                                        bool& allow_os_execution) {
  CEF_REQUIRE_IO_THREAD();
  allow_os_execution = false;
  if (!request) return;
  const auto url =
      CheckedString(request->GetURL(), kMaxActionUrlCharacters);
  if (!url) return;
  CefPostTask(TID_UI,
              new ExternalProtocolTask(this, std::move(browser), *url));
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
  const auto safe_origin =
      CheckedString(requesting_origin, kMaxSecurityOriginCharacters);
  if (!safe_origin) {
    callback->Cancel();
    return true;
  }
  owner_->OnCefMediaPermissionRequest(
      browser, *safe_origin, requested_permissions, std::move(callback));
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
  const auto safe_origin =
      CheckedString(requesting_origin, kMaxSecurityOriginCharacters);
  if (!safe_origin) {
    callback->Continue(CEF_PERMISSION_RESULT_DENY);
    return true;
  }
  owner_->OnCefPermissionRequest(
      browser, prompt_id, *safe_origin, requested_permissions,
      std::move(callback));
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
    const auto safe_url = CheckedString(target_url, kMaxActionUrlCharacters);
    if (safe_url) {
      owner_->OnCefPopupRequested(browser, *safe_url, target_disposition);
    }
  }
  return true;
}

void BrowserClient::DetachOwner() {
  CEF_REQUIRE_UI_THREAD();
  owner_.clear();
}

void BrowserClient::NotifyExternalProtocol(CefRefPtr<CefBrowser> browser,
                                           const QString& url) {
  CEF_REQUIRE_UI_THREAD();
  if (owner_) owner_->OnCefExternalProtocol(std::move(browser), url);
}

void BrowserClient::NotifyAudioState(CefRefPtr<CefBrowser> browser,
                                     bool playing) {
  CEF_REQUIRE_UI_THREAD();
  if (owner_) owner_->OnCefAudioStateChanged(browser, playing);
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

