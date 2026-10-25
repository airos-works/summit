#pragma once
#include <SupportDefs.h>

// Messages of the October 2026 features (issues #64-#75): tabs that can be
// pinned, muted and dragged between windows, printing, Picture in Picture,
// pop-ups, spelling, passwords and web apps. The older ones are in
// Messages.h.
namespace summit {
// Tab strip to its window. kToggleTabMute: mute or unmute tab "id".
// kPinTab: "id", "pinned" (bool). kMoveTab: put tab "id" at "index" (a drag
// along the strip). kTabDragOut: tab "id" was dropped at "where" (a screen
// point) outside the strip.
constexpr uint32 kToggleTabMute = 'tmut';
// Unload tab "id" until it is selected again (#116).
constexpr uint32 kDiscardTab = 'tdsc';
// Every 30 s: unload a long-unseen tab when memory is low (#116).
constexpr uint32 kCheckMemory = 'tmem';
// The page's context menu went ("serial" names it).
constexpr uint32 kPageMenuClosed = 'pgmc';
constexpr uint32 kPageMenuOpened = 'pgmo';
constexpr uint32 kPinTab = 'tpin';
constexpr uint32 kMoveTab = 'tmov';
constexpr uint32 kTabDragOut = 'tdro';
constexpr uint32 kCloseTabsToRight = 'ctrt';
// Moving a live tab to another window. kTransferTab, to the application:
// "window" (the messenger of the window that has it), "id", and "where" (a
// screen point: the window under it gets the tab, else a new window there)
// or "target" (that window). kDetachTab, to the window with the tab:
// "id", "target" (the window it goes to). kAdoptTab, to that window: "tab"
// (a pointer to the detached tab, which it then owns) and "index".
constexpr uint32 kTransferTab = 'ttrn';
constexpr uint32 kDetachTab = 'tdet';
constexpr uint32 kAdoptTab = 'tadp';
// File › Print… and Save as PDF…; the print job's pages come back as
// B_WEBKIT_PRINT_PAGES.
constexpr uint32 kPrint = 'prnt';
constexpr uint32 kSaveAsPDF = 'spdf';
constexpr uint32 kSaveAsPDFChosen = 'spdc';
constexpr uint32 kPageSetup = 'pstp';
// Picture in Picture of the current tab's video ("token" when the page or a
// context menu chose one; otherwise the largest playing video).
constexpr uint32 kPictureInPicture = 'pipt';
// The Picture in Picture window's controls and its closing, to the window
// that opened it: "tab", "session", "command" ("toggle", "seek:<s>",
// "skip:<s>", "volume:<0-1>", "mute", "unmute", "back", "size", "closed").
constexpr uint32 kPictureInPictureCommand = 'pipc';
// Pop-ups the current page tried to open: open the last one ("tab"), or
// always allow the page's site.
constexpr uint32 kOpenBlockedPopup = 'pbop';
constexpr uint32 kAllowSitePopups = 'pbal';
// A page that stopped responding: wait, or end its process ("tab").
constexpr uint32 kWaitForPage = 'wtpg';
constexpr uint32 kStopPage = 'stpg';
// The context menu's spelling items: "command" (B_WEBKIT_SPELLING_*),
// "word"; and Check Spelling While Typing.
constexpr uint32 kSpellingCommand = 'splc';
constexpr uint32 kToggleSpellChecking = 'spck';
// View › Zoom Text Only (Profile::zoomTextOnly).
constexpr uint32 kToggleZoomTextOnly = 'ztxt';
// View › Caret Browsing (F7), Reduce Motion and Increase Contrast: the
// toggles of the profile's settings, which every window then applies.
constexpr uint32 kToggleCaretBrowsing = 'crbt';
constexpr uint32 kToggleReducedMotion = 'rdmt';
constexpr uint32 kToggleIncreasedContrast = 'incn';
// The password bar's answers: "tab", "which" (0 save, 1 never for the site,
// 2 not now).
constexpr uint32 kPasswordBarAnswer = 'pwba';
// File › Install as App… for the current page; a web app window asks to
// open a link outside its scope in an ordinary window ("url").
constexpr uint32 kInstallWebApp = 'iwap';
constexpr uint32 kOpenInBrowser = 'oibr';
}
namespace summit {
// A print job (PrintJob.h) to its window: draw pages of tab "tab" with these
// settings ("identifier", "paper" BSize, "margins" BRect, "first", "count",
// "scale" pixels a point) and answer "reply" with B_WEBKIT_PRINT_PAGES.
constexpr uint32 kPrintPagesRequest = 'prpr';
}
namespace summit {
// The key store answered, off the window's thread (PasswordStore can wait
// for the user to allow Summit or unlock the keyring). kPasswordChecked:
// a sign-in on "origin" ("tab", "username", "password") is new ("update":
// another password was saved for the account) or already saved ("same").
// kPasswordSaved: "error" when saving failed.
constexpr uint32 kPasswordChecked = 'pwck';
constexpr uint32 kPasswordSaved = 'pwsv';
}
namespace summit {
// A page that could not be loaded (#89): its error page's Try Again ("tab"),
// and the window's look at whether the network is back.
constexpr uint32 kRetryFailedLoad = 'lerr';
constexpr uint32 kCheckNetwork = 'lenc';
// The application to a window: show "text" in the notice bar until the user
// dismisses it (kDismissWindowNotice). A damaged profile was restored (#93).
constexpr uint32 kWindowNotice = 'wntc';
constexpr uint32 kDismissWindowNotice = 'wntd';
constexpr uint32 kDismissSoftwareRenderingNotice = 'srnd';
}
namespace summit {
// Web apps (#75). kWebAppInstalled, to the window that installed it: the
// new app's "id", "name", "start_url", "scope", "launcher", or "error".
// kOpenWebApp, to the application: open app "id" (in its window, if open).
// kRemoveWebApp, to the application: uninstall app "id".
constexpr uint32 kWebAppInstalled = 'wapi';
constexpr uint32 kOpenWebApp = 'wapo';
constexpr uint32 kRemoveWebApp = 'wapr';
}
namespace summit {
// Alt+1 to Alt+8 select those tabs, Alt+9 the last one ("number").
constexpr uint32 kSelectTabNumber = 'stnb';
}
namespace summit {
// The tab menu's Bookmark All Tabs: a new folder in Other Bookmarks.
constexpr uint32 kBookmarkAllTabs = 'bkat';
}
