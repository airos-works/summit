#if SUMMIT_MODERN_WEBKIT
// MCP tools that ask the engine about itself (WebKitDiagnostics.h): its
// processes and pages, each web process's memory, media players and
// compositors, and how a view presents its frames (McpService.h).
#include "McpService.h"
#include "BrowserWindow.h"
#include <WebKit/WebKitDiagnostics.h>
#include <WebKit/WebKitView.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <DataIO.h>
#include <Screen.h>
#include <TranslatorRoster.h>
#include <View.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <memory>

namespace summit {
namespace {
using mcp::Schema;
using mcp::ToolError;
using mcp::ToolResult;

const char* kTabArgument = "Tab as the tabs tool names it (\"WINDOW.TAB\", or TAB in the front window); "
    "omitted: the front window's selected tab";

Json Parse(const std::string& text, const char* what)
{
    if (text.empty()) throw ToolError(std::string(what) + " gave no answer (not an engine process, or it is gone)");
    Json value = Json::parse(text, nullptr, false);
    if (value.is_discarded()) throw ToolError(std::string(what) + " answered something that is not JSON");
    return value;
}

// A tab's details as its window gives them.
Json TabDetails(const McpTabRef& ref)
{
    const Json window = McpAsk(ref.window, {{"action", "tabs"}}, 3000000, "The window");
    for (const auto& tab : window.value("tabs", Json::array()))
        if (ref.tab ? tab.value("id", "") == ref.id : tab.value("selected", false)) return tab;
    throw ToolError("no tab " + ref.id);
}

std::vector<team_id> WebProcesses()
{
    std::vector<team_id> teams;
    for (team_id team : McpSummitTeams())
        if (McpTeamRole(team) == "web") teams.push_back(team);
    return teams;
}

Json ProcessReport(team_id team, const Json& request, bigtime_t timeout)
{
    return Parse(BWebKitProcessDiagnostics(team, request.dump().c_str(), timeout), ("Process " + std::to_string(team)).c_str());
}

// The tab's view, locked in its window, for function.
void WithTabView(const McpTabRef& ref, int64 tab, const std::function<void(BWebKitView&)>& function)
{
    if (ref.window.LockTargetWithTimeout(3000000) != B_OK)
        throw ToolError("the window could not be locked within 3 s: its thread is busy or hung");
    BLooper* looper = nullptr;
    ref.window.Target(&looper);
    auto* browser = dynamic_cast<BrowserWindow*>(looper);
    const bool found = browser && browser->McpWithView(tab, function);
    if (looper) looper->Unlock();
    if (!found) throw ToolError("no tab " + ref.id);
}

int64 TabNumber(const Json& details)
{
    const std::string id = details.value("id", "");
    const size_t dot = id.find('.');
    return dot == std::string::npos ? 0 : std::stoll(id.substr(dot + 1));
}

std::vector<std::string> Topics(const Json& arguments, const char* fallback)
{
    std::vector<std::string> topics;
    if (mcp::HasArgument(arguments, "topics") && arguments["topics"].is_array())
        for (const auto& topic : arguments["topics"]) if (topic.is_string()) topics.push_back(topic);
    if (topics.empty()) topics.push_back(fallback);
    return topics;
}

std::string Joined(const std::vector<std::string>& parts)
{
    std::string text;
    for (const auto& part : parts) text += (text.empty() ? "" : ",") + part;
    return text;
}
}

void RegisterMcpEngineTools(mcp::Server& server, const std::shared_ptr<McpContext>&)
{
    server.Add({"engine_report",
        "Asks one of the engine's processes about itself; it answers from a thread of its own, so it comes back "
        "while that process's main thread hangs (mainThread.responsive false then, with what other threads can "
        "read). Topics: pages (addresses, identifiers, visibility, rendering updates, frames), memory (JavaScript "
        "heap size and objects, JIT, memory cache by type, back/forward cache, fonts, memory pressure), media "
        "(every media element with its player: decoder, hardware or not, frames presented and dropped), "
        "graphics and compositor (GL renderer, frames rendered and their times), network (the network "
        "process's transfer workers). With no team or tab: this browser's own view of the engine (web and network "
        "processes, their pages and responsiveness).",
        Schema().Integer("team", "An engine process's team (see processes)").String("tab", "A tab, for its web process")
            .StringArray("topics", "Topics (default all)").Integer("timeout_ms", "Time given to its main thread (default 2000)"),
        [](const Json& arguments) {
            const bigtime_t timeout = mcp::IntegerArgument(arguments, "timeout_ms", 2000, 50, 120000) * 1000;
            team_id team = -1;
            if (mcp::HasArgument(arguments, "team")) team = team_id(mcp::IntegerArgument(arguments, "team", 0, 1, INT32_MAX));
            else if (mcp::HasArgument(arguments, "tab")) {
                const Json details = TabDetails(McpResolveTab(arguments));
                team = details.value("webProcess", 0);
                if (team <= 0) throw ToolError("the engine has not named tab " + details.value("id", "?") + "'s web process yet");
            }
            if (team < 0 || team == getpid()) {
                Json report = Parse(BWebKitDiagnostics(timeout), "The engine");
                return ToolResult::Data(report);
            }
            const Json request = {{"topics", Joined(Topics(arguments, "all"))}, {"timeoutMs", timeout / 1000}};
            return ToolResult::Data(ProcessReport(team, request, timeout + 2000000));
        }});

    server.Add({"media",
        "The engine's media players: for a tab's web process (or every web process), each media element with its "
        "player as the engine sees it: engine (Media Kit or MSE/DASH), the video decoder (name, short name, a "
        "named hardware add-on such as amduvd, cedar or rpi_mmal, or libavcodec), decoded size, frames decoded, "
        "presented, dropped and replaced before they were shown, audio output, streaming. What the page sees is in "
        "page_media.",
        Schema().String("tab", "A tab, for its web process (default every web process)"),
        [](const Json& arguments) {
            std::vector<team_id> teams;
            if (mcp::HasArgument(arguments, "tab")) {
                const Json details = TabDetails(McpResolveTab(arguments));
                if (details.value("webProcess", 0) <= 0) throw ToolError("the engine has not named this tab's web process yet");
                teams.push_back(details.value("webProcess", 0));
            } else teams = WebProcesses();
            Json processes = Json::array();
            for (team_id team : teams) {
                try {
                    Json report = ProcessReport(team, {{"topics", "media"}, {"timeoutMs", 2000}}, 5000000);
                    report["team"] = team;
                    processes.push_back(report);
                } catch (const ToolError& error) {
                    processes.push_back({{"team", team}, {"error", error.what()}});
                }
            }
            return ToolResult::Data({{"processes", processes}});
        }});

    server.Add({"graphics",
        "How a tab's page gets to the screen: the view (size, screen origin, device scale, visible, focused, "
        "whether frames go straight into the screen by direct present, the last frame's identifier and size in "
        "device pixels, the page's web process and whether it answers), the web process's compositor (GL "
        "renderer, vendor and version, compositors, frames rendered, render time median/95th/longest, last frame "
        "age), frames delivered to the view in the last 5 s, and the screen (mode, refresh, accelerant).",
        Schema().String("tab", kTabArgument),
        [](const Json& arguments) {
            McpTabRef ref = McpResolveTab(arguments);
            const Json details = TabDetails(ref);
            const int64 tab = TabNumber(details);
            std::string view;
            WithTabView(ref, tab, [&](BWebKitView& webView) { view = webView.Diagnostics(1000000); });
            Json result = {{"tab", details.value("id", ref.id)}, {"url", details.value("url", "")}};
            result["view"] = Parse(view, "The view");
            if (const team_id team = details.value("webProcess", 0); team > 0) {
                try {
                    result["webProcess"] = ProcessReport(team, {{"topics", "compositor,graphics"}, {"timeoutMs", 1500}}, 4000000);
                } catch (const ToolError& error) {
                    result["webProcess"] = {{"error", error.what()}};
                }
            }
            result["framesDelivered"] = McpAsk(ref.window, {{"action", "frames"}, {"tab", tab}, {"seconds", 5.0}}, 3000000, "The window")
                .value("frames", Json::object());
            BScreen screen(B_MAIN_SCREEN_ID);
            display_mode mode;
            if (screen.GetMode(&mode) == B_OK)
                result["screen"] = {{"width", mode.virtual_width}, {"height", mode.virtual_height}, {"colorSpace", int(mode.space)},
                    {"refreshHz", mode.timing.h_total && mode.timing.v_total ? std::round(double(mode.timing.pixel_clock) * 1000.0
                        / (double(mode.timing.h_total) * mode.timing.v_total) * 100) / 100 : 0.0},
                    {"deviceScale", BWebKitDisplayScale()}};
            return ToolResult::Data(result);
        }});

    server.Add({"page_frame",
        "The last frame the engine presented to a tab's view, as it reached the browser (device pixels, before "
        "app_server draws it): compare with screenshot (what the screen shows) and page_render (what WebCore "
        "paints). Frames put straight into the screen (direct present) are not kept, so then this may be older.",
        Schema().String("tab", kTabArgument).Integer("max_width", "Scale down to at most this width (default 1600, 0: full size)"),
        [](const Json& arguments) {
            McpTabRef ref = McpResolveTab(arguments);
            const Json details = TabDetails(ref);
            std::unique_ptr<BBitmap> frame;
            WithTabView(ref, TabNumber(details), [&](BWebKitView& view) { frame.reset(view.CopyPresentedFrame()); });
            if (!frame) throw ToolError("the view has no frame (nothing presented yet, or every frame went straight into the screen)");
            const BRect bounds = frame->Bounds();
            const int32 maxWidth = int32(mcp::IntegerArgument(arguments, "max_width", 1600, 0, 16384));
            if (maxWidth && bounds.Width() + 1 > maxWidth) {
                const float scale = maxWidth / (bounds.Width() + 1);
                const BRect target(0, 0, maxWidth - 1, std::max(0.0f, std::round((bounds.Height() + 1) * scale) - 1));
                auto scaled = std::make_unique<BBitmap>(target, B_BITMAP_ACCEPTS_VIEWS, B_RGB32);
                auto* view = new BView(target, "scale", B_FOLLOW_NONE, 0);
                scaled->AddChild(view);
                if (scaled->Lock()) {
                    view->DrawBitmap(frame.get(), bounds, target, B_FILTER_BITMAP_BILINEAR);
                    view->Sync();
                    scaled->Unlock();
                }
                scaled->RemoveChild(view);
                delete view;
                frame = std::move(scaled);
            }
            BBitmapStream stream(frame.release());
            BMallocIO output;
            const status_t status = BTranslatorRoster::Default()->Translate(&stream, nullptr, nullptr, &output, B_PNG_FORMAT);
            BBitmap* detached = nullptr;
            stream.DetachBitmap(&detached);
            delete detached;
            if (status != B_OK) throw ToolError("the PNG translator failed");
            return ToolResult::Image(mcp::Base64(output.Buffer(), output.BufferLength()), "image/png",
                "Last frame presented to tab " + details.value("id", ref.id) + ", " + std::to_string(int(bounds.Width() + 1)) + "×"
                + std::to_string(int(bounds.Height() + 1)) + " device px");
        }});

    server.Add({"memory_action",
        "Asks a web process (a tab's, a team, or every web process) to free memory: gc runs a full JavaScript "
        "garbage collection (heap before and after), release-memory drops caches as under critical memory "
        "pressure (memory report after). Shows what a page holds on to versus what can be released.",
        Schema().Enum("action", "gc or release-memory", {"gc", "release-memory"}, true).String("tab", "A tab, for its web process")
            .Integer("team", "A web process's team").Boolean("all", "Every web process"),
        [](const Json& arguments) {
            std::vector<team_id> teams;
            if (mcp::BooleanArgument(arguments, "all", false)) teams = WebProcesses();
            else if (mcp::HasArgument(arguments, "team")) teams.push_back(team_id(mcp::IntegerArgument(arguments, "team", 0, 1, INT32_MAX)));
            else {
                const Json details = TabDetails(McpResolveTab(arguments));
                if (details.value("webProcess", 0) <= 0) throw ToolError("the engine has not named this tab's web process yet");
                teams.push_back(details.value("webProcess", 0));
            }
            Json results = Json::array();
            for (team_id team : teams) {
                try {
                    Json report = ProcessReport(team, {{"action", mcp::StringArgument(arguments, "action")}, {"timeoutMs", 60000}}, 65000000);
                    report["team"] = team;
                    results.push_back(report);
                } catch (const ToolError& error) {
                    results.push_back({{"team", team}, {"error", error.what()}});
                }
            }
            return ToolResult::Data({{"results", results}});
        }, false});
}
}
#endif
