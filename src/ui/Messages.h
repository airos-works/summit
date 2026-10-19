#pragma once
#include <SupportDefs.h>
namespace summit {
constexpr uint32 kNavigate = 'navi';
constexpr uint32 kNewTab = 'ntab';
constexpr uint32 kCreateTabOnApp = 'atap';
constexpr uint32 kCloseTab = 'ctab';
constexpr uint32 kRequestWindowClose = 'rwcl';
constexpr uint32 kWindowReadyToClose = 'wrcl';
constexpr uint32 kSelectTab = 'stab';
constexpr uint32 kBack = 'back';
constexpr uint32 kForward = 'frwd';
constexpr uint32 kReload = 'reld';
constexpr uint32 kHome = 'home';
constexpr uint32 kFocusAddress = 'fadr';
// The address field's text changed (BTextControl modification message).
constexpr uint32 kAddressModified = 'admo';
// A row of the address field's suggestion list was clicked ("index", "url").
constexpr uint32 kSuggestionChosen = 'sgch';
// After a click: closes the suggestion list if the field lost the focus.
constexpr uint32 kAddressFocusCheck = 'adfc';
// Test input for the address field (needs SUMMIT_ENABLE_INPUT_SYNTHESIS=1):
// "text" is typed into the field as key presses, or "key" ("down", "up",
// "enter", "escape", "backspace") is pressed; "select_all" first selects the
// field's text. The reply describes the field and its suggestion list.
constexpr uint32 kTypeInAddress = 'tyad';
constexpr uint32 kBookmark = 'bkmk';
constexpr uint32 kShowBookmarks = 'sbkm';
constexpr uint32 kShowHistory = 'shis';
constexpr uint32 kOpenBookmark = 'obkm';
constexpr uint32 kAddToBookmarksBar = 'abkb';
constexpr uint32 kRemoveFromBookmarksBar = 'rbkb';
constexpr uint32 kRemoveBookmark = 'dbkm';
constexpr uint32 kBookmarkButton = 'bkbt';
constexpr uint32 kToggleBookmarksBar = 'tbkb';
// Bookmarks and folders by "id" (int64, Bookmarks' node ids), to the window.
// Edit: opens the editor ("id"; without one the current page's bookmark).
constexpr uint32 kEditBookmark = 'edbk';
// The editor for a new folder in "folder" (default Other Bookmarks).
constexpr uint32 kNewBookmarkFolder = 'nbkf';
// The editor for bookmarking the current page in a folder of one's choice.
constexpr uint32 kAddBookmarkToFolder = 'abkf';
// Deletes "id"; a folder with bookmarks in it is confirmed first (the
// alert answers with kDeleteBookmarkReply, "which" and "id").
constexpr uint32 kDeleteBookmarkNode = 'dlbk';
constexpr uint32 kDeleteBookmarkReply = 'dlbr';
// The editor's answer: "mode" (BookmarkEditor::Mode), "id", "title", "url",
// "folder".
constexpr uint32 kBookmarkEdited = 'bked';
// Moves "id" into "folder" before its child at "index" (absent: at the end).
constexpr uint32 kMoveBookmark = 'mvbk';
// A page dropped on the bookmarks bar ("url", "title"): bookmarked in
// "folder" at "index", or moved there when it already is.
constexpr uint32 kDropBookmark = 'drbk';
// Opens every bookmark of folder "id" in new tabs.
constexpr uint32 kOpenBookmarkFolder = 'obkf';
// File › Import Bookmarks and History…, File › Export Bookmarks… (the save
// panel answers with kExportBookmarksChosen: "directory", "name").
constexpr uint32 kImportBookmarks = 'imbk';
constexpr uint32 kExportBookmarks = 'exbk';
constexpr uint32 kExportBookmarksChosen = 'exbc';
// The import window's worker is done: "message" (what to show), "error" (bool).
constexpr uint32 kBookmarkImportDone = 'imdn';
constexpr uint32 kClearHistory = 'clhi';
constexpr uint32 kClearHistoryReply = 'clhr';
constexpr uint32 kShowPreferences = 'pref';
constexpr uint32 kPreferencesChanged = 'prch';
constexpr uint32 kPreferencesUseCurrentPage = 'prcp';
constexpr uint32 kPreferencesState = 'prst';
constexpr uint32 kPreferencesClosed = 'prcl';
// Preferences › General › Default browser: make Summit the system's browser
// (to the application), and what the system uses now (to the Preferences
// window: "is_default", "current" the name of the browser in use, "error").
constexpr uint32 kMakeDefaultBrowser = 'dfbr';
constexpr uint32 kDefaultBrowserState = 'dfbs';
// Preferences › History and Data, sent to the application; it answers the
// window with kDataCleared ("kind": the request, "error" on failure).
constexpr uint32 kClearHistoryRequest = 'pclh';
constexpr uint32 kClearCacheRequest = 'pclc';
constexpr uint32 kClearSiteDataRequest = 'pcld';
constexpr uint32 kDataCleared = 'pdcl';
// Preferences › History and Data › Forget Trusted Certificates, to the
// application; answered with kDataCleared like the requests above.
constexpr uint32 kForgetCertificatesRequest = 'pclt';
// Preferences › Site Permissions, to the application: "permission", "origin",
// "state" (1 allow, 0 block, -1 forget).
constexpr uint32 kSitePermissionChange = 'pspc';
// Preferences › Link Handlers: forget the choice and denials for this scheme.
constexpr uint32 kProtocolHandlerRemove = 'pphr';
// To a browser window: select the tab showing "view" (a BWebKitView messenger)
// and come to the front (a click on that page's notification).
constexpr uint32 kShowTabOfView = 'shtv';
// The user chose to continue to a site whose certificate failed verification
// (a window to the application): "host", "sha256", "subject", "private". The
// application trusts it in every context and, unless private, saves it.
constexpr uint32 kTrustCertificate = 'trce';
// The answer of a window's certificate warning (its BAlert): "which", "tab",
// "generation".
constexpr uint32 kCertificateDecision = 'cedc';
constexpr uint32 kFind = 'find';
constexpr uint32 kFindNext = 'fnxt';
constexpr uint32 kFindPrevious = 'fprv';
constexpr uint32 kCloseFind = 'cfin';
constexpr uint32 kZoomIn = 'zmin';
constexpr uint32 kZoomOut = 'zmot';
constexpr uint32 kZoomReset = 'zres';
// Answers an open permission prompt as its buttons would (summitctl
// permission): "which" 0 blocks, 1 is Not Now, 2 allows; for a screen,
// "device_index" picks the screen or window shared (the first by default).
// The reply's "answered" says whether a prompt was open.
constexpr uint32 kAnswerPermissionPrompt = 'apmp';
constexpr uint32 kSaveSession = 'sess';
constexpr uint32 kNextTab = 'next';
constexpr uint32 kPreviousTab = 'prev';
constexpr uint32 kReopenTab = 'rtab';
constexpr uint32 kOpenFile = 'file';
// Opens the Downloads folder in Tracker.
constexpr uint32 kShowDownloads = 'down';
constexpr uint32 kDownloadSave = 'dsav';
constexpr uint32 kDownloadQuitReply = 'dqre';
// The toolbar's downloads button and Window › Downloads: opens or closes the
// window's downloads panel.
constexpr uint32 kToggleDownloadsPanel = 'dpnl';
// From a downloads panel to its window when it closes ("deactivated": it
// closed because something else was clicked).
constexpr uint32 kDownloadsPanelClosed = 'dpcd';
// From a downloads panel: to the application, cancel the download
// "identifier"; to its window, download "url" again.
constexpr uint32 kDownloadCancel = 'dlcn';
constexpr uint32 kDownloadRetry = 'dlrt';
constexpr uint32 kShowExtensions = 'exts';
constexpr uint32 kShowCertificate = 'cert';
constexpr uint32 kToggleReader = 'read';
constexpr uint32 kInspectExtension = 'exin';
constexpr uint32 kExtensionSelected = 'exsl';
constexpr uint32 kExtensionStoreInstall = 'exsi';
constexpr uint32 kBrowseExtensionStore = 'exbs';
constexpr uint32 kExtensionsState = 'exst';
constexpr uint32 kExtensionApprove = 'exap';
constexpr uint32 kExtensionCancel = 'exca';
constexpr uint32 kExtensionEnable = 'exen';
constexpr uint32 kExtensionRemove = 'exrm';
constexpr uint32 kExtensionManagerClosed = 'exmc';
constexpr uint32 kCloseExtensionManager = 'excl';
constexpr uint32 kActivateExtensionAction = 'exac';
// The pointer reached an action button: load its popup before the click.
constexpr uint32 kPreloadExtensionAction = 'expl';
constexpr uint32 kShowExtensionActions = 'exam';
// To the application ("extension_identifier" and "pinned" or "allowed"; from
// the Extensions window also "window"): whether the extension's action has a
// button on the toolbar, and whether the extension works in private windows.
constexpr uint32 kExtensionSetPinned = 'expn';
constexpr uint32 kExtensionSetPrivate = 'expv';
// A secondary click on an extension's toolbar button ("extension_identifier",
// "where": the screen point).
constexpr uint32 kExtensionActionMenu = 'exmn';
constexpr uint32 kBrowserState = 'stat';
// Windows. kNewWindow goes to the application ("url": repeated, optional;
// "new_page"/"new_page_url": a page opened by another page; "popup": bool;
// "frame": BRect). A window's close box asks the application with
// kCloseWindowRequest ("window"); the application answers with
// kRequestWindowClose ("quitting": bool) and the window reports
// kWindowReadyToClose or kWindowCloseCancelled.
constexpr uint32 kNewWindow = 'nwin';
// A private window (kNewWindow with "private" true). Its pages use a separate,
// in-memory website data store and leave no history.
constexpr uint32 kNewPrivateWindow = 'npwn';
constexpr uint32 kCloseWindow = 'cwin';
constexpr uint32 kCloseWindowRequest = 'cwrq';
constexpr uint32 kWindowCloseCancelled = 'wccn';
constexpr uint32 kWindowActivated = 'wact';
constexpr uint32 kActivateWindow = 'awin';
constexpr uint32 kMoveTabToNewWindow = 'mtnw';
constexpr uint32 kDuplicateTab = 'dtab';
constexpr uint32 kCloseOtherTabs = 'cotb';
constexpr uint32 kReloadTab = 'rtbx';
constexpr uint32 kProfileChanged = 'prfc';
// Page context menu commands ("url" names the link, image or media).
constexpr uint32 kOpenLink = 'olnk';
constexpr uint32 kOpenLinkInNewTab = 'olnt';
constexpr uint32 kOpenLinkInNewWindow = 'olnw';
constexpr uint32 kOpenLinkInNewPrivateWindow = 'olnp';
constexpr uint32 kCopyText = 'cptx';
constexpr uint32 kDownloadLink = 'dlnk';
constexpr uint32 kSaveLinkAs = 'slas';
constexpr uint32 kSaveLinkAsChosen = 'slac';
constexpr uint32 kSearchFor = 'srch';
constexpr uint32 kBookmarkLink = 'bklk';
constexpr uint32 kInterfaceStyle = 'istl';
// An extension's item in a page context menu ("token").
constexpr uint32 kExtensionMenuItem = 'exmi';
// View › Developer Tools, for the window's current tab. Sent to an open
// Developer Tools window it brings that window forward ("panel": "network"
// or "console", optional). The tools tell the browser window when they close
// (kDeveloperToolsClosed: "tab", "window") and are told the page's title
// (kDeveloperToolsPage: "title").
constexpr uint32 kShowDeveloperTools = 'dvtl';
constexpr uint32 kDeveloperToolsClosed = 'dvtc';
constexpr uint32 kDeveloperToolsPage = 'dvtp';
// View › View Page Source: the current page's HTML as the server sent it.
constexpr uint32 kViewPageSource = 'vpsr';
// Drives a Developer Tools window as its controls do and reports what it
// shows, for tests ("action", "argument"; the reply has "json" or "error").
// Refused unless SUMMIT_ENABLE_INPUT_SYNTHESIS=1; see docs/developer-tools.md.
constexpr uint32 kDeveloperToolsCommand = 'dvcm';
// Benchmark input synthesis, refused unless SUMMIT_ENABLE_INPUT_SYNTHESIS=1.
constexpr uint32 kSimulateScroll = 'sscr';
constexpr uint32 kScrollBurstCompleted = 'ssco';
constexpr uint32 kFrameStats = 'fsts';
}
