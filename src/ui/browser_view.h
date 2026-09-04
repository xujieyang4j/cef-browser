#pragma once

#include <QWidget>

#include "include/cef_browser.h"

class BrowserClient;
class QFocusEvent;
class QResizeEvent;
class QShowEvent;

class BrowserView final : public QWidget {
  Q_OBJECT

 public:
  explicit BrowserView(QString initial_url, QWidget* parent = nullptr);
  ~BrowserView() override;

  void LoadUrl(const QString& url);
  void GoBack();
  void GoForward();
  void Reload();
  void Stop();
  void ShowDevTools();
  // Returns true when no asynchronous browser shutdown is required.
  bool RequestClose();

  // Called by BrowserClient on CEF's UI thread, which is integrated with Qt's
  // main thread by the external message pump.
  void OnCefBrowserCreated(CefRefPtr<CefBrowser> browser);
  void OnCefBrowserClosed(CefRefPtr<CefBrowser> browser);
  void OnCefTitleChanged(CefRefPtr<CefBrowser> browser, const QString& title);
  void OnCefAddressChanged(CefRefPtr<CefBrowser> browser, const QString& url);
  void OnCefLoadingStateChanged(CefRefPtr<CefBrowser> browser, bool loading,
                                bool can_go_back,
                                bool can_go_forward);
  void OnCefPopupRequested(CefRefPtr<CefBrowser> browser, const QString& url);

 signals:
  void TitleChanged(const QString& title);
  void AddressChanged(const QString& url);
  void LoadingStateChanged(bool loading, bool can_go_back,
                           bool can_go_forward);
  void PopupRequested(const QString& url);
  void BrowserClosed();

 protected:
  void showEvent(QShowEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void focusInEvent(QFocusEvent* event) override;

 private:
  friend class BrowserClient;

  void CreateBrowserIfNeeded();
  void ResizeBrowser();

  QString initial_url_;
  CefRefPtr<BrowserClient> client_;
  CefRefPtr<CefBrowser> browser_;
  bool create_requested_ = false;
  bool closing_ = false;

  Q_DISABLE_COPY_MOVE(BrowserView)
};

