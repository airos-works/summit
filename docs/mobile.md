# Mobile view (air/OS Mobile)

When air/OS Mobile (`MobileShell`) is running, or a 2-in-1 is folded into a
tablet with the shell enabled, Summit's windows switch to a touch layout
built with Mobile Kit (`src/ui/compat/MobileKit.h`, version 3; the system's
header is `headers/os/MobileKit.h` in the Haiku fork). The layout takes after
Safari on the early, skeuomorphic iPhone and iPad (brushed metal bars, a
glossy share sheet, a Reader pill, red Delete buttons) and borrows Chrome's
grid of tabs and its private tab section. The code is
`src/ui/BrowserWindowMobile.cpp`; the desktop's tabs, page views, address
field and history are the same objects in both layouts, so switching back
and forth keeps everything.

## Phone and tablet

| Width | Top | Bottom |
| --- | --- | --- |
| Narrower than 15 touch targets (a phone) | Address field, Reload or Stop | Back, Forward, Share, Bookmarks, Tabs (with the count) |
| Wider (a tablet) | Back, Forward, Reload, address field, Share, Bookmarks, Tabs, New Tab; a row of tabs under it | — |

- The address field is a white plate sunk into the bar, with the lock on the
  left and Safari's purple **Reader** pill on the right when the page can be
  read in Reader. Typing in it hides the bottom bar (the on-screen keyboard
  takes its place) and shows **Cancel**, which puts the address back.
- A blue line along the bottom of the top bar shows the page loading.
- Pages get Chrome on Android's user agent, so sites send their phone
  layouts and play video through Media Source. **Desktop Site** in the share
  sheet asks again with the desktop agent, for that tab only.
- Links opened from other applications open in a new tab without bringing
  the keyboard up; a blank New Tab focuses the address field.

## Tabs

**Tabs** shows a grid of the open pages (two columns on a phone, three on a
tablet), each card with the page's icon, title, ✕ and a picture of the page
as it was last seen. Tap a card to go to it, tap ✕ or swipe the card
sideways to close it. Under the grid: **Close All** (asks first), **+** and
**Done**. The grid is also keyboard-driven (arrows, Enter, Delete).

On a tablet the row of tabs under the bar works like the iPad's: tap a tab,
✕ on the lit tab closes it, and the row scrolls sideways when there are more
tabs than fit.

## Private browsing

The switcher's **Tabs | Private** segments move between the normal window and
the private window. Private tabs live in a private window (as on the desktop),
whose bars are graphite. Choosing **Private** with no private window open
explains what private browsing keeps and forgets; **+** opens the first
private tab. **New Private Tab** is also in the share sheet. Closing all
private tabs ends private browsing and returns to the normal tabs.

## Bookmarks, Reading List and History

**Bookmarks** (the open book) shows one of three lists, chosen with the
segments at the bottom:

- **Bookmarks**: Favorites (the bookmarks bar's folder) and the other
  bookmarks; folders open with a chevron and the back button names the folder
  above. **Edit** shows red buttons to delete bookmarks and folders (a folder
  with bookmarks in it is confirmed first), lets a row be tapped to edit it,
  and offers **New Folder**.
- **Reading List**: unread items, then read ones; opening one marks it read
  (and opens its saved copy when the site cannot be reached).
- **History**: newest first under Today, Yesterday and dates (the last 400
  pages). **Clear** asks before clearing history; **Edit** removes pages one
  by one.

The share sheet's **Bookmark** opens the editor for the page (or its
existing bookmark): name, address and the folder, chosen from a list with a
check mark. Saved bookmarks and other actions are confirmed with a short
message in the middle of the window.

## Share sheet

The share button opens a sheet rising from the bottom: a grid of actions
(Bookmark, Reading List, Copy Link, Find on Page, Reader, Desktop/Mobile
Site, History, Downloads), then New Tab, New Private Tab, Settings and a red
Close Tab, and Cancel. The downloads panel opens under the top bar at the
window's width.

## Testing without a tablet

`MobileShell` takes the whole desktop over. To test a phone-sized layout on
any air/OS machine, a stand-in shell is enough: a `BApplication` with the
signature `application/x-vnd.airOS-MobileShell` that answers `'mVpr'` with a
`B_REPLY` holding `"area"` (the rectangle the window should fill). Summit
then starts in the mobile view, filling that area. A running window can be
switched with

```
hey Summit mVch of Window 0 with mobile=bool\(true\)    # or false
hey Summit set Look of Window 0 to 19                   # 1 for the desktop
hey Summit set Frame of Window 0 to "BRect(1380,40,1819,999)"
```

`SUMMIT_MOBILE_TRACE=1` prints what the tab switcher's page pictures were
made from.

## Not done yet

- Preferences is still the desktop window (about 900 pixels wide).
- Swiping a card away needs touch scrolling from `mobile_touch`; it was not
  tried on a touch screen yet.
- Tabs cannot be reordered in the grid or the tablet's row.
