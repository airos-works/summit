# Bookmark folders, editing, import and export

Added 19 October 2026 for [issue #69](https://github.com/jmgasper/summit/issues/69).
Summit's bookmarks used to be a flat list with a "bar" flag; they are now two
folders, **Bookmarks Bar** and **Other Bookmarks**, that hold bookmarks,
separators and folders nested up to 32 deep. The original folder support
required no engine change; bookmarklet execution uses the modern engine API
added on 22 October 2026 for #110.

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
- **Bookmarklets** are saved `javascript:` addresses. Import or edit one as
  a bookmark, then select it from the Bookmarks menu or bar while viewing a
  webpage. A saved keyword can run it too. It acts on the selected page;
  Open All in Tabs skips scripts, and scripts have no Open in New Tab command.
  The Bookmarks tab shows an inert entry with editing controls and instructions
  to run it on a webpage. Ordinary address-field input still rejects raw
  `javascript:` URLs. Execution requires the modern engine and an HTTP(S) or
  local-file document; internal pages, error pages, Reader Mode, loading pages
  and documents that disable scripts cannot run bookmarklets.
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
  with their folders. For formats without item identities, addresses that are bookmarked already keep their existing
  title and location, gain new tags and keyword aliases, and receive a description only if they have
  none. Importing again adds only what is new; folders left empty by duplicates are not made,
  and an "Imported from …" folder that exists is filled again (its folders of
  the same name merged). Bookmarklets retain their script source and metadata;
  Firefox's `place:` queries retain their source and metadata, including queries
  Summit cannot yet evaluate (see **Live smart folders** below). Empty folders present in the source are
  kept, including imports containing no addresses. The window reports added
  bookmarks, folders and separators, updated metadata, skipped addresses, and imported history.
  Firefox databases retain each distinct bookmark item, including the same
  address in multiple folders. Reimporting the same database matches those
  identities, preserving local names, moves and edited addresses. Separate
  databases have separate namespaces, even when their folder IDs overlap.
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
  Chrome may be running and holding them. Firefox's roots are found by GUID:
  "Bookmarks Toolbar", "Bookmarks Menu", "Other Bookmarks" and "Mobile
  Bookmarks" remain real folders, including empty roots, so shortcuts to one
  root cannot include its siblings. Firefox type-3
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
- **Bookmarklet execution** uses `BWebKitView::RunBookmarklet`, carrying the
  selected tab's navigation generation and the renderer's document identifier.
  Both processes reject a changed document. WebCore executes the decoded
  source in the page's main world with a user gesture. A string completion
  replaces the document as HTML at the same URL without adding history;
  other results leave the document in place. Explicit browser activation
  bypasses the JavaScript URL's CSP/Trusted Types pre-navigation checks, while
  the document's policy still applies to page links and injected scripts.
  Sandbox restrictions and the script-enabled setting remain enforced.
  Script addresses are bounded to 64 KiB and reject embedded NULs. Ordinary
  URL suggestions omit them; their deliberately assigned keywords remain
  available. Bookmarklet POST requests are rejected.
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
  profile or the window's icon cache. The copied menu entries carry their
  cached site icons, as do the bar's buttons (follow-up `fe98c85`).
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

## Live smart folders

The 22 October 2026 follow-up for #110 preserves Firefox `place:` bookmarks
from HTML and SQLite, through profile saves, editing and HTML export. Opening
one evaluates its saved query against Summit's current bookmarks and history.
The bar and Bookmarks menu show results as a submenu; the Bookmarks tab opens
a result page with **All Bookmarks** and **Edit Smart Folder…** controls.
A fixed keyword can open a saved query too. Visiting pages, changing bookmarks
and clearing history update results without reimporting or saving derived
bookmarks. Open All in Tabs opens up to 50 direct page results and skips scripts
and nested queries.

`src/core/BookmarkQueries.cpp` implements a subset of the query fields and
constants in Mozilla's
[Places interface](https://github.com/mozilla-firefox/firefox/blob/main/toolkit/components/places/nsINavHistoryService.idl)
and [query parser](https://github.com/mozilla-firefox/firefox/blob/main/toolkit/components/places/nsNavHistoryQuery.cpp).
Examples supported by this implementation:

| Saved query | Live results |
| --- | --- |
| `place:sort=8&maxResults=10` | Ten most visited pages in Summit's retained history |
| `place:queryType=1&sort=12&maxResults=10&excludeQueries=1` | Ten most recently added ordinary bookmarks |
| `place:queryType=1&tag=work` | Bookmarks tagged `work` |
| `place:queryType=1&terms=reading` | Bookmarks whose title, address or tags contain `reading` |
| `place:type=6&sort=1` | Tag folders containing their bookmarked pages |
| `place:type=4&sort=1` | Retained history grouped by exact host, with pages sorted by title |
| `place:type=3&sort=4` | Calendar folders containing their matching history pages, newest visits first |
| `place:type=1&sort=4` | One row per retained visit, newest first, with a visible timestamp |
| `place:type=8&sort=1` | The imported profile’s bookmark roots, with sorted descendants |
| `place:transition=7&sort=4` | Pages with a retained Firefox download visit, newest page visits first |
| `place:type=9` | Library links to History, Downloads, Tags and All Bookmarks |
| `place:type=5&sort=4` | Calendar folders containing host folders, with pages sorted by title |
| `place:parent=toolbar_____` | The referenced toolbar's direct folder tree |
| `place:folder=10` | A legacy numeric reference to folder 10 in the imported database |

Other implemented filters include exact URI, domain (optionally exact host),
inclusive visit-time ranges, visit-count bounds, multiple required or excluded tags, bookmarked-only history,
and exclusion of queries or ordinary items. Sorts include title, address,
last visit, visit count, bookmark addition/modification times and tags. A zero result limit
means no additional query limit. For ordinary page queries, title sorting with
a limit first selects the most recently visited matches. Menus evaluate an immutable snapshot on opening;
profile changes replace the snapshot for subsequent menus. Circular query
expansion stops at a repeated query in its source scope or eight nested queries. Tag expansion
is bounded to 10,000 page memberships.

**Folder references** retain the target's imported GUID and numeric ID. Simple
one-parent shortcuts expose ordered direct children, including folders,
separators and nested shortcuts. Constrained searches (for example a parent
plus search terms or a result limit) search descendants, as
[Firefox's query implementation](https://github.com/mozilla-firefox/firefox/blob/main/toolkit/components/places/nsNavHistory.cpp)
does. Multiple parents select the union of their subtrees.
Legacy root names `BOOKMARKS_MENU`, `TOOLBAR`, `UNFILED_BOOKMARKS`,
`MOBILE_BOOKMARKS` and `PLACES_ROOT` are recognised. Missing or ambiguous targets
show an explanation with no broader substitute results. Moving, renaming or
editing a target keeps its identity; membership follows its current contents.

Each imported item keeps an `origin` containing a source scope, GUID and numeric
ID. The scope hashes the database's canonical source path, so it contains no
path text and reimporting that file remains stable. Importing a database from a
different path creates a distinct source; Summit HTML exports carry their
existing identities in `SUMMIT_ORIGIN` attributes. Profile saves and Summit
HTML export/reimport preserve those identities, even after runtime node IDs
are renumbered. Reimports find moved items by identity instead of restoring
their former positions. Keywords and native query activation retain the saved
node ID, so identical query URLs in different source scopes do not select the
first matching URL accidentally.

Standard Firefox HTML exports lack the database identities needed to recover
arbitrary folder references. Such queries remain saved with an unresolved
target explanation; they are never redirected to similarly named Summit
folders. Import `places.sqlite` when folder references must be resolved.

### Visit-time ranges

`beginTime` and `endTime` use microseconds since the Unix epoch, with inclusive
bounds. `beginTimeRef` and `endTimeRef` select the epoch (`0`, the default),
local midnight today (`1`), or the current time (`2`). For example,
`place:beginTimeRef=2&beginTime=-3600000000` finds pages visited in the last
hour. Each opening resolves relative times again. Local-midnight calculations
use the timezone offset at midnight, including daylight-saving transitions.
A zero time with reference `0` means no bound; a zero offset from today or now
still constrains the query. Invalid references and overflowing offsets produce
an explanation instead of wrapping into another date.

A page matches when **any retained visit** falls within the interval, even if
it was also visited later. Date sorting still uses the page's latest visit.
Time constraints also turn a one-parent folder shortcut into a search through
that folder's descendants, as in Firefox's
[query implementation](https://github.com/mozilla-firefox/firefox/blob/main/toolkit/components/places/nsNavHistory.cpp).

Summit records new visits at microsecond precision. Firefox `moz_historyvisits`
and Chrome `visits` imports retain exact timestamps, including multiple visits
on the same day. Profiles retain up to 128 recent individual timestamps per
page, plus the last visit on each of up to 60 earlier local days; at most 188
timestamps per page. Existing profiles and databases without visit tables can
only contribute the second-resolution times they already retained. Unrecorded
or expired visits cannot be reconstructed; unknown dates do not match a time
constraint. This retention limit applies within the existing 2,000-page limit.

Importing additional visits to an existing URL merges and saves those times,
notifies browser windows and refreshes an open history query. Reimporting the
same visits is idempotent. Clearing a time range removes matching retained
visits and preserves earlier visits on the same day, when available. Saving
and reopening the profile preserves the remaining timestamps and daily History
entries.

### Imported visit transitions

Ordinary page searches and individual-visit queries accept `transition=` using
Firefox's type numbers: link (1), typed address (2), bookmark (3), embed (4),
permanent/temporary redirect (5/6), download (7), framed link (8) and reload (9).
The importer retains `moz_historyvisits.visit_type` alongside each retained
microsecond timestamp. Older databases without that column, older profiles and
visits without a recorded type remain unknown; they do not become link visits.

Repeated filters require every requested type somewhere in the page's retained
history. For example, `transition=1&transition=7` selects pages with both a link
visit and a download visit. A time range may match a different visit to the same
page. Individual-visit results then show every retained visit to the selected
page, including visits with other types or outside the range. Transition filters
also combine with text, domain, URI, bookmark/folder restrictions and legacy OR
groups. Overlapping groups still return each page or retained timestamp once.

Firefox download and reload visits can have a real `visit_count` of zero. Those
visible pages are now imported when they have a last-visit timestamp. Profile
saves distinguish a known zero from an older unknown count, which retains its
existing one-visit fallback. Count filters and ordering use the known value.
Summit's aggregate navigation counter retains its existing behavior.

Summit's own downloads are history visits too, as in Firefox: starting a
download in a normal window records a download visit (transition 7) of its
original address, which does not add to the page's visit count, keeps an
existing title and otherwise takes the file's name once it is known
(`Profile::VisitDownload`, `SetDownloadTitle`). Resuming a download in the
same session adds no second visit. Private windows' downloads are not
recorded: each engine context has its own download listener
(`DownloadListener`), so a private download can never reach the public
profile, and a closed private session's listener stops recording. So
`place:transition=7` and the library's Downloads link show Summit's
downloads as well as imported ones. Tests: `DownloadVisitTests` (host and
native), `NativeDownloadHistoryTests`, `NativeDownloadVisitBrowserTests`.

Reimporting known types for an existing timestamp updates and saves history,
notifies windows and refreshes open queries without adding another URL or visit.
Repeated imports are idempotent. Sources reporting different types at the same
timestamp retain each distinct pair; Firefox visit IDs are not retained.
Discarding a timestamp through retention or history deletion also discards its
types. Removing known non-counting Firefox visits does not subtract an unrelated
counted visit. The existing 2,000-page and recent/daily timestamp bounds still
apply; hidden pages remain excluded from history imports.

This supports **retained Firefox transition data**. Classification of new Summit
navigations/downloads and mapping Chrome's different transition encoding remain
unfinished. Grouped tag, site, date and virtual-root queries with transition
conditions show an explanation. Invalid numeric filters also show an explanation
instead of reproducing Firefox's fallback to a broader query.

The [transition oracle fixture](../tests/fixtures/bookmarks/transition-query-firefox158.json)
records 99 queries from Firefox 158.0.1. Comparisons cover 69 valid current queries;
15 invalid-value and six grouped cases are checked as explanations. Nine cases
using removed `onlyBookmarked` or legacy `OR` syntax are recorded separately;
Summit's existing legacy semantics are tested independently.

### Modification-time ordering

`place:queryType=1&sort=14&maxResults=10` lists the ten most recently modified
bookmarks; `sort=13` puts older changes first. A simple folder shortcut can also
sort its direct bookmark/folder children by modification time. Equal dates use
the title and then the item's position in its parent, reversing all three keys
for descending order. Title comparison currently retains Summit's ASCII case
folding rather than Firefox's locale-aware collation.

General bookmark searches exclude saved `place:` query rows, including folder
shortcuts, as Firefox's bookmark SQL does. Direct simple-folder views still
include their child queries, with circular expansion detection. This keeps a
recently modified search from returning itself as a result.

Firefox SQLite `lastModified`, HTML `LAST_MODIFIED` and Chrome `date_modified`
values are retained when present. The in-memory and profile `modified` field
uses microseconds since the Unix epoch; zero means the source did not supply a
known date. HTML exports include the standard second-resolution `LAST_MODIFIED`
plus `SUMMIT_LAST_MODIFIED_US` so a Summit round trip preserves precise ordering.
Folders and separators retain their dates too; profiles also keep the fixed
bar/Other roots' dates. Chrome stores dates relative to 1601, which are converted
on import. Missing source dates remain unknown. An extra destination folder
created by Summit receives its local creation time.

Creating a local item sets its first modification date. Title/URL edits,
metadata changes, moves and membership changes update the affected items and
ancestor folders. Invalid operations and no-op edits leave dates alone. Imported
new items retain their source dates; identical reimports do not change dates or
undo local edits/moves. A reimport can fill a previously missing date. When it
actually enriches an existing item's metadata, that local change updates the
item and its containing folders. Query results follow these changes without
reimporting the saved query.

### Legacy combined queries

Older Firefox saved searches can separate condition groups with `&OR&`, for
example `place:domain=example.com&OR&tag=work&sort=8&maxResults=10`. Conditions
within each group must all match; a result can match either group. Domain,
text, URI, tags and tag exclusion, visit bounds, times, transitions and folder
references belong to their own group. Sorting, result grouping, query type, exclusions
and the result limit apply to the combined result, wherever those options
appear in the URL. Repeating an option keeps its last value.

Overlapping groups return each bookmark once; distinct bookmark entries with
the same address remain distinct. Sorting and limiting happen after combining
matches. Title sorts retain the existing Places rule of choosing the newest
matches before applying a limited title sort. Multiple folder groups search
descendants, rather than exposing a direct folder hierarchy. All folder
references resolve within the saved query's imported source, including when
different databases use identical IDs and query URLs. A folder condition makes
the combined search a bookmark query, as in the legacy Places builder.

The legacy SQL builder omits empty condition groups when another group is
constrained; a trailing `OR` therefore does not widen the search to everything.
An entirely unconstrained query still returns the normal unfiltered result.
An unsupported or unresolved group produces an explanation for the whole
query, without substituting results from only its supported groups. The
256-field bound includes separators and options. These rules follow the
[Firefox 60 parser](https://hg.mozilla.org/releases/mozilla-release/raw-file/FIREFOX_60_0_RELEASE/toolkit/components/places/nsNavHistoryQuery.cpp)
and [combined query builder](https://hg.mozilla.org/releases/mozilla-release/raw-file/FIREFOX_60_0_RELEASE/toolkit/components/places/nsNavHistory.cpp),
which support saved multi-query URLs from older profiles.

**This does not complete Firefox smart-folder compatibility or #110.** Folder
references, visit-time ranges and imported transitions are implemented for retained
data. Classification of new navigations remains unfinished. Annotations, frecency
sorts, other groupings and more complex grouped filters remain unsupported.
Unsupported queries stay saved and show an explicit explanation, with no
broader substitute results. Query history is limited to Summit's retained 2,000
pages; Firefox frecency is not stored. Text matching uses
ASCII case folding, and tag names match exactly, rather than Firefox's full
locale-aware collation. Parameterized query keywords are not supported.

### History grouped by site

`place:type=4&sort=1` displays retained history in host folders, with pages
sorted by title within each folder. Subdomains are separate hosts; scheme and
port do not split a host. Local files use a `(local files)` folder. These are
live, transient results in the query page and native bookmark menus. A host's
native **Open All in Tabs** action carries its page URLs, without creating a
saved folder. New visits, history imports and clearing history update the query.
Only the original `place:` URL is saved or exported.

The result limit applies to host folders, not their children. As in Firefox,
title sorting happens after choosing the limited set of hosts: with
`sort=2&maxResults=2`, the first two hosts in the underlying ascending host order
are displayed in reverse title order. URI sorting (`sort=5/6`) orders host
containers before limiting. Date, visit-count and metadata sorts order pages
inside each host; they leave the host folders in their natural order. With
`sort=0`, child order follows Summit's retained history order; Firefox's
unspecified order depends on its database insertion order.

Site queries accept an inclusive time range with both `beginTime` and `endTime`,
including relative references, optionally combined with `domain` and
`domainIsHost`. An earlier retained visit can qualify a page even after a newer
visit falls outside that range. `queryType=1` still produces history containers,
as Firefox does for this result type. `excludeQueries=1` hides the generated
folders; `excludeItems=1` or `expandQueries=0` leaves them empty.

Grouped text/tag/URI/bookmark-only filters, visit-count conditions, folder
references, legacy OR groups and one-sided time bounds remain unsupported and
show an explanation. Firefox's site SQL and subsequent container filtering do
not apply these conditions like an ordinary page search; Summit does not
substitute a broader search for an unsupported grouped query.

The ordering and supported membership cases were compared with the official
Firefox 158.0.1 Linux release, using an isolated generated history profile.
The checked-in [oracle fixture](../tests/fixtures/bookmarks/site-query-firefox158.json)
contains 38 result trees, with the exact input visits and source revision.
The implementation follows the release's
[site query builder](https://hg.mozilla.org/releases/mozilla-release/raw-file/48a735cc74a7e6d730f9bf59c3de58f750287a23/toolkit/components/places/nsNavHistory.cpp)
and [result sorting](https://hg.mozilla.org/releases/mozilla-release/raw-file/48a735cc74a7e6d730f9bf59c3de58f750287a23/toolkit/components/places/nsNavHistoryResult.cpp),
subject to Summit's retained-history and ASCII-collation limits described above.

### History grouped by date

`place:type=3&sort=4` displays history in **Today**, **Yesterday**, **Last 7 days**,
**This month**, the previous five calendar months, and **Older than 6 months**.
Empty periods are omitted. A prior year's month includes its year. Local
calendar boundaries follow the system timezone, including 23/25-hour DST days,
leap days and fractional-hour offsets. Opening the query resolves its dates
again; imports, visits and clearing history refresh its results.

These folders overlap, following Firefox's Places queries. Last 7 days includes
the recent day folders but appears only when there are visits older than
yesterday. This month includes recent days but appears only when there are
visits older than the last-week boundary. A folder's visibility uses a half-open
interval; its child query uses inclusive bounds. An exact midnight can therefore
appear in both Today and Yesterday when Yesterday has another qualifying visit.
The same rule applies at month boundaries. A page appears only once within each
period even if several of its retained visits qualify.

`place:type=5&sort=4` adds host folders beneath each date. The date/site view
sorts hosts and pages by title, as Firefox's generated child queries do,
independently of the root's sorting option. Date-only folders inherit the
requested page sort, defaulting to title order for `sort=0`. Equal visit counts
and last-visit timestamps retain Summit's history order; Firefox's underlying
SQL order is unspecified for those equal-ranked rows.

The result limit selects calendar folders in their normal recent-to-old order;
title sorting (`sort=1/2`) then rearranges that selected set. For example,
`type=3&sort=2&maxResults=2` displays Yesterday then Today if both exist. Other
root sort modes preserve calendar order. The limit never truncates a selected
folder's pages or host folders. `queryType=1` still produces history groups.
`excludeQueries=1` hides them; `excludeItems=1` hides leaf pages. With
`expandQueries=0`, date-only folders are empty, while date/site folders retain
their empty host folders.

The older-period lower bound preserves Firefox 158's generated value of
1,970,000,000 microseconds after the Unix epoch, resulting from its SQLite
`datetime(0,'unixepoch')` numeric conversion. This excludes visits from the
first 1,970 seconds of 1970 from that folder. Additional conditions on date
queries, including legacy OR groups, remain unsupported and show an explanation.
Firefox's date SQL does not apply those conditions like an ordinary page search.
The library-sidebar result type remains unfinished.

The [date oracle fixture](../tests/fixtures/bookmarks/date-query-firefox158.json)
captures 68 result trees from the same official Firefox release and source
revision used for site queries. Comparison preserves ordering between unequal
visit ranks and allows either order only within consecutive equal-ranked runs.
Native menus expose the date/host hierarchy and page URLs without persisting
generated folders. Query pages use nested headings and indentation for hosts.

The result pages use separate files per query so Back can restore the preceding
view. Their addresses map to the internal Bookmarks page, and their actions use
the same per-run secret and source checks as the manager. Saved `place:` URLs
are never handed to the web engine. Query pages show bookmarklets as inert
entries with instructions to run them from the bar or menu.

### Individual history visits

`place:type=1&sort=4` shows one row for each retained visit, newest first.
Repeated visits to a page remain separate. Each result displays its local date,
time, microseconds when present, and UTC offset; the offset distinguishes the
repeated hour when daylight saving ends. Native bookmark menus include the same
timestamp beside the page title. Links navigate to the original page address,
and **Open All in Tabs** opens one tab per selected visit, subject to the existing
50-tab limit. Only the query URL is saved or exported.

Visit queries use history even with `queryType=1`. Text, URI, domain, tag,
visit-count and legacy bookmark-only filters apply to the page; overlapping
legacy OR branches do not repeat its visits. Time ranges follow Firefox's
page-level `EXISTS` predicate: if a page has one retained visit in the inclusive
range, all its retained visits are returned, including those outside the range.
Visit-count conditions and ordering use the page's total count, not the number
of retained samples or visits in the range.

Date sorting uses each row's timestamp. Title and URI ties use that timestamp,
and visit-count ties do likewise. A result limit counts visits rather than
unique pages. Title limits choose the newest visits before sorting their titles.
Metadata sorts apply after limiting; their initial subset is unspecified in
Firefox's SQL. Tags are available to the visit tag comparator with
`queryType=1` unless `excludeItems=1`; otherwise it falls back to title order.
`excludeItems`, `excludeQueries` and `expandQueries` do not remove visit rows.
Parent/folder restrictions on visit results remain unsupported and show an
explanation because Firefox can route them through a separate folder shortcut.

The [visit oracle fixture](../tests/fixtures/bookmarks/visit-query-firefox158.json)
records 66 queries from Firefox 158.0.1, with the same pinned primary sources
used above. Tests preserve exact ordering wherever the source defines it. For
SQL limits that cut through equal ranks, they validate the primary ranks,
retained membership, uniqueness and subsequent ordering; unspecified subsets
are not treated as stable Firefox output. Current Firefox has removed
`onlyBookmarked`; Summit retains the already supported legacy filter.

Results remain bounded by Summit's history retention: at most 2,000 pages,
128 recent timestamps per page and up to 60 older daily samples. Aggregate
visit counts do not manufacture missing timestamps. Visits with identical
stored timestamps cannot be distinguished because Summit retains timestamps,
not Firefox visit IDs. These limits also apply to imported history.

### Virtual bookmark roots

`place:type=8` displays the bookmark roots belonging to the query's imported
Firefox source: **Bookmarks Toolbar**, **Bookmarks Menu** and **Other Bookmarks**.
The source scope and GUIDs identify them even after moving or renaming a root.
An identical query imported from a second database resolves only that source's
roots. Missing or ambiguous identities produce an explanation for the whole
query; they never fall back to another imported profile. A locally created query
without source identities addresses Summit's own
**Bookmarks Bar** and **Other Bookmarks**. Plain Firefox HTML imports retain
an unresolved source marker and show a missing-target explanation, since their
original root identities are unavailable.

Firefox's `browser.bookmarks.showMobileBookmarks` preference is not part of a
bookmark import. Scoped root queries use its default three-root view. Imported
mobile bookmarks remain in the normal bookmark tree and are accessible through
`place:parent=mobile______` in their source scope.

Empty roots stay visible. The result limit applies to roots, not their children.
Title sorts run after selecting that limit; URI sorts select their ordered roots
before limiting. Other sorts retain the root order and sort descendants instead.
Descendant folders retain their hierarchy, separators and saved query links;
visit counts, timestamps and bookmark metadata supply their sort keys. Addition
times use Summit's existing second precision, and text comparison retains the
ASCII-collation limitation described above.

`excludeItems=1` hides pages and separators throughout the hierarchy.
`excludeQueries=1` hides ordinary saved queries while retaining folder shortcuts.
`expandQueries=0` leaves saved queries as normal native actions, including in
nested folders; folder shortcuts remain expandable. Native **Open All in Tabs**
uses the displayed page URLs and order from its immutable menu snapshot. Virtual
folder IDs are transient, while saved query links keep their real bookmark IDs
and source scope. No generated tree is persisted or exported.

Additional root conditions are unsupported and show an explanation. Firefox's
virtual-root SQL returns no rows or errors for the captured condition cases;
Summit does not substitute a broader view. The
[root oracle fixture](../tests/fixtures/bookmarks/root-query-firefox158.json)
records 98 queries with both values of the mobile preference. Comparisons cover
the default root view, recursive order/filtering, limits, query links and error
cases; the non-default preference is recorded as unavailable import context.

### Library links

`place:type=9` shows Firefox's four library sections: History, Downloads, Tags
and All Bookmarks. They are generated query links, not new saved bookmarks.
History opens the date-grouped history view; Downloads matches retained Firefox
download visits; Tags follows saved bookmark metadata; All Bookmarks opens the
virtual roots belonging to the saved library query's imported source. Downloads
does not yet classify new Summit download events.

The default order is History, Downloads, Tags, All Bookmarks. Title sorting runs
after the result limit; URI sorting runs before it. `excludeQueries=1` removes
the generated links, while `excludeItems=1` retains them. `expandQueries=0`
keeps clickable links in the native menu instead of expanding submenus. Other
query conditions remain unsupported and show an explanation.

Opening a generated link retains its saved parent and a stable section, so
equal query URLs in different imports keep their own source context. Query
pages support reload, Back/Forward and a link back to their saved parent.
Changing or deleting the parent revalidates the selected section; removing it
shows an explanation. Native menus use an immutable bookmark/history snapshot,
retain the source context for unexpanded links, and stop circular expansion.
Open All uses the displayed page URLs for generated containers.

Moving imported roots preserves their identity. HTML export/reimport and profile
saves retain the original library query and source identities without storing
generated children. Plain HTML without identities leaves All Bookmarks with an
unresolved-target explanation, rather than selecting the native roots.

The [library oracle fixture](../tests/fixtures/bookmarks/sidebar-query-firefox158.json)
records 41 Firefox 158.0.1 cases. Comparisons cover 27 current queries, including
sorting, limits and exclusions; 14 cases using additional conditions, unavailable
options or removed syntax are checked as explicit explanations. Result type 7
is the deprecated tag-contents view, not the library sidebar, and remains
unsupported.

## Testing

The library-sidebar follow-up passes **244 sidebar-query checks** on the host
and native airOS. Its Firefox oracle records **41 cases**, with 27 current-query
comparisons and 14 explicit explanations. All **25 host suites** and **12
bookmark/query sanitizer suites** pass. Native coverage includes **120 UI checks**
and all previous bookmark/query suites.

The final browser passes **311 checks and 343 native action assertions**.
It covers both imported sources, all four generated sections, reload, Back/Forward,
real unexpanded native menu actions, parent edits/deletion, root moves/reimports,
HTML round trips and history clearing. The first browser run exposed a stale
parent after Back; changing the selected query/section now invalidates its page
revision. The fresh final run includes that regression. Both rendered library
pages were visually inspected. All **723 frozen hashes** remain unchanged;
all four engine images loaded; normal shutdown leaves no helpers or debugger
events across a complete log interval. The owned RAMFS was removed after an
all-team scan. Earlier failures remain in the evidence.
Evidence: [`bookmark-sidebar-queries-native-2026-10-22.json`](bookmark-sidebar-queries-native-2026-10-22.json).

The imported-transition follow-up passes **233 transition-query checks** on the
host and native airOS, including real SQLite imports, zero counts, metadata-only
reimports, profile restart, daily sampling, retention and history deletion.
The Firefox fixture records **99 cases**: 69 current-query comparisons, 15 invalid
values, six unsupported grouped cases and nine separately recorded legacy cases.
All **24 host suites** and **11 bookmark/query sanitizer suites** pass. Native
coverage also retains all earlier query suites and **106 UI checks**.

The full browser passes **260 checks and 265 native action assertions**.
It verifies live refresh when a reimport adds only transition metadata, retained
zero counts, unknown provenance, conjunctive and legacy-OR filters, page-level
time membership, visit labels, native actions, export/reimport and history
clearing. The rendered page was visually inspected. All **718 frozen hashes**
remain unchanged, all four engine images loaded, and normal shutdown leaves no
helpers or debugger events across a complete log interval. Temporary RAMFS
cleanup follows an all-team scan. Earlier test-setup and compatibility failures
are recorded separately in the evidence.
Evidence: [`bookmark-transition-queries-native-2026-10-22.json`](bookmark-transition-queries-native-2026-10-22.json).

The virtual-root follow-up passes **825 root-query checks** on the host and
native airOS. The oracle records **98 Firefox cases** across both mobile-root
preference values; comparisons cover the **49 default cases**, including 36
result trees and 13 unsupported-condition cases. All previous core/query suites
pass, as do all ten bookmark/query suites under AddressSanitizer and
UndefinedBehaviorSanitizer. Native UI coverage is **106 checks**, including
transient Open All, circular queries and expansion controls through shortcuts.

The full browser passes **227 checks and 238 native action assertions**.
It covers two source scopes, unresolved HTML roots, missing identities, hierarchy,
ordering/limits, exclusions, saved links, moving a root and its pages,
export/reimport and history clearing. The root page was visually inspected.
All **713 frozen hashes** remain unchanged, all four engine images loaded,
and normal shutdown leaves no helpers or crash events across a complete log
interval. Earlier harness failures, scope-expectation corrections and incomplete
crash-log coverage are recorded separately in the evidence. Owned RAMFS cleanup
follows an all-team scan.
Evidence: [`bookmark-root-queries-native-2026-10-22.json`](bookmark-root-queries-native-2026-10-22.json).

The individual-visit follow-up passes **907 visit-query checks** on the host
and native airOS, using **66 Firefox oracle captures**. All previous core and
bookmark/query suites pass, as do all nine bookmark/query suites under
AddressSanitizer and UndefinedBehaviorSanitizer. Native UI coverage is **91
checks**. Native DST checks exposed Haiku's incorrect negative `%z` output;
Summit formats the correct signed `tm_gmtoff` directly, with positive/negative
fractional-hour offsets and repeated DST hours covered.

The full browser passes **191 checks and 215 native action assertions**,
including repeated visit rows, visible microseconds, HTML/SQLite imports,
page-level range membership, row limits, live refresh, export/reimport, native
Open All/menu actions, page navigation and history clearing. The individual-visit
page was visually inspected. All **706 frozen hashes** remain unchanged,
all four engine images loaded, and shutdown leaves no helpers or crash events
across a complete log interval. The owned RAM filesystem was unmounted after an
all-team scan.
Evidence: [`bookmark-visit-queries-native-2026-10-22.json`](bookmark-visit-queries-native-2026-10-22.json).

The date-query follow-up passes **2,945 date-query checks**, including **68
Firefox oracle result trees**, on the host and native airOS. The existing core
and bookmark/query suites pass too; all eight bookmark/query suites pass
AddressSanitizer and UndefinedBehaviorSanitizer. Native UI coverage is **83
checks**, including calendar/host submenus and transient Open All URLs.

The full browser passes **161 checks and 188 native action assertions**.
It exercises the previous query scenarios plus HTML/SQLite calendar queries,
overlapping midnight membership, nested host headings, native activation,
additional history refreshing an open query, page navigation, group limits,
presentation flags, unsupported conditions, export/reimport and history
clearing. The nested date/site page was visually inspected. All **700 frozen
hashes** remain unchanged, all four engine images loaded, and the browser exits
normally with no helpers or crash events across a complete log interval. The
owned RAM filesystem was unmounted after scanning all 42 native teams.
Evidence: [`bookmark-date-queries-native-2026-10-22.json`](bookmark-date-queries-native-2026-10-22.json).

The site-query follow-up passes **483 site-query checks**, including **38
Firefox oracle result trees**, on the host and native airOS. The existing core
and bookmark/query suites also pass; all seven bookmark/query suites pass
AddressSanitizer and UndefinedBehaviorSanitizer. Native UI coverage is **77
checks**, including host submenus, transient Open All URLs and stable snapshots.

The full browser passes **136 checks and 179 native action assertions**.
It exercises the previous query cases plus HTML/SQLite site queries, native
activation, newly imported host folders appearing in an open query, child
navigation, host limits, earlier visits, unsupported filters, export/reimport
and history clearing. The site page was visually inspected. All **693 frozen
hashes** remain unchanged, all four engine images loaded, and the browser exits
normally with no helpers or crash events across a complete log interval. The
owned RAM filesystem was unmounted after scanning all 42 native teams.
Evidence: [`bookmark-site-queries-native-2026-10-22.json`](bookmark-site-queries-native-2026-10-22.json).

The combined-query follow-up passes **63 OR-query checks** on host and native
airOS, plus **384 host core**, **328 bookmark**, **97 general query**, **70
folder-query**, **88 time-query** and **76 modification-query** checks. Native
UI coverage totals **71 checks**. All six bookmark/query suites also pass
AddressSanitizer and UndefinedBehaviorSanitizer. Tests cover independent group
conditions, empty groups, global options/limits, overlapping matches, scoped
folder references, local changes, profile saves and HTML round trips.

The full browser passes **115 checks and 170 native action assertions**.
Two SQLite imports reuse IDs and the exact combined-query URL; native menus
and query pages resolve the correct source. Moving an item while its query is
open immediately fills the global result limit from the other group. Live
visit counts, unsupported groups and reimports are also checked. The combined
folder page was visually inspected. All **688 frozen hashes** remained unchanged,
all four engine images were loaded, and the browser exited normally without
helpers or crash events across a complete log interval. The owned RAM filesystem
was unmounted after scanning all 42 teams. Evidence:
[`bookmark-combined-queries-native-2026-10-22.json`](bookmark-combined-queries-native-2026-10-22.json).
An initial run stopped on the new harness's visit-count check: an omitted
profile count means one visit, not zero. The corrected fresh-profile run above
is the acceptance result; the application did not need a change.

The modification-time follow-up passes **76 modification-query checks** on
host and native airOS, plus **384 host core**, **328 bookmark**, **98 general
query**, **70 folder-query** and **88 time-query** checks. Native UI coverage
totals **66 checks**. The bookmark and modification suites also pass
AddressSanitizer and UndefinedBehaviorSanitizer. Source dates, unknown dates,
microsecond ordering, local edits/moves, metadata enrichment, no-op edits,
same-named separators and profile/HTML round trips are covered.

The final full browser passes **93 browser checks and 140 native action
assertions**. An open modification query reorders immediately after native
editing and moving; metadata edits update the order, while unchanged saves and
reimports preserve dates. The rendered recently-modified result was visually
inspected. All **684 frozen hashes** stayed unchanged, all four engine images
were loaded, and the browser exited normally with no helpers or crash events
across a complete log interval. The owned RAM filesystem was unmounted after
scanning all 42 teams. The first browser run stopped on an incorrectly
copied test helper's execute permission; it is not counted as acceptance.
Evidence: [`bookmark-modified-queries-native-2026-10-22.json`](bookmark-modified-queries-native-2026-10-22.json).

The visit-time follow-up passes **88 time-query checks** on host and native
airOS, along with **384 host core**, **328 bookmark**, **99 general query** and
**70 folder-query** checks. Native UI coverage increases to **62 checks**,
including additional-visit import notifications, saved timestamps and time-query
menus. The core, bookmark and time suites also pass AddressSanitizer and
UndefinedBehaviorSanitizer. Tests cover inclusive microsecond bounds, relative
times, DST midnight, fractional-hour zones, same-day history clearing, profile
round trips, both SQLite formats, repeat import and the full 2,000-page retention
limit.

The final full-browser run passes **73 browser checks and 112 native action
assertions**. Real navigation records timestamps; importing an older visit for
an existing URL refreshes an open time query and saves the data; clearing history
removes its matches. The rendered imported-visit result was visually inspected.
All **682 frozen hashes** remained unchanged, all four engine images were
verified as loaded, and the browser exited normally without helpers or crash
events across a complete log interval. The owned RAM filesystem was removed
after scanning all 42 teams. An earlier run with concurrent stress tests timed
out on one window request and lacked complete crash-log coverage; it is not
counted as acceptance. Exact artifacts, source hashes, validation corrections
and retention limits are recorded in
[`bookmark-time-queries-native-2026-10-22.json`](bookmark-time-queries-native-2026-10-22.json).

The folder-reference follow-up adds **70 folder-query checks** on host and
native airOS. The existing **328 bookmark**, **100 general query** and **384
host core** checks pass; native UI checks now total **52**. Fixtures cover
GUID, numeric and symbolic references, direct trees versus descendant
searches, separators, missing targets, duplicate addresses, overlapping
identities across two databases, moves, renames, native keywords, profile
reloads and HTML round trips. The 328 bookmark and 70 folder-query checks also
pass with AddressSanitizer and UndefinedBehaviorSanitizer.

The full browser passes **60 browser/fixture checks and 99 native action
assertions**, including the prior common-query checks. Two SQLite imports
deliberately reuse every GUID, numeric ID and query URL. Native menu and
saved-query activation select the correct source; moving targets or nested
folders changes live results, native editing keeps the source scope, and
reimports preserve local moves without adding duplicate items. The presented
scoped-folder page was visually inspected. All four selected engine images were
loaded, all **676 frozen hashes** remained unchanged, and the browser exited
normally with no helpers or crash events across the complete log interval.
The full-browser run preceded a final separator-only correction: comparing
truncated names on reimport prevents overlong separator names from multiplying.
That correction is covered by the final host, sanitizer and native focused
runs; the query evaluator and browser UI are unchanged. Evidence:
[`bookmark-folder-queries-native-2026-10-22.json`](bookmark-folder-queries-native-2026-10-22.json).

The live-query follow-up passes **328 bookmark checks** and **100 query checks**
on the host and native airOS, **384 host core checks**, and **46 native UI
checks**. `tests/BookmarkQueryTests.cpp` covers parsing, filtering, ordering,
limits, live changes, import/persistence/export, unsupported clauses and inert
script results. `tools/bench/bookmarks-native.sh` builds the core and native UI
bookmark test programs.

The full browser passes **36 browser/fixture checks and 61 native action
assertions** using HTML and Firefox SQLite imports in an isolated profile.
`tests/NativeBookmarkQueryTests.cpp` exercises native Back, actual toolbar
popup selection, address-field keyword entry, editor Save, Open All and the
confirmed clear-history handler. The controller verifies live visit ordering,
updates while a query page is open, tag results, both editors, retained
unsupported source and export. Three presented pages were captured and
visually inspected. This is airOS x86_64 candidate acceptance, not published
package or stock-Haiku runtime acceptance. All four engine images were verified
as loaded; all **673 frozen hashes** remained unchanged. The browser exited
normally, left no helpers and had no crash events across the complete log
interval. After scanning all 41 native teams, the owned RAM filesystem was
unmounted and its empty mount directory removed. Evidence:
[`bookmark-queries-native-2026-10-22.json`](bookmark-queries-native-2026-10-22.json).

The bookmarklet follow-up passes **327 bookmark checks** on both the host and
native airOS, the host core suite, and **38 native UI checks**. These cover HTML,
Chrome JSON and Firefox SQLite import, profile/export round trips, script
address validation, editing, keyword binding and native menu construction.

`tests/BookmarkletTests.cpp` and `tools/test-bookmarklets.py` pass **62 native
checks and 3 runner assertions** against the candidate engine. They verify
UTF-8 percent decoding, main-world changes, user activation, non-string and
string completion, exceptions, stale generations and document replacement,
CSP preservation for page links and injected scripts, and script-disabled
sandbox rejection. All four candidate engine images are verified as loaded;
all **75 frozen hashes** remain unchanged. The process exits normally with no
helpers or crash events across the complete log interval.

`tests/NativeBookmarkletActivationTests.cpp` passes **48 assertions** in the
full browser, including actual bar clicks, address-field keyword entry,
native editor Save, background-tab isolation, internal-page rejection and
Open All skipping scripts. A further **20 browser/fixture checks** cover
import, inline editing, persisted profile and HTML, exactly-once page requests,
and lifecycle/provenance. The saved Bookmarks page was rendered by WebCore and
visually inspected. All **660 frozen source/runtime hashes** remain unchanged;
the browser exits zero, leaves no helpers and has a clean complete crash-log
interval. These runs use an isolated airOS candidate. Stock-Haiku and published
package acceptance, local-file execution, native menu clicks and the native
editor's visual layout were not checked in this run. Results and provenance:
[`bookmarklets-native-2026-10-22.json`](bookmarklets-native-2026-10-22.json).

Run the execution probe on Haiku with `tools/test-bookmarklets.py --probe
/path/to/BookmarkletTests --bundle /path/to/lib/summit-webkit --images-helper
/path/to/NativeLoadedImages --output /path/to/new-result-directory`. Build
`tests/BookmarkletTests.cpp` against the candidate engine's headers and
`libWebKit`, with `-std=c++23 -Ivendor -lWebKit -lbe -lnetwork`. The probe includes
the helpers in `tests/ModernPDFTests.cpp`; compile only the bookmarklet source.
The runner serves its fixtures on loopback and uses a private browser context.

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

- Advanced Firefox smart-folder clauses remain unsupported; see **Live smart
  folders** above for the implemented subset and outstanding work. Keywords
  and bookmarklets are supported by the 22 October #110 follow-ups.
- ~~Exporting and importing back leaves out empty folders.~~ Done 20 October
  (#110): a folder empty in the file is imported (one only duplicates filled is
  still left out), and the HTML reader drops the browsers' own root folders
  (`PERSONAL_TOOLBAR_FOLDER`, `UNFILED_BOOKMARKS_FOLDER`) when they are empty,
  so an export with an empty bar does not come back with an empty "Bookmarks Bar".
- Tracker's Open With delivery was checked on the X399 on 20 October: a Haiku
  bookmark file delivered through `B_REFS_RECEIVED` opens its address in a new
  tab. This updates the original 19 October test limitation above.
