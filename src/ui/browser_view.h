#pragma once

#include <QSet>
#include <QWidget>

#include "include/cef_browser.h"

class BrowserClient;
class QFocusEvent;
class QHideEvent;
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
  };

  explicit BrowserView(QString initial_url, QWidget* parent = nullptr);
  ~BrowserView() override;

  void LoadUrl(const QString& url);
  void GoBack();
  void GoForward();
  void Reload();
  void Stop();
  void ShowDevTools();
  void FinalizeClose();
  const QString& current_url() const { return current_url_; }
  const QString& page_title() const { return page_title_; }
  bool is_loading() const { return is_loading_; }
  bool can_go_back() const { return can_go_back_; }
  bool can_go_forward() const { return can_go_forward_; }

  // Starts an asynchronous close and returns true if no browser exists.
  bool RequestClose();

  // Called by BrowserClient on CEF's UI thread, which is integrated with Qt's
  // main thread by the external message pump.
  void OnCefBrowserCreated(CefRefPtr<CefBrowser> browser);
  void OnCefBrowserClosed(CefRefPtr<CefBrowser> browser);
  void OnCefDialogClosed(CefRefPtr<CefBrowser> browser);
  bool OnCefKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent& event);
  void OnCefTitleChanged(CefRefPtr<CefBrowser> browser, const QString& title);
  void OnCefAddressChanged(CefRefPtr<CefBrowser> browser, const QString& url);
  void OnCefLoadingStateChanged(CefRefPtr<CefBrowser> browser, bool loading,
                                bool can_go_back,
                                bool can_go_forward);
  void OnCefPopupRequested(CefRefPtr<CefBrowser> browser, const QString& url,
                           cef_window_open_disposition_t disposition);

 signals:
  void TitleChanged(const QString& title);
  void AddressChanged(const QString& url);
  void LoadingStateChanged(bool loading, bool can_go_back,
                           bool can_go_forward);
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

  QString initial_url_;
  QString current_url_;
  QString page_title_;
  CefRefPtr<BrowserClient> client_;
  CefRefPtr<CefBrowser> browser_;
  QSet<int> browser_ids_;
  int primary_browser_id_ = 0;
  bool create_requested_ = false;
  bool closing_ = false;
  bool primary_browser_closed_ = false;
  bool is_loading_ = false;
  bool can_go_back_ = false;
  bool can_go_forward_ = false;

  Q_DISABLE_COPY_MOVE(BrowserView)
};

