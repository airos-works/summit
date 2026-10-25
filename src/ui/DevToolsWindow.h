#pragma once
#if SUMMIT_MODERN_WEBKIT
#include "DevToolsPanels.h"
#include "core/DevTools.h"
#include <Messenger.h>
#include <Window.h>
#include <memory>
#include <string>

class BMessageRunner;
class BTabView;
class BWebKitInspectorSession;

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
    DevToolsWindow(BMessenger owner, int64 tab, const std::string& pageTitle, BRect browserFrame);
    ~DevToolsWindow() override;
    // Before Show(): the connection to the tab's page, whose messages are
    // announced to this window, and the page's target ("page-N") if it is
    // known, which the engine tells only the page's first connection.
    void SetInspector(std::shared_ptr<BWebKitInspectorSession> inspector, const std::string& pageTarget = { });
    void MessageReceived(BMessage* message) override;
    bool QuitRequested() override;

private:
    void Sync() override;
    void DispatchMessage(BMessage* message, BHandler* handler) override;
    // Tells the panels which one is shown, when that changed.
    void PanelShown();
    void Drain();
    void Trace(const devtools::Changes& changes);
    void Command(const BMessage& message, BMessage& reply);
    std::string StateJSON();
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
