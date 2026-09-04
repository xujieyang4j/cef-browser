#pragma once

#include <cstdint>
#include <functional>
#include <mutex>

#include "include/cef_app.h"

class BrowserApp final : public CefApp, public CefBrowserProcessHandler {
 public:
  using SchedulePump = std::function<void(std::int64_t)>;
  BrowserApp() = default;

  void OnBeforeCommandLineProcessing(
      const CefString& process_type,
      CefRefPtr<CefCommandLine> command_line) override;
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override;
  void OnScheduleMessagePumpWork(std::int64_t delay_ms) override;

  void SetSchedulePump(SchedulePump callback);

 private:
  std::mutex callback_mutex_;
  SchedulePump schedule_pump_;

  IMPLEMENT_REFCOUNTING(BrowserApp);
  DISALLOW_COPY_AND_ASSIGN(BrowserApp);
};

