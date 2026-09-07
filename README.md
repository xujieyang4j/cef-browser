# Trail Browser

Trail Browser is a small native browser shell built with Qt 6 Widgets and the
Chromium Embedded Framework (CEF). It embeds a windowed CEF browser in a Qt
main window and integrates CEF's external message pump with Qt's event loop.

Current features include tabbed browsing, navigation controls, URL/search
normalization, page titles, and pop-up routing based on the requested
disposition. Pop-up targets are strictly revalidated and limited to HTTP(S) or
a blank page before they can replace a page or create a tab. Regular tabs,
restored tabs, single-instance open requests, duplication, and page pop-ups
share a 100-tab window limit; rejected pop-ups cannot create hidden CEF hosts.
The browser also
provides page-title and status-text sanitization so untrusted page metadata
cannot grow the native UI without limit. It also provides
recently closed tab recovery, F12 DevTools, persistent CEF cache, and orderly
asynchronous browser shutdown. Downloads have a window-level manager with
progress, speed, pause/resume, cancellation, completion actions, and a platform
save dialog; they continue when the originating tab closes, and quitting with
active or paused downloads requires explicit confirmation. Finished download
records survive application restarts and can be cleared without deleting the
downloaded files. Individual finished records can also be removed from the
download panel. CEF download URLs, local paths, suggested names, and progress
values are bounded before entering the Qt model, credentials are removed from
displayed source URLs, and at most 32 downloads may remain active at once.
Restored history is treated as untrusted profile data: source
URLs and local paths are revalidated, filenames are derived from valid paths,
and only paths delivered directly by CEF in the current process may invoke an
operating-system open or reveal action. Main-frame load
failures and renderer crashes display a retry page while preserving the
requested URL.
Only one main process owns a browser profile at a time. Launching Trail Browser
again activates the existing window and opens an explicitly supplied URL in a
new tab instead of starting a competing CEF instance. Address-bar text, first
startup arguments, and forwarded open requests share the same normalization:
search terms use the selected engine, host names gain an HTTP scheme, and only
HTTP(S), local files, and `about:blank` are accepted as explicit navigation
schemes. Unsupported explicit schemes and navigation inputs whose raw or
normalized UTF-8 representation exceeds 64 KiB are blocked rather than turned
into a search. The address field enforces the same character ceiling. The
same-user local activation channel bounds concurrent connections,
queued startup requests, request bytes, and idle connection lifetime so a
stalled secondary process cannot consume resources without limit.
Window geometry, open tab URLs, and the active tab are saved atomically and
restored on the next regular launch. A previous unclean exit is detected and
reported after its tabs are recovered. As soon as a saved session is accepted,
the primary process atomically marks that same snapshot as running without
changing its tabs; a crash during the window startup grace period therefore
cannot leave a stale clean-exit marker. An explicit startup URL takes priority
over the saved session. Session fields and total output are bounded so even a
page-generated extreme URL or title cannot create a file the next launch will
refuse to restore; the active tab is retained when trimming is necessary.
Restored tabs, recently closed tabs, bookmarks, visit history, and download
history are revalidated before they can navigate or invoke local-file actions,
so a damaged or modified profile cannot reintroduce blocked URL schemes.
Bookmark and history recovery
also removes duplicate URLs, bounds display text and visit counters, and
repairs invalid timestamps before the data reaches browser menus. The browser
also provides in-page search, per-tab page zoom, persistent bookmarks, and a
bounded visit history. Find-in-page text is limited to 4,096 characters and
8 KiB before reaching Chromium, and bookmark-name editing is limited to the
same 512 characters retained by profile storage. Download and browsing-data
files share their read and write byte limits and retain the newest records
when trimming is required. Download-history budget eviction is applied to the
live model only after the atomic commit succeeds, so the visible finished list
matches the next restart. Browsing data prioritizes bookmarks over older visit
history so every saved profile remains reloadable; history trimmed by that
budget is removed from the live menus only after the atomic save succeeds, and
a failed history write restores the previous in-memory state. Session, settings,
download-history, browsing-data, and bookmark
HTML input is read through strict byte budgets rather than trusting an earlier
file-size check, so files that grow while being opened cannot bypass the
limits or replace the corresponding in-memory state.
Bookmark HTML export is streamed through the same 5 MiB budget and committed
atomically; an oversized export cannot consume a large assembly buffer or
replace an existing destination file with partial output.
If a session, settings, download-history, or browsing-data file exists but
cannot be parsed safely, Trail Browser moves it to a timestamped `.corrupt-*`
backup in the same directory before new state may use the original path. If
that preservation step fails, writes for the affected data are disabled for
the process so the unreadable source is never silently overwritten.
Sensitive site capabilities use explicit one-time allow/block prompts, invalid
HTTPS certificates are blocked with a dedicated error page, and only a small
allowlist of external URL schemes can reach an OS application after user
confirmation. External hand-offs are strictly parsed and normalized, reject
empty targets, credentials, control characters, and oversized values, stay
bound to the browser that requested them, and share a per-tab prompt gate with
site permissions and HTTP authentication. Permission origins and
authentication metadata are normalized, size-limited, and displayed as plain
text. Concurrent page requests are denied instead of stacking or replacing
pending callbacks. JavaScript alert, confirmation, prompt, and before-unload
requests use the same non-blocking per-tab gate. Their page-controlled display
text and prompt values are bounded, navigation resets complete callbacks
exactly once, and oversized address-change values are rejected before they can
enter the native UI or persisted session state.
Generated failure pages also enforce a final 2 MiB encoded-output ceiling, and
load/certificate failure URLs must satisfy the same 64 KiB UTF-8 boundary as
normal navigation before being reflected into the page or native UI.
Web pages can enter native full screen, hovered-link destinations appear in the
status bar, and Ctrl/Cmd+P opens the platform print flow.
Scripted pop-ups without a user gesture are blocked. User-initiated pop-ups are
limited to four new tabs per source tab in each 10-second window, in addition
to the global 100-tab ceiling, so one page cannot rapidly exhaust browser
process and UI resources.
Navigation shows page-load progress and supports standard back, forward, and
refresh shortcuts even while the embedded page owns keyboard focus. HTTP and
proxy authentication use an in-memory, non-blocking credential prompt; entered
credentials are never stored by Trail Browser. Page-driven dialogs,
permissions, external-protocol confirmations, and authentication prompts
automatically reject after 60 seconds so an abandoned request cannot retain a
CEF callback or block later prompts indefinitely.
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
command-line URL always takes priority. Home-page URLs are strictly normalized
and bounded so saved settings always remain reloadable. The recently closed tab stack is part of the atomic
session file, so Ctrl/Cmd+Shift+T continues to work after an application
restart. Page favicons are loaded into their corresponding tabs with stale
navigation results rejected. Each tab streams at most one favicon request at a
time, keeps only the newest pending candidate, and cancels responses that grow
beyond 256 KiB before decoding. A 10-second per-request deadline prevents a
stalled response from blocking the latest candidate indefinitely. Favicon URLs
and decoded dimensions are also bounded before the image reaches the Qt UI.
The History menu can selectively clear history,
recently closed tabs, download history, or site data including HTTP cache,
cookies, HTTP credentials, and certificate exceptions
while preserving bookmarks; the updated session is persisted immediately, and
completion is reported only after every asynchronous CEF operation finishes.
Site-data clearing has a 15-second deadline; stalled tasks are reported as
failed, and late or duplicate callbacks cannot complete a newer clear request.
If clearing browsing history or recently closed tabs cannot be persisted, the
corresponding in-memory list is restored instead of pretending the data was
removed for the current run.
Individual bookmark and history entries can be removed from their menus with a
right click. Bookmark names can be edited from the same menu, with an empty
name falling back to the URL. Standard browser bookmark HTML can be imported
or exported for migration and backup. Bookmark additions, renames, and imports
are rejected transactionally if the complete bookmark set would exceed the
2 MiB profile budget; history may be trimmed to fit, but an accepted bookmark
is never silently omitted from persistence. Every change is persisted
atomically.
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
bounds for address, search, find, and bookmark-name input, browsing-data
cleanup, download-ingress and active-count limits, favicon
mapping, streamed response limits, request coalescing and cancellation,
timeout recovery, audio/mute state, user-gesture and rate-limited pop-up policy,
security-policy, and authentication-dialog checks.
Oversized session, settings, download-history, browsing-data, and bookmark
HTML files are also rejected using the bytes actually read, with existing
in-memory settings and profile data preserved.
Bookmark export checks additionally cover strict output limits and preservation
of an existing destination when the generated document would be too large.
Bookmark profile checks cover transactional add, rename, and import rejection
at the exact persistence budget and verify that all accepted entries reload.
Corrupt-profile recovery verifies that all four persistent stores preserve the
original bytes before replacement files are written and successfully reloaded.
JavaScript-dialog limits, concurrency suppression, navigation reset, and
before-unload cancellation are checked as well. Authentication, media, general
permission, and before-unload tests also verify timeout fail-closed behavior.
Keyboard-accessible browser surfaces and full-screen exit routing are also
covered, along with numeric tab navigation, the all-tabs menu, and selective
recent-tab restoration, native application-menu actions, search-setting
persistence, safe external-input normalization, and single-instance URL
forwarding. Linux additionally verifies that a renderer is running with
`NoNewPrivs` and a seccomp filter. All twenty-four
checks are registered when `xvfb-run` is available:

~~~sh
ctest --test-dir build --output-on-failure
~~~

## Security and platform notes

Linux builds run CEF renderer and supported utility processes inside Chromium's
sandbox. Chromium uses the kernel's unprivileged user-namespace support when
available; distributions that disable user namespaces must install the copied
`chrome-sandbox` helper as root with mode 4755. Windows and macOS builds still
run with `no_sandbox` while their platform-specific release packaging remains
unfinished: CEF 138 and newer requires the Windows bootstrap flow, and macOS
requires signed helper entitlements. Those two targets are not ready for
browsing untrusted content in production.

On Linux, windowed CEF requires X11. Trail Browser selects Qt's xcb backend
when QT_QPA_PLATFORM is unset. On native Wayland-only systems, install XWayland
and the Qt xcb plugin or migrate the view to off-screen rendering.

Runtime data is stored under Qt's per-user application-data locations. The CEF
log is named cef.log.
