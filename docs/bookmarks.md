# Bookmark folders, editing, import and export

Added 19 October 2026 for [issue #69](https://github.com/jmgasper/summit/issues/69).
Summit's bookmarks used to be a flat list with a "bar" flag; they are now two
folders, **Bookmarks Bar** and **Other Bookmarks**, that hold bookmarks and
folders nested up to 32 deep. No engine change.

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
  Edit Folder…, New Folder…) is a small window with Name, Address and a
  Folder menu that shows the folders indented by depth. An address typed
  without a scheme gets `https://`. **Delete** removes the bookmark; deleting
  a folder that has bookmarks in it asks first ("Delete the folder “Work” and
  the 12 bookmarks in it?").
- **The bookmarks bar's secondary-button menu**: for a bookmark Open, Open in
  New Tab, Edit…, Remove from Bookmarks Bar (it moves to Other Bookmarks) and
  Delete Bookmark; for a folder Open All in Tabs, Edit Folder…, New Folder…
  and Delete Folder; on the empty part of the bar New Folder… and Show All
  Bookmarks.
- **Dragging on the bar.** A button dragged along the bar moves there (a line
  shows where); dropped on the middle of a folder it goes into the folder.
  A **tab** dragged onto the bar is bookmarked where it is dropped; so are a
  link or address dragged from another application (text) and a Haiku
  bookmark file dragged from Tracker. A page that is bookmarked already moves
  to the drop place instead of being added twice. A bar button or a tab
  dragged **to Tracker** (a folder or the Desktop) becomes a Haiku bookmark
  file (`application/x-vnd.Be-bookmark`, address in `META:url`), and opening
  such a file with Summit (it is now one of Summit's supported types) opens
  its page instead of the empty file.
- **The Bookmarks tab** (`summit:bookmarks`, Alt+B) shows both folders as a
  tree. Folders open and close (the page remembers which while the tab
  lives); the search field shows matching bookmarks with the folders they are
  in, and a matching folder with everything in it. Each row has **Edit**,
  which turns it into a small form (Name, Address, Folder) saved with Enter
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
  with their folders. Addresses that are bookmarked already are skipped, so
  importing again adds only what is new; folders left empty are not made,
  and an "Imported from …" folder that exists is filled again (its folders of
  the same name merged). javascript: bookmarklets and Firefox's `place:`
  queries are counted and left out. The window then says what happened:
  "Imported 16 bookmarks from WebPositive into the folder “Imported from
  WebPositive” in Other Bookmarks. 1 was already bookmarked. Added 21 pages
  to History."
- **File › Export Bookmarks…** saves `bookmarks.html` (Netscape bookmarks
  HTML, the format every browser imports) where the save panel says: the
  bookmarks bar as a folder marked as the toolbar (`PERSONAL_TOOLBAR_FOLDER`),
  then Other Bookmarks' contents, as Chrome writes it.

## How it works

- **The model** is `src/core/Bookmarks.{h,cpp}`: `BookmarkNode` (title,
  address or children, time added) and `Bookmarks`, which owns the two root
  folders and is the only way to change them (`Add`, `AddFolder`, `Edit`,
  `Move` — before the child now at an index, so a drag's index needs no
  adjusting — `Remove`, `RemoveURL`, `Import`). Node ids are numbered when the
  profile is read and are unique while Summit runs; the pages, menus and
  messages name bookmarks by id. Every change rebuilds a flat list of all
  bookmarks (`Pages()`, the bar's first, those directly on the bar flagged),
  which the address field's suggestions, the star and history clearing use.
  Limits: 10000 bookmarks (as before), 5000 folders, 32 levels.
- **`profile.json`** keeps the folders in a new `bookmarkTree` (`bar` and
  `other`, nested `children`) and still writes `bookmarks`, the flat list,
  with `bar` set for the bar's own bookmarks. Older builds read only the flat
  list: they show every bookmark (the bar's on their bar, the rest in Other
  Bookmarks) and ignore the folders. A profile without `bookmarkTree` (saved
  by an older build) is read from the flat list. Unusable tree entries are
  dropped rather than failing the profile.
- **Formats** are `src/core/BookmarkIO.{h,cpp}`: a tolerant Netscape HTML
  reader (`<DL>`/`<DT>`/`<H3>`/`<A>`, entities, quoted or bare attributes,
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
  first-level, then "Other Bookmarks" and "Mobile Bookmarks"; tags and
  separators are left out. History is the 2000 most recent visible pages with
  visit counts.
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
  HTML drag and drop does not work inside pages on this port (the drag client
  starts no system drag), so the page moves rows with mouse events of its own.
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

- Separators, keywords, tags and descriptions are not imported; Firefox's
  smart folders (`place:` queries) and bookmarklets are skipped.
- Links cannot be dragged out of pages onto the bar (the port starts no
  drags from pages); tabs, Tracker files and text from other applications can.
- ~~Exporting and importing back leaves out empty folders.~~ Done 20 October
  (#110): a folder empty in the file is imported (one only duplicates filled is
  still left out), and the HTML reader drops the browsers' own root folders
  (`PERSONAL_TOOLBAR_FOLDER`, `UNFILED_BOOKMARKS_FOLDER`) when they are empty,
  so an export with an empty bar does not come back with an empty "Bookmarks Bar".
- The Bookmarks menu shows no site icons.
- Opening a bookmark file with Summit from Tracker (Open With) was not tried
  on the X399; dropping one on the bar was.
