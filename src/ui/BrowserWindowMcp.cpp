#if SUMMIT_MODERN_WEBKIT
// The browser window's part of the MCP server (McpService.h): its tabs, their
// pages and input, answered with the window locked.
#include "BrowserWindow.h"
#include "FrameHistory.h"
#include "McpService.h"
#include "Messages.h"
#include "core/Address.h"
#include <WebKit/WebKitView.h>
#include <InterfaceDefs.h>
#include <Screen.h>
#include <StringView.h>
#include <TextControl.h>
#include <app/AppMisc.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>

namespace summit {
namespace {
using nlohmann::json;

double Milliseconds(bigtime_t micros)
{
    return std::round(double(micros) / 100.0) / 10.0;
}

json Rect(const BRect& rect)
{
    return {{"left", rect.left}, {"top", rect.top}, {"width", rect.Width() + 1}, {"height", rect.Height() + 1}};
}

// What the frame history says about the last `seconds`.
json FrameReport(const FrameHistory* frames, double seconds)
{
    json report = json::object();
    if (!frames) return report;
    const bigtime_t now = system_time();
    const auto times = frames->Times();
    const auto delays = frames->QueueDelays();
    report["total"] = frames->Total();
    report["direct"] = frames->Direct();
    if (times.empty()) return report;
    report["lastFrameAgoMs"] = Milliseconds(now - times.back());
    const bigtime_t window = bigtime_t(seconds * 1000000);
    std::vector<bigtime_t> recent, gaps;
    bigtime_t queueMax = 0;
    for (size_t i = 0; i < times.size(); ++i) {
        if (now - times[i] > window) continue;
        recent.push_back(times[i]);
        if (delays[i] > queueMax) queueMax = delays[i];
    }
    for (size_t i = 1; i < recent.size(); ++i) gaps.push_back(recent[i] - recent[i - 1]);
    report["windowSeconds"] = seconds;
    report["frames"] = recent.size();
    report["fps"] = recent.size() / seconds;
    if (!gaps.empty()) {
        std::vector<bigtime_t> sorted = gaps;
        std::sort(sorted.begin(), sorted.end());
        report["gapMedianMs"] = Milliseconds(sorted[sorted.size() / 2]);
        report["gapP95Ms"] = Milliseconds(sorted[std::min(sorted.size() - 1, sorted.size() * 95 / 100)]);
        report["gapMaxMs"] = Milliseconds(sorted.back());
        report["gapsOver33Ms"] = std::count_if(gaps.begin(), gaps.end(), [](bigtime_t gap) { return gap > 33333; });
        report["gapsOver50Ms"] = std::count_if(gaps.begin(), gaps.end(), [](bigtime_t gap) { return gap > 50000; });
        report["gapsOver100Ms"] = std::count_if(gaps.begin(), gaps.end(), [](bigtime_t gap) { return gap > 100000; });
        // The longest gaps, with how long ago they ended.
        std::vector<size_t> order(gaps.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return gaps[a] > gaps[b]; });
        json longest = json::array();
        for (size_t i = 0; i < order.size() && i < 5; ++i)
            longest.push_back({{"gapMs", Milliseconds(gaps[order[i]])}, {"endedAgoMs", Milliseconds(now - recent[order[i] + 1])}});
        report["longestGaps"] = longest;
    }
    if (queueMax > 0) report["windowQueueMaxMs"] = Milliseconds(queueMax);
    return report;
}

// Named keys: Haiku key code and the byte a B_KEY_DOWN carries.
struct NamedKey {
    const char* name;
    int32 code;
    char byte;
};
const NamedKey kKeys[] = {
    {"enter", 0x47, B_ENTER}, {"return", 0x47, B_ENTER}, {"tab", 0x26, B_TAB}, {"escape", 0x01, B_ESCAPE},
    {"backspace", 0x1e, B_BACKSPACE}, {"delete", 0x34, B_DELETE}, {"space", 0x5e, ' '},
    {"left", 0x61, B_LEFT_ARROW}, {"right", 0x63, B_RIGHT_ARROW}, {"up", 0x57, B_UP_ARROW}, {"down", 0x62, B_DOWN_ARROW},
    {"home", 0x20, B_HOME}, {"end", 0x35, B_END}, {"pageup", 0x21, B_PAGE_UP}, {"pagedown", 0x36, B_PAGE_DOWN},
    {"f1", 0x02, B_FUNCTION_KEY}, {"f2", 0x03, B_FUNCTION_KEY}, {"f3", 0x04, B_FUNCTION_KEY}, {"f4", 0x05, B_FUNCTION_KEY},
    {"f5", 0x06, B_FUNCTION_KEY}, {"f6", 0x07, B_FUNCTION_KEY}, {"f7", 0x08, B_FUNCTION_KEY}, {"f8", 0x09, B_FUNCTION_KEY},
    {"f9", 0x0a, B_FUNCTION_KEY}, {"f10", 0x0b, B_FUNCTION_KEY}, {"f11", 0x0c, B_FUNCTION_KEY}, {"f12", 0x0d, B_FUNCTION_KEY},
};

uint32 Modifiers(const json& request)
{
    uint32 modifiers = 0;
    for (const auto& name : request.value("modifiers", json::array())) {
        if (!name.is_string()) continue;
        const std::string text = name;
        if (text == "shift") modifiers |= B_SHIFT_KEY | B_LEFT_SHIFT_KEY;
        else if (text == "control" || text == "ctrl") modifiers |= B_CONTROL_KEY | B_LEFT_CONTROL_KEY;
        else if (text == "command" || text == "alt") modifiers |= B_COMMAND_KEY | B_LEFT_COMMAND_KEY;
        else if (text == "option" || text == "meta") modifiers |= B_OPTION_KEY | B_LEFT_OPTION_KEY;
    }
    return modifiers;
}
}

std::string BrowserWindow::McpRequest(const std::string& text)
{
    const json request = json::parse(text, nullptr, false);
    auto fail = [](const std::string& error) { return json{{"error", error}}.dump(); };
    if (!request.is_object()) return fail("not a request");
    const std::string action = request.value("action", "");
    const int64 tabID = request.value("tab", int64(0));
    Tab* tab = tabID ? FindTabByID(tabID) : ActiveTab();
    auto tabName = [this](const Tab& tab) { return std::to_string(fKey) + "." + std::to_string(tab.id); };
    if (action == "summary") {
        json ids = json::array();
        for (const auto& item : fTabs) ids.push_back(item.id);
        return json{{"key", fKey}, {"private", fPrivate}, {"active", IsActive()}, {"activatedAt", fActivatedAt},
            {"selected", fSelected}, {"tabIds", ids}, {"frame", Rect(Frame())}}.dump();
    }
    if (action == "tabs") {
        json tabs = json::array();
        const bigtime_t now = system_time();
        for (const auto& item : fTabs) {
            json entry = {{"id", tabName(item)}, {"url", item.url}, {"title", item.title}, {"selected", item.id == fSelected},
                {"loading", item.loading}, {"progress", item.progress}, {"canGoBack", item.back}, {"canGoForward", item.forward},
                {"pageZoom", item.pageZoom}, {"loadOutcome", item.loadOutcome}, {"webProcess", item.processID},
                {"pageIdentifier", item.pageIdentifier}};
            if (!item.deferredURL.empty()) entry["notLoadedYet"] = item.deferredURL;
            if (!item.loadError.empty())
                entry["loadError"] = {{"text", item.loadError}, {"description", item.loadErrorDescription},
                    {"domain", item.loadErrorDomain}, {"code", item.loadErrorCode}, {"url", item.loadErrorURL}};
            if (item.processExited) entry["processExited"] = item.processError;
            if (item.loadStartedAt) {
                entry["loadStartedAgoMs"] = Milliseconds(now - item.loadStartedAt);
                if (item.loadFinishedAt >= item.loadStartedAt)
                    entry["loadDurationMs"] = Milliseconds(item.loadFinishedAt - item.loadStartedAt);
            }
            if (item.capturingCamera || item.capturingMicrophone || item.capturingScreen)
                entry["capturing"] = {{"camera", item.capturingCamera}, {"microphone", item.capturingMicrophone},
                    {"screen", item.capturingScreen}};
            if (item.devTools.IsValid()) entry["developerToolsOpen"] = true;
            if (item.readerActive) entry["readerMode"] = true;
            if (!item.notice.empty() && item.noticeUntil > now) entry["notice"] = item.notice;
            if (item.view) {
                entry["view"] = {{"hidden", item.view->IsHidden()}, {"width", item.view->Bounds().Width() + 1},
                    {"height", item.view->Bounds().Height() + 1}};
            }
            entry["frames"] = FrameReport(item.frames, 1.0);
            tabs.push_back(entry);
        }
        json window = {{"key", fKey}, {"private", fPrivate}, {"active", IsActive()}, {"minimized", IsMinimized()},
            {"hidden", IsHidden()}, {"frame", Rect(Frame())}, {"title", Title()}, {"fullscreen", fFullScreenTab != 0},
            {"workspaces", Workspaces()}, {"address", fAddress->Text()}, {"status", fStatus->Text()},
            {"downloads", fDownloads.size()}, {"closing", fClosingWindow}, {"tabs", tabs}};
        return window.dump();
    }
    if (action == "open") {
        const std::string url = request.value("url", "");
        CreateTab(url.empty() ? NewTabAddress() : url, request.value("select", true));
        return json{{"opened", std::to_string(fKey) + "." + std::to_string(fNextID - 1)}}.dump();
    }
    if (!tab) return fail(tabID ? "This window has no tab " + std::to_string(tabID) : "This window has no tab");
    if (action == "frames") return json{{"tab", tabName(*tab)}, {"frames", FrameReport(tab->frames,
        std::clamp(request.value("seconds", 5.0), 0.1, 60.0))}}.dump();
    if (action == "geometry") {
        BRect bounds = tab->view->Bounds();
        BRect screen = tab->view->ConvertToScreen(bounds);
        return json{{"tab", tabName(*tab)}, {"view", Rect(screen)}, {"window", Rect(Frame())}, {"hidden", tab->view->IsHidden()},
            {"selected", tab->id == fSelected}, {"displayScale", BWebKitDisplayScale()}}.dump();
    }
    if (action == "select") {
        SelectTab(tab->id);
        if (request.value("activate", false)) Activate(true);
        return json{{"selected", tabName(*tab)}}.dump();
    }
    if (action == "close") {
        const std::string name = tabName(*tab);
        CloseTab(tab->id);
        return json{{"closing", name}}.dump();
    }
    if (action == "navigate") {
        const std::string url = request.value("url", "");
        if (url.empty()) return fail("no address");
        if (tab->id != fSelected) SelectTab(tab->id);
        Navigate(url);
        return json{{"navigating", tabName(*tab)}, {"url", url}}.dump();
    }
    // As the toolbar's buttons do on an error page (#89): Reload tries the
    // address again, Back shows the page before it.
    if (action == "reload") {
        if (tab->errorView && !tab->loading) RetryFailedLoad(*tab); else tab->view->Reload();
        return json{{"reloading", tabName(*tab)}}.dump();
    }
    if (action == "stop") { tab->view->Stop(); return json{{"stopped", tabName(*tab)}}.dump(); }
    if (action == "back") {
        if (!LeaveLoadErrorView(*tab)) tab->view->GoBack();
        return json{{"back", tabName(*tab)}}.dump();
    }
    if (action == "forward") { tab->view->GoForward(); return json{{"forward", tabName(*tab)}}.dump(); }
    if (action == "zoom") {
        const double factor = request.value("factor", 1.0);
        if (!(factor >= 0.25 && factor <= 5)) return fail("zoom factor must be between 0.25 and 5");
        tab->view->SetZoomFactor(factor);
        tab->pageZoom = factor;
        return json{{"tab", tabName(*tab)}, {"pageZoom", factor}}.dump();
    }
    if (action == "devtools") {
        if (tab->id != fSelected) SelectTab(tab->id);
        ShowDeveloperTools(request.value("panel", "").empty() ? nullptr : request.value("panel", "").c_str());
        return json{{"tab", tabName(*tab)}, {"developerTools", "open"}}.dump();
    }
    if (action == "input") {
        // Synthesized input, posted to the page view as app_server would
        // deliver it (no input_server, the pointer does not move).
        const std::string kind = request.value("kind", "");
        BWebKitView* view = tab->view;
        if (request.value("activate", false)) {
            if (tab->id != fSelected) SelectTab(tab->id);
            Activate(true);
        }
        if (tab->id != fSelected) return fail("the tab is not selected: input goes to the page on screen (pass activate)");
        const BRect bounds = view->Bounds();
        BPoint at(request.value("x", bounds.Width() / 2), request.value("y", bounds.Height() / 2));
        if (!bounds.Contains(at)) return fail("the point is outside the page view (" + std::to_string(int(bounds.Width() + 1))
            + "×" + std::to_string(int(bounds.Height() + 1)) + ")");
        const BPoint screen = view->ConvertToScreen(at);
        const BPoint inWindow = ConvertFromScreen(screen);
        const uint32 modifiers = Modifiers(request);
        auto mouse = [&](uint32 what, int32 buttons, int32 clicks) {
            BMessage event(what);
            event.AddInt64("when", system_time());
            event.AddPoint("screen_where", screen);
            event.AddPoint("where", inWindow);
            event.AddPoint("be:view_where", at);
            event.AddInt32("buttons", buttons);
            event.AddInt32("modifiers", modifiers);
            if (what == B_MOUSE_DOWN) event.AddInt32("clicks", clicks);
            if (what == B_MOUSE_MOVED) event.AddInt32("be:transit", B_INSIDE_VIEW);
            event.AddInt32("_view_token", _get_object_token_(view));
            PostMessage(&event, view);
        };
        if (kind == "move") mouse(B_MOUSE_MOVED, 0, 0);
        else if (kind == "click" || kind == "down" || kind == "up") {
            const std::string button = request.value("button", "left");
            const int32 buttons = button == "right" ? B_SECONDARY_MOUSE_BUTTON : button == "middle" ? B_TERTIARY_MOUSE_BUTTON
                : B_PRIMARY_MOUSE_BUTTON;
            const int32 clicks = std::clamp(request.value("clicks", 1), 1, 3);
            if (kind == "click") mouse(B_MOUSE_MOVED, 0, 0);
            for (int32 click = 1; click <= (kind == "click" ? clicks : 1); ++click) {
                if (kind != "up") mouse(B_MOUSE_DOWN, buttons, kind == "click" ? click : clicks);
                if (kind != "down") mouse(B_MOUSE_UP, 0, 0);
            }
        } else if (kind == "scroll") {
            // One wheel message per notch, as a mouse sends them; a burst
            // paced in time goes through the scroll tool's burst instead.
            const double dx = request.value("dx", 0.0), dy = request.value("dy", 3.0);
            BMessage event(B_MOUSE_WHEEL_CHANGED);
            event.AddInt64("when", system_time());
            event.AddFloat("be:wheel_delta_x", float(dx));
            event.AddFloat("be:wheel_delta_y", float(dy));
            event.AddPoint("summit:view_where", at);
            event.AddInt32("_view_token", _get_object_token_(view));
            PostMessage(&event, view);
        } else if (kind == "text" || kind == "key") {
            view->MakeFocus(true);
            auto key = [&](const std::string& bytes, int32 code, char raw) {
                for (uint32 what : { uint32(B_KEY_DOWN), uint32(B_KEY_UP) }) {
                    BMessage event(what);
                    event.AddInt64("when", system_time());
                    event.AddInt32("modifiers", int32(modifiers));
                    event.AddInt32("key", code);
                    event.AddInt32("raw_char", raw);
                    event.AddString("bytes", bytes.c_str());
                    for (char byte : bytes) event.AddInt8("byte", byte);
                    PostMessage(&event, view);
                }
            };
            if (kind == "text") {
                const std::string typed = request.value("text", "");
                for (size_t i = 0; i < typed.size();) {
                    size_t length = 1;
                    const unsigned char lead = typed[i];
                    if (lead >= 0xF0) length = 4; else if (lead >= 0xE0) length = 3; else if (lead >= 0xC0) length = 2;
                    const std::string character = typed.substr(i, length);
                    key(character, 0, length == 1 ? character[0] : 0);
                    i += length;
                }
            } else {
                std::string name = request.value("key", "");
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
                const NamedKey* named = nullptr;
                for (const auto& candidate : kKeys) if (name == candidate.name) named = &candidate;
                if (named) {
                    const std::string bytes(1, named->byte);
                    key(bytes, named->code, named->byte);
                } else if (name.size() == 1) key(name, 0, name[0]);
                else return fail("unknown key \"" + name + "\" (enter, tab, escape, backspace, delete, space, arrows, "
                    "home, end, pageup, pagedown, f1-f12, or one character)");
            }
        } else return fail("unknown input kind \"" + kind + "\"");
        return json{{"tab", tabName(*tab)}, {"sent", kind}, {"x", at.x}, {"y", at.y}}.dump();
    }
    if (action == "burst") {
        BMessage burst(kSimulateScroll), reply;
        burst.AddBool("mcp", true);
        burst.AddInt32("count", std::clamp(request.value("count", 30), 1, 10000));
        burst.AddInt32("interval_ms", std::clamp(request.value("interval_ms", 16), 0, 1000));
        burst.AddFloat("delta", float(request.value("delta", 3.0)));
        if (tab->id != fSelected) return fail("the tab is not selected");
        SimulateScroll(burst, reply);
        if (const char* error = nullptr; reply.FindString("error", &error) == B_OK) return fail(error);
        return json{{"tab", tabName(*tab)}, {"burst", "started"}}.dump();
    }
    return fail("unknown action \"" + action + "\"");
}

bool BrowserWindow::McpWithView(int64 id, const std::function<void(BWebKitView&)>& function)
{
    Tab* tab = id ? FindTabByID(id) : ActiveTab();
    if (!tab || !tab->view) return false;
    function(*tab->view);
    return true;
}
}
#endif
