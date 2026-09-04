#include "platform/cef_application_mac.h"

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include <string>

#include "include/cef_application_mac.h"

namespace {

using SendEventImplementation = void (*)(id, SEL, NSEvent*);

bool g_installed = false;
SendEventImplementation g_original_send_event = nullptr;

const void* HandlingSendEventKey() {
  static char key;
  return &key;
}

BOOL IsHandlingSendEvent(id application, SEL) {
  return [objc_getAssociatedObject(application, HandlingSendEventKey())
      boolValue];
}

void SetHandlingSendEvent(id application, SEL, BOOL handling) {
  objc_setAssociatedObject(application, HandlingSendEventKey(),
                           @(handling == YES), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

void SendEventWithCefState(id application, SEL selector, NSEvent* event) {
  CefScopedSendingEvent sending_event;
  g_original_send_event(application, selector, event);
}

const char* IsHandlingTypeEncoding() {
  static const std::string encoding = std::string(@encode(BOOL)) + "@:";
  return encoding.c_str();
}

const char* SetHandlingTypeEncoding() {
  static const std::string encoding = std::string("v@:") + @encode(BOOL);
  return encoding.c_str();
}

}  // namespace

bool InstallCefMacApplicationHooks() {
  if (g_installed) return true;

  Class application_class = [NSApp class];
  if (!application_class) return false;
  class_addProtocol(application_class, @protocol(CefAppProtocol));
  class_addMethod(application_class, @selector(isHandlingSendEvent),
                  reinterpret_cast<IMP>(IsHandlingSendEvent),
                  IsHandlingTypeEncoding());
  class_addMethod(application_class, @selector(setHandlingSendEvent:),
                  reinterpret_cast<IMP>(SetHandlingSendEvent),
                  SetHandlingTypeEncoding());

  Method send_event_method =
      class_getInstanceMethod(application_class, @selector(sendEvent:));
  if (!send_event_method) return false;
  g_original_send_event = reinterpret_cast<SendEventImplementation>(
      method_getImplementation(send_event_method));
  const char* type_encoding = method_getTypeEncoding(send_event_method);
  if (!class_addMethod(application_class, @selector(sendEvent:),
                       reinterpret_cast<IMP>(SendEventWithCefState),
                       type_encoding)) {
    method_setImplementation(send_event_method,
                             reinterpret_cast<IMP>(SendEventWithCefState));
  }

  g_installed = true;
  return true;
}
