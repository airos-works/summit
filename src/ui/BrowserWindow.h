#pragma once
#include "core/Profile.h"
#include "core/Suggest.h"
#if SUMMIT_MODERN_WEBKIT
#include <WebKit/WebKitView.h>
#include <DirectWindow.h>
#include <Window.h>
#else
#include <WebWindow.h>
#endif
#include <Messenger.h>
#include <Message.h>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

class BCardLayout;
class BFilePanel;
class BGroupLayout;
class BStatusBar;
class BGroupView;
class BMenu;
class BMenuItem;
class BMessageRunner;
class BStringView;
class BTextControl;
#if !SUMMIT_MODERN_WEBKIT
class BWebDownload;
#endif

namespace summit {
class FrameHistory;
class ToolButton;
class ExtensionActionButton;
class TabStrip;
class ProgressLine;
class DownloadsButton;
class ZoomButton;
class BookmarksBar;
class AddressSuggestions;
class FaviconCache;
class SharedProfile;
class ProtocolHandlers;
class PageTranslator;
struct ExtensionStoreItem;
// How a new window starts.
struct BrowserWindowOptions {
    // The window's place in the saved session (windows are saved in key order).
    uint64 key = 0;
    // Pages to open; when empty, the saved tabs of session are restored.
    std::vector<std::string> urls;
    WindowSession session;
    // A page another page opened (B_WEBKIT_NEW_PAGE_REQUESTED), shown as the only tab.
    uint64 newPage = 0;
    std::string newPageURL;
    // Invalid: placed by the window itself.
    BRect frame;
    // Start without tabs; an extension's windows.create() fills the window.
    bool empty = false;
    // A private window: its context is the application's private one, and it
    // records no history, session or icons.
    bool privateBrowsing = false;
    // A web app's window (#75): the app's identifier (Profile::webApps).
    std::string webApp;
    // A pop-up a page opened with a size (window.open() with width and
    // height): it may be small, and has no bookmarks bar (#95).
    bool popup = false;
};
#if SUMMIT_MODERN_WEBKIT
// A direct window, so that on a GPU the engine can put pages straight into
// the screen (BWebKitView::WindowDirectConnected).
using BrowserWindowBase = BDirectWindow;
using BrowserWebView = BWebKitView;
#else
using BrowserWindowBase = BWebWindow;
using BrowserWebView = BWebView;
#endif
class BrowserWindow : public BrowserWindowBase {
public:
#if SUMMIT_MODERN_WEBKIT
    BrowserWindow(std::shared_ptr<SharedProfile> profile, std::string startURL, const BrowserWindowOptions& options,
        std::shared_ptr<BWebKitContext> context, bool extensionsEnabled = false);
#else
    BrowserWindow(std::shared_ptr<SharedProfile> profile, std::string startURL, const BrowserWindowOptions& options);
#endif
    uint64 Key() const { return fKey; }
    bool IsPrivate() const { return fPrivate; }
    // Closing, or quitting with Summit: no new menus.
    bool IsClosing() const
    {
#if SUMMIT_MODERN_WEBKIT
        return fClosingWindow;
#else
        return false;
#endif
    }
    // Browser windows that are open and not closing, across the application.
    static int32 CountOpenWindows();
    ~BrowserWindow() override;
    void MessageReceived(BMessage* message) override;
    // The "Icon" property: the site icon of the page shown, which the Deskbar
    // shows for the window in its window lists (see NoteWindowIcon()).
    BHandler* ResolveSpecifier(BMessage* message, int32 index, BMessage* specifier, int32 what,
        const char* property) override;
    status_t GetSupportedSuites(BMessage* data) override;
    bool QuitRequested() override;
    void MenusBeginning() override;
    void FrameMoved(BPoint where) override;
    void FrameResized(float width, float height) override;
#if SUMMIT_MODERN_WEBKIT
    // index < 0 appends. A nonzero command is an extension's browser command
    // that is answered once this tab exists or could not be created.
    // A nonzero replaces closes that tab once the new one exists: extension pages and web
    // pages need different kinds of view, so moving a tab between them swaps its view.
    // A nonzero newPage shows the page of a B_WEBKIT_NEW_PAGE_REQUESTED
    // notification (url is then only what the tab shows until it loads).
    void CreateTab(const std::string& url, bool select = true, const char* extensionIdentifier = nullptr,
        int32 index = -1, uint64 command = 0, int64 replaces = 0, uint64 newPage = 0);
    void WindowActivated(bool active) override;
    // The MCP server's requests (BrowserWindowMcp.cpp): JSON text in and out,
    // with the window locked (a kMcpRequest message, or McpService's lock).
    std::string McpRequest(const std::string& request);
    // Runs function with the page view of the tab (with the window locked);
    // false when the window has no such tab. tab 0: the selected one.
    bool McpWithView(int64 tab, const std::function<void(BWebKitView&)>& function);
#if SUMMIT_MODERN_WEBKIT
    void DirectConnected(direct_buffer_info* info) override;
    // Whether app_server killed the window's direct daemon thread, which it
    // does to a DirectConnected() over half a second: the pages then get no
    // word of the window moving or closing (BrowserWindow.cpp).
    bool DirectDaemonLost() const;
    // Until app_server's first DirectConnected() for the shown window has
    // returned, timeout at most; whether it has.
    bool WaitForDirectConnection(bigtime_t timeout) const;
#endif
#else
    void NavigationRequested(const BString& url, BWebView* view) override;
    void NewWindowRequested(const BString& url, bool primary) override;
    void NewPageCreated(BWebView* view, BRect frame, bool modal, bool resizable, bool activate) override;
    void CloseWindowRequested(BWebView* view) override;
    void LoadNegotiating(const BString& url, BWebView* view) override;
    void LoadCommitted(const BString& url, BWebView* view) override;
    void LoadProgress(float progress, BWebView* view) override;
    void LoadFailed(const BString& url, BWebView* view) override;
    void LoadFinished(const BString& url, BWebView* view) override;
    void MainDocumentError(const BString& url, const BString& error, BWebView* view) override;
    void TitleChanged(const BString& title, BWebView* view) override;
    void StatusChanged(const BString& status, BWebView* view) override;
    void NavigationCapabilitiesChanged(bool back, bool forward, bool stop, BWebView* view) override;
    void CreateTab(const std::string& url, bool select = true, BWebView* adopted = nullptr);
#endif
private:
#if SUMMIT_MODERN_WEBKIT
    struct CloseFocusState {
        int64 selected = 0;
        BMessenger focus;
        std::string address;
        int32 selectionStart = 0, selectionEnd = 0;
        uint64 generation = 0;
    };
#endif
    struct Tab {
        int64 id;
        BrowserWebView* view;
        std::string url, title;
        bool loading = false, back = false, forward = false;
        // The page captures the camera, microphone or a screen.
        bool capturingCamera = false, capturingMicrophone = false, capturingScreen = false;
        float progress = 0;
        // For summit:history and summit:bookmarks, the page revision shown.
        uint64 pageRevision = 0;
        int64 bookmarkQuery = 0;
        unsigned bookmarkQuerySection = 0;
        // The site whose remembered zoom the page has (ZoomKey()).
        std::string zoomKey;
#if SUMMIT_MODERN_WEBKIT
        BMessenger messenger { };
        // The engine's web process (team) showing the page and the page's
        // identifier in it, from the engine's state notifications.
        int32 processID = 0;
        uint64 pageIdentifier = 0;
        // When frames reached the view (FrameHistory.h); the view owns it.
        FrameHistory* frames = nullptr;
        bool processExited = false;
        std::string processError;
        std::string loadError { }, loadErrorDescription { }, loadErrorDomain { }, loadErrorURL { };
        // A notice the status line shows instead of "Ready" until noticeUntil
        // (system_time), such as a passkey request no extension answered.
        std::string notice { };
        bigtime_t noticeUntil = 0;
        std::string loadOutcome { "idle" };
        int32 loadErrorCode = 0;
        bool loadErrorProvisional = false;
        uint64 loadGeneration = 0, loadSuccessSequence = 0;
        bool closeQueued = false;
        bool closeRequested = false;
        bool closeApproved = false;
        std::optional<CloseFocusState> closeFocus { };
        double pageZoom = 1, textZoom = 1;
        // system_time() of the last load's start and end, for benchmarks.
        bigtime_t loadStartedAt = 0, loadFinishedAt = 0;
        // A restored background tab loads this when it is first selected.
        std::string deferredURL;
        // The tab's Developer Tools window, while it is open.
        BMessenger devTools { };
        // The last load failed because the server's certificate failed
        // verification: what the warning shows and what Continue trusts.
        struct CertificateProblem {
            std::string url, host, sha256, subject, issuer, problem, names;
            double validFrom = 0, validUntil = 0;
            uint64 generation = 0;
            bool asked = false;
        };
        std::optional<CertificateProblem> certificate { };
        BMessage connectionCertificate;
        bool readerAvailable = false, readerActive = false, readerExtracting = false;
        uint64 readerProbedGeneration = 0, readerRequest = 0, readerGeneration = 0;
        std::string readerRequestURL, readerSource, readerURL, readerHTML;
        // Older in-memory previews reopen their original web page when the
        // tab's single cached article has been replaced.
        std::map<std::string, std::string> readerOriginals;
        struct TranslationState {
            std::string phase, from, to, message, action, pendingIDs;
            uint64 probed = 0, generation = 0, request = 0, epoch = 0, job = 0;
            bigtime_t probeAfter = 0;
            unsigned probeAttempts = 0;
            bool active = false, dismissed = false, truncated = false;
        } translation;
        // The card on an extension store page: what was last put in the page
        // (empty: none), and the page whose card was closed.
        std::string storeOffer, storeOfferDismissed;
        // A small tab at the start of the strip (#68), whether the page plays
        // sound and whether it is muted (#70).
        bool pinned = false, playingAudio = false, audioMuted = false;
        bool softwareRendering = false;
        // The page asks for the screen to stay on: a video with sound plays,
        // or it holds a screen wake lock (#88).
        bool keepScreenAwake = false;
        // Why the page's process ended ("crash", "memory"...), the view that
        // says so in place of the page, and whether that happened while the
        // tab was in the background (it then reloads when selected) (#71).
        std::string exitReason;
        BView* crashView = nullptr;
        bool exitedInBackground = false;
        // The page stopped answering input (#71).
        bool unresponsive = false, waitedForPage = false;
        // The page area says why the last page could not be loaded (#89):
        // the view, the address that failed and what the page and tab are
        // called, the load it is for, whether Back returns to the page
        // behind it and whether the network was down (it then loads again
        // by itself when the network is back).
        BView* errorView = nullptr;
        std::string errorURL, errorTitle;
        uint64 errorGeneration = 0;
        bool errorBack = false, errorOffline = false;
        // The load whose media document took over (#95), recorded in history once.
        uint64 mediaGeneration = 0;
        // The address field shows the address that failed, except while
        // another load is on its way.
        const std::string& ShownURL() const { return errorView && (errorGeneration == loadGeneration || !loading) ? errorURL : url; }
        const std::string& ShownTitle() const { return errorView ? errorTitle : title; }
        // The last pop-up the page tried to open without a click, and how
        // many (#73).
        std::string blockedPopup;
        int32 blockedPopups = 0;
        // A sign-in the page just submitted, waiting for Save or Never (#67).
        struct PendingPassword { std::string origin, username, password; bool update = false; };
        std::optional<PendingPassword> pendingPassword;
        // The video a context menu or the page chose for Picture in Picture (#74).
        std::string pipToken;
#endif
    };
#if SUMMIT_MODERN_WEBKIT
    Tab* FindTab(const BMessenger& view);
    void WebKitStateChanged(const BMessage& message);
    void ShowTabStatus(const Tab&);
    // Asks whether to continue to a site whose certificate is not trusted.
    void AskAboutCertificate(Tab&);
    void CertificateDecision(const BMessage&);
    void ShowCertificate();
    void ProbeReader(Tab&);
    void ToggleReader();
    void ReaderResult(const BMessage&);
    void ReaderResource(const BMessage&);
    uint64 fReaderRequest = 0;
    void ProbeTranslation(Tab&, bool manual = false);
    void StartTranslation(Tab&);
    void RestoreTranslation(Tab&);
    void CancelTranslation(Tab&, bool invalidated = true);
    void PrepareTranslation(Tab&);
    void TranslationScript(Tab&, const char* action, const std::string& expression, bool library = false);
    bool TranslationResult(const BMessage&);
    void TranslationStatus(const BMessage&);
    void TranslationTick();
    void ScheduleTranslationTick();
    bool TranslationNotice(Tab&);
    std::unique_ptr<PageTranslator> fTranslator;
    std::unique_ptr<BMessageRunner> fTranslationTimer;
    int64 fTranslationTab = 0;
    void WebKitFindResult(const BMessage& message);
    void WebKitCloseResult(const BMessage& message);
    void WebKitClosePrompt(const BMessage& message);
    void WebKitCloseCommitted(const BMessage& message);
    void CommitTabCloses(const std::vector<int64>&, bool wholeWindow);
    bool PrepareNavigation();
    void InvalidateWindowClose();
    void FinishCloseTab(int64 id);
    void StartCloseRequest(Tab&);
    void ContinueTabCloses();
    void BeginWindowClose();
    void ContinueWindowClose();
    void CancelWindowClose();
    CloseFocusState CaptureCloseFocus() const;
    void RestoreCloseFocus(const CloseFocusState&);
    std::shared_ptr<BWebKitContext> fWebKitContext;
    bool fClosingWindow = false;
    // Set while a saved session's tabs are created; see CreateTab().
    bool fRestoringSession = false;
    std::string fRestoringTitle;
    bool fWindowCloseInvalidated = false;
    bool fWindowCloseQueued = false;
    bool fCloseCommitPending = false;
    bool fCommitWholeWindow = false;
    uint64 fCloseCommitIdentifier = 0;
    std::vector<int64> fCommitTabs;
    uint64 fSelectionGeneration = 0;
    int64 fClosePromptTab = 0;
    std::optional<CloseFocusState> fWindowCloseFocus;
    std::set<uint64> fDownloads;
    // The toolbar's downloads button and its panel (DownloadsPanel.h).
    void ToggleDownloadsPanel(bool open = false);
    void RefreshDownloadsButton();
    // wait: until the panel is gone (a window being destroyed must not
    // leave it in its subset).
    void CloseDownloadsPanel(bool wait = false);
    DownloadsButton* fDownloadsButton = nullptr;
    BMessenger fDownloadsPanel;
    std::shared_ptr<std::atomic<bool>> fDownloadsPanelMenus;
    // When the panel closed because the button was clicked: that click
    // closes it, and does not open it again.
    bigtime_t fDownloadsPanelClosedAt = 0;
    // The card Summit puts on extension store pages (core/StoreOffer.h):
    // Install Extension, or Manage Extension when it is installed.
    void UpdateStoreOffer(Tab&);
    // A click on one of the card's links; true when url was one.
    bool StoreOfferChosen(Tab&, const std::string& url);
    void StoreItemResolved(const BMessage&);
    // The identifier an installed copy of the store page's extension has;
    // empty while (or when) that is not known.
    std::string StoreItemIdentifier(const ExtensionStoreItem&);
    // The secret the card's links carry.
    std::string fStoreOfferToken;
    // Firefox Add-ons pages name an extension by its slug; installed
    // extensions are known by their add-on ID, which Mozilla's API gives.
    struct StoreItemIdentity {
        bool asked = false, resolved = false;
        // Not a theme or a dictionary, which Summit cannot install.
        bool extension = true;
        std::string identifier, name;
    };
    std::map<std::string, StoreItemIdentity> fStoreItems;
#else
    Tab* FindTab(BWebView* view);
#endif
    Tab* ActiveTab();
    Tab* FindTabByID(int64 id);
    void ProfileChanged(const BMessage&);
    void ApplyInterfaceStyle();
    void RequestNewWindow(const std::vector<std::string>& urls, bool privateWindow);
    // Gives the tab its site's remembered zoom when its site changed.
    void ApplySiteZoom(Tab&);
    // Zooms the current page one step in (> 0) or out (< 0), or back to 100% (0).
    // Zooms the tab and remembers the zoom for its site (its other tabs follow).
    void SetTabZoom(Tab& tab, double zoom);
    // Moves/resizes the window to a browser command's left/top/width/height.
    void ApplyCommandFrame(const BMessage& message);
    void ChangeZoom(int direction);
    void ShowZoom();
    void ShowSearchEngine();
    void ShowTabMenu(const BMessage&);
    void UpdateWindowList();
#if SUMMIT_MODERN_WEBKIT
    void ShowPageContextMenu(const BMessage&);
    // Opens the current tab's Developer Tools, or brings them forward;
    // panel ("elements", "network"...) chooses what they show. request's
    // "pick" starts the element picker, its "inspect" (a point of the page
    // view) selects the element there.
    void ShowDeveloperTools(const char* panel = nullptr, const BMessage* request = nullptr);
    // View › View Page Source: the current page's HTML as the server sent it.
    void ShowPageSource();
    // wait: until the window is gone, which a window that is being destroyed
    // owes the engine (the tools' connection ends on the WebKit thread).
    void CloseDeveloperTools(Tab&, bool wait = false);
    void NewPageRequested(const BMessage&);
    void LinkOpenRequested(const BMessage&);
    void LinkHovered(const BMessage&);
    void SaveLinkAs(const std::string& url, const std::string& filename);
    void DownloadFinishedForSave(const BMessage&);
    // Save panels waiting for a chosen place, and downloads to move there.
    std::map<std::string, std::string> fSaveAsTargets;
    std::map<uint64, std::string> fSaveAsDownloads;
    std::unique_ptr<BFilePanel> fSavePanel;
    std::string fSavePanelURL;
    std::string fHoveredLink;
    // Background tabs opened from the current tab go after it, in order.
    int32 fBackgroundInsert = -1;
    int32 BackgroundTabIndex();
    bool fQuittingApp = false;
    uint64 fCloseWindowCommand = 0;
    // When the window was last activated (the MCP server's "front window").
    bigtime_t fActivatedAt = 0;
#endif
    // Benchmark input synthesis: posts a paced burst of mouse wheel or key
    // events to the active page, so scrolling can be measured the way a person
    // produces it. Refused unless SUMMIT_ENABLE_INPUT_SYNTHESIS=1 (see
    // docs/performance.md); nothing in the browser sends this by itself.
    void SimulateScroll(const BMessage&, BMessage& reply);
    struct ScrollBurst {
        BMessenger view;
        BMessenger window;
        BMessage event;
        int32 count = 0;
        bigtime_t interval = 16666;
    };
    bool fScrollBurstActive = false;
    int32 fScrollBurstRequested = 0;
    int32 fScrollBurstSent = 0;
    bigtime_t fScrollBurstDuration = 0;
    status_t fScrollBurstStatus = B_OK;
#if SUMMIT_MODERN_WEBKIT
    BMessage fScrollBurstCompletionFrameStats;
    bool fScrollBurstCompletionFrameStatsAvailable = false;
#endif
    static status_t RunScrollBurst(void* burst);
    void SelectTab(int64 id, bool forClose = false);
    void CloseTab(int64 id);
#if SUMMIT_MODERN_WEBKIT
    void ReplaceTabView(const Tab& tab, const std::string& url);
#endif
    void Navigate(const std::string& text);
    void RunBookmarklet(const std::string& url);
    // Address field type-ahead: the typed site is completed in the field and
    // matching pages are listed under it (core/Suggest.h, AddressSuggestions.h).
    void AddressModified();
    // The arrow keys, Escape and Tab in the field; true when used.
    bool AddressKey(char key);
    void ShowSuggestions(const std::string& typed);
    void HideSuggestions();
    // Has the network connect to the server of an address the user is likely
    // to go to next (DNS, TCP, TLS), so its first request leaves at once.
    void PreconnectTo(const std::string& url);
    // Where Enter in the field goes: the selected suggestion, the completed
    // site, or the text as typed.
    std::string AddressTarget() const;
    std::unique_ptr<AddressSuggestions> fSuggestions;
    // What was typed in the field, and what the field shows for it (the
    // typed text and its completion).
    std::string fAddressTyped, fAddressShown;
    Autofill fAutofill;
    // Servers connected to ahead of time, by origin, and when.
    std::map<std::string, bigtime_t> fPreconnected;
    // When typing in the field last asked the engine for a spare web process.
    bigtime_t fProcessPrewarmedAt = 0;
#if SUMMIT_MODERN_WEBKIT
    std::atomic<thread_id> fDirectDaemon { -1 };
    std::atomic<int32> fDirectConnections { 0 };
    bool fDirectDaemonLostHandled = false;
#endif
    void RefreshChrome();
    // The window's icon for the Deskbar: the site icon of the selected tab, as
    // an archived BBitmap in "result" (no "result" without one).
    void AddWindowIcon(BMessage& message);
    // Tells the window's observers (the Deskbar watches it with
    // BHandler::StartWatching) when that icon changes.
    void NoteWindowIcon();
    const BBitmap* fNotedWindowIcon = nullptr;
    // Site icons were stored or forgotten: a new icon can sit at a freed
    // icon's address.
    bool fWindowIconStale = true;
    void AnnouncePointer();
#if SUMMIT_MODERN_WEBKIT
    void SyncBrowserWindow();
    // Extension requests to change tabs and windows (B_WEBKIT_BROWSER_COMMAND).
    // Every accepted command is answered exactly once through the context.
    struct OpenCommand {
        uint64 identifier = 0;
        size_t pending = 0;
        std::vector<BWebKitView*> views;
        std::string error;
        bool window = false;
        std::string extension;
    };
    struct CloseCommand {
        uint64 identifier = 0;
        std::set<int64> tabs;
    };
    void BrowserCommand(const BMessage&);
    void OpenTabsForCommand(const BMessage&, uint64 identifier, bool window);
    void CloseTabsForCommand(uint64 identifier, const std::vector<int64>&);
    void TabOpenedForCommand(uint64 command, Tab*, const std::string& error);
    void TabCloseSettled(int64 id, bool closed);
    void RespondToCommand(uint64 identifier, status_t, const std::vector<BWebKitView*>& = { }, const char* error = nullptr);
    bool PrepareTabNavigation(const Tab&);
    std::vector<OpenCommand> fOpenCommands;
    std::vector<CloseCommand> fCloseCommands;
    // Tabs opened for windows.create(), per extension. This single-window
    // browser closes those, never itself, when that extension removes "its" window.
    std::map<std::string, std::vector<int64>> fExtensionWindowTabs;
    void RefreshExtensionActions();
    void ExtensionActionsReceived(const BMessage&);
    void ActivateExtensionAction(const BMessage&);
    // An extension's keyboard shortcut (manifest "commands"); false when the
    // keys are not one, or Summit uses them.
    bool RunExtensionShortcut(const BMessage& keyDown);
    void PreloadExtensionAction(const BMessage&);
public:
    void DispatchMessage(BMessage*, BHandler*) override;
private:
    void ShowExtensionActions();
    // Unpin from Toolbar, for a secondary click on an action button.
    void ShowExtensionActionMenu(const BMessage& message);
    bool fExtensionsEnabled = false;
    BGroupView* fExtensionActions = nullptr;
    ToolButton* fExtensionActionsOverflow = nullptr;
    std::vector<ExtensionActionButton*> fExtensionActionButtons;
    std::vector<BMessage> fExtensionActionState;
    uint64 fExtensionActionRequest = 0;
    uint64 fExtensionActionSnapshot = 0;
    uint64 fExtensionActionRevision = 0;
    uint64 fExtensionActionInvocation = 0;
    uint64 fExtensionActionResultIdentifier = 0;
    status_t fExtensionActionResultError = B_OK;
    std::shared_ptr<std::atomic<bool>> fExtensionMenuCancelled;
#endif
#if SUMMIT_MODERN_WEBKIT
    // October 2026 features (BrowserWindowFeatures.cpp).
    // The scripts and script message handlers every web tab gets: pop-up
    // detection, the Picture in Picture API and the password manager.
    void AddPageScripts(BWebKitView* view);
    void PageScriptMessage(BMessage& message);
    void MoveTabTo(int64 id, size_t index);
    void SetTabPinned(int64 id, bool pinned);
    // Records a visit in the profile's history; outside private windows,
    // extensions hear of it (history.onVisited).
    void RecordVisit(const PageRecord& visit);
    // An extension's chrome.history call (B_WEBKIT_BROWSER_HISTORY).
    void HistoryCommand(const BMessage& message, uint64 identifier);
    // An extension's chrome.bookmarks call (B_WEBKIT_BROWSER_BOOKMARKS).
    void BookmarksCommand(const BMessage& message, uint64 identifier);
    // An extension's chrome.search call (B_WEBKIT_BROWSER_SEARCH): searches in
    // tab (the one named, else the active one), a new tab or a new window.
    void SearchCommand(const BMessage& message, uint64 identifier, Tab* tab);
    // An extension's chrome.browsingData call (B_WEBKIT_BROWSER_BROWSING_DATA),
    // and the context's answers to the removals it asked for.
    void BrowsingDataCommand(const BMessage& message, uint64 identifier);
    void BrowsingDataReply(const BMessage& message);
    // An extension's chrome.sessions call (B_WEBKIT_BROWSER_SESSIONS).
    void SessionsCommand(const BMessage& message, uint64 identifier);
    // Remembers a closed tab, or a closed window's tabs, for
    // sessions.getRecentlyClosed (not of private windows).
    void RecordClosedSession(const std::vector<PageRecord>& tabs, bool window, size_t index);
    // Reopen Closed Tab took url back.
    void ForgetClosedSessionTab(const std::string& url);
    // Tells extensions how the bookmarks changed since the last report
    // (bookmarks.onCreated and the rest), whoever changed them. Windows call
    // it when the bookmarks change; the first report only remembers them.
    void ReportBookmarkChanges();
    void ToggleTabMute(int64 id);
    // Hands tab id to target (another window), which adopts its live view.
    void DetachTab(int64 id, const BMessenger& target);
    void AdoptTab(BMessage& message);
    void TabDraggedOut(int64 id, BPoint where);
    // The page area shows the crash view of a tab whose process ended.
    void ShowCrashView(Tab& tab);
    void RemoveCrashView(Tab& tab);
    void ReloadCrashedTab(Tab& tab);
    // The bar between the toolbar and the page: an unresponsive page, a
    // password to save, a blocked pop-up (NoticeBar.h).
    void RefreshNoticeBar();
    void ResponsivenessChanged(const BMessage& message);
    void ProcessExitReason(Tab& tab, const std::string& reason);
    void PrintPage(uint64 frameIdentifier = 0, uint64 expectedGeneration = 0);
    void SaveAsPDF();
    void SaveAsPDFChosen(const BMessage& message);
    void PrintPagesReceived(BMessage& message);
    void StartPictureInPicture(const std::string& token);
    void PictureInPictureCommand(const BMessage& message);
    void SpellingCommand(const BMessage& message);
    void PasswordBarAnswer(const BMessage& message);
    void SchemeOpenRequested(const BMessage& message, Tab& tab);
    // The error page of a page that could not be loaded (#89,
    // BrowserWindowLoadErrors.cpp): shown, updated or taken away after each
    // state notification of the tab.
    void UpdateLoadErrorView(Tab& tab);
    void ShowLoadErrorView(Tab& tab);
    // focus: the page takes the focus again.
    void RemoveLoadErrorView(Tab& tab, bool focus = true);
    // Back from the error page: the page before it, which is still loaded;
    // false when there is none.
    bool LeaveLoadErrorView(Tab& tab);
    // Try Again (the button, Return, Reload): the address that failed.
    void RetryFailedLoad(Tab& tab);
    // Offline error pages load again once the network is back.
    void CheckNetworkForErrorPages();
    // A state notification whose load error is a media document taking the
    // load over: the page loaded (#95). Records the visit once.
    bool MediaTookOverLoad(Tab& tab, const BMessage& message, uint64 generation);
    std::unique_ptr<BMessageRunner> fNetworkCheck;
    bool fNetworkWasUp = false;
    // A notice for the whole window (a damaged profile was restored, #93),
    // shown in the notice bar until dismissed.
    std::string fWindowNotice;
    bool fSoftwareRenderingNoticeShown = false;
    bool fSoftwareRenderingNoticePending = false;
    // A pop-up window (BrowserWindowOptions::popup).
    bool fPopup = false;
    // When the window last got a mouse button press, and the buttons down:
    // the click that activates a window can ask a page to go full screen
    // before the window knows it is active (#95).
    bigtime_t fLastMouseDown = 0;
    int32 fMouseButtons = 0;
    class NoticeBar* fNoticeBar = nullptr;
    BMenuItem* fSpellingItem = nullptr;
    BMenuItem* fZoomTextOnlyItem = nullptr;
    BMenuItem* fCaretBrowsingItem = nullptr;
    // Save as PDF: the chosen file, waiting for the pages.
    std::unique_ptr<BFilePanel> fPDFPanel;
    std::map<uint64, std::string> fPDFTargets;
    uint64 fPrintRequest = 0;
    BMessenger fPictureInPicture;
    int64 fPictureInPictureTab = 0;
    uint64 fPictureInPictureSession = 0;
    // HTTP authentication (#87, BrowserWindowAuthentication.cpp): requests
    // that wait for a name and password, in the order they came, and the
    // sign-in sheet showing one of the selected tab's.
    struct AuthenticationRequest {
        uint64 identifier = 0;
        int64 tab = 0;
        // origin: the site's, where Remember saves (empty for a proxy).
        std::string origin, host, realm, scheme, user;
        int32 port = 0, failures = 0;
        bool secure = false, proxy = false, lookingUp = false;
        std::vector<std::pair<std::string, std::string>> accounts;
    };
    void AuthenticationRequested(const BMessage& message);
    void AuthenticationAccounts(const BMessage& message);
    void AuthenticationAnswered(const BMessage& message);
    void ShowAuthenticationPrompt();
    // A tab closes or moves to another window: its requests go on without
    // a name and password.
    void DropTabAuthentications(int64 tab);
    std::vector<AuthenticationRequest> fAuthentications;
    BMessenger fAuthenticationPrompt;
    uint64 fAuthenticationShown = 0;
    // The name last signed in with, by protection space (AuthenticationSpace).
    std::map<std::string, std::string> fAuthenticationUsers;
    // A web app's window (#75): no tabs, toolbar or bookmarks bar.
    std::string fWebApp;
    std::string fWebAppScope, fWebAppName;
    void InstallWebAppResult(const BMessage& message);
    void WebAppInstalled(const BMessage& message);
public:
    const std::string& WebAppID() const { return fWebApp; }
private:
#endif
    // Built-in pages (summit:home, summit:history, summit:bookmarks) are
    // files; this gives the address to load for any address.
    std::string LoadableURL(const std::string& url);
    std::filesystem::path InternalPagePath(const std::string& url, int64 bookmarkQuery = 0, unsigned section = 0) const;
    int64 BookmarkQueryForURL(const BString& url, unsigned* section = nullptr) const;
    void UpdateBookmarkQueryLocation(Tab&, const BString& url);
    bool WriteInternalPage(const std::string& url, int64 bookmarkQuery = 0, unsigned section = 0);
    void ShowBookmarkQuery(int64 id, unsigned section = 0);
    void ShowInternalPage(const std::string& url);
    void RefreshInternalPage(Tab& tab, bool force = false, bool navigate = false);
    void PagesChanged();
    void BookmarksChanged();
    void RefreshBookmarks();
    void RebuildDynamicMenus();
    void ShowBookmarkMenu();
    // Editing bookmarks (BookmarkEditor) and the summit:bookmarks page's
    // requests (summit-bookmarks: links; true when url was one).
    void OpenBookmarkEditor(int64 id);
    void OpenNewBookmarkEditor(int64 folder, bool newFolder);
    void BookmarkEdited(const BMessage& message);
    void DeleteBookmark(int64 id, bool confirm);
    bool BookmarkPageRequest(Tab& tab, const std::string& source, const std::string& url);
    void ExportBookmarks();
    void ExportBookmarksTo(const BMessage& message);
    std::unique_ptr<BFilePanel> fExportPanel;
    void IconLoaded(const BMessage& message);
#if SUMMIT_MODERN_WEBKIT
    void FullScreenRequested(const BMessage&);
    bool SetPageFullScreen(int64 tab, bool enter);
    void ExitPageFullScreen();
    int64 fFullScreenTab = 0;
    BRect fBeforeFullScreen;
    window_look fBeforeFullScreenLook = B_TITLED_WINDOW_LOOK;
    uint32 fBeforeFullScreenFlags = 0;
    std::vector<BView*> fFullScreenHidden;
    BStringView* fFullScreenNotice = nullptr;
    std::unique_ptr<BMessageRunner> fFullScreenNoticeTimer;
    // Keeps the screen blanker away while the current tab asks (#88).
    void UpdateKeepScreenAwake();
    void KeepScreenAwake();
    std::unique_ptr<BMessageRunner> fKeepAwakeTimer;
#endif
    void SetBookmarksBarVisible(bool visible);
    std::string HomeAddress() const;
    // An extension's chrome_url_overrides.newtab page, else the home page.
    std::string NewTabAddress() const;
    std::string DisplayURL(const std::string& url) const;
    void SaveSession();
    void ShowError(const std::string& error);
    std::string StoredURL(const BString& url) const;
    std::shared_ptr<SharedProfile> fShared;
    std::unique_ptr<ProtocolHandlers> fProtocolHandlers;
    uint64 fKey = 0;
    bool fPrivate = false;
    ZoomButton* fZoomButton = nullptr;
    std::string fStartURL;
    bool fBookmarksBarVisible = true;
    std::string fInterfaceStyle;
    BGroupLayout* fLayout = nullptr;
    BGroupView* fToolbar = nullptr;
    BStatusBar* fStatusProgress = nullptr;
    // Set when the window starts closing; its open pop-up menus then close.
    std::shared_ptr<std::atomic<bool>> fMenusCancelled = std::make_shared<std::atomic<bool>>(false);
    BMenu* fWindowMenu = nullptr;
    int32 fWindowMenuFixed = 0;
    std::vector<Tab> fTabs;
    std::vector<PageRecord> fClosedTabs;
#if SUMMIT_MODERN_WEBKIT
    // browsingData removals waiting for the context, by request identifier.
    struct BrowsingDataRequest {
        uint64 command = 0;
        uint32 types = 0;
        std::vector<std::string> hosts, excludeHosts;
    };
    std::map<uint64, BrowsingDataRequest> fBrowsingDataRequests;
    uint64 fNextBrowsingDataRequest = 0;
#endif
    int64 fNextID = 1;
    int64 fSelected = 0;
    TabStrip* fTabStrip;
    BookmarksBar* fBookmarksBar;
    BCardLayout* fCards;
    BView* fPages;
    BMenu* fHistoryMenu;
    BMenu* fBookmarksMenu;
    int32 fHistoryMenuFixed = 0, fBookmarksMenuFixed = 0;
    BMenuItem* fBookmarksBarItem;
    BMenuItem* fRemoveBookmarkItem;
    BMenuItem* fEditBookmarkItem = nullptr;
    ToolButton* fBookmarkButton;
#if SUMMIT_MODERN_WEBKIT
    ToolButton* fCertificateButton = nullptr;
    BMessenger fCertificateWindow;
    int64 fCertificateTab = 0;
    uint64 fCertificateGeneration = 0;
    bool fCertificateVerified = false;
    bool fCertificateMixedContent = false;
#endif
    std::unique_ptr<FaviconCache> fFavicons;
    // Bumped whenever history, bookmarks or icons change, so open built-in
    // pages can be brought up to date when they are shown again.
    uint64 fPagesRevision = 1;
    BStringView* fStatus;
    BTextControl* fAddress;
    BTextControl* fFindText;
    BGroupView* fFindBar;
    ToolButton* fBack;
    ToolButton* fForward;
    ToolButton* fReload;
    ProgressLine* fProgress;
    std::unique_ptr<BFilePanel> fOpenPanel;
    std::unique_ptr<BMessageRunner> fSaveTimer;
#if !SUMMIT_MODERN_WEBKIT
    std::vector<BWebDownload*> fDownloads;
#endif
};
}
