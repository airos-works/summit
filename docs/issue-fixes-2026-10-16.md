# Issues #56 to #60 (16 October 2026)

Five issues, worked on the Framework 13 laptop (air/OS x86_64, the user's
machine). The fixes are in Summit 1dfa147 with 492492b (#60), 97a499c (#58),
0b73148 (#59) and 1487b19 (#57), in air/OS CI 9f2b062 (#56) and 4b86b43 (the
browser's objects), and in air/OS master cff7621e4a (the Deskbar half of
#57). The CI build of 1487b19 with 4b86b43 (`summit 0.1.0~git20261016.0858`,
`summit_webkit 1.10.1~git20261016.0858`) is installed on the laptop with
`pkgman`; there the web processes are left out of the Deskbar, pages show
Hobart time, the power-flow copy takes 29% of a core (0.3% minimized), a
simulated shutdown quits in 0.03 s without cancelling it and the next start
reopens the tabs, a copy of the user's own profile reopens its window, and
the windows answer the Deskbar's "Icon" request.

## #56 "WebProcess" in the Deskbar

`WebProcess.rdef` declares `B_MULTIPLE_LAUNCH | B_BACKGROUND_APP`, and the
Deskbar leaves background applications out. The x86_64 package had no
resources at all: airos-ci's `build_summit_webkit` attached them with `xres`
and then ran a second `patchelf --set-rpath` for the private Mesa, and
patchelf drops the resources Haiku appends to an ELF file. The processes now
get their final RPATH in one step and the resources last (airos-ci 9f2b062).
With the 1.10.1~git20261016.0844 package the web process lists BEOS:APP_SIG
and BEOS:APP_FLAGS, and three running web processes leave the Deskbar
showing only Summit.

## #60 Page times are in UTC

The engine's ICU 78 finds the host's zone in `TZ` or the `/etc/localtime`
link. Haiku has neither (the zone is in the locale settings), so ICU fell
back to UTC: `new Date().toString()`, `Intl` and every page's clock read
UTC, 11 hours off in Hobart. `wtf/haiku/TimeZoneHaiku.cpp` sets the zone of
the locale settings (`BLocaleRoster::GetDefaultTimeZone`) as ICU's default
when the first DateCache is made and follows `B_LOCALE_CHANGED`, which the
Time preferences broadcast with the new zone; JSC reads ICU's default zone on
Haiku. Two traps: `ucal_setDefaultTimeZone` wants a terminated string
(`upconvertedCharacters()` is not), and ICU's headers must come before
Haiku's `TimeZone.h`, which otherwise declares `namespace icu` and breaks
ICU's `icu = icu_78` alias (492492b).

A page that already formatted a time with `toLocaleTimeString()` keeps its
formatter's zone after a zone change until it reloads, as on other ports
(JSC never rebuilds the global object's default `Intl.DateTimeFormat`).

## #58 Shutdown cancelled by Summit

Before a shutdown the registrar sends each application `B_QUIT_REQUESTED`
with `_shutdown_`; a reply whose `result` is false cancels the shutdown, and
an application still running after 3 s is killed. Summit's quit answers "not
yet" on its first turns (windows close one at a time), so every shutdown
stopped at Summit. `SummitApp::DispatchMessage` now keeps the registrar's
request unanswered, writes the profile at once and runs the quit without the
downloads question; only a page that keeps its window open answers it, with
false. Preferences › General › "Reopen the windows and tabs from last time
when Summit starts" (`restoreSession`, on by default) chooses whether the
next start reopens them. Tested by sending the registrar's message by hand
(scratchpad `fakeshutdown`): the old build answered false, the new one quit
in 0.03 s and the next start reopened its tabs, or one window with the
preference off.

## #59 An animated SVG card keeps 1.6 cores busy

A Home Assistant power-flow card draws each flow as its own absolutely
positioned SVG with a dot moving along a path (SMIL `animateMotion`). Each
SVG root is a layout boundary, but WebCore keeps a single subtree layout
root: the second animating SVG turned every frame's layout into a full
layout, which laid out each SVG's parent block, and that block's line layout
repainted the whole line around the SVG. The whole card was painted 60
times a second (tile trace: one 353x396 dirty rectangle per frame on each of
two layers). A layout boundary whose own box does not need layout now marks
its ancestors for simplified layout only, as a single subtree root would:
they lay it out in place and each dot repaints its own few pixels
(`RenderObject::markContainingBlocksForLayout`). Text inputs, `contain:
strict` boxes and fixed-size clipping boxes changing beside animated SVGs
still update in place.

Pages of a minimized window, or of a window on another workspace, counted as
visible and went on animating and painting. `BWebKitView` now follows its
window's `B_MINIMIZE` and workspace messages, so WebKit's hidden-page
handling applies.

A reduced copy of the card (`pf.html`: six SVGs, window 1120x680 at 200%) on
the laptop: web process 77% of a core before, 29% after (Skia painting 58%
to 13%), app_server 6.5% to 2.6%; minimized or on another workspace 26% to
0.2%. Painting each small dirty rectangle on its own instead of one per tile
saved Skia time but cost as much in recording and composition, and was left
out. The Mesa thread names of the issue's side note were fixed in Mesa (the
issue's comment).

## #57 Site icons in the Deskbar's window list

The Deskbar drew the same generic icon for every window. air/OS's Deskbar
(cff7621e4a) now asks each window it lists for an "Icon" scripting property
(`B_GET_PROPERTY`, direct specifier) and takes an archived `BBitmap` from
"result"; it then watches the window with `BHandler::StartWatching` for
`'wicn'` notices that carry the next icon. Windows that do not know the
property keep the generic icon and are not asked again. Summit's browser
windows answer with the selected tab's site icon and notify when it changes
(1487b19). Tested by running the built Deskbar in place of the system one on
the laptop: Wikipedia's and then GitHub's icon on the Summit window's row,
dimmed while minimized, the generic icon for a page without one. The laptop
shows them once its air/OS package includes cff7621e4a.

## The browser's objects in CI

airos-ci's `build_summit` reused an object whenever its `.cpp` was older, so
objects that included a changed header kept the old layout. 97a499c's new
`Profile::restoreSession` sat in padding that `SharedProfile.cpp`'s stale
copy code did not copy, and a new profile was saved with it false; 1487b19's
larger `BrowserWindow` would have been allocated by a stale `main.o`. gcc now
writes each object's header list (`-MMD`) and an object is reused only when
it is newer than all of them (airos-ci 4b86b43). The 0844 packages, built
before that, were taken off the laptop again.
