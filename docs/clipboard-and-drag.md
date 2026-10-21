# Clipboard and native drag and drop

Added 2 October 2026 for [issue #14](https://github.com/jmgasper/summit/issues/14)
(drag and drop files into the browser) and
[issue #15](https://github.com/jmgasper/summit/issues/15) (paste, for example a
screenshot into a GitHub issue).

## What the user sees

- **Pasting an image.** An image on the clipboard pastes into pages that
  accept files from the clipboard: GitHub's issue and comment editor uploads a
  pasted screenshot and inserts its Markdown link, as in other browsers. Haiku's
  Screenshot (*Copy to clipboard*, or `screenshot -c`) and ShowImage put a
  BBitmap on the clipboard; pages receive it as a PNG file named `image.png`.
  PNG, JPEG, GIF or WebP data that an application puts on the clipboard under
  its MIME type is passed through as it is.
- **Pasting into editable content** (a mail compose box, a rich text editor)
  inserts the clipboard's HTML, or its image, or its text. Before, only plain
  text fields took a paste; editable content received nothing.
- **Copy Image** (the page context menu) puts the image on the clipboard as PNG
  data and as a BBitmap, so it pastes into pages and into Haiku applications.
- **Dropping files.** Files dragged from Tracker onto a page are offered to it
  as an HTML drag: drop zones highlight and receive the files (GitHub uploads
  an image dropped on its editor; `<input type="file">` takes a dropped file).
  A file dropped where the page takes nothing opens in the tab, as in other
  browsers.

## How it works

The original clipboard and file-drop support is in the engine. Page drags also
use the browser’s bookmarks bar and tab strip, described below.

- **Clipboard** (`Source/WebCore/platform/haiku/PasteboardHaiku.cpp`). The
  pasteboard reads `be_clipboard` directly in the web process (it has a
  BApplication). `fileContentState()` reports an image when the clipboard holds
  image data or a BBitmap archive (`image/bitmap`, `image/x-be-bitmap`), and
  `read(PasteboardFileReader&)` hands it to the page as a file: a BBitmap is
  converted to PNG with the Translation Kit. Upstream WebCore only lets Cocoa
  pages see pasted or dropped files (`DataTransfer::allowsFileAccess()`, see
  webkit.org/b/271957); Haiku now follows the Cocoa rule (files for paste and
  for file drags, none for other drags). Only the web's own string types are
  listed in `clipboardData.types` (BBitmap archives and application-private
  fields stay hidden).
- **Rich paste.** Editor's paste on GLib-style ports asks the pasteboard for a
  web content reader, which the Haiku pasteboard never implemented, so
  editable content received nothing. On Haiku it now uses the pasteboard's
  own `documentFragment()` (HTML, else an image as an `<img>` with a blob URL,
  else plain text).
- **Drops.** `BWebKitView::MouseMoved()` sees the drag message of a drag from
  another application. A drag that carries refs becomes a WebKit drag (entered,
  updated, exited; `WasDropped()` performs it) with the files' paths
  (`viewDragHaiku` → `WebView` → `WebPageProxy::dragEntered` and the rest).
  `DragData` carries file names over IPC on Haiku as on Cocoa, `DragDataHaiku`
  answers from them, and a drag-and-drop `Pasteboard` holds the drag's files
  instead of reading the clipboard (before, a drop read the clipboard and a
  drag that started in a page wrote it). WebKit's generic path already grants
  the network process access to dropped files, which uploads need. For a
  drop the page does not take, WebCore loads the first file in the tab; the
  UI process only lets a web process navigate to a file it was given read
  access to, so `WebPageProxy::performDragOperation` now grants it (as GTK
  does) and sends the drop once the grant is in place. Before, the
  navigation was refused ("outside the sandbox") and nothing happened.

## Native page drags (#90)

The Haiku drag client now starts a native drag for HTML draggable elements,
selected content, links and images. The page receives drag progress and the
accepted operation on `dragend`. Skia supplies the drag preview and its hotspot;
the native view uses a bounded BGRA bitmap in view coordinates.

Each drag has its own pasteboard. Text, HTML and URLs cross the native boundary
under their MIME types. Custom string data carries its source origin, so pages
at the same origin can exchange card IDs and other custom formats; a different
origin receives only the standard public formats. The protected drag store
exposes types during hover without exposing values or file contents. Incoming
HTML is sanitized before scripts read it. This does not enable the unfinished
custom-data API for the system clipboard.

Page images supply PNG bytes. Tracker negotiates a file destination and the
browser writes the image and its MIME attribute. The export refuses to replace
a nonempty file or follow a destination symlink. Native applications can also
request MIME bytes directly. Incoming drops accept files, text, URLs, HTML and
PNG/JPEG/GIF/WebP data. For sources such as ShowImage that advertise a format
before producing its bytes, Summit requests that format asynchronously; a
navigation, another drag, a closed view or the five-second timeout invalidates
the pending reply.

The bookmarks bar accepts a page link with its title. Hovering over another tab
during a drag selects it, allowing a drop into that page; dropping a URL directly
on a tab navigates it. Native drag messages keep browser bookkeeping separate
from page-supplied format names.

## Checking it

`tools/bench/pages/clipboard-drop.html` reports every paste and drop: the
types, items and files the page received, image sizes, and previews.
`summitctl paste` performs Edit › Paste on the current page (no keyboard
needed on the shared desktop). `tools/bench/clipimage.cpp` puts an image file
on the clipboard as a BBitmap, as Screenshot does (`screenshot -c` works too),
and `tools/bench/dragsource.cpp` opens a small window that, when pressed,
drags the given files as Tracker does; `vnc-input.py drag` from it to the page
performs a drop with pointer events only.

Checked on the X399 on 2 October 2026 with a 320×200 PNG:

- Paste into the text area: `types` `["Files"]`, `items`
  `["file:image/png"]`, one file `image.png`, `image/png`, which decodes to
  320×200 (what GitHub's editor uploads).
- Paste into editable content: the same event, and the image is inserted as an
  `<img>` with a blob URL.
- Drop of a PNG and a text file from another application onto the drop zone:
  `items` `["file:image/png", "file:text/plain"]`, both files with their names,
  types and sizes, the image decodes and the text reads back.
- The same drop on the page's heading (no drop handler) opens the PNG in the
  tab, and Back returns to the page.
- Dropped on `<input type="file" multiple>`, both files are selected (its
  `change` event lists them) and the page stays.

`DataTransfer.items` was missing (undefined) until the engine turned on
`DataTransferItemsEnabled`, which defaults to on only for the Cocoa, GTK, WPE
and Windows ports; scripts that look at `items` before `files` (GitHub's
among them) threw.

For page drag checks, `tests/fixtures/drag-and-drop.html` provides a card board,
custom text/HTML data and preview, editable targets, a link and a generated
128×96 image. Serve it over localhost. Use a second localhost tab and a
127.0.0.1 tab to check same-origin and cross-origin custom data. Native pointer
input is required: dispatched JavaScript drag events do not exercise Haiku’s
drag session.

`tools/bench/negotiated-drag.cpp` is a MIME-only native source with an adjustable
reply delay. Use it to check delayed image/text/URL/HTML drops, navigation while
a reply is pending, and replies arriving after the timeout. The helper retains
the delivered message for its reply; copying a Haiku `BMessage` clears delivery
metadata ([Haiku implementation](https://github.com/haiku/haiku/blob/master/src/kits/app/Message.cpp)).

### Native acceptance, 21 October 2026 (#90)

An isolated airOS workstation bundle at `5b804d7` passed **23 native checks**
with real pointer drags. The complete payload build passed 30 steps, followed
by five steps for the preview pixel-format correction. The deployed library
hashes and all 1,456 modified engine source files matched the pinned patch.

- The original HTML5 demo, hosted locally, and the live
  `https://html5demos.com/drag` redirect both accept a dragged item into the bin.
  The live site redirects to `https://bestvpn.org/html5demos/drag`; its footer
  covers the first item, so the test used the visible second item without
  changing the page. The fixture board reorders C/A/B and reports a move.
- Plain text, original HTML and custom string data reach the page. The textarea
  receives the exact Unicode text; rich editing preserves bold text (the
  sanitizer adds style attributes, so this is checked semantically).
- Same-origin cross-tab drops retain custom data. A 127.0.0.1 destination hides
  the localhost custom type and value while receiving standard text and HTML.
  Hover handlers cannot read the protected values or files.
- A page image arrives as a 128×96 PNG file. Dragging it into Tracker saves
  `Image.png`, 361 bytes, with MIME type `image/png` and exact red/green/blue
  stripe pixels; the source reports a copy. Dragging that saved Tracker file
  back into a file input preserves its bytes, dimensions and pixels.
- StyledEdit inserts the exact selected Unicode text and newline into the
  textarea. A page link dropped on the bookmarks bar has the expected title
  and URL. Rejected drops report no accepted operation. Drags leave the system
  clipboard count unchanged.
- ShowImage's negotiated PNG drop succeeds. A delayed native image reply is
  accepted; replies after navigation or the five-second timeout are ignored.
  Negotiated text, URI-list and HTML also work, and script markup is stripped
  from the HTML. Source logs confirm the late replies were actually sent.

The owned test browser quit cleanly, fixture windows were closed and the pointer
was restored. Native drag-preview pixels and hotspot appearance were not
independently certified: VNC and BScreen captures omitted both Summit's preview
and a standalone control application's solid magenta drag bitmap. The native
session, bounded BGRA bitmap transport and functional drop checks above were
verified; this capture limitation is not a visual acceptance claim.
