# Trail Browser

Trail Browser is a small native browser shell built with Qt 6 Widgets and the
Chromium Embedded Framework (CEF). It embeds a windowed CEF browser in a Qt
main window and integrates CEF's external message pump with Qt's event loop.

Current features include tabbed browsing, navigation controls, URL/search
normalization, page titles, pop-up routing based on the requested disposition,
recently closed tab recovery, F12 DevTools, persistent CEF cache, and orderly
asynchronous browser shutdown. Downloads have a window-level manager with
progress, speed, pause/resume, cancellation, completion actions, and a platform
save dialog; they continue when the originating tab closes, and quitting with
active or paused downloads requires explicit confirmation. Finished download
records survive application restarts and can be cleared without deleting the
downloaded files. Individual finished records can also be removed from the
download panel. Main-frame load
failures and renderer crashes display a retry page while preserving the
requested URL.
Window geometry, open tab URLs, and the active tab are saved atomically and
restored on the next regular launch. A previous unclean exit is detected and
reported after its tabs are recovered. An explicit startup URL takes priority
over the saved session. The browser also provides in-page search, per-tab page
zoom, persistent bookmarks, and a bounded visit history.
Sensitive site capabilities use explicit one-time allow/block prompts, invalid
HTTPS certificates are blocked with a dedicated error page, and only a small
allowlist of external URL schemes can reach an OS application after user
confirmation.
Web pages can enter native full screen, hovered-link destinations appear in the
status bar, and Ctrl/Cmd+P opens the platform print flow.
Navigation shows page-load progress and supports standard back, forward, and
refresh shortcuts even while the embedded page owns keyboard focus. HTTP and
proxy authentication use an in-memory, non-blocking credential prompt; entered
credentials are never stored by Trail Browser.
The address bar suggests page titles alongside their URLs from bookmarks and
recent history. Suggestions match either field, keep bookmarks ranked first,
deduplicate shared URLs, and navigate to the underlying URL rather than the
display label. Its default search engine can be switched between Google,
DuckDuckGo, and Bing from the Settings menu; the choice is stored atomically
with the browser profile. The same settings file stores a validated HTTP(S)
home page. The toolbar Home button and Alt+Home (Cmd+Shift+H on macOS) navigate
the active tab there, while the Settings menu can capture the current page or
restore the default. New tabs open blank by default, with a persistent option
to open the configured home page instead. Startup can independently restore
the last session, open the home page, or open a blank page; an explicit
command-line URL always takes priority. The recently closed tab stack is part of the atomic
session file, so Ctrl/Cmd+Shift+T continues to work after an application
restart. Page favicons are loaded into their corresponding tabs with stale
navigation results rejected. The History menu can selectively clear history,
recently closed tabs, download history, or site data including HTTP cache,
cookies, HTTP credentials, and certificate exceptions
while preserving bookmarks; the updated session is persisted immediately, and
completion is reported only after every asynchronous CEF operation finishes.
Individual bookmark and history entries can be removed from their menus with a
right click. Bookmark names can be edited from the same menu, with an empty
name falling back to the URL. Standard browser bookmark HTML can be imported
or exported for migration and backup. Every change is persisted atomically.
Tabs also have a context menu for opening a new tab, duplicating the selected
tab, copying its address, closing it, closing other tabs, or closing tabs to
its right. Multi-tab closes wait for each CEF browser to finish shutting down
and preserve the order used by recently closed tab recovery.
Tabs indicate active audio playback and expose a per-tab mute/unmute action in
the same context menu. Audio events are associated only with the owning main
browser so auxiliary DevTools windows cannot change the tab state.
Tabs can be pinned from the context menu. Pinned tabs stay grouped at the left,
survive session restoration, and are protected from "close other tabs" and
"close tabs to the right"; explicitly closing a pinned tab still works.
An all-tabs menu lists every open page with its current pin, audio, title, and
selection state for quick navigation when the tab strip is crowded. It also
lists the ten most recently closed tabs in newest-first order, allowing any
entry to be restored instead of only the latest one. Closed-tab titles are
stored with the session, including across restarts and after history changes;
sessions written by earlier Trail Browser versions remain compatible.
Native File, Edit, View, History, Bookmarks, and Window menus expose the main
browser commands and their keyboard shortcuts. Editing commands target Qt text
fields or the active web page according to keyboard focus.
Linux, Windows, and macOS build paths are represented in the project.

## Prerequisites

- CMake 3.24 or newer
- A C++20 compiler
- Qt 6.4 or newer with Core, Gui, and Widgets
- Python 3.10 or newer (only for the helper scripts)
- Linux: X11 development headers and Qt's xcb platform plugin
- Windows: Visual Studio 2022 is recommended
- macOS: Xcode 12 or newer; the CEF architecture must match Qt

The CEF archive is large (typically 130–400 MiB) and is intentionally excluded
from Git.

## Build

Download the latest stable minimal CEF distribution for the host platform:

~~~sh
python3 scripts/fetch_cef.py
~~~

The downloader reads the official CEF build index, verifies the published
SHA-1, checks archive paths, and extracts to third_party/cef. Use --print-url
to inspect the selected build without downloading. Exact builds can be selected
with --version; cross-builds can use --platform, such as windows64,
macosarm64, or linuxarm64.

Configure and build:

~~~sh
python3 scripts/configure.py --generator Ninja
cmake --build build --config Release
~~~

If CEF lives elsewhere, pass --cef-root /path/to/cef or set CEF_ROOT. Any
unrecognized arguments given to configure.py are forwarded to CMake, so a Qt
installation can be selected with -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/....
The direct CMake equivalent is:

~~~sh
cmake -S . -B build -DCEF_ROOT=/path/to/cef -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
~~~

With a single-configuration generator, run build/trail-browser on Linux. With
Visual Studio, Xcode, or another multi-configuration generator, binaries are
usually under build/Release (including trail-browser.exe on Windows and Trail
Browser.app on macOS). The first positional argument may be a startup URL.
When a minimal CEF package does not include Debug runtime binaries, Debug
application builds automatically use the Release CEF runtime.

## Shortcuts

- Ctrl/Cmd+T: open a new tab
- Ctrl/Cmd+W: close the active tab
- Ctrl/Cmd+Shift+T: reopen the most recently closed tab
- Ctrl+Tab / Ctrl+Shift+Tab: switch tabs
- Ctrl/Cmd+1 through Ctrl/Cmd+8: activate the corresponding tab
- Ctrl/Cmd+9: activate the last tab
- Ctrl/Cmd+Shift+A: show all open tabs
- Ctrl/Cmd+L: focus and select the address bar
- Alt+Home (Cmd+Shift+H on macOS): open the configured home page
- Ctrl/Cmd+F: find text in the active page
- F3 / Shift+F3: move between page matches
- Ctrl/Cmd++ / Ctrl/Cmd+-: zoom the active page
- Ctrl/Cmd+0: reset page zoom
- Ctrl/Cmd+D: add or remove a bookmark for the active page
- Ctrl+J (Cmd+Shift+J on macOS): show downloads
- Ctrl+Shift+B / Cmd+Shift+B: show bookmarks
- Ctrl+H (Cmd+Y on macOS): show history
- Ctrl+Shift+Delete (Cmd+Shift+Backspace on macOS): clear browsing data
- Ctrl/Cmd+P: print the active page
- F12: open CEF DevTools

Tabs can also be reordered by dragging and closed with either their close
button or a middle click. Right-click a tab for duplicate, copy-address, and
pin, mute, and bulk-close actions. Closing the final tab closes the browser
window.

## Smoke test

After building on Linux with Xvfb installed, run the deterministic tab and
shutdown smoke test with:

~~~sh
timeout 20s xvfb-run -a ./build/trail-browser --smoke-test-tabs \
  'data:text/html,<title>First</title>'
~~~

A passing run prints `TAB_SMOKE_OK` and exits with status 0 after creating,
closing, reopening, and finally shutting down multiple CEF browser instances.
CTest also runs deterministic download-state, download-exit protection,
tab-action, pinned-tab persistence, failure-page, atomic session restore,
page-search/zoom, titled address suggestions, bookmark/history persistence,
browsing-data cleanup, favicon
mapping, audio/mute state, security-policy, and authentication-dialog checks.
Keyboard-accessible browser surfaces and full-screen exit routing are also
covered, along with numeric tab navigation, the all-tabs menu, and selective
recent-tab restoration, native application-menu actions, and search-setting
persistence. All nineteen
checks are registered when `xvfb-run` is available:

~~~sh
ctest --test-dir build --output-on-failure
~~~

## Security and platform notes

This project currently runs CEF with no_sandbox enabled and forces
USE_SANDBOX=OFF at configure time. That keeps the starter project portable but
is not appropriate for browsing untrusted content in production. Enabling the
sandbox requires platform-specific packaging and startup changes, notably
Windows bootstrap/sandbox setup and macOS helper entitlements.

On Linux, windowed CEF requires X11. Trail Browser selects Qt's xcb backend
when QT_QPA_PLATFORM is unset. On native Wayland-only systems, install XWayland
and the Qt xcb plugin or migrate the view to off-screen rendering.

Runtime data is stored under Qt's per-user application-data locations. The CEF
log is named cef.log.
