#if SUMMIT_MODERN_WEBKIT
// MCP tools for the browser itself: what runs, its tabs, what they show, and
// input to them (McpService.h).
#include "McpService.h"
#include "BrowserWindow.h"
#include "core/BuildTarget.h"
#include <WebKit/WebKitInfo.h>
#include <WebKit/WebKitView.h>
#include <Accelerant.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <DataIO.h>
#include <File.h>
#include <Node.h>
#include <Screen.h>
#include <TranslatorRoster.h>
#include <View.h>
#include <fs_attr.h>
#include <image.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>

extern char** environ;

namespace summit {
namespace {
using mcp::Schema;
using mcp::Tool;
using mcp::ToolError;
using mcp::ToolResult;

const char* kTabArgument = "Tab as the tabs tool names it (\"WINDOW.TAB\", or TAB in the front window); "
    "omitted: the front window's selected tab";

// The package a file came from (packagefs's SYS:PACKAGE attribute), if any.
std::string PackageOf(const char* path)
{
    BNode node(path);
    char name[B_FILE_NAME_LENGTH] = "";
    const ssize_t length = node.ReadAttr("SYS:PACKAGE", B_STRING_TYPE, 0, name, sizeof(name) - 1);
    return length > 0 ? std::string(name, size_t(length)) : std::string();
}

std::string ImagePath(const char* fileName)
{
    int32 cookie = 0;
    image_info image;
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &image) == B_OK) {
        const char* slash = std::strrchr(image.name, '/');
        if (!std::strcmp(slash ? slash + 1 : image.name, fileName)) return image.name;
    }
    return { };
}

std::string ExecutablePath()
{
    int32 cookie = 0;
    image_info image;
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &image) == B_OK)
        if (image.type == B_APP_IMAGE) return image.name;
    return { };
}

Json Environment(const std::string& pattern)
{
    Json variables = Json::object();
    static const char* const prefixes[] = { "SUMMIT_", "WEBKIT_", "JSC_", "MESA_", "GALLIUM_", "ZINK_", "LIBGL_",
        "__EGL_", "EGL_", "GBM_", "VK_", "LIBRARY_PATH", "MALLOC", "MIMALLOC_", "TZ", "LANG", "LC_" };
    for (char** entry = environ; entry && *entry; ++entry) {
        const std::string text = *entry;
        const size_t equals = text.find('=');
        if (equals == std::string::npos) continue;
        const std::string name = text.substr(0, equals);
        bool wanted = false;
        if (!pattern.empty()) wanted = mcp::Matches(pattern, name);
        else for (const char* prefix : prefixes) wanted = wanted || name.rfind(prefix, 0) == 0;
        // The server's own token is not repeated back.
        const bool secret = name.find("TOKEN") != std::string::npos || name.find("SECRET") != std::string::npos
            || name.find("PASSWORD") != std::string::npos;
        if (wanted) variables[name] = secret ? std::string("(hidden)") : text.substr(equals + 1);
    }
    return variables;
}

Json DisplayInfo()
{
    BScreen screen(B_MAIN_SCREEN_ID);
    Json display = {{"frame", {{"width", screen.Frame().Width() + 1}, {"height", screen.Frame().Height() + 1}}},
        {"deviceScale", BWebKitDisplayScale()}, {"colorSpace", int(screen.ColorSpace())}};
    display_mode mode;
    if (screen.GetMode(&mode) == B_OK && mode.timing.h_total && mode.timing.v_total)
        display["refreshHz"] = std::round(double(mode.timing.pixel_clock) * 1000.0 / (double(mode.timing.h_total) * mode.timing.v_total) * 100) / 100;
    accelerant_device_info device;
    if (screen.GetDeviceInfo(&device) == B_OK)
        display["accelerant"] = {{"name", device.name}, {"chipset", device.chipset}, {"memoryBytes", device.memory}};
    int32 count = 1;
    for (BScreen other(B_MAIN_SCREEN_ID); other.SetToNext() == B_OK && count < 16;) ++count;
    display["screens"] = count;
    return display;
}

// The screen (or part of it) as app_server shows it.
std::unique_ptr<BBitmap> CaptureScreen(const BRect* frame)
{
    BScreen screen(B_MAIN_SCREEN_ID);
    BBitmap* bitmap = nullptr;
    BRect area = frame ? *frame : screen.Frame();
    area = area & screen.Frame();
    if (!area.IsValid()) throw ToolError("that part of the screen is not on the screen");
    const status_t status = screen.GetBitmap(&bitmap, false, &area);
    if (status != B_OK || !bitmap) throw ToolError(std::string("app_server gave no picture: ") + std::strerror(status));
    return std::unique_ptr<BBitmap>(bitmap);
}

// Scales down to at most maxWidth pixels wide (bilinear), for the client's sake.
std::unique_ptr<BBitmap> Scaled(std::unique_ptr<BBitmap> source, int32 maxWidth)
{
    const BRect bounds = source->Bounds();
    if (maxWidth <= 0 || bounds.Width() + 1 <= maxWidth) return source;
    const float scale = maxWidth / (bounds.Width() + 1);
    const BRect target(0, 0, maxWidth - 1, std::max(0.0f, std::round((bounds.Height() + 1) * scale) - 1));
    auto scaled = std::make_unique<BBitmap>(target, B_BITMAP_ACCEPTS_VIEWS, B_RGB32);
    auto* view = new BView(target, "scale", B_FOLLOW_NONE, 0);
    scaled->AddChild(view);
    if (scaled->Lock()) {
        view->DrawBitmap(source.get(), bounds, target, B_FILTER_BITMAP_BILINEAR);
        view->Sync();
        scaled->Unlock();
    }
    scaled->RemoveChild(view);
    delete view;
    return scaled;
}

std::string EncodePNG(std::unique_ptr<BBitmap> bitmap)
{
    BBitmapStream stream(bitmap.release());
    BMallocIO output;
    const status_t status = BTranslatorRoster::Default()->Translate(&stream, nullptr, nullptr, &output, B_PNG_FORMAT);
    BBitmap* detached = nullptr;
    stream.DetachBitmap(&detached);
    delete detached;
    if (status != B_OK) throw ToolError(std::string("the PNG translator failed: ") + std::strerror(status));
    return std::string(static_cast<const char*>(output.Buffer()), output.BufferLength());
}

ToolResult ImageResult(const std::string& png, const Json& arguments, const std::string& caption)
{
    const std::string saveTo = mcp::StringArgument(arguments, "save_to");
    if (!saveTo.empty()) {
        BFile file(saveTo.c_str(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
        if (file.InitCheck() != B_OK || file.Write(png.data(), png.size()) != ssize_t(png.size()))
            throw ToolError("could not write " + saveTo);
        return ToolResult::Text(caption + "; saved to " + saveTo + " (" + std::to_string(png.size()) + " bytes)");
    }
    return ToolResult::Image(mcp::Base64(png.data(), png.size()), "image/png", caption);
}

// The request for a tab's window, with the tab added.
Json AskTab(const McpTabRef& ref, Json request, bigtime_t timeout = 3000000)
{
    if (ref.tab) request["tab"] = ref.tab;
    return McpAsk(ref.window, request, timeout, ("Window " + std::to_string(ref.windowKey)).c_str());
}

Json AllTabs()
{
    Json windows = Json::array();
    for (const auto& window : McpWindows()) {
        try {
            windows.push_back(McpAsk(window.messenger, {{"action", "tabs"}}, 3000000,
                ("Window " + std::to_string(window.key)).c_str()));
        } catch (const ToolError& error) {
            windows.push_back({{"key", window.key}, {"error", error.what()}});
        }
    }
    return windows;
}
}

void RegisterMcpBrowserTools(mcp::Server& server, const std::shared_ptr<McpContext>& context)
{
    server.Add({"summit_info",
        "What is running: Summit and engine versions and packages, the processes (browser, web, network) with "
        "their roles, the display (size, refresh, device scale, accelerant/GPU), the environment's trace and "
        "graphics switches, profile folder, uptime, windows and tabs. Start here.",
        Schema(),
        [context](const Json&) {
            Json info;
            const std::string executable = ExecutablePath();
            const std::string webkit = ImagePath("libWebKit.so.1");
            info["summit"] = {{"target", SUMMIT_TARGET_NAME}, {"executable", executable}, {"package", PackageOf(executable.c_str())},
                {"team", getpid()}, {"uptimeSeconds", double(system_time() - context->startedAt) / 1e6},
                {"profile", context->profile.string()}};
            info["engine"] = {{"webkit", BWebKitVersion()}, {"port", BWebKitPortVersion()}, {"revision", BWebKitSourceRevision()},
                {"library", webkit}, {"package", PackageOf(webkit.c_str())}};
            utsname name;
            if (uname(&name) == 0)
                info["system"] = {{"name", name.sysname}, {"release", name.release}, {"version", name.version}, {"machine", name.machine}};
            system_info system;
            if (get_system_info(&system) == B_OK) {
                info["system"]["cpus"] = system.cpu_count;
                info["system"]["memoryBytes"] = uint64(system.max_pages) * B_PAGE_SIZE;
                info["system"]["usedMemoryBytes"] = uint64(system.used_pages) * B_PAGE_SIZE;
                info["system"]["uptimeSeconds"] = double(system_time()) / 1e6;
            }
            info["display"] = DisplayInfo();
            Json processes = Json::array();
            for (team_id team : McpSummitTeams()) {
                team_info teamInfo;
                if (get_team_info(team, &teamInfo) != B_OK) continue;
                processes.push_back({{"team", team}, {"role", McpTeamRole(team)}, {"threads", teamInfo.thread_count},
                    {"ageSeconds", double(system_time() - teamInfo.start_time) / 1e6}});
            }
            info["processes"] = processes;
            info["environment"] = Environment({ });
            info["windows"] = McpWindows().size();
            info["log"] = {{"capturing", mcplog::Capturing()}, {"sinceSecondsAgo", mcplog::Capturing()
                ? double(system_time() - mcplog::Since()) / 1e6 : 0.0}};
            try {
                info["application"] = McpAsk(context->app, {{"action", "state"}}, 2000000, "The application thread (WebKit's main loop)");
            } catch (const ToolError& error) {
                info["application"] = {{"error", error.what()}};
            }
            return ToolResult::Data(info);
        }});

    server.Add({"tabs",
        "Every browser window and its tabs: id (\"WINDOW.TAB\"), address, title, loading state and errors, web "
        "process (team), frames delivered in the last second, zoom, capture state; window frame, focus and "
        "status line. A window that does not answer is reported (busy or hung).",
        Schema(),
        [](const Json&) { return ToolResult::Data({{"windows", AllTabs()}}); }});

    server.Add({"open_tab",
        "Opens a tab (in the front window, or the given window) with an address or search text.",
        Schema().String("url", "Address (or text to search for); omitted: the new tab page", false)
            .Integer("window", "Window key (from tabs); omitted: the front window")
            .Boolean("select", "Select the new tab (default true)"),
        [](const Json& arguments) {
            Json windowArgument = Json::object();
            if (mcp::HasArgument(arguments, "window"))
                windowArgument["tab"] = std::to_string(mcp::IntegerArgument(arguments, "window", 0)) + ".0";
            McpTabRef ref = McpResolveTab(windowArgument);
            return ToolResult::Data(McpAsk(ref.window, {{"action", "open"}, {"url", mcp::StringArgument(arguments, "url")},
                {"select", mcp::BooleanArgument(arguments, "select", true)}}, 3000000, "The window"));
        }, false});

    server.Add({"navigate",
        "Loads an address (or searches for text) in a tab, which is selected first.",
        Schema().String("url", "Address or search text", true).String("tab", kTabArgument),
        [](const Json& arguments) {
            return ToolResult::Data(AskTab(McpResolveTab(arguments), {{"action", "navigate"},
                {"url", mcp::StringArgument(arguments, "url")}}));
        }, false});

    server.Add({"tab_control",
        "Acts on a tab: reload, stop, back, forward, select (activate=true also raises the window), close, zoom "
        "(factor 0.25-5), devtools (opens Summit's Developer Tools window for it, panel elements, network, console or storage).",
        Schema().Enum("action", "What to do", {"reload", "stop", "back", "forward", "select", "close", "zoom", "devtools"}, true)
            .String("tab", kTabArgument).Number("factor", "Zoom factor for zoom").Boolean("activate", "For select: raise the window")
            .String("panel", "For devtools: elements, network, console or storage"),
        [](const Json& arguments) {
            Json request = {{"action", mcp::StringArgument(arguments, "action")}};
            if (mcp::HasArgument(arguments, "factor")) request["factor"] = mcp::NumberArgument(arguments, "factor", 1);
            if (mcp::HasArgument(arguments, "activate")) request["activate"] = mcp::BooleanArgument(arguments, "activate", false);
            if (mcp::HasArgument(arguments, "panel")) request["panel"] = mcp::StringArgument(arguments, "panel");
            return ToolResult::Data(AskTab(McpResolveTab(arguments), request));
        }, false});

    server.Add({"screenshot",
        "A PNG of what is shown: target screen (the whole main screen), window (a browser window's frame), page "
        "(the tab's page view as on screen). All come from app_server's frame buffer copy, so they show what the "
        "user sees, including drawing bugs; compare with page_render (the engine's own rendering) to tell a "
        "compositor or presentation fault from a layout one. max_width scales the picture down (default 1600).",
        Schema().Enum("target", "screen, window or page (default page)", {"screen", "window", "page"})
            .String("tab", kTabArgument).Integer("max_width", "Scale down to at most this width (0: full size)")
            .String("save_to", "Write the PNG to this path on the machine instead of returning it"),
        [](const Json& arguments) {
            const std::string target = mcp::StringArgument(arguments, "target", "page");
            const int32 maxWidth = int32(mcp::IntegerArgument(arguments, "max_width", 1600, 0, 16384));
            std::unique_ptr<BBitmap> bitmap;
            std::string caption;
            if (target == "screen") {
                bitmap = CaptureScreen(nullptr);
                caption = "The main screen";
            } else {
                McpTabRef ref = McpResolveTab(arguments);
                const Json geometry = AskTab(ref, {{"action", "geometry"}});
                if (geometry.contains("error")) throw ToolError(geometry["error"].get<std::string>());
                const Json& rect = geometry[target == "window" ? "window" : "view"];
                const BRect frame(rect["left"].get<float>(), rect["top"].get<float>(),
                    rect["left"].get<float>() + rect["width"].get<float>() - 1, rect["top"].get<float>() + rect["height"].get<float>() - 1);
                if (target == "page" && geometry.value("hidden", false))
                    throw ToolError("tab " + ref.id + " is not on screen (not selected); select it first or use page_render");
                bitmap = CaptureScreen(&frame);
                caption = (target == "window" ? "Window " + std::to_string(ref.windowKey) : "Page view of tab " + geometry.value("tab", ref.id))
                    + " at " + std::to_string(int(frame.left)) + "," + std::to_string(int(frame.top)) + " "
                    + std::to_string(int(frame.Width() + 1)) + "×" + std::to_string(int(frame.Height() + 1));
            }
            // A blanked desktop captures black.
            if (team_info blanker; [&] {
                    int32 cookie = 0;
                    while (get_next_team_info(&cookie, &blanker) == B_OK)
                        if (std::strstr(blanker.args, "screen_blanker")) return true;
                    return false;
                }())
                caption += " (screen_blanker is running: a blanked desktop captures black)";
            const BRect full = bitmap->Bounds();
            bitmap = Scaled(std::move(bitmap), maxWidth);
            if (bitmap->Bounds().Width() != full.Width())
                caption += " (scaled from " + std::to_string(int(full.Width() + 1)) + " px wide)";
            return ImageResult(EncodePNG(std::move(bitmap)), arguments, caption);
        }});

    server.Add({"frame_stats",
        "Frames the engine delivered to a tab's view over the last seconds (default 5): frames per second, "
        "gaps between frames (median, 95th percentile, longest, counts over 33/50/100 ms), frames put straight "
        "into the screen (direct present), the longest wait in the window's queue. Use while scrolling, playing "
        "video or animating; an idle page delivers no frames. With scroll=true a wheel burst of 60 notches at 60 "
        "per second is sent first and measured.",
        Schema().String("tab", kTabArgument).Number("seconds", "Window to report (0.1-60, default 5)")
            .Boolean("scroll", "Scroll the page first (a burst; then the report covers it)")
            .Number("delta", "For scroll: notches per event (positive down, default 3)"),
        [](const Json& arguments) {
            McpTabRef ref = McpResolveTab(arguments);
            double seconds = mcp::NumberArgument(arguments, "seconds", 5);
            if (mcp::BooleanArgument(arguments, "scroll", false)) {
                const Json started = AskTab(ref, {{"action", "burst"}, {"count", 60}, {"interval_ms", 16},
                    {"delta", mcp::NumberArgument(arguments, "delta", 3)}});
                if (started.contains("error")) throw ToolError(started["error"].get<std::string>());
                std::this_thread::sleep_for(std::chrono::milliseconds(1400));
                if (!mcp::HasArgument(arguments, "seconds")) seconds = 1.5;
            }
            return ToolResult::Data(AskTab(ref, {{"action", "frames"}, {"seconds", seconds}}));
        }});

    auto input = [](const Json& arguments, Json request) {
        McpTabRef ref = McpResolveTab(arguments);
        if (mcp::BooleanArgument(arguments, "activate", false)) request["activate"] = true;
        if (mcp::HasArgument(arguments, "modifiers")) request["modifiers"] = arguments["modifiers"];
        const Json answer = AskTab(ref, request);
        if (answer.contains("error")) throw ToolError(answer["error"].get<std::string>());
        return ToolResult::Data(answer);
    };
    server.Add({"click",
        "Clicks in a tab's page at view coordinates (pixels from the page view's top left; default the middle), "
        "with synthesized mouse messages (the pointer does not move). The tab must be selected (activate=true "
        "selects it and raises the window).",
        Schema().String("tab", kTabArgument).Number("x", "X in the page view").Number("y", "Y in the page view")
            .Enum("button", "left (default), right or middle", {"left", "right", "middle"}).Integer("clicks", "1-3")
            .Enum("kind", "click (default), down, up or move", {"click", "down", "up", "move"})
            .StringArray("modifiers", "shift, control, command, option").Boolean("activate", "Select the tab and raise the window first"),
        [input](const Json& arguments) {
            Json request = {{"action", "input"}, {"kind", mcp::StringArgument(arguments, "kind", "click")},
                {"button", mcp::StringArgument(arguments, "button", "left")}, {"clicks", mcp::IntegerArgument(arguments, "clicks", 1, 1, 3)}};
            if (mcp::HasArgument(arguments, "x")) request["x"] = mcp::NumberArgument(arguments, "x", 0);
            if (mcp::HasArgument(arguments, "y")) request["y"] = mcp::NumberArgument(arguments, "y", 0);
            return input(arguments, request);
        }, false});
    server.Add({"type_text",
        "Types text into the focused element of a tab's page (key messages per character; click a field first).",
        Schema().String("text", "What to type", true).String("tab", kTabArgument).Boolean("activate", "Select the tab and raise the window first"),
        [input](const Json& arguments) {
            return input(arguments, {{"action", "input"}, {"kind", "text"}, {"text", mcp::StringArgument(arguments, "text")}});
        }, false});
    server.Add({"press_key",
        "Presses a key in a tab's page: enter, tab, escape, backspace, delete, space, left/right/up/down, home, "
        "end, pageup, pagedown, f1-f12 or one character, with modifiers.",
        Schema().String("key", "Key name", true).String("tab", kTabArgument).StringArray("modifiers", "shift, control, command, option")
            .Boolean("activate", "Select the tab and raise the window first"),
        [input](const Json& arguments) {
            return input(arguments, {{"action", "input"}, {"kind", "key"}, {"key", mcp::StringArgument(arguments, "key")}});
        }, false});
    server.Add({"scroll",
        "Scrolls a tab's page with mouse wheel messages at a point (default the middle): one event of dy notches "
        "(positive scrolls down), or with count a burst paced at interval_ms (default 16 ms, as a fast wheel), "
        "which frame_stats can then measure.",
        Schema().String("tab", kTabArgument).Number("dy", "Vertical notches per event (default 3)").Number("dx", "Horizontal notches")
            .Number("x", "X in the page view").Number("y", "Y in the page view").Integer("count", "Events in a burst")
            .Integer("interval_ms", "Time between burst events").Boolean("activate", "Select the tab and raise the window first"),
        [input](const Json& arguments) {
            if (mcp::HasArgument(arguments, "count")) {
                Json request = {{"action", "burst"}, {"count", mcp::IntegerArgument(arguments, "count", 30, 1, 10000)},
                    {"interval_ms", mcp::IntegerArgument(arguments, "interval_ms", 16, 0, 1000)},
                    {"delta", mcp::NumberArgument(arguments, "dy", 3)}};
                return input(arguments, request);
            }
            Json request = {{"action", "input"}, {"kind", "scroll"}, {"dy", mcp::NumberArgument(arguments, "dy", 3)},
                {"dx", mcp::NumberArgument(arguments, "dx", 0)}};
            if (mcp::HasArgument(arguments, "x")) request["x"] = mcp::NumberArgument(arguments, "x", 0);
            if (mcp::HasArgument(arguments, "y")) request["y"] = mcp::NumberArgument(arguments, "y", 0);
            return input(arguments, request);
        }, false});

    server.Add({"extensions",
        "Installed extensions: identifier, name, version, enabled, loaded, the base URL of its pages "
        "(webkit-extension://…, open its pages with open_tab to inspect them), errors and notices from loading, "
        "private-window access, toolbar pin. Extension background pages' and popups' console messages are in "
        "logs (filter by the extension's name or URL) for pages made after the server started: restart an "
        "extension with extension_control to have its background page's.",
        Schema(),
        [context](const Json&) {
            return ToolResult::Data(McpAsk(context->app, {{"action", "extensions"}}, 3000000, "The application thread (WebKit's main loop)"));
        }});
    server.Add({"extension_control",
        "Enables or disables an installed extension (disabling unloads it; enabling loads it again: a quick way "
        "to restart an extension's background page).",
        Schema().String("extension", "Identifier from extensions", true).Boolean("enabled", "true loads it, false unloads it", true),
        [context](const Json& arguments) {
            return ToolResult::Data(McpAsk(context->app, {{"action", "extension_enable"},
                {"extension", mcp::StringArgument(arguments, "extension")}, {"enabled", mcp::BooleanArgument(arguments, "enabled", true)}},
                3000000, "The application thread (WebKit's main loop)"));
        }, false});

    server.Add({"bookmarks",
        "The bookmarks as a tree: the bookmarks bar and Other Bookmarks with their folders, each node with its id. "
        "action=import reads path as File › Import Bookmarks and History does (WebPositive's Bookmarks folder, an "
        "exported bookmarks HTML file, Chrome's Bookmarks file, Firefox's places.sqlite) into \"Imported from …\"; "
        "action=export writes every bookmark to path as bookmarks HTML.",
        Schema().Enum("action", "list (default), import or export", {"list", "import", "export"})
            .String("path", "For import and export: a path on this machine")
            .Boolean("history", "For import: the history of visited pages too (default true)"),
        [context](const Json& arguments) {
            const std::string action = mcp::StringArgument(arguments, "action", "list");
            if (action != "list" && mcp::StringArgument(arguments, "path", "").empty()) throw ToolError(action + " needs a path");
            return ToolResult::Data(McpAsk(context->app, {{"action", "bookmarks"}, {"do", action},
                {"path", mcp::StringArgument(arguments, "path", "")}, {"history", mcp::BooleanArgument(arguments, "history", true)}},
                60000000, "The application thread (WebKit's main loop)"));
        }, false});
    server.Add({"preferences",
        "Summit's settings as the profile keeps them (home page, search engine, session restore, appearance, "
        "site permissions, link handlers, trusted certificates; the MCP server's port). show=true also opens the "
        "Preferences window (tab: general, data, sites, links or developer).",
        Schema().Boolean("show", "Open the Preferences window").Enum("tab", "Which tab to show",
            {"general", "data", "sites", "links", "developer"}),
        [context](const Json& arguments) {
            if (mcp::BooleanArgument(arguments, "show", false)) {
                BMessage show('pref');
                show.AddString("tab", mcp::StringArgument(arguments, "tab", "general").c_str());
                context->app.SendMessage(&show);
            }
            return ToolResult::Data(McpAsk(context->app, {{"action", "preferences"}}, 3000000, "The application thread (WebKit's main loop)"));
        }, false});

    server.Add({"updates",
        "Summit's own updates (File › Check for Updates…): the build that runs (its summit and summit_webkit "
        "packages and commit, or why it cannot update itself), the release it updates from, the daily check, the "
        "last check's result and the update window (phase, status, progress). do=check checks now as the menu "
        "does (a newer build opens the update window); install, later and changes press that window's buttons.",
        Schema().Enum("do", "status (default), check, install, later or changes", {"status", "check", "install", "later", "changes"}),
        [context](const Json& arguments) {
            return ToolResult::Data(McpAsk(context->app, {{"action", "updates"}, {"do", mcp::StringArgument(arguments, "do", "status")}},
                5000000, "The application thread (WebKit's main loop)"));
        }, false});
}
}
#endif
