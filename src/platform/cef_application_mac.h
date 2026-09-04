#pragma once

// Adds CefAppProtocol behavior to the NSApplication subclass created by Qt.
// Must be called after QApplication construction and before CefInitialize.
bool InstallCefMacApplicationHooks();
