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
// The Picture in Picture window's buttons and its closing, to the window
// that opened it: "tab", "command" ("toggle", "back", "close").
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
