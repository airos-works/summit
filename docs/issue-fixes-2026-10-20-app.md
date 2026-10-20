# Issues #89, #93, #94, #95 (20 October 2026)

Four browser fixes: error pages, a profile that survives damage and is used by
one Summit at a time, links opened while Summit is closed, and three window
and loading bugs. All are in the browser; no engine change. They were tried on
the X399 with test copies of the browser on the released 0816 engine
(`/boot/home/summit/claude-s36/bundle-app*`, profiles `claude-s36/app-p*`),
driven through Summit's MCP server and, where a real click mattered, VNC
pointer clicks (`tools/bench/vnc-input.py`).

## #89 An error page with Try Again

A page that can't be loaded (no such host, a refused or broken connection, a
server that never answers, no network, a failed secure connection) no longer
leaves the previous page in place with a line in the status bar. The tab
shows a page that says what went wrong, with the address and **Try Again**.
Its title names the failure: "Can't find example.com", "Can't connect to
example.com", "The server took too long", "You're offline", "Secure
connection failed". The address stays in the address field. Try Again, Reload
and Return (the button has the focus) load it again; Back shows the page that
was there before. A page that failed for want of a network loads again by
itself when the network is back.

How it works: [modern-navigation-errors.md](modern-navigation-errors.md#error-pages-89).
The page is a native view in the tab's card, as the crashed-page view of #71 is
(`src/ui/BrowserWindowLoadErrors.cpp`); the kinds and texts are in
`src/core/LoadError.cpp` (host tests in `tests/CoreTests.cpp`). Cancellations,
downloads and policy hand-offs stay silent, and a certificate Summit can trust
still asks first. The error page's lines are single-line string views with
whole-pixel widths: a `BTextView` got the wrong height for its width in this
layout, and `BStringView` asks for a fractional width that the layout rounds
down, so each line lost its last letters to an ellipsis.

Checked on the X399:

- `https://nonexistent-summit-test.invalid/page`: "Can't find
  nonexistent-summit-test.invalid", the address in the field, Back enabled.
  Back (the toolbar button, a real click) showed the page before with its
  address.
- `http://127.0.0.1:8811/hello.html` with nothing listening: "Can't connect to
  127.0.0.1"; after starting a server on that port, Try Again (a real click)
  loaded it.
- A host server that accepts and never answers: "The server took too long"
  after curl's 60 s; while Try Again ran the page stayed with "Trying Again…";
  once the server answered, the toolbar's Reload loaded the page.
- `https://192.168.1.200:8796/` (TLS spoken to a plain HTTP server): "Secure
  connection failed". `https://self-signed.badssl.com/` still asked about the
  certificate.
- Offline: the shared X399 was not taken off the network (its two interfaces
  carry every session's access). A test-only build (not committed) answered
  "offline" while a marker file existed: a refused load then showed "You're
  offline", and four seconds after the marker went and a server came up the
  page loaded by itself. Unplugging the network for real is still to be tried.

Not checked with a real key: Return on the focused button (VNC keys are not
used on the shared desktop); the button had the focus.

## #93 A damaged profile, and two Summits on one profile

**Damage.** Every save keeps the file it replaces as `profile.json.1`, and the
first save of each day also as `profile-backup-DATE.json` for three days
(`Profile::Save`). When `profile.json` cannot be read, Summit moves it to
`profile.damaged-DATE-TIME.json`, loads the newest copy that can be read
(`Profile::LoadOrRestore`) and goes on saving. A bar under the toolbar says
"Your profile was damaged. Summit restored it from Tuesday, 15:06 and saves it
again. The damaged file was kept as …" until OK is pressed; the modal alert
that held every click is gone. With no readable copy at all, Summit starts a
new profile and keeps the damaged file. Only a newer Summit's profile (another
`version`) is left untouched and not written, as before. A profile that is
missing while copies exist (a save cut short between its two renames) is
restored the same way.

Extensions' `catalog.json` keeps its own rule (a damaged catalog is never
written over; `tests/ExtensionCatalogTests.cpp` holds it to that), so it has no
copies yet.

**One Summit per profile.** Summit holds an `flock` on the profile folder's
`lock` file while it runs (`src/core/ProfileLock.cpp`); the file names its
team. The lock goes with the process, so a crash leaves nothing to clean up,
and Summit lets it go after its last save, before an update starts the new
build. A second Summit, of another build or package (the registrar only keeps
one instance per executable), finds the folder locked and hands what it was
asked to open to the team that holds it: `B_ARGV_RECEIVED` with the addresses
and `--app` web apps, or `B_SILENT_RELAUNCH`, which brings that Summit to the
front. It waits for the reply that says the message was handled and quits. If
that Summit does not answer within 2 s plus 6 s, the second one says "Another
copy of Summit is using this profile and does not answer" and quits; the
message is sent once, so the pages open once if the first one wakes up.

Checked on the X399 (profile `app-p93`): after a few page visits the folder
had `profile.json`, `profile.json.1` and `profile-backup-2026-10-20.json`.
`profile.json` zero-filled, then truncated to 700 bytes: each start restored
the previous copy, showed the bar and kept the damaged file, and the next
visit was in `profile.json` again. A second bundle started on the same
profile with an address handed it over in 0.14 s (the tab opened in the first
Summit) and exited; without an address it handed over `B_SILENT_RELAUNCH`
and exited the same way.
With the first Summit stopped (`kill -STOP`), the second showed the message
after about 6 s and quit on its button; after `kill -CONT` the address opened
once.

The X399's real profile (`~/config/settings/SummitModern/profile.json`, damaged
since 13 October) was not touched. A build with this change will move it
aside as `profile.damaged-….json` and start a new profile there, since it has
no copies.

## #94 Links opened while Summit is closed

With "Reopen the windows and tabs from last time" on, a start with addresses
(a link from Mail, `open https://…`, `Summit https://…`) restores the saved
windows and opens the addresses as new tabs of the window in front, the last
one restored (`src/main.cpp`, `ReadyToRun`). The session keeps all of them.
With the preference off it opens just the addresses, as before; web app
launches (`--app`) are as they were.

Checked with a profile of three saved windows (2, 1 and 3 tabs): a start with
`https://example.com/` brought back the three windows with example.com as a
fourth tab of the third; quit and start again without an address kept all of
them. With `restoreSession` false the same start opened one window with
example.com.

## #95 Pop-up sizes, full screen from an inactive window, opened media

**Pop-ups.** A `window.open()` with a width and height opens a window whose
page has that size; the window has no bookmarks bar and its minimum size is
200×150 (in practice the menu bar's width, about 335 pixels, since the window
takes its limits from its layout). The address field can shrink to 120 pixels
in a pop-up. Ordinary windows keep 760×450; web app windows can go down to
320×240. Checked: `window.open(url, 'pop', 'width=500,height=600')` gave a
page of 500×600 (before: 761×601); 250×180 gave 335×180.

**Full screen from an inactive window.** A click on a full-screen button in a
window that is not active only activated the window. Tracing the page's
events showed why: between the click's mousedown and mouseup the page got a
mouse-moved message saying no button was down (the window hearing that it is
active), so the mouseup was taken for another button's and the page got
`auxclick`, not `click`. The same lost the first click on links and buttons
of an inactive window. The window now keeps the buttons it saw go down and,
for a second after the press, corrects a mouse-moved message that says none
are (`BrowserWindow::DispatchMessage`). A full-screen request that comes with
that click is then granted, and the window comes to the front, although the
activation has not arrived yet (`SetPageFullScreen`). Checked with real VNC
clicks on a page with a full-screen button: before, 0 of 5 first clicks in an
inactive window entered full screen (the page saw `d u M0 a0`: no click); after,
4 of 4 did (`d M1 u c E`).

**Opened media.** A video or audio file opened in a tab plays in a media
document, which takes the load over; the engine reports that as
WebKitErrorDomain 204 ("Plug-in handled load"). The browser now counts it as a
loaded page (`MediaTookOverLoad`): no error in the status bar, the lock shows,
MCP's tabs say `loadOutcome: succeeded`, and the visit is in history. Checked
with `https://interactive-examples.mdn.mozilla.net/media/cc0-videos/flower.mp4`.
