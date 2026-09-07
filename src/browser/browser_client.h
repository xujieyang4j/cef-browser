#pragma once

#include <QPointer>

#include "include/cef_client.h"

class BrowserView;

class BrowserClient final : public CefClient,
                            public CefDisplayHandler,
                            public CefJSDialogHandler,
                            public CefKeyboardHandler,
                            public CefLifeSpanHandler,
                            public CefLoadHandler {
 public:
  explicit BrowserClient(BrowserView* owner);

  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefJSDialogHandler> GetJSDialogHandler() override { return this; }
  CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }

  void OnTitleChange(CefRefPtr<CefBrowser> browser,
                     const CefString& title) override;
  void OnAddressChange(CefRefPtr<CefBrowser> browser,
                       CefRefPtr<CefFrame> frame,
                       const CefString& url) override;
  void OnLoadingStateChange(CefRefPtr<CefBrowser> browser,
                            bool is_loading,
                            bool can_go_back,
                            bool can_go_forward) override;
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
  bool DoClose(CefRefPtr<CefBrowser> browser) override;
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;
  void OnDialogClosed(CefRefPtr<CefBrowser> browser) override;
  bool OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                     const CefKeyEvent& event, CefEventHandle os_event,
                     bool* is_keyboard_shortcut) override;
  bool OnBeforePopup(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     int popup_id,
                     const CefString& target_url,
                     const CefString& target_frame_name,
                     WindowOpenDisposition target_disposition,
                     bool user_gesture,
                     const CefPopupFeatures& popup_features,
                     CefWindowInfo& window_info,
                     CefRefPtr<CefClient>& client,
                     CefBrowserSettings& settings,
                     CefRefPtr<CefDictionaryValue>& extra_info,
                     bool* no_javascript_access) override;

  void DetachOwner();

 private:
  QPointer<BrowserView> owner_;

  IMPLEMENT_REFCOUNTING(BrowserClient);
  DISALLOW_COPY_AND_ASSIGN(BrowserClient);
};

