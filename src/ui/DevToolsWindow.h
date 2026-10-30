#pragma once
#if SUMMIT_MODERN_WEBKIT
#include "DevToolsPanels.h"
#include "core/DevTools.h"
#include <Messenger.h>
#include <Window.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class BMessageRunner;
class BTabView;
class BButton;
class BWebKitContext;
class BWebKitInspectorSession;
class BWebKitView;

namespace summit {
// View › Developer Tools: a tab's page's DOM and styles, what it requests
// from the network and writes to its console, and its storage. One window
// for each tab that has asked for one; it records from the moment it opens
// (and shows the console messages the page kept from before).
// docs/developer-tools.md describes it.
//
// The window owns the model (devtools::Session) and gives it the engine's
// protocol messages on its own thread, so a busy page's log costs the browser
// window nothing.
class DevToolsWindow final : public BWindow, private DevToolsHost {
public:
    // owner hears kDeveloperToolsClosed ("tab") when the window closes.
    // context: the inspected tab's, for extensions' devtools pages.
    DevToolsWindow(BMessenger owner, int64 tab, const std::string& pageTitle, BRect browserFrame,
        std::shared_ptr<BWebKitContext> context = nullptr);
    ~DevToolsWindow() override;
    // Extensions' developer tools (docs/webextensions-devtools.md): the
    // window's identifier in the engine's DEVTOOLS commands, and its theme
    // ("light" or "dark", from the system's colours).
    uint64 ToolsIdentifier() const { return fToolsIdentifier; }
    static const char* Theme();
    // On the application thread with this window locked
    // (kCreateDevToolsPanelOnApp): the page of a panel an extension created,
    // in a tab of its own; answers the browser command.
    void AddExtensionPanel(uint64 panel, const std::string& title, uint64 command);
    // Before Show(): the connection to the tab's page, whose messages are
    // announced to this window, and the page's target ("page-N") if it is
    // known, which the engine tells only the page's first connection.
    void SetInspector(std::shared_ptr<BWebKitInspectorSession> inspector, const std::string& pageTarget = { });
    void MessageReceived(BMessage* message) override;
    bool QuitRequested() override;
    // Whether new tools open docked: as the last ones were left.
    static bool DockedByDefault();
    // 0 under the page, 1 beside it.
    static int32 DockSideByDefault();

private:
    // Docked under the browser's page (kDeveloperToolsDock), or a window of
    // its own again.
    void Dock(const BMessage& place);
    void Undock();
    void UpdateDockButtons(int32 side);
    void Sync() override;
    void DispatchMessage(BMessage* message, BHandler* handler) override;
    // Tells the panels which one is shown, when that changed.
    void PanelShown();
    void Drain();
    void Trace(const devtools::Changes& changes);
    void Command(const BMessage& message, BMessage& reply);
    std::string StateJSON();
    // An extension devtools page's DEVTOOLS browser command.
    void ExtensionCommand(const BMessage& message);
    // Tells extensions what changed: panels shown, navigations, finished
    // requests, the Elements selection; answers their evaluations.
    void ExtensionEvents(const devtools::Changes& changes);
    void ExtensionResults();
    void ExtensionPanelState(const BMessage& message);
    std::shared_ptr<BWebKitContext> fContext;
    uint64 fToolsIdentifier;
    struct ExtensionPage {
        BWebKitView* view;
        std::string extension;
    };
    std::vector<ExtensionPage> fExtensionPages;
    // Tabs after the built-in panels, in order.
    struct ExtensionPanel {
        uint64 panel;
        std::string title;
        BWebKitView* view;
        bool loaded = false;
    };
    std::vector<ExtensionPanel> fExtensionPanels;
    BView* fExtensionPageHolder;
    // The panel extensions were told is shown (0: none).
    uint64 fShownExtensionPanel = 0;
    // Evaluations for extensions: the session's serial, the command's identifier.
    std::map<uint64_t, uint64> fExtensionEvaluations;
    uint64_t fNavigationsTold = 0;
    // panels.openResource before the Sources panel has listed the page's
    // scripts (the debugger starts when it is first shown): tried again as
    // scripts arrive, for a few seconds.
    struct PendingResource {
        uint64 command;
        std::string url;
        bigtime_t deadline;
    };
    std::vector<PendingResource> fPendingResources;
    bool OpenResource(const std::string& url, std::string& error);
    int fSelectionTold = 0;
    std::set<uint64_t> fRequestsTold;
    BMessenger fOwner;
    int64 fTab;
    devtools::Session fSession;
    std::shared_ptr<BWebKitInspectorSession> fInspector;
    std::unique_ptr<BMessageRunner> fPulse;
    BTabView* fTabs;
    ElementsPanel* fElements;
    NetworkPanel* fNetwork;
    ConsolePanel* fConsole;
    StoragePanel* fStorage;
    SourcesPanel* fSources;
    PerformancePanel* fPerformance;
    int32 fShownPanel = -1;
    BButton* fDockButton;
    BButton* fDockRightButton;
    BButton* fUndockButton;
    bool fDocked = false;
    BWindow* fDockOwner = nullptr;
    BRect fUndockedFrame;
    bool fSyncing = false;
    bool fOverflowShown = false;
    // SUMMIT_DEVTOOLS_TRACE=1: requests and messages also go to standard error.
    bool fTrace = false;
    // SUMMIT_DEVTOOLS_TRACE=2: every protocol message received as well.
    bool fTraceProtocol = false;
    size_t fTracedEntries = 0;
};
}
#endif
