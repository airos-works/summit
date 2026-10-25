#if SUMMIT_MODERN_WEBKIT
#include "DevToolsWindow.h"
#include "Messages.h"
#include <WebKit/WebKitInspector.h>
#include <LayoutBuilder.h>
#include <MessageRunner.h>
#include <Screen.h>
#include <TabView.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace summit {
using namespace devtools;
namespace {
constexpr uint32 kPulse = 'dvpu', kTogglePicker = 'dvpk', kFocusSearch = 'dvfs';
// Where the last Developer Tools window was, for the next one.
BRect sLastFrame;

BRect FrameBeside(BRect browser)
{
    const BRect screen = BScreen().Frame();
    BRect frame = sLastFrame.IsValid() ? sLastFrame.OffsetByCopy(24, 24) : BRect(0, 0, 1039, 759);
    if (!sLastFrame.IsValid()) {
        // Over the lower part of the page it is about.
        frame.OffsetTo(browser.left + 40, browser.top + 60);
    }
    frame.right = std::min(frame.right, frame.left + screen.Width() - 20);
    frame.bottom = std::min(frame.bottom, frame.top + screen.Height() - 50);
    if (frame.right > screen.right - 5) frame.OffsetBy(screen.right - 5 - frame.right, 0);
    if (frame.bottom > screen.bottom - 5) frame.OffsetBy(0, screen.bottom - 5 - frame.bottom);
    if (frame.left < 5) frame.OffsetTo(5, frame.top);
    if (frame.top < 30) frame.OffsetTo(frame.left, 30);
    return frame;
}

// The tabs, in their order.
const char* const kPanels[] = {"elements", "network", "console", "storage", "sources", "performance"};
enum { kElementsPanel, kNetworkPanel, kConsolePanel, kStoragePanel, kSourcesPanel, kPerformancePanel };

int32 PanelIndex(const char* name)
{
    for (int32 i = 0; i < static_cast<int32>(std::size(kPanels)); ++i)
        if (!std::strcmp(name, kPanels[i])) return i;
    return kElementsPanel;
}

std::string TitleFor(const std::string& page)
{
    return page.empty() ? std::string("Developer Tools") : "Developer Tools — " + page;
}
}

DevToolsWindow::DevToolsWindow(BMessenger owner, int64 tab, const std::string& pageTitle, BRect browserFrame)
    : BWindow(FrameBeside(browserFrame), TitleFor(pageTitle).c_str(), B_DOCUMENT_WINDOW,
        B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS)
    , fOwner(owner)
    , fTab(tab)
{
    const char* trace = std::getenv("SUMMIT_DEVTOOLS_TRACE");
    fTrace = trace && (!std::strcmp(trace, "1") || !std::strcmp(trace, "2"));
    fTraceProtocol = trace && !std::strcmp(trace, "2");
    fElements = new ElementsPanel(fSession, *this);
    fNetwork = new NetworkPanel(fSession, *this);
    fConsole = new ConsolePanel(fSession, *this);
    fStorage = new StoragePanel(fSession, *this);
    fSources = new SourcesPanel(fSession, *this);
    fPerformance = new PerformancePanel(fSession, *this);
    fTabs = new BTabView("panels", B_WIDTH_FROM_LABEL);
    fTabs->SetBorder(B_NO_BORDER);
    const auto add = [this](BView* panel, const char* label) {
        auto* tab = new BTab();
        fTabs->AddTab(panel, tab);
        tab->SetLabel(label);
    };
    add(fElements, "Elements");
    add(fNetwork, "Network");
    add(fConsole, "Console");
    add(fStorage, "Storage");
    add(fSources, "Sources");
    add(fPerformance, "Performance");
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .SetInsets(0, 4, 0, 0)
        .Add(fTabs);
    SetSizeLimits(560, 10000, 360, 10000);
    // The element picker, as in other browsers; Alt+F finds in the tree.
    AddShortcut('C', B_SHIFT_KEY, new BMessage(kTogglePicker));
    AddShortcut('F', 0, new BMessage(kFocusSearch));
}

DevToolsWindow::~DevToolsWindow() = default;

void DevToolsWindow::SetInspector(std::shared_ptr<BWebKitInspectorSession> inspector, const std::string& pageTarget)
{
    fInspector = std::move(inspector);
    fSession.Start();
    fSession.AssumePageTarget(pageTarget);
    for (const auto& message : fSession.TakeOutgoing()) fInspector->Send(message.c_str());
    // An announcement that found the window's port full is made up for here.
    BMessage pulse(kPulse);
    fPulse = std::make_unique<BMessageRunner>(BMessenger(this), &pulse, 500000);
}

bool DevToolsWindow::QuitRequested()
{
    sLastFrame = Frame();
    fPulse.reset();
    // The page's highlight and picker go with the tools, whoever else is
    // connected to it.
    if (fInspector) {
        fSession.DOM().SetPicking(false);
        fSession.DOM().HideHighlight();
        for (const auto& message : fSession.TakeOutgoing()) fInspector->Send(message.c_str());
    }
    // The page stops reporting when its session ends.
    fInspector.reset();
    BMessage closed(kDeveloperToolsClosed);
    closed.AddInt64("tab", fTab);
    closed.AddMessenger("window", BMessenger(this));
    fOwner.SendMessage(&closed);
    return true;
}

void DevToolsWindow::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case B_WEBKIT_INSPECTOR_MESSAGES:
        case kPulse:
            PanelShown();
            Drain();
            // Styles the page changed are read again a moment later.
            if (fSession.DOM().StylesWaiting()) Sync();
            break;
        case kTogglePicker:
            fTabs->Select(kElementsPanel);
            PanelShown();
            fElements->SetPicking(!fElements->Picking());
            break;
        case kFocusSearch:
            if (fTabs->Selection() == kElementsPanel) fElements->FocusSearch();
            break;
        case kDeveloperToolsPage: {
            const char* title = nullptr;
            if (message->FindString("title", &title) == B_OK) SetTitle(TitleFor(title).c_str());
            break;
        }
        case kShowDeveloperTools: {
            // "panel": which one to show ("elements", "network", "console" or
            // "storage"), if any. "pick": the element picker is started, and
            // the page keeps the focus. "inspect": the element at that point
            // of the page view, whose zoom is "zoom", is selected.
            const char* panel = nullptr;
            if (message->FindString("panel", &panel) == B_OK) fTabs->Select(PanelIndex(panel));
            PanelShown();
            BPoint where;
            if (message->FindPoint("inspect", &where) == B_OK) {
                const double zoom = std::max(0.1, message->GetDouble("zoom", 1));
                fElements->InspectAt(static_cast<float>(where.x / zoom), static_cast<float>(where.y / zoom));
            }
            if (message->GetBool("pick", false)) {
                fElements->SetPicking(true);
                if (!IsHidden()) break;
            }
            if (IsMinimized()) Minimize(false);
            Activate(true);
            break;
        }
        case kDeveloperToolsCommand: {
            BMessage reply(B_REPLY);
            Command(*message, reply);
            message->SendReply(&reply);
            break;
        }
        default: BWindow::MessageReceived(message);
    }
}

void DevToolsWindow::Command(const BMessage& message, BMessage& reply)
{
    // A way to run script in any page must not be open to every program.
    const char* allowed = std::getenv("SUMMIT_ENABLE_INPUT_SYNTHESIS");
    if (!allowed || std::strcmp(allowed, "1")) {
        reply.AddString("error", "Refused: Summit was not started with SUMMIT_ENABLE_INPUT_SYNTHESIS=1");
        return;
    }
    const std::string action = message.GetString("action", ""), argument = message.GetString("argument", "");
    std::string error;
    Drain();
    if (action == "panel") {
        fTabs->Select(PanelIndex(argument.c_str()));
        PanelShown();
    } else if (action != "state" && !fElements->Command(action, argument, error) && !fNetwork->Command(action, argument, error)
        && !fConsole->Command(action, argument, error) && !fStorage->Command(action, argument, error)
        && !fSources->Command(action, argument, error) && !fPerformance->Command(action, argument, error))
        error = "Unknown action";
    Drain();
    if (!error.empty()) reply.AddString("error", error.c_str());
    reply.AddString("json", StateJSON().c_str());
}

std::string DevToolsWindow::StateJSON()
{
    using nlohmann::json;
    static const char* const levels[] = {"debug", "log", "info", "warning", "error"};
    static const char* const kinds[] = {"message", "navigation", "input", "result"};
    static const char* const bodies[] = {"none", "waiting", "loaded", "failed"};
    json requests = json::array(), entries = json::array();
    for (const auto& request : fSession.Requests()) {
        requests.push_back({{"serial", request.serial}, {"url", request.url}, {"name", request.Name()}, {"method", request.method},
            {"status", request.StatusLabel()}, {"type", request.type}, {"mimeType", request.mimeType}, {"size", request.size},
            {"transferred", request.transferred}, {"duration", request.Duration()}, {"pending", request.Pending()},
            {"error", request.IsError()}, {"redirected", request.redirected}, {"source", request.source},
            {"initiator", request.initiator}, {"requestHeaders", request.requestHeaders.size()},
            {"responseHeaders", request.responseHeaders.size()}, {"hasRequestBody", request.hasRequestBody},
            {"body", bodies[static_cast<int>(request.bodyState)]}, {"bodySize", request.body.size()}, {"bodyError", request.bodyError}});
    }
    for (const auto& entry : fSession.Entries()) {
        entries.push_back({{"kind", kinds[static_cast<int>(entry.kind)]}, {"level", levels[static_cast<int>(entry.level)]},
            {"text", entry.text}, {"place", entry.Location()}, {"repeat", entry.repeat}, {"stack", entry.stack},
            {"source", entry.source}});
    }
    json state = {{"title", Title()}, {"panel", kPanels[std::clamp<int32>(fTabs->Selection(), 0, std::size(kPanels) - 1)]}, {"page", fSession.PageURL()},
        {"requests", requests}, {"entries", entries}, {"elements", json::parse(fElements->StateJSON())},
        {"network", json::parse(fNetwork->StateJSON())}, {"console", json::parse(fConsole->StateJSON())},
        {"storage", json::parse(fStorage->StateJSON())}, {"sources", json::parse(fSources->StateJSON())},
        {"performance", json::parse(fPerformance->StateJSON())}};
    return state.dump(-1, ' ', false, json::error_handler_t::replace);
}

void DevToolsWindow::PanelShown()
{
    // The Storage panel reads the page only while it is looked at; the
    // Elements panel reads the document the first time it is.
    const int32 panel = fTabs->Selection();
    if (panel == fShownPanel) return;
    fShownPanel = panel;
    fStorage->SetShown(panel == kStoragePanel);
    fElements->SetShown(panel == kElementsPanel);
    fSources->SetShown(panel == kSourcesPanel);
}

void DevToolsWindow::DispatchMessage(BMessage* message, BHandler* handler)
{
    // Escape stops the element picker, wherever the focus is.
    if (message->what == B_KEY_DOWN && fElements->Picking()) {
        const char* bytes = nullptr;
        if (message->FindString("bytes", &bytes) == B_OK && bytes && bytes[0] == B_ESCAPE) {
            fElements->SetPicking(false);
            return;
        }
    }
    BWindow::DispatchMessage(message, handler);
    // A click on a tab changes the selection without telling the window.
    if (message->what == B_MOUSE_UP || message->what == B_KEY_DOWN) PanelShown();
}

void DevToolsWindow::Drain()
{
    if (!fInspector) return;
    std::vector<std::string> messages;
    if (fInspector->Take(messages)) {
        for (const auto& message : messages) {
            if (fTraceProtocol) std::fprintf(stderr, "Summit devtools protocol: %.400s\n", message.c_str());
            fSession.Receive(message);
        }
        Sync();
    }
    if (!fOverflowShown && fInspector->Overflowed()) {
        fOverflowShown = true;
        std::fprintf(stderr, "Summit developer tools: the page reported faster than its tools could read; some of it was lost\n");
    }
}

void DevToolsWindow::Sync()
{
    // A panel that is shown a change may ask the session for more (a body);
    // the question is sent and its effect shown in the next round.
    if (fSyncing) return;
    fSyncing = true;
    for (int round = 0; round < 8; ++round) {
        bool sent = false;
        if (fInspector) {
            for (const auto& message : fSession.TakeOutgoing()) {
                fInspector->Send(message.c_str());
                sent = true;
            }
        }
        const auto changes = fSession.TakeChanges();
        if (!changes.Any() && !sent) break;
        if (fTrace) Trace(changes);
        fElements->Apply(changes);
        fNetwork->Apply(changes);
        fConsole->Apply(changes);
        fStorage->Apply(changes);
        fSources->Apply(changes);
        fPerformance->Apply(changes);
    }
    fSyncing = false;
    // An element chosen in the page is shown.
    if (fElements->TakePicked()) {
        fTabs->Select(kElementsPanel);
        PanelShown();
        if (IsMinimized()) Minimize(false);
        Activate(true);
    }
}

void DevToolsWindow::Trace(const Changes& changes)
{
    if (changes.origins) {
        std::string origins;
        for (const auto& origin : fSession.StorageOrigins()) origins += " " + origin;
        std::fprintf(stderr, "Summit devtools: origins%s\n", origins.c_str());
    }
    if (changes.requestsReset) std::fprintf(stderr, "Summit devtools: requests reset, %zu kept\n", fSession.Requests().size());
    for (const auto serial : changes.requests) {
        const auto* request = fSession.FindRequest(serial);
        if (!request || request->Pending()) continue;
        std::fprintf(stderr, "Summit devtools: request %s %s %s %s %s %s\n", request->StatusLabel().c_str(), request->method.c_str(),
            request->type.c_str(), FormatBytes(request->size).c_str(), FormatDuration(request->Duration()).c_str(), request->url.c_str());
    }
    for (const auto serial : changes.bodies) {
        const auto* request = fSession.FindRequest(serial);
        if (!request) continue;
        std::fprintf(stderr, "Summit devtools: body %s %zu bytes %s %s\n", request->bodyState == BodyState::Loaded ? "loaded" : "unavailable",
            request->body.size(), request->bodyError.c_str(), request->url.c_str());
    }
    if (changes.consoleReset) {
        std::fprintf(stderr, "Summit devtools: console reset, %zu kept\n", fSession.Entries().size());
        fTracedEntries = 0;
    }
    const auto& entries = fSession.Entries();
    static const char* const levels[] = {"debug", "log", "info", "warning", "error"};
    for (size_t i = std::min(fTracedEntries, entries.size()); i < entries.size(); ++i)
        std::fprintf(stderr, "Summit devtools: console %s %s [%s]\n", levels[static_cast<int>(entries[i].level)],
            entries[i].text.c_str(), entries[i].Location().c_str());
    fTracedEntries = entries.size();
}
}
#endif
