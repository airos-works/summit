# Issues #63–#77 (19 October 2026, evening)

The feature review of 19 October listed twelve things people expect from a
browser (#65–#76); #63 and #64 were already open. This page covers the ones
built in the main checkout. Bookmarks (#69) are in [bookmarks.md](bookmarks.md);
the developer tools' Elements panel (#76) and the update checker (#72) have
their own pages. Everything was tried on the X399 with a test copy of the
browser on a test engine built from CI's tree
(`/boot/home/summit/claude-s35`).

## #63 Extension installation tweaks

The card on an extension's store page is centred at the top of the page, and
the toolbar's Install extension… button, which the card replaced, is gone
(`src/core/StoreOffer.cpp`, [extension-stores.md](extension-stores.md)).

## #64 App links from redirects (OAuth callbacks)

A navigation to a scheme no page can show (`x-github-desktop-dev-auth:`,
`zoommtg:`…) now reaches the browser in more cases. The engine's
`decidePolicyForNavigationAction` (`UIProcess/haiku/WebView.cpp`) sends
`B_WEBKIT_SCHEME_OPEN_REQUESTED` for every such navigation, with
`user_gesture`, `main_frame`, `redirect` and **`trusted`**: a click asked for
it (the gesture survives the redirects of the navigation it started), or the
main-frame load was started by the browser itself (typed, or opened from
another application with `open`) and its server redirects ended there. That
is the OAuth case: GitHub answers the sign-in page Summit opened with a 302 to
the app's scheme.

- Trusted: the usual prompt, or the remembered application.
- A main-frame navigation a script made by itself: the prompt every time,
  even for a remembered application (`ProtocolHandlers::Open(..., askAlways)`).
- A frame without a gesture: refused, and the status line says so.

Checked with a server redirect to `x-summit-test://redirected` from a page the
browser opened: the "Open this link in another application?" prompt appears.

## #65 Printing and Save as PDF

- **File › Print…** (Alt+P, also on the page's context menu, and what
  `window.print()` does) uses the Haiku print kit, so every printer works,
  the Preview and PDF Writer drivers included. A `PrintJob` looper
  (`src/ui/PrintJob.cpp`) runs the printer's own dialogs (page setup the
  first time, then pages and copies) on its own thread, asks the window for
  one page at a time, and draws each into the job: the page as a picture at
  the printer's resolution (at most 200 dpi), with the title and date at the
  top and the address and "3 of 8" at the bottom. **File › Page Setup…**
  changes the paper.
- **File › Save as PDF…** (Shift+Alt+S) writes a vector PDF: text stays text,
  fonts are embedded. A4, 0.5 inch margins (or the paper of the last print).
- Engine: `WebPage::drawPagesForPrintingHaiku` (`WebProcess/WebPage/haiku/
  WebPageHaiku.cpp`) lays the page out with WebCore's `PrintContext` (the
  site's print style sheet applies; 1 CSS pixel = 0.75 point, as in other
  browsers) and draws pages into an `SkPDF` document or into pictures, which
  `BWebKitView::DrawPagesForPrinting` returns as `B_WEBKIT_PRINT_PAGES`. The
  PDF writer needs Skia's JPEG helpers (it aborts without them).
  `printFrame` in the UI client sends `B_WEBKIT_PRINT_REQUESTED`.

Checked: an English Wikipedia article printed to the Preview printer (8
Letter pages with headers and footers) and saved as PDF (7 A4 pages, the
site's print layout, embedded Noto fonts, `application/pdf`).

## #66 Spell checking

Misspelt words in text fields, `<textarea>` and `contenteditable` are
underlined as you type; the context menu on one lists up to six suggestions,
then **Ignore Spelling** and **Add to Dictionary**. **Edit › Check Spelling
While Typing** and Preferences › Browsing turn it off.

- Engine: `UIProcess/haiku/TextCheckerHaiku.cpp` implements WebKit's
  `TextChecker` on Hunspell, loaded with `dlopen` (`libhunspell-1.7.so.0`,
  part of air/OS and in HaikuPorts), so the engine has no build dependency.
  `BWebKitSetSpellChecking(enabled, languages, directory, wordList)` picks the
  dictionary: the first of `languages` found in the given directory or
  `data/hunspell` / `data/myspell` (user, then system), else en_US. Learned
  words are appended to the word list and read back at start.
  `B_WEBKIT_CONTEXT_MENU` carries `misspelled` and `spelling_guess`;
  `BWebKitView::PerformSpellingCommand` replaces the word (WebCore's own
  spelling-guess action), ignores it or learns it (after `SelectWord`).
- Summit carries the en_US dictionary (SCOWL, [licence](dictionary-en_US-license.txt))
  in its resources and copies it to `~/config/settings/Summit/Dictionaries`.
  `SUMMIT_SPELLING_LANGUAGES=de_DE,en_US` names others to try first.

Checked: typing "Anothr sentnce." underlined both; the menu offered
Another/Anthrax and replaced the word; Add to Dictionary wrote `sentnce` to
`words.txt` and removed the underline.

## #67 Saved passwords

When a sign-in form is submitted, a bar offers **Save**, **Never for This
Site** or **Not Now** (and **Update** for a changed password). Passwords are
kept in the **Haiku key store** (keyring "Summit", `BPasswordKey` with
`B_KEY_PURPOSE_WEB`, the origin as identifier and the account as secondary
identifier), never in the profile. They are filled when the user clicks into
the sign-in form, not when a page loads; with several accounts a small list
appears under the field. A field marked `autocomplete="new-password"` offers
a strong generated password. Private windows neither save nor fill.
Preferences › Browsing lists the saved accounts (Show Password…, Remove) and
imports and exports the CSV files Firefox and Chrome use.

- The page script (`src/ui/PageScripts.cpp`, `summitPasswords`) reports
  submissions and asks for the accounts of its own origin; only the main
  frame is answered. Key store calls run on threads of their own: the key
  store can stop to ask the user (`src/ui/PasswordStore.cpp`).
- The first time, Haiku asks whether Summit may use the keyring, separately
  for listing, reading and adding keys; "Allow always" answers for good.

Checked: sign-in on a test form → bar → Save (after the key store's three
questions) → "The password was saved."; on the next visit the fields stayed
empty until the name field was clicked, then held the account.

## #68 Tabs: drag, pin and more

- **Drag a tab along the strip** to reorder it; **drag it off** to move it to
  the window it is dropped on, or to a new window there; drop it on the
  **bookmarks bar** to bookmark it. The page is not reloaded: its view moves
  to the other window (`BWebKitView::SetNotificationTarget` sends its
  notifications there from then on).
- **Pin Tab / Unpin Tab**: pinned tabs are icon-only, at the start of the
  strip, kept in the session, and not closed by Close Other Tabs.
- The tab menu adds **Mute Tab**, **Close Tabs to the Right** and **Reopen
  Closed Tab**.

Checked: reordering, a playing video's tab dragged out into a new window
(still playing), pinned tabs after a restart.

## #70 Sound: tab indicator, mute, autoplay

A tab whose page plays sound shows a speaker; a click on it mutes the tab
(struck-through speaker) without selecting it. Preferences › Browsing ›
Autoplay: allow audio and video, **block audio** (the default: video without
sound still plays) or block both; Site Permissions can give a site its own
choice. Engine: the state notice carries `playingAudio` and `audioMuted`;
`BWebKitView::SetAudioMuted`; `SetSitePermission("autoplay", origin or "*",
0|1|2)` sets the website policy of the site's next main-frame navigation.

Checked: YouTube-style test page with sound in a background tab (speaker, then
muted with one click); with the default setting a video with `autoplay` and
sound stays paused and the same video `muted` plays.

## #71 Crashed and hung pages

A tab whose web process ended shows a page saying so ("This page crashed",
"…used too much memory", "…was stopped") with **Reload** and **Report a
Problem…** (a prefilled GitHub issue with the versions and only the site's
origin). A tab that crashed in the background reloads when selected. When a
page stops answering input for a few seconds, a bar offers **Wait** or **Stop
Page**. The engine reports the reason (`B_WEBKIT_PROCESS_EXITED` with
`reason`) and responsiveness (`B_WEBKIT_RESPONSIVENESS_CHANGED`), stops
WebKit's own silent reload, and `BWebKitView::TerminateWebProcess` ends a
hung page.

Checked: `kill -9` of a tab's web process (crash page, Reload), and a page
spinning in `for (;;)` after a click (bar within seconds, Stop Page).

## #73 Blocked pop-ups

When a page opens a window without a click, a bar says "Summit blocked a
pop-up from example.com" with **Open Pop-up**, **Always Allow** and Close.
Always Allow saves a Pop-ups site permission; the engine then lets that
site's pages open windows (`SetSitePermission("popups", origin, 1)` →
`WebsitePopUpPolicy::Allow`). A small page script wraps `window.open` to
notice the refusals.

Checked: a page that opens a window from a timer shows the bar; Always Allow
opened the pop-up, and on reload the page's own `window.open` worked.

## #74 Picture in Picture

Videos can play in a small floating window above every other window, on all
workspaces, keeping the video's shape; it has play/pause, back to the tab and
close. It opens from a page's own Picture in Picture button (the standard
API: `requestPictureInPicture()`, `document.pictureInPictureEnabled`,
`exitPictureInPicture()` and the enter/leave events), from **Picture in
Picture** on a video's context menu, or with **View › Picture in Picture**
(Shift+Alt+P, the page's largest video).

The engine sends the video's frames, scaled to the window, from the web
process to the window (`B_WEBKIT_PICTURE_IN_PICTURE_FRAME`, at most 30 a
second and only when the picture changed); the window's buttons go back
through `BWebKitView::PictureInPictureCommand`. It keeps playing while the
tab is in the background.

Checked: a page's own button (`requestPictureInPicture()`), View › Picture in
Picture, play/pause from the window, the picture still moving with the tab in
the background, and a second request replacing the first window.

## #75 Websites as apps

**File › Install as App…** reads the page's web app manifest (name, start
address, scope, icons) or its title and icon, asks, and makes a launcher in
`~/config/settings/Summit/Apps/` (a file of the type
`application/x-vnd.Kunanyi-Summit-webapp` with the app's icon and its
identifier in `SUMMIT:app`), linked into the Deskbar's **Applications**
menu. Opening it (Deskbar, Tracker, or `Summit --app ID`) shows the app in a
window of its own: the app's name as its title, no tabs, toolbar or
bookmarks bar, the same cookies and sign-ins. Pages it opens go to ordinary
windows; a page outside the app's scope offers **Open in Summit**.
Preferences › Browsing lists installed apps with **Uninstall**. The manifest
rules are in `src/core/WebAppManifest.cpp` (host-tested).

The app's windows belong to Summit's team, so the Deskbar lists them under
Summit; an entry of their own would need a team (and signature) per app.

Checked: Squoosh installed (launcher with its icon, Deskbar link), opened
from its launcher and with `--app`.

## #77 "Error in the HTTP2 framing layer"

Pages sometimes failed with "Could not load …: Error in the HTTP2 framing
layer. Enter the address again to retry". That is libcurl's `CURLE_HTTP2`: the
HTTP/2 connection the request went out on broke (typically a pooled
connection the server was closing, its GOAWAY crossing the request). Other
browsers send such a request again without showing anything; the engine
showed the error.

`CurlRequest::retryAfterHttp2ErrorHaiku` (`WebCore/platform/network/curl/
CurlRequest.cpp`) now sends a GET, HEAD or OPTIONS request that failed with
`CURLE_HTTP2` or `CURLE_HTTP2_STREAM` before any of its response arrived
again: first on a fresh connection, then, if that fails the same way, over
HTTP/1.1. The log gets one line each time ("Summit net: Error in the HTTP2
framing layer from example.com; sending the request again on a fresh
connection"). Requests with a body are not repeated (the server may have
acted on them).

Checked on the X399 with a test server that answers chosen HTTP/2 requests
with a DATA frame on stream 0 (a connection error, curl's exit 16). Before the
change the page failed with exactly the message above. After it, a page whose
request broke on a pooled connection loaded from a new HTTP/2 connection; one
that broke twice loaded over HTTP/1.1; a favicon request that broke was sent
again the same way.
