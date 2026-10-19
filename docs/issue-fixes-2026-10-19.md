# Issues #61 and #62 (19 October 2026)

Two changes to the browser itself (no engine change): a downloads button and
panel like Firefox's, and a card on extension store pages with Install
Extension or Manage Extension. Tested on the X399 with a test copy of the
browser on CI's 1.10.1~git20261018.2008 engine (`/boot/home/summit/claude-dl`).

## #61 A better download indicator

The engine's download notifications (`B_WEBKIT_DOWNLOAD_*`: name, path, bytes
so far and expected, result) go to the application, which keeps this
session's downloads in `DownloadList` (`src/ui/DownloadList.h`) before it
passes each one to the window whose page started it. Every window listens to
the list:

- **The toolbar button** (`DownloadsButton`) draws a progress bar under its
  arrow while files download (all of them together; a moving segment when a
  server does not give the size), and a dot once a download has finished that
  its panel has not shown yet: green, or red when it failed. Its tooltip
  counts the downloads and the percentage.
- **The panel** (`DownloadsPanel`) opens under the button when it is clicked,
  with Window › Downloads (Alt-J), and by itself when a download starts in the
  active window, as Firefox's does. It lists the last eight downloads, newest
  first: the file's icon (its type's, from the name while it downloads), its
  name and "3.1 of 15.0 MiB — 1.5 MiB/s, 8 seconds left", then "15.0 MiB —
  example.com", "Failed — reason", "Cancelled" or "File moved or missing".
  The round button cancels a download, shows a finished one in Tracker (the
  folder opens with the file selected) or retries one that failed. A click
  on a finished row opens the file. The secondary button offers Open, Show in
  Folder, Retry or Cancel, Copy Download Link, Remove from List and Clear
  List. Below are Clear List and Show Downloads Folder. The panel closes when
  something else is clicked or with Escape; a click on the button closes it.
- Private windows list only their own downloads, which are forgotten with the
  private session. Cancelling goes to the application, which knows the
  download's context (`kDownloadCancel`). A finished file's type is set in the
  background (`update_mime_info`) for its icon and the application that opens
  it.

The text is `core/DownloadText.h`, tested in `summit_core`.

Verified on the X399: 40 MiB at 3 MiB/s (bar in the button, panel opened by
itself, time left, then the show-in-folder button), a size-less transfer
(moving segment), a dropped connection (red dot, "Failed — Transferred a
partial file", Retry), cancel, retry, the menu and Show in Folder, the panel
closing on a click elsewhere and on the button, and a private window's panel
with only its download.

## #62 Extension installation tweaks

1. **A card on store pages.** On an extension's page in the Chrome Web Store
   or on Firefox Add-ons, Summit puts a card at the page's bottom right, like
   1Password's: the Summit icon, the extension's name, "Available for Summit"
   and **Install Extension**, which starts the install from the store as the
   toolbar's button does (the permission review comes first). It follows the
   stores' in-page navigation, and × closes it for that page. Theme pages on
   Firefox Add-ons get no card.
2. **Icons in the Extensions window.** Each extension is listed with its own
   icon, read from its package as the profile keeps it: the manifest's
   `icons` (the smallest at least 24 points, at the screen's density), else
   its action's icon. Disabled extensions are dimmed; one without an icon
   Summit can read gets its initial on a tile.
3. **Manage Extension.** When the page's extension is installed, the card says
   "Installed in Summit" (", turned off" when disabled) and **Manage
   Extension** opens the Extensions window with that extension selected. The
   toolbar's button says Manage extension… too. Installing or removing an
   extension updates open store pages at once.

How the card works (`core/StoreOffer.h`): the browser runs a script in an
isolated world (`EvaluateJavaScriptIsolated`) when a store page has loaded or
its address, title or the installed extensions change. It builds the card in
a closed shadow root from elements and a constructed style sheet, because the
Chrome Web Store enforces Trusted Types (no markup parsing) and Firefox
Add-ons restricts inline styles. The icon is `resources/artwork/app.svg`
turned into an element tree by `tools/make-store-offer-logo.py`. The card's
links are `summit-extension:install|manage|dismiss?token=…`: the engine loads
no such address, and a click on one reaches the browser as
`B_WEBKIT_SCHEME_OPEN_REQUESTED` (only with a user gesture). The browser acts
only on links with its window's secret token, on a store page that has a
card. Chrome Web Store items are named by their extension ID; Firefox Add-ons
pages by a slug, so the browser asks Mozilla's API once per slug for the
add-on ID, name and type (`ResolveFirefoxStoreItem`).

Package files are read by `core/ExtensionPackage.h`: CRX3, ZIP and XPI
archives (a small inflater, each file checked against its CRC) or unpacked
folders. `summit_extension_package` tests it with fixtures and, given paths,
real packages (uBlock Origin's CRX and XPI, Dark Reader's).

Verified on the X399: uBlock Origin's Firefox Add-ons page (installed: Manage
Extension opened the Extensions window with it selected; icons for
1Password, uBlock Origin and later Dark Reader), Dark Reader on the Chrome Web
Store (Install Extension → review → Install, and the card turned into Manage
Extension), Privacy Badger (not installed), a theme (no card), × and a move to
another extension inside the Chrome Web Store.
