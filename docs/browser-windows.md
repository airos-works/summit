# Haiku look, context menus and multiple windows

Added 26 September 2026, on top of the browser chrome in
[browser-ui.md](browser-ui.md).

## What the user sees

### Haiku look (default) and the Safari-like look

Edit › Preferences… › Appearance chooses between:

- **Haiku** (the default, also for existing profiles): tabs drawn with
  `BControlLook` like `BTabView` and WebPositive, directly under the menu bar,
  with a close box on each tab and a new-tab button at the end; below them a
  toolbar of real push buttons (Back, Forward, Reload/Stop, Home), a
  left-aligned address field that fills the width, a Go button, then the
  bookmark star (gold when the page is bookmarked), Downloads and extension
  buttons. Bookmarks-bar items rise as flat buttons under the pointer. Page
  progress is a `BStatusBar` in the status line, which also shows the address
  of the link under the pointer.
- **Safari-like**: the previous flat toolbar with a centred address field,
  the bookmarks bar, then rounded tabs, and a thin progress line.

The choice applies at once to every window and is saved as `interfaceStyle`
in `profile.json`. The Preferences window now belongs to the application, so
there is one for all windows; "Use Current Page" takes the front window's page.

### Context menus

A secondary click on a page shows a native menu built from what was clicked:

| Clicked | Items |
| --- | --- |
| Link | Open Link, Open Link in New Tab (in the background, after the current tab), Open Link in New Window, Download Linked File, Save Linked File As…, Copy Link, Bookmark Link |
| Image | Open Image in New Tab, Save Image As…, Download Image, Copy Image Address |
| Audio/video | Open Media in New Tab, Save Media As…, Copy Media Address (not for streamed `blob:` media) |
| Editable field | Undo, Redo, Cut, Copy, Paste, Select All |
| Selection | Copy, Search for “…” |
| Page | Back, Forward, Reload/Stop, Bookmark This Page, Copy Page Address, Select All |

A linked image gets both the link and the image sections. Items that
extensions add with the `menus`/`contextMenus` API follow, under the
extension's name when it has more than one (as in Safari). *Save … As* opens
a save panel in Downloads; the file is downloaded as usual and then moved to
the chosen place.

A secondary click on a tab offers New Tab, Reload Tab, Duplicate Tab, Move Tab
to New Window, Close Tab and Close Other Tabs. A double click on the empty
part of the tab bar opens a tab.

Links now also behave as in other browsers: `target=_blank` and
`window.open()` open a tab after the opener (in front); a middle click or
Command-click opens a background tab (Shift brings it to the front); a
`window.open()` that asks for a size opens its own window, sized to the
request, and keeps its `window.opener`. Pop-ups need a click or key press;
pages cannot open windows by themselves.

### Multiple windows

- File › New Window (Alt+N), File › Close Window (Shift+Alt+W), Window › Move
  Tab to New Window; Open Link in New Window in the context menu.
- Every window is its own `BWindow`, so the Deskbar lists them under Summit;
  the Window menu lists them too, with the current one checked.
- New windows cascade from the front window. Closing the last tab of a window
  closes the window when another is open. Closing a window forgets its tabs;
  closing the last one, or File › Quit, quits Summit and keeps every window's
  tabs and frame for the next start. Preferences › General › "Reopen the
  windows and tabs from last time" (`restoreSession`, on by default) turns
  that off: Summit then starts with one window.
- A shutdown or restart of the system quits Summit the same way, without the
  downloads question, and the next start reopens its windows (#58).
- Bookmarks, history, the bookmarks bar and the look are shared: a change in
  one window shows in all of them.
- Downloads are reported in the window whose page started them. Quitting with
  downloads running asks once, for the whole application.
- Extensions: `windows.create()` opens a real window; commands naming a tab or
  window go to that window, others to the front window; `windows.remove()`
  closes the window unless it is the last one.

## How it works

- `src/ui/SharedProfile.*` owns the profile: every window reads it with
  `Read()` and changes it with `Change()`, both under one lock, and every
  window (and the application) gets `kProfileChanged` with what changed.
  Visits and title updates are saved with the next session save; everything
  else at once. Each window publishes its tabs under its own key
  (`SetWindowSession`), and the profile is saved with the windows in key order.
- `profile.json` gains `windows` (tabs, selection and frame per window) and
  `interfaceStyle`. `tabs`/`selected` still hold the first window, so older
  builds can read the profile, and older profiles load as one window.
- `src/main.cpp` keeps the windows (front first), opens them (`kNewWindow`),
  and runs the quit: windows close one at a time so pages can ask about
  unsaved work; if one stays open, the quit stops. A window's close box asks
  the application (`kCloseWindowRequest`), which quits if it is the last.
- Before a shutdown the registrar sends each application `B_QUIT_REQUESTED`
  with `_shutdown_` and cancels the shutdown on a reply whose `result` is
  false; an application that has not quit after 3 s is killed. Summit's own
  quit answers "not yet" several times (`QuitRequested`), so
  `DispatchMessage` keeps the registrar's request unanswered, writes the
  profile at once and starts the quit. Only a page that keeps its window open
  answers it, with false, which cancels the shutdown as it should.
- Each window keeps its own `FaviconCache`; when one stores an icon, the
  others drop their copy and reread it from disk.
- An open `BPopUpMenu` holds its window's close lock. The page, tab and
  bookmark menus are `CancellableMenu`s that close as soon as their window
  starts closing (found when quitting with a context menu open hung).
- Engine (`Source/WebKit/UIProcess/haiku/WebView.cpp`):
  - The context menu proxy sends `B_WEBKIT_CONTEXT_MENU` with the hit test
    (link, image, media, selection, editable) and the extension items that
    match it, then dismisses WebKit's own menu. The host runs an extension
    item with `BWebKitView::PerformContextMenuExtensionItem(token)`, which
    calls `WebExtensionContext::performMenuItem` as a user action.
  - `API::UIClient::createNewPage` sends `B_WEBKIT_NEW_PAGE_REQUESTED` and
    keeps the opener's configuration and completion handler until the host
    creates a `BWebKitView` with the request identifier (the engine then loads
    the page itself) or calls `BWebKitView::DeclineNewPage`.
  - Link navigations with the middle button or Command are refused and sent to
    the host as `B_WEBKIT_LINK_OPEN_REQUESTED`.
  - `mouseDidMoveOverElement` sends `B_WEBKIT_LINK_HOVERED` when the link
    under the pointer changes.
  - `JavaScriptCanOpenWindowsAutomatically` is off, as on GTK and WPE.

## Tests

`build-host/summit_tests` covers the new profile fields (several windows with
frames, the style, and a profile from an older build). On the workstation the
following were driven over VNC with `tools/bench/vnc-input.py` (whose `key`
command now takes any character and `--mod alt/ctrl/shift`):

- New Window, the Deskbar and Window menu lists, switching windows with both;
- link, linked-image, editable-field and tab menus; Open Link in New Tab;
  Save Image As… (a 330×231 PNG landed under the chosen name); Move Tab to New
  Window;
- `target=_blank`, `window.open()` into a tab and into a sized pop-up window
  (the opener saw a window object), middle click, and a blocked automatic
  pop-up;
- switching between the two looks while two windows were open;
- Quit with three windows and a context menu open, then a restart that
  reopened all three at their places; closing one window, and closing every
  tab of a window.

## Not done

- Dragging a tab out to make a window, or between windows; Move Tab to New
  Window reloads the page there (its back list stays behind).
- An extension's `tabs.remove()` naming tabs in several windows only closes
  those in the first tab's window.
- Pop-up windows keep the normal minimum window size (760×450).
- No Copy Image (only its address), no Inspect Element.
- The legacy (WebKitLegacy) build was not updated or compiled.

## Installed

The workstation desktop launcher `/boot/home/Desktop/Summit-current.sh` points
to `bundle-s_6szlm3` (the `SkiaCGMiPGO` engine rebuilt with these engine
changes). The previous launcher is `Summit-current.pre-windows-20260926.sh`.
A Summit already running from another bundle keeps the old interface until it
is restarted. Test launches used `/boot/home/summit/claude-mw-launch.sh BUNDLE`
with the profile `/boot/home/summit/claude-mw-profile`.
