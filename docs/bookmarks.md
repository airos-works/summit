# Bookmark folders, editing, import and export

Added 19 October 2026 for [issue #69](https://github.com/jmgasper/summit/issues/69).
Summit's bookmarks used to be a flat list with a "bar" flag; they are now two
folders, **Bookmarks Bar** and **Other Bookmarks**, that hold bookmarks,
separators and folders nested up to 32 deep. No engine change.

## What the user sees

- **Folders on the bookmarks bar** have a folder icon and open as a menu when
  clicked: their bookmarks, their folders as submenus, and **Open All in
  Tabs**. This works in both interface styles (the Haiku look draws a yellow
  folder, the Safari-like look one in the text colour). Folders that do not
  fit are in the » menu with the bookmarks that do not.
- **The Bookmarks menu** lists the fixed commands (Bookmark This Page, Add to
  Bookmarks Bar, Edit Bookmark…, Remove Bookmark, New Folder…, Show All
  Bookmarks), then **Bookmarks Bar** as a submenu and Other Bookmarks' own
  bookmarks and folders, as Firefox's does. Submenus are filled when they
  open, so a menu of thousands of imported bookmarks costs nothing until it is
  used; a folder shows at most 300 entries and then More Bookmarks…, which
  opens the Bookmarks tab.
- **The star button** says where the page is bookmarked ("In Bookmarks Bar ›
  Work") and offers **Edit Bookmark…**; for a page that is not bookmarked it
  offers Add to Bookmarks Bar, Add to Bookmarks and **Add to Folder…**.
- **The bookmark editor** (Edit Bookmark…, Add to Folder…, the bar's Edit… and
  Edit Folder…, New Folder…) has Name, Address, Tags, Keywords, a multiline Description,
  and a Folder menu that shows the folders indented by depth. Separate tags
  with commas; quote a tag containing a comma, doubling any quote inside it.
  Keywords are address-field shortcuts, also separated by commas. For example,
  a bookmark of `https://example.com/search?q=%s` with keyword `find` makes
  `find two words` open that site's search. A fixed URL can have a keyword too.
  Existing keyword names retain their request data; new or renamed aliases
  inherit the first keyword's request, including POST data and character set.
  Folders do not have keywords.
  An address typed
  without a scheme gets `https://`. **Delete** removes the bookmark; deleting
  a folder that has bookmarks in it asks first ("Delete the folder “Work” and
  the 12 bookmarks in it?").
- **Imported separators** retain their positions. Menus show native separator
  lines, the bar shows vertical lines, and the Bookmarks tab shows draggable
  separator rows with Delete. A separator on the bar can be dragged or removed
  with its secondary-button menu. Clicking it opens no page, and Open All in
  Tabs ignores separators.
- **The bookmarks bar's secondary-button menu**: for a bookmark Open, Open in
  New Tab, Edit…, Remove from Bookmarks Bar (it moves to Other Bookmarks) and
  Delete Bookmark; for a folder Open All in Tabs, Edit Folder…, New Folder…
  and Delete Folder; on the empty part of the bar New Folder… and Show All
  Bookmarks.
- **Dragging on the bar.** A button dragged along the bar moves there (a line
  shows where); dropped on the middle of a folder it goes into the folder.
  A **tab** dragged onto the bar is bookmarked where it is dropped; so are a
  link dragged from a page, a link or address from another application (text),
  and a Haiku bookmark file dragged from Tracker. A page that is bookmarked already moves
  to the drop place instead of being added twice. A bar button or a tab
  dragged **to Tracker** (a folder or the Desktop) becomes a Haiku bookmark
  file (`application/x-vnd.Be-bookmark`, address in `META:url`), and opening
  such a file with Summit (it is now one of Summit's supported types) opens
  its page instead of the empty file.
- **The Bookmarks tab** (`summit:bookmarks`, Alt+B) shows both folders as a
  tree. Folders open and close (the page remembers which while the tab
  lives); the search field matches titles, addresses, tags, descriptions and keywords with the folders they are
  in, and a matching folder with everything in it. Bookmark and folder rows have **Edit**,
  which turns it into a form (Name, Address, Folder, Tags, Keywords, Description) saved with Enter
  or Save, and **Delete**. Rows are dragged with the mouse: before or after
  another row, or onto a folder's row to go inside it. **New Folder…**,
  **Import…** and **Export…** are at the top.
- **File › Import Bookmarks and History…** opens a window with:
  - **WebPositive**: its bookmarks in `~/config/settings/WebPositive/Bookmarks`
    (the count is shown; disabled when there are none) and its history.
  - **Choose File…** for Firefox (a profile's `places.sqlite`, bookmarks and
    history, or an exported bookmarks HTML file), Chrome and Chromium (the
    `Bookmarks` file of a profile; the `History` database beside it is read
    too) and any other browser's exported bookmarks HTML. A folder of
    bookmark files can be chosen too. What a file holds is recognised from
    its contents, not its name.
  - **Also import the history of visited pages** (on by default).

  Bookmarks go into a folder **Imported from WebPositive** (Firefox, Chrome,
  …; for an HTML file from another browser its file name) in Other Bookmarks,
  with their folders. Addresses that are bookmarked already keep their existing
  title and location, gain new tags and keyword aliases, and receive a description only if they have
  none. Importing again adds only what is new; folders left empty by duplicates are not made,
  and an "Imported from …" folder that exists is filled again (its folders of
  the same name merged). javascript: bookmarklets and Firefox's `place:`
  queries are counted and left out. Empty folders present in the source are
  kept, including imports containing no addresses. The window reports added
  bookmarks, folders and separators, updated metadata, skipped addresses, and imported history.
- **File › Export Bookmarks…** saves `bookmarks.html` (Netscape bookmarks
  HTML, the format every browser imports) where the save panel says: the
  bookmarks bar as a folder marked as the toolbar (`PERSONAL_TOOLBAR_FOLDER`),
  then Other Bookmarks' contents, as Chrome writes it.

## How it works

- **The model** is `src/core/Bookmarks.{h,cpp}`: `BookmarkNode` (title,
  address or children, time added, separator flag, tags and description) and `Bookmarks`, which owns the two root
  folders and is the only way to change them (`Add`, `AddFolder`, `AddSeparator`, `Edit`, `SetMetadata`,
  `Move` — before the child now at an index, so a drag's index needs no
  adjusting — `Remove`, `RemoveURL`, `Import`). Node ids are numbered when the
  profile is read and are unique while Summit runs; the pages, menus and
  messages name bookmarks by id. Every change rebuilds a flat list of all
  bookmarks (`Pages()`, the bar's first, those directly on the bar flagged),
  which the address field's suggestions, the star and history clearing use.
  Separators stay out of that flat list and address lookups. Limits: 10000
  bookmarks (as before), 5000 folders, 5000 separators, 32 levels. Each node's
  metadata is bounded to a 16 KiB description and 100 tags of up to 256 bytes;
  truncation preserves UTF-8 character boundaries. Empty and duplicate tags
  are removed, and embedded NULs are removed from metadata.
- **`profile.json`** keeps the folders in a new `bookmarkTree` (`bar` and
  `other`, nested `children`) and still writes `bookmarks`, the flat list,
  with `bar` set for the bar's own bookmarks. Older builds read only the flat
  list: they show every bookmark (the bar's on their bar, the rest in Other
  Bookmarks) and ignore the folders. A profile without `bookmarkTree` (saved
  by an older build) is read from the flat list. Unusable tree entries are
  dropped rather than failing the profile. Separators are saved with
  `"separator": true` and no address; older builds ignore them.
  Optional `description`, `tags` and `keywords` fields preserve metadata in the tree;
  older builds ignore these fields, and saving with one loses them.
- **Formats** are `src/core/BookmarkIO.{h,cpp}`: a tolerant Netscape HTML
  reader (`<DL>`/`<DT>`/`<H3>`/`<A>`/`<HR>`, entities, quoted or bare attributes,
  `ADD_DATE` in seconds, milliseconds or microseconds, unclosed lists) and
  writer; Chrome's JSON (`roots`: the bar becomes a folder, "other" comes
  first-level, other roots such as mobile bookmarks become folders; dates in
  microseconds since 1601); a folder walker for WebPositive, which asks a
  callback for each file's address so it can be tested off Haiku (on Haiku
  it reads `META:url`; the title is the file name, as Tracker shows it);
  and `places.sqlite` and Chrome's `History` through SQLite. SQLite is not
  linked (the CI link line is fixed): `/boot/system/lib/libsqlite3.so.0` is
  `dlopen()`ed for the eight functions needed. The database and its
  write-ahead log are copied to a temporary folder first, because Firefox or
  Chrome may be running and holding them. Firefox's roots are found by guid:
  the toolbar becomes "Bookmarks Toolbar", the Bookmarks Menu's own come
  first-level, then "Other Bookmarks" and "Mobile Bookmarks". Firefox type-3
  records become separators. History is the 2000 most
  recent visible pages with visit counts.
- **Separator import/export** uses `<HR>`, as in
  [Firefox's HTML importer/exporter](https://searchfox.org/firefox-main/source/toolkit/components/places/BookmarkHTMLUtils.sys.mjs).
  Optional names and added dates survive a Summit round trip. Repeat imports
  match existing separators by name and occurrence in each merged folder;
  existing nodes retain their positions, just as existing bookmarks do.
  `BookmarkImportResult::Changed()` includes folders and separators so an
  import with no new page addresses still saves and refreshes browser windows.
- **Tags and descriptions** are read from HTML `TAGS` attributes and `<DD>`
  text following bookmarks or folders. Inline HTML becomes plain text. Firefox
  databases associate tags with URLs through folders under the tags root, as
  in its [tagging service](https://github.com/mozilla-firefox/firefox/blob/main/toolkit/components/places/TaggingService.sys.mjs).
  When present, old `bookmarkProperties/description` item annotations supply
  descriptions; newer profiles can omit these annotation tables. Data already
  removed by Firefox cannot be recovered by importing it.
  HTML export writes standard `TAGS` and `<DD>` plus a `SUMMIT_TAGS` JSON
  attribute so names containing commas survive a Summit round trip. Other
  browsers can read the standard attributes; the standard comma list cannot
  distinguish a comma inside a tag name. Description whitespace is entity
  encoded so indentation in the exported file does not change it on reimport.
  Metadata-only imports mark the profile dirty and notify windows, even when
  the folder allowance is full; repeat imports with identical metadata do
  neither. Duplicate URLs are resolved by
  stable node IDs so growing folder vectors cannot invalidate merge targets.
- **Keywords** come from HTML `SHORTCUTURL`, `POST_DATA` and `LAST_CHARSET`,
  or Firefox's `moz_keywords` URL associations and optional character-set
  annotations. Multiple aliases can have different POST templates for one URL;
  all are kept. Export writes the first alias in the standard HTML attributes
  and all aliases in `SUMMIT_KEYWORDS` JSON, preserving them on a Summit round
  trip. Other browsers see the first alias through the standard attributes.
  Each bookmark keeps up to 16 aliases, with names up to 256 bytes and POST
  templates up to 64 KiB. Invalid or oversized entries are omitted rather than
  truncated into a different shortcut. Import keeps existing data when the
  same URL and keyword already exist.
- **Shortcut navigation** (`src/core/BookmarkKeywords.cpp`) follows
  [Firefox's keyword binding rules](https://github.com/mozilla-firefox/firefox/blob/main/toolkit/modules/KeywordUtils.sys.mjs):
  `%s` substitutes escaped search text, `%S` substitutes raw text, and escaped
  POST templates are decoded before parameter binding. A fixed shortcut with
  extra text falls back to normal address/search handling. `mozcharset` and
  imported character sets select the parameter encoding; unavailable or failed
  conversions fall back to UTF-8. Haiku uses its installed `libiconv.so.2`.
  ASCII keyword letters are case-insensitive; non-ASCII names currently need
  the imported spelling. Conflicting names assigned to different requests
  produce an error asking the user to edit them, rather than selecting an
  arbitrary bookmark. Expansions are bounded before allocating large results.
  Selecting a suggestion retains the shortcut input so POST data is preserved.
- **POST shortcut requests** use the modern engine's `BWebKitView::LoadForm`,
  with `application/x-www-form-urlencoded` and HTTP(S) destinations. The engine
  retains its normal navigation, redirect and history behavior. An approved
  certificate retry retains the failed request's method and body; an empty
  form body remains POST. Legacy Summit builds show an explicit unsupported
  message for POST shortcuts. GET shortcuts use the existing URL loader.
- **History import** (`Profile::ImportHistory`) adds pages history does not
  have, keeps the later visit and larger count of pages both have, and keeps
  the 2000 most recent. WebPositive's `BrowsingHistory` (a flattened message
  of "history item"s with `BDateTime` archives) is read in
  `src/ui/BookmarkImport.cpp`.
- **The Bookmarks tab** is still a file written to `<profile>/Pages`
  (`RenderBookmarksPage` in `src/core/InternalPages.cpp`). It changes
  bookmarks by navigating to `summit-bookmarks:ACTION?token=…&id=…` links
  (save, delete, move, new-folder, import, export). No page can show that
  scheme, so the engine reports the navigation to the window
  (`B_WEBKIT_SCHEME_OPEN_REQUESTED`, as the store-page card's
  `summit-extension:` links are) and nothing loads. The window acts only on
  links from the Bookmarks tab itself (the source address maps back to
  `summit:bookmarks`) carrying the token, 32 random hex digits made once per
  run and shared by all windows, which write the same page file; other pages
  cannot read a `file:` page, so they cannot learn it. Script message
  handlers were not used: they would give every page `window.webkit`, which
  some sites take to mean an iOS app's web view. The page reloads after each
  change, as before; open folders and the search text survive it.
  The Bookmarks page moves rows with its own mouse handlers. Native HTML drag
  and drop is also supported by the engine (see `clipboard-and-drag.md`).
- **Menus**: the window copies the bookmarks into `BookmarkButton` trees
  (`src/ui/Chrome.h`); `BookmarkFolderMenu` fills a submenu from its copy in
  `AttachedToWindow`, on the menu's own thread, so nothing there touches the
  profile or the window's icon cache. Only the bar's own buttons have site
  icons.
- **Drags** carry `summit:url`, `summit:title` and `summit:bookmark` (the
  node id, for moves) or `summit:tab`, plus what Tracker's negotiated drop
  needs (`be:actions` B_COPY_TARGET, `be:types` B_FILE_MIME_TYPE,
  `be:filetypes` application/x-vnd.Be-bookmark, `be:clip_name`) and the
  address as `text/plain`. Tracker answers with B_COPY_TARGET naming the
  folder and file, and the bar or tab strip writes the file
  (`WriteDroppedBookmarkFile`).
- **MCP**: the `bookmarks` tool lists the tree with ids, and imports or
  exports a path as the File menu does (for test scripts; see
  [mcp.md](mcp.md)).

## Testing

The keyword follow-up passes **295 bookmark checks** on both the host and
native airOS, the host core suite, and **33 native UI checks**. HTML and Firefox
imports, multiple aliases, profile/export round trips, metadata-only merges,
editing and renaming, UTF-8/legacy encodings, raw and escaped substitutions,
fixed shortcuts, conflicts and request bounds are covered.

`tests/NativeBookmarkKeywordTests.cpp` passes **82 assertions** against the
full browser: real address-field keys, the first suggestion's retained input,
arrow selection, GET and POST navigation, and the native bookmark editor's
Save action. A separate **19 full-browser/fixture checks** verify inline
editing/filtering, saved profile and HTML output, and the exact requests
received by a loopback server. All five expected POST requests arrive once
with the correct bytes and content type, including after each editor saves
and when using a new alias. The saved Bookmarks view was rendered by WebCore
and visually inspected. All **660 frozen source/runtime hashes** are unchanged;
the browser exits zero with no owned helpers or crash events across the
complete log interval.

`tests/CertificateDecisionTests.cpp` and `tools/test-certificate-decisions.py`
pass **64 native checks and 10 runner assertions** with this engine. They
verify that certificate approval retains the exact POST request, empty bodies
remain POST, 307 redirects retain the body, 303 redirects become GET, and
invalid/stale certificate approvals stay ineffective. The probe exits normally,
75 hashes remain unchanged, and its complete crash-log interval is clean.
These are isolated airOS candidate runs; published-package and stock-Haiku
keyword acceptance have not been performed. Full results and provenance:
[`bookmark-keywords-native-2026-10-22.json`](bookmark-keywords-native-2026-10-22.json).

The tag/description follow-up passes **237 bookmark checks** on the host and
native airOS, the host core suite, and **31 native UI checks**. It covers
Firefox tag associations, optional legacy annotations, descriptions attached
to the correct HTML node, duplicate enrichment, exact profile/export round
trips (including newlines, repeated spaces, Unicode and commas in tag names),
clearing metadata, malformed profile metadata, and size bounds. Native checks
exercise metadata-only import saving and notifications and the real native
editor's Save button with quoted tag names and a multiline description.

The full browser passes **14 checks** for importing and listing metadata,
filtering by tags and descriptions, opening the inline editor, saving quoted
tags and multiline text through the page's actual Save route, and preserving
the result in the saved profile and exported HTML. WebCore screenshots of the
editor and saved result were inspected; the editor has no horizontal overflow.
The isolated browser exits normally, leaves no helpers, and records no crash
events across the complete log interval. All **628 source and runtime hashes**
remain unchanged. This uses the previously accepted PDF engine from `4548921`
with a freshly built application, not a new published package. The native
editor probe uses a hidden window; its Save action is verified, but its layout
was not visually inspected. Results and provenance are in
[`bookmark-metadata-native-2026-10-22.json`](bookmark-metadata-native-2026-10-22.json).

The 22 October #110 follow-up passes **171 bookmark checks** on the host and
native airOS, plus the host core suite. Separator ordering, repeated and
separator-only imports, HTML/SQLite parsing, profile and export round trips,
escaped names, URL exclusion, moves/deletes, and folder/separator limits are
covered. The missing-SQLite test uses a nonexistent library name: Haiku can
resolve `/nonexistent/libsqlite3.so.0` to its installed SQLite by basename.

`tests/NativeBookmarkTests.cpp` passes **21 native UI checks** on airOS
hrev60207+1733. These use the actual `SharedProfile`/import UI code and native
menus and toolbar: both empty-folder and separator-only imports reach disk,
repeat imports send no change notification, native menu separators have no
navigation command, toolbar pixels contain a separator line, and clicking the
line opens nothing while the adjacent bookmark opens once. The tests use a
fresh isolated profile and a hidden test window, exit normally, and leave no
test process. They do not launch WebKit. The modern `BrowserWindow` translation
unit and the application's `main.cpp` also compile with these changes. Source
and binary hashes and the native results are recorded in
[`bookmark-separators-native-2026-10-22.json`](bookmark-separators-native-2026-10-22.json).

Build the native probes with `tools/bench/bookmarks-native.sh OUTPUT_DIRECTORY`
on Haiku; set `WEBKIT_INCLUDE_DIR` to the modern engine's public header directory
when it is not installed. Run `OUTPUT_DIRECTORY/BookmarkTests` and
`OUTPUT_DIRECTORY/NativeBookmarkTests NEW_PROFILE_DIRECTORY`. `CXX`, `CPPFLAGS`
and `LDFLAGS` can select the Haiku cross compiler/sysroot when building elsewhere.

Host: `tests/BookmarkTests.cpp` (`summit_bookmark_tests`): the tree
operations and limits, `profile.json` both ways (an older build's flat list,
unusable entries, what older builds read), import merging (duplicates,
unsupported addresses, folders reused, empty folders), the Netscape reader on
a Firefox-style export and malformed input, Chrome's JSON, export and
re-import, `ReadImport` on each kind of file, a WebPositive-style folder,
`places.sqlite` and Chrome's `History` made with the host's SQLite, history
merging, the Bookmarks page and its links. `tests/CoreTests.cpp` follows the
new API.

On the X399 (19 October, test copies `claude-s35/bundle-bm1` to `bm3` on CI's 0340
engine, profile `p-bm`):

- A profile in the old flat format (three bookmarks on the bar, one other)
  loaded into the bar and Other Bookmarks.
- Imports: a WebPositive-style folder made with `addattr -t string META:url`
  (subfolders kept; the one address already bookmarked skipped), a Chrome
  `Bookmarks` file (nested folders), a Firefox HTML export (the bookmarklet
  counted and left out), a `places.sqlite` through the system's
  `libsqlite3.so.0` (merged into the existing "Imported from Firefox", history
  added), and the real WebPositive folder from File › Import Bookmarks and
  History… (16 bookmarks in its folders, 21 pages of history).
- On the Bookmarks tab: folders opened, a bookmark dragged into a folder, the
  bar reordered by dragging, a bookmark renamed with a new address typed
  without `https://` and moved to a folder with the inline form, a folder
  made with New Folder… on the bar and others dragged into it, a folder
  renamed, a folder with six bookmarks deleted after the question.
- The bar: the folder opened as a menu with a submenu filled on opening (in
  the Haiku and the Safari-like look), a bookmark opened from it, Open All in
  Tabs, buttons reordered and dropped into the folder by dragging, a tab
  dragged onto the bar, a button dragged to the Desktop (a bookmark file with
  `META:url`, `META:title` and the type) and that file dragged back.
- The Bookmarks menu with the bar and the imported folders as submenus; Edit
  Bookmark… opened the editor, whose Folder menu moved the bookmark into a
  nested folder; the star's menu showed the folder path.
- File › Export Bookmarks… wrote `~/bookmarks.html` (`text/html`); imported
  again it added nothing ("every bookmark … is bookmarked already"); imported
  into a fresh profile it recreated the whole tree under "Imported from
  Summit" (compared node by node).
- The CI 0340 build (before this change) read a copy of the new profile: 39
  bookmarks, the bar's on its bar.
- In the page's form, Enter with nothing changed closes the form; an empty
  address keeps it open and the status line says why it was not saved.

## Not done

- Keywords are not imported; Firefox's
  smart folders (`place:` queries) and bookmarklets are skipped.
- ~~Exporting and importing back leaves out empty folders.~~ Done 20 October
  (#110): a folder empty in the file is imported (one only duplicates filled is
  still left out), and the HTML reader drops the browsers' own root folders
  (`PERSONAL_TOOLBAR_FOLDER`, `UNFILED_BOOKMARKS_FOLDER`) when they are empty,
  so an export with an empty bar does not come back with an empty "Bookmarks Bar".
- The Bookmarks menu shows no site icons.
- Opening a bookmark file with Summit from Tracker (Open With) was not tried
  on the X399; dropping one on the bar was.
