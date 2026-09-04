#include "app/browser_app.h"

#include <utility>

#include "include/cef_command_line.h"

void BrowserApp::OnBeforeCommandLineProcessing(
    const CefString& process_type, CefRefPtr<CefCommandLine> command_line) {
  if (process_type.empty()) {
    // Embedded applications do not provide Chrome's first-run/EULA UI.
    command_line->AppendSwitch("no-first-run");
  }
}

CefRefPtr<CefBrowserProcessHandler> BrowserApp::GetBrowserProcessHandler() {
  return this;
}

void BrowserApp::OnScheduleMessagePumpWork(std::int64_t delay_ms) {
  SchedulePump callback;
  {
    std::lock_guard lock(callback_mutex_);
    callback = schedule_pump_;
  }
  if (callback) {
    callback(delay_ms);
  }
}

void BrowserApp::SetSchedulePump(SchedulePump callback) {
  std::lock_guard lock(callback_mutex_);
  schedule_pump_ = std::move(callback);
}

