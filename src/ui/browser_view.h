#pragma once

#include <optional>

#include <QByteArray>
#include <QSet>
#include <QHash>
#include <QIcon>
#include <QPointer>
#include <QStringList>
#include <QWidget>

#include "include/cef_browser.h"
#include "include/cef_auth_callback.h"
#include "include/cef_callback.h"
#include "include/cef_download_handler.h"
#include "include/cef_permission_handler.h"

class BrowserClient;
class QFocusEvent;
class QHideEvent;
class QMessageBox;
class QResizeEvent;
class QShowEvent;

class BrowserView final : public QWidget {
  Q_OBJECT

 public:
  enum class ShortcutAction {
    NewTab,
    CloseTab,
    ReopenClosedTab,
    FocusAddress,
    NextTab,
    PreviousTab,
    FindInPage,
    FindNext,
    FindPrevious,
    ZoomIn,
    ZoomOut,
    ResetZoom,
    ToggleBookmark,
    ShowDownloads,
    ShowBookmarks,
    ShowHistory,
    ShowAllTabs,
    ActivateTab1,
    ActivateTab2,
    ActivateTab3,
    ActivateTab4,
    ActivateTab5,
    ActivateTab6,
    ActivateTab7,
    ActivateTab8,
    ActivateLastTab,
    ClearBrowsingData,
    ExitFullscreen,
    GoBack,
    GoForward,
    Reload,
    GoHome,
  };

  BrowserView(QString initial_url,
              CefRefPtr<CefDownloadHandler> download_handler,
              QWidget* parent = nullptr);
  ~BrowserView() override;

  void LoadUrl(const QString& url);
  void GoBack();
  void GoForward();
  void Reload();
  void Stop();
  void ShowDevTools();
  void Find(const QString& text, bool forward, bool find_next);
  void StopFinding(bool clear_selection = true);
  void Undo();
  void Redo();
  void Cut();
  void Copy();
  void Paste();
  void SelectAll();
  void ZoomIn();
  void ZoomOut();
  void ResetZoom();
  void Print();
  void ExitFullscreen();
  void FinalizeClose();
  const QString& current_url() const { return current_url_; }
  const QString& page_title() const { return page_title_; }
  bool is_loading() const { return is_loading_; }
  bool can_go_back() const { return can_go_back_; }
  bool can_go_forward() const { return can_go_forward_; }
  bool failure_page_active() const { return failure_page_active_; }
  bool render_process_failed() const { return render_process_failed_; }
  bool audio_playing() const { return audio_playing_; }
  bool audio_muted() const { return audio_muted_; }
  int zoom_percent() const;
  static QString MediaPermissionDescription(uint32_t permissions);
  static QString PermissionDescription(uint32_t permissions);
  static std::optional<QString> NormalizeExternalUrl(QString url);
  static bool IsAllowedExternalScheme(const QString& url);
  void ShowFailureForTesting(bool render_process_failed);
  bool ShowAuthForTesting(CefRefPtr<CefAuthCallback> callback);
  void SetFaviconForTesting(const QIcon& icon);
  void SetAudioStateForTesting(bool playing, bool muted);
  void ToggleAudioMuted();

  // Starts an asynchronous close and returns true if no browser exists.
  bool RequestClose();

  // Called by BrowserClient on CEF's UI thread, which is integrated with Qt's
  // main thread by the external message pump.
  void OnCefBrowserCreated(CefRefPtr<CefBrowser> browser);
  void OnCefBrowserClosed(CefRefPtr<CefBrowser> browser);
  void OnCefDialogClosed(CefRefPtr<CefBrowser> browser);
  bool OnCefKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent& event);
  void OnCefTitleChanged(CefRefPtr<CefBrowser> browser, const QString& title);
  void OnCefFaviconURLChanged(CefRefPtr<CefBrowser> browser,
                              const QStringList& icon_urls);
  void OnCefFaviconDownloaded(int browser_id, const QString& image_url,
                              quint64 generation, const QByteArray& png_data);
  void OnCefAudioStateChanged(CefRefPtr<CefBrowser> browser, bool playing);
  void OnCefFullscreenChanged(CefRefPtr<CefBrowser> browser, bool fullscreen);
  void OnCefStatusMessage(CefRefPtr<CefBrowser> browser,
                          const QString& value);
  void OnCefLoadingProgressChanged(CefRefPtr<CefBrowser> browser,
                                   double progress);
  void OnCefFindResult(CefRefPtr<CefBrowser> browser, int count,
                       int active_match_ordinal, bool final_update);
  void OnCefAddressChanged(CefRefPtr<CefBrowser> browser, const QString& url);
  void OnCefLoadingStateChanged(CefRefPtr<CefBrowser> browser, bool loading,
                                bool can_go_back,
                                bool can_go_forward);
  void OnCefLoadError(CefRefPtr<CefBrowser> browser, int error_code,
                      const QString& error_text, const QString& failed_url);
  void OnCefRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                    int status, int error_code,
                                    const QString& error_string);
  void OnCefCertificateError(CefRefPtr<CefBrowser> browser, int error_code,
                             const QString& request_url,
                             CefRefPtr<CefCallback> callback);
  void OnCefMediaPermissionRequest(
      CefRefPtr<CefBrowser> browser, const QString& requesting_origin,
      uint32_t requested_permissions,
      CefRefPtr<CefMediaAccessCallback> callback);
  void OnCefPermissionRequest(
      CefRefPtr<CefBrowser> browser, quint64 prompt_id,
      const QString& requesting_origin, uint32_t requested_permissions,
      CefRefPtr<CefPermissionPromptCallback> callback);
  void OnCefPermissionDismissed(CefRefPtr<CefBrowser> browser,
                                quint64 prompt_id);
  void OnCefExternalProtocol(CefRefPtr<CefBrowser> browser,
                             const QString& url);
  void OnCefAuthRequest(CefRefPtr<CefBrowser> browser,
                        const QString& origin_url, bool is_proxy,
                        const QString& host, int port, const QString& realm,
                        const QString& scheme,
                        CefRefPtr<CefAuthCallback> callback);
  void OnCefPopupRequested(CefRefPtr<CefBrowser> browser, const QString& url,
                           cef_window_open_disposition_t disposition);

 signals:
  void TitleChanged(const QString& title);
  void FaviconChanged(const QIcon& icon);
  void AudioStateChanged(bool playing, bool muted);
  void AddressChanged(const QString& url);
  void LoadingStateChanged(bool loading, bool can_go_back,
                           bool can_go_forward);
  void FindResultChanged(int count, int active_match_ordinal,
                         bool final_update);
  void ZoomChanged(int percent);
  void NavigationCompleted();
  void SecurityMessage(const QString& message);
  void FullscreenChanged(bool fullscreen);
  void StatusMessageChanged(const QString& message);
  void LoadingProgressChanged(double progress);
  void PopupRequested(const QString& url, int disposition);
  void ShortcutRequested(int action);
  void BrowserClosed();
  void CloseCancelled();

 protected:
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void focusInEvent(QFocusEvent* event) override;

 private:
  friend class BrowserClient;

  void CreateBrowserIfNeeded();
  void UpdateNativeVisibility();
  void ResizeBrowser();
  void DismissOpenDialogs();
  void ShowFailurePage(const QString& heading, const QString& summary,
                       const QString& detail, const QString& failed_url,
                       bool render_process_failed);
  static QString FailurePageUrl(const QString& heading,
                                const QString& summary,
                                const QString& detail,
                                const QString& retry_url);

  QString initial_url_;
  QString current_url_;
  QString page_title_;
  QString favicon_url_;
  quint64 favicon_request_generation_ = 0;
  CefRefPtr<CefDownloadHandler> download_handler_;
  CefRefPtr<BrowserClient> client_;
  CefRefPtr<CefBrowser> browser_;
  QSet<int> browser_ids_;
  int primary_browser_id_ = 0;
  bool create_requested_ = false;
  bool closing_ = false;
  bool primary_browser_closed_ = false;
  bool is_loading_ = false;
  bool audio_playing_ = false;
  bool audio_muted_ = false;
  bool can_go_back_ = false;
  bool can_go_forward_ = false;
  bool failure_page_active_ = false;
  bool render_process_failed_ = false;
  QString failure_page_url_;
  QHash<quint64, QPointer<QMessageBox>> permission_dialogs_;
  QPointer<QMessageBox> external_protocol_dialog_;
  QString certificate_failure_url_;

  Q_DISABLE_COPY_MOVE(BrowserView)
};

