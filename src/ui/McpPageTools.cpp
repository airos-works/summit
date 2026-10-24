#if SUMMIT_MODERN_WEBKIT
// MCP tools that look into a tab's page through the Web Inspector protocol
// (McpService.h): JavaScript, console, network, DOM, timings, the timeline,
// compositing layers, the JavaScript heap. One inspector session per tab is
// made on first use and kept, so console and network are collected from then
// on (InspectorChannel, core/McpInspector.h).
#include "McpService.h"
#include "BrowserWindow.h"
#include "core/McpInspector.h"
#include <WebKit/WebKitInspector.h>
#include <WebKit/WebKitView.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <DataIO.h>
#include <Looper.h>
#include <TranslationUtils.h>
#include <TranslatorRoster.h>
#include <View.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <memory>
#include <set>
#include <thread>

namespace summit {
namespace {
using mcp::InspectorChannel;
using mcp::Schema;
using mcp::ToolError;
using mcp::ToolResult;

const char* kTabArgument = "Tab as the tabs tool names it (\"WINDOW.TAB\", or TAB in the front window); "
    "omitted: the front window's selected tab";
// Instead of a tab: an extension's background page (started if it is not
// running), as the extensions tool names it.
const char* kExtensionArgument = "Instead of tab: an extension's background page, by the identifier or baseURL the "
    "extensions tool gives (its background is started when it is not running)";

struct PageConnection {
    std::string id;
    BMessenger window;
    int64 tab = 0;
    const void* view = nullptr;
    uint64 identifier = 0;
    std::shared_ptr<BWebKitInspectorSession> session;
    InspectorChannel channel;
    bigtime_t attachedAt = 0;
};

std::mutex sLock;
std::condition_variable sChanged;
std::map<uint64, std::shared_ptr<PageConnection>> sByIdentifier;
std::map<std::string, std::shared_ptr<PageConnection>> sByTab;
std::set<std::string> sResourceRecordings; // Protected by sLock, including replacement connections.
uint64 sNextIdentifier = 1;

void Flush(PageConnection& connection, std::vector<std::string> outgoing)
{
    for (const auto& message : outgoing) connection.session->Send(message.c_str());
}

// Takes the sessions' messages as the engine announces them.
class InspectorPump final : public BLooper {
public:
    InspectorPump() : BLooper("Summit MCP inspector") { }
    void MessageReceived(BMessage* message) override
    {
        if (message->what != B_WEBKIT_INSPECTOR_MESSAGES) {
            BLooper::MessageReceived(message);
            return;
        }
        std::shared_ptr<PageConnection> connection;
        {
            std::lock_guard lock(sLock);
            auto found = sByIdentifier.find(message->GetUInt64("identifier", 0));
            if (found == sByIdentifier.end()) return;
            connection = found->second;
        }
        std::vector<std::string> messages;
        connection->session->Take(messages);
        std::vector<std::string> outgoing;
        {
            std::lock_guard lock(sLock);
            for (const auto& text : messages) connection->channel.Receive(text);
            outgoing = connection->channel.TakeOutgoing();
        }
        Flush(*connection, std::move(outgoing));
        sChanged.notify_all();
    }
};

BMessenger Pump()
{
    static InspectorPump* pump = [] {
        auto* looper = new InspectorPump();
        looper->Run();
        return looper;
    }();
    return BMessenger(pump);
}

// The tab's concrete id: the selected tab when none was named.
int64 ConcreteTab(const McpTabRef& ref)
{
    if (ref.tab) return ref.tab;
    const Json summary = McpAsk(ref.window, {{"action", "summary"}}, 3000000, "The window");
    const int64 selected = summary.value("selected", int64(0));
    if (!selected) throw ToolError("the window has no selected tab");
    return selected;
}

// Runs function with the tab's view while its window is locked.
bool WithView(const BMessenger& window, int64 tab, const std::function<void(BWebKitView&)>& function)
{
    if (window.LockTargetWithTimeout(3000000) != B_OK)
        throw ToolError("the window could not be locked within 3 s: its thread is busy or hung");
    BLooper* looper = nullptr;
    window.Target(&looper);
    auto* browser = dynamic_cast<BrowserWindow*>(looper);
    const bool found = browser && browser->McpWithView(tab, function);
    if (looper) looper->Unlock();
    return found;
}

// The engine names a page's target after the page; a connection that is not
// the page's first (the Developer Tools may be open) is not told the name.
std::string PageTarget(const BMessenger& window, const std::string& id)
{
    if (window.LockTargetWithTimeout(3000000) != B_OK) return { };
    BLooper* looper = nullptr;
    window.Target(&looper);
    std::string answer;
    if (auto* browser = dynamic_cast<BrowserWindow*>(looper)) answer = browser->McpRequest("{\"action\":\"tabs\"}");
    if (looper) looper->Unlock();
    const Json tabs = Json::parse(answer, nullptr, false);
    if (!tabs.is_object() || !tabs.contains("tabs") || !tabs["tabs"].is_array()) return { };
    for (const auto& tab : tabs["tabs"]) {
        if (!tab.is_object() || tab.value("id", std::string()) != id) continue;
        const uint64 page = tab.value("pageIdentifier", uint64(0));
        return page ? "page-" + std::to_string(page) : std::string();
    }
    return { };
}

// A session on an extension's background page (the extensions tool's
// identifier or base URL), through the front window's engine context.
std::shared_ptr<PageConnection> ConnectExtension(const std::string& extension)
{
    const std::string id = "extension:" + extension;
    {
        std::lock_guard lock(sLock);
        if (auto found = sByTab.find(id); found != sByTab.end()) return found->second;
    }
    McpTabRef ref = McpResolveTab(Json::object());
    std::shared_ptr<BWebKitContext> context;
    if (ref.window.LockTargetWithTimeout(3000000) != B_OK)
        throw ToolError("the window could not be locked within 3 s: its thread is busy or hung");
    BLooper* looper = nullptr;
    ref.window.Target(&looper);
    if (auto* browser = dynamic_cast<BrowserWindow*>(looper)) context = browser->McpWebKitContext();
    if (looper) looper->Unlock();
    if (!context) throw ToolError("no browser window has an engine context");
    const BMessenger pump = Pump();
    auto connection = std::make_shared<PageConnection>();
    connection->id = id;
    connection->window = ref.window;
    connection->attachedAt = system_time();
    {
        std::lock_guard lock(sLock);
        connection->identifier = sNextIdentifier++;
    }
    connection->session = BWebKitInspectorSession::CreateForExtension(*context, extension.c_str(), pump, connection->identifier);
    if (!connection->session) throw ToolError("the engine made no inspector session for extension " + extension);
    std::vector<std::string> outgoing;
    {
        std::lock_guard lock(sLock);
        if (auto found = sByTab.find(id); found != sByTab.end()) return found->second;
        sByIdentifier[connection->identifier] = connection;
        sByTab[id] = connection;
        connection->channel.Start();
        outgoing = connection->channel.TakeOutgoing();
    }
    BMessage ready(B_WEBKIT_INSPECTOR_MESSAGES);
    ready.AddUInt64("identifier", connection->identifier);
    if (pump.SendMessage(&ready, static_cast<BHandler*>(nullptr), 1000000) != B_OK)
        throw ToolError("the inspector message pump could not be reached");
    Flush(*connection, std::move(outgoing));
    return connection;
}

std::shared_ptr<PageConnection> Connect(const Json& arguments)
{
    if (const std::string extension = mcp::StringArgument(arguments, "extension", ""); !extension.empty())
        return ConnectExtension(extension);
    McpTabRef ref = McpResolveTab(arguments);
    const int64 tab = ConcreteTab(ref);
    const std::string id = std::to_string(ref.windowKey) + "." + std::to_string(tab);
    std::shared_ptr<PageConnection> existing;
    {
        std::lock_guard lock(sLock);
        if (auto found = sByTab.find(id); found != sByTab.end()) existing = found->second;
    }
    const BMessenger pump = Pump();
    std::shared_ptr<PageConnection> connection;
    const bool found = WithView(ref.window, tab, [&](BWebKitView& view) {
        // A tab whose view was replaced (an extension page and a web page
        // need different views) needs a new session.
        if (existing && existing->view == &view) {
            connection = existing;
            return;
        }
        connection = std::make_shared<PageConnection>();
        connection->id = id;
        connection->window = ref.window;
        connection->tab = tab;
        connection->view = &view;
        connection->attachedAt = system_time();
        {
            std::lock_guard lock(sLock);
            connection->identifier = sNextIdentifier++;
        }
        connection->session = BWebKitInspectorSession::Create(view, pump, connection->identifier);
    });
    if (!found) {
        std::lock_guard lock(sLock);
        if (existing) {
            sByIdentifier.erase(existing->identifier);
            sByTab.erase(id);
        }
        throw ToolError("there is no tab " + id);
    }
    if (connection == existing) return connection;
    if (!connection->session) throw ToolError("the engine made no inspector session for tab " + id);
    const std::string target = PageTarget(ref.window, id);
    std::vector<std::string> outgoing;
    {
        std::lock_guard lock(sLock);
        // Another first request may have attached while Create() ran. Keep
        // one connection per view; the unused session detaches on destruction.
        if (auto found = sByTab.find(id); found != sByTab.end() && found->second->view == connection->view)
            return found->second;
        if (existing) sByIdentifier.erase(existing->identifier);
        sByIdentifier[connection->identifier] = connection;
        sByTab[id] = connection;
        connection->channel.Start();
        connection->channel.AssumePageTarget(target);
        outgoing = connection->channel.TakeOutgoing();
    }
    // Create() attaches on the WebKit main loop. Its first notification may
    // reach the pump before this connection is registered above. The engine
    // coalesces notifications until Take(), so explicitly drain once after
    // registration even if that early notification was missed.
    BMessage ready(B_WEBKIT_INSPECTOR_MESSAGES);
    ready.AddUInt64("identifier", connection->identifier);
    if (pump.SendMessage(&ready, static_cast<BHandler*>(nullptr), 1000000) != B_OK)
        throw ToolError("the inspector message pump could not be reached");
    Flush(*connection, std::move(outgoing));
    return connection;
}

// Sends a command to the page and waits for its answer's result.
Json Command(PageConnection& connection, const std::string& method, const Json& parameters, bigtime_t timeout = 10000000)
{
    uint64 id;
    std::vector<std::string> outgoing;
    {
        std::lock_guard lock(sLock);
        id = connection.channel.Command(method, parameters);
        outgoing = connection.channel.TakeOutgoing();
    }
    Flush(connection, std::move(outgoing));
    std::unique_lock lock(sLock);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeout);
    while (!connection.channel.Answered(id)) {
        if (sChanged.wait_until(lock, deadline) == std::cv_status::timeout && !connection.channel.Answered(id)) {
            const bool target = !connection.channel.PageTarget().empty();
            const bool extension = connection.id.starts_with("extension:");
            throw ToolError(method + ": the page did not answer within " + std::to_string(timeout / 1000) + " ms"
                + (target ? " (its web process may be busy or hung: see threads and engine_report)"
                    : extension ? " (no background page: check the extensions tool for the identifier, that the extension is enabled and that it has a background)"
                    : " (no page target yet: the page may be loading in a new process)"));
        }
    }
    Json answer = *connection.channel.TakeAnswer(id);
    if (answer.contains("error")) {
        const Json& error = answer["error"];
        throw ToolError(method + " failed: " + (error.is_object() ? error.value("message", error.dump()) : error.dump()));
    }
    return answer["result"];
}

// Queues a command without waiting for its answer (which is dropped).
void Send(PageConnection& connection, const std::string& method, const Json& parameters)
{
    std::vector<std::string> outgoing;
    {
        std::lock_guard lock(sLock);
        connection.channel.Command(method, parameters);
        outgoing = connection.channel.TakeOutgoing();
    }
    Flush(connection, std::move(outgoing));
}

// The value of an expression: by value where it serializes, awaited when a
// promise, else the engine's description.
Json Evaluate(PageConnection& connection, const std::string& expression, bigtime_t timeout, bool userGesture = false)
{
    const Json evaluated = Command(connection, "Runtime.evaluate", {{"expression", expression}, {"objectGroup", "summit-mcp"},
        {"includeCommandLineAPI", true}, {"returnByValue", false}, {"generatePreview", true}, {"emulateUserGesture", userGesture}}, timeout);
    const Json& object = evaluated.value("result", Json::object());
    Json answer = {{"type", object.value("type", "undefined")}};
    if (object.contains("subtype")) answer["subtype"] = object["subtype"];
    if (evaluated.value("wasThrown", false)) {
        answer["exception"] = object.value("description", object.dump());
        Send(connection, "Runtime.releaseObjectGroup", {{"objectGroup", "summit-mcp"}});
        return answer;
    }
    if (object.contains("value")) answer["value"] = object["value"];
    else if (object.contains("objectId")) {
        try {
            const Json value = Command(connection, "Runtime.callFunctionOn", {{"objectId", object["objectId"]},
                {"functionDeclaration", "function() { return this; }"}, {"returnByValue", true}, {"awaitPromise", true}}, timeout);
            const Json& result = value.value("result", Json::object());
            if (value.value("wasThrown", false)) answer["exception"] = result.value("description", result.dump());
            else if (result.contains("value")) answer["value"] = result["value"];
            if (object.value("subtype", "") == "promise" && result.contains("type")) answer["resolvedType"] = result["type"];
        } catch (const ToolError&) {
        }
        if (!answer.contains("value") && !answer.contains("exception")) {
            answer["description"] = object.value("description", "");
            if (object.contains("preview")) answer["preview"] = object["preview"];
        }
    }
    // A DOM node or another object that does not serialize comes back as {}:
    // the engine's description says what it is.
    const bool empty = answer.contains("value") && (answer["value"].is_object() || answer["value"].is_array()) && answer["value"].empty();
    if ((!answer.contains("value") || empty) && object.contains("description") && !answer.contains("description"))
        answer["description"] = object["description"];
    if (empty && object.contains("preview")) answer["preview"] = object["preview"];
    Send(connection, "Runtime.releaseObjectGroup", {{"objectGroup", "summit-mcp"}});
    return answer;
}

// A script's value, or a tool error with its exception.
Json Script(PageConnection& connection, const std::string& expression, bigtime_t timeout = 15000000)
{
    const Json answer = Evaluate(connection, expression, timeout);
    if (answer.contains("exception")) throw ToolError("the page's script failed: " + answer["exception"].get<std::string>());
    return answer.value("value", Json());
}

std::string Quoted(const std::string& text)
{
    return Json(text).dump();
}

const char* LevelName(devtools::Level level)
{
    switch (level) {
        case devtools::Level::Debug: return "debug";
        case devtools::Level::Log: return "log";
        case devtools::Level::Info: return "info";
        case devtools::Level::Warning: return "warning";
        case devtools::Level::Error: return "error";
    }
    return "log";
}

void Pause(bigtime_t micros)
{
    if (micros > 0) std::this_thread::sleep_for(std::chrono::microseconds(micros));
}

const char* kMetricsScript = R"JS((async () => {
  const round = (v) => v === undefined || v === null ? null : Math.round(v * 10) / 10;
  const nav = performance.getEntriesByType('navigation')[0];
  const timing = nav ? {
    type: nav.type, redirectMs: round(nav.redirectEnd - nav.redirectStart), dnsMs: round(nav.domainLookupEnd - nav.domainLookupStart),
    connectMs: round(nav.connectEnd - nav.connectStart), tlsMs: nav.secureConnectionStart ? round(nav.connectEnd - nav.secureConnectionStart) : 0,
    firstByteMs: round(nav.responseStart), responseEndMs: round(nav.responseEnd), domInteractiveMs: round(nav.domInteractive),
    domContentLoadedMs: round(nav.domContentLoadedEventEnd), loadEventEndMs: round(nav.loadEventEnd),
    transferBytes: nav.transferSize, protocol: nav.nextHopProtocol } : null;
  const paints = {};
  for (const p of performance.getEntriesByType('paint')) paints[p.name] = round(p.startTime);
  let lcp = null;
  try {
    lcp = await new Promise((resolve) => {
      let last = null;
      const observer = new PerformanceObserver((list) => { for (const e of list.getEntries()) last = e; });
      observer.observe({ type: 'largest-contentful-paint', buffered: true });
      setTimeout(() => { observer.disconnect(); resolve(last ? { ms: round(last.startTime), size: last.size,
        element: last.element ? last.element.tagName + (last.element.id ? '#' + last.element.id : '') : null, url: last.url || null } : null); }, 250);
    });
  } catch (e) { lcp = 'unsupported'; }
  const resources = performance.getEntriesByType('resource');
  const byType = {};
  let bytes = 0;
  for (const r of resources) {
    const t = byType[r.initiatorType] || (byType[r.initiatorType] = { count: 0, transferBytes: 0, totalMs: 0 });
    t.count++; t.transferBytes += r.transferSize || 0; t.totalMs = round(t.totalMs + r.duration); bytes += r.transferSize || 0;
  }
  const slowest = [...resources].sort((a, b) => b.duration - a.duration).slice(0, 8)
    .map(r => ({ url: r.name.slice(0, 200), ms: round(r.duration), startMs: round(r.startTime), type: r.initiatorType, bytes: r.transferSize }));
  const frames = await new Promise((resolve) => {
    let count = 0; const start = performance.now();
    const tick = () => { count++; if (performance.now() - start < 500) requestAnimationFrame(tick); else resolve(count / ((performance.now() - start) / 1000)); };
    requestAnimationFrame(tick);
    setTimeout(() => resolve(count ? count / ((performance.now() - start) / 1000) : 0), 1500);
  });
  const all = document.getElementsByTagName('*').length;
  return {
    url: location.href, title: document.title, readyState: document.readyState, visibility: document.visibilityState,
    navigation: timing, paint: paints, largestContentfulPaint: lcp,
    resources: { count: resources.length, transferBytes: bytes, byType, slowest },
    dom: { elements: all, iframes: document.querySelectorAll('iframe').length, images: document.images.length,
      scripts: document.scripts.length, styleSheets: document.styleSheets.length, canvases: document.querySelectorAll('canvas').length,
      videos: document.querySelectorAll('video').length, audios: document.querySelectorAll('audio').length,
      shadowRoots: [...document.querySelectorAll('*')].filter(e => e.shadowRoot).length },
    viewport: { width: innerWidth, height: innerHeight, devicePixelRatio, scrollX, scrollY,
      documentWidth: document.documentElement.scrollWidth, documentHeight: document.documentElement.scrollHeight },
    animationFramesPerSecond: round(frames),
    serviceWorker: navigator.serviceWorker ? !!navigator.serviceWorker.controller : null
  };
})())JS";

const char* kMediaScript = R"JS((() => {
  const ranges = (r) => { const out = []; for (let i = 0; i < r.length; i++) out.push([Math.round(r.start(i) * 100) / 100, Math.round(r.end(i) * 100) / 100]); return out; };
  return [...document.querySelectorAll('video, audio')].map((m, index) => {
    const q = m.getVideoPlaybackQuality ? m.getVideoPlaybackQuality() : null;
    const box = m.getBoundingClientRect();
    return {
      index, tag: m.tagName.toLowerCase(), id: m.id || undefined, src: (m.currentSrc || m.src || '').slice(0, 300),
      mediaSource: (m.currentSrc || '').startsWith('blob:'), readyState: m.readyState, networkState: m.networkState,
      paused: m.paused, ended: m.ended, seeking: m.seeking, currentTime: Math.round(m.currentTime * 1000) / 1000,
      duration: m.duration, playbackRate: m.playbackRate, volume: m.volume, muted: m.muted, autoplay: m.autoplay, loop: m.loop,
      videoWidth: m.videoWidth, videoHeight: m.videoHeight, buffered: ranges(m.buffered), played: ranges(m.played),
      error: m.error ? { code: m.error.code, message: m.error.message } : null,
      quality: q ? { totalVideoFrames: q.totalVideoFrames, droppedVideoFrames: q.droppedVideoFrames, corruptedVideoFrames: q.corruptedVideoFrames } : null,
      webkitDecodedFrameCount: m.webkitDecodedFrameCount, webkitDroppedFrameCount: m.webkitDroppedFrameCount,
      audioDecodedByteCount: m.webkitAudioDecodedByteCount, videoDecodedByteCount: m.webkitVideoDecodedByteCount,
      box: { x: Math.round(box.x), y: Math.round(box.y), width: Math.round(box.width), height: Math.round(box.height) },
      pictureInPicture: document.pictureInPictureElement === m, fullscreen: document.fullscreenElement === m
    };
  });
})())JS";
}

void RegisterMcpPageTools(mcp::Server& server, const std::shared_ptr<McpContext>&)
{
    server.Add({"evaluate",
        "Runs JavaScript in a tab's page (its main frame, the page's own world, with the console's command line "
        "API such as $$ and copy) and returns the value: by value when it serializes as JSON, awaited when it is "
        "a promise, else the engine's description; exceptions are reported. Statements are fine; the last "
        "expression's value is returned.",
        Schema().String("expression", "JavaScript", true).String("tab", kTabArgument).String("extension", kExtensionArgument)
            .Integer("timeout_ms", "How long to wait (default 15000)").Boolean("user_gesture", "Run as if the user clicked")
            .Integer("max_bytes", "Most of the value returned (default 100000)"),
        [](const Json& arguments) {
            auto connection = Connect(arguments);
            const Json answer = Evaluate(*connection, mcp::StringArgument(arguments, "expression"),
                mcp::IntegerArgument(arguments, "timeout_ms", 15000, 100, 300000) * 1000, mcp::BooleanArgument(arguments, "user_gesture", false));
            const size_t limit = size_t(mcp::IntegerArgument(arguments, "max_bytes", 100000, 100, 16 << 20));
            if (answer.contains("value") && answer["value"].is_string() && answer.size() == 2)
                return ToolResult::Text(mcp::Truncate(answer["value"].get<std::string>(), limit));
            return ToolResult::Text(mcp::Truncate(answer.dump(1, ' ', false, Json::error_handler_t::replace), limit));
        }, false});

    server.Add({"console",
        "A tab's console: what its scripts logged and the engine reported (errors, warnings, security and network "
        "messages), with source locations and stacks for errors, kept from the first page tool call on that tab. "
        "reload=true reloads the page and waits (wait_ms, default 4000) to catch its load. since: the last serial "
        "of a previous call.",
        Schema().String("tab", kTabArgument).String("extension", kExtensionArgument).Integer("since", "Only entries after this serial")
            .Enum("level", "Lowest level shown (default debug: all)", {"debug", "log", "info", "warning", "error"})
            .String("filter", "Text or glob on the message").Integer("limit", "Most entries (default 200)")
            .Boolean("reload", "Reload the page first").Integer("wait_ms", "Wait before reading").Boolean("clear", "Forget the entries afterwards"),
        [](const Json& arguments) {
            auto connection = Connect(arguments);
            const bool reload = mcp::BooleanArgument(arguments, "reload", false);
            if (reload) Send(*connection, "Page.reload", {{"ignoreCache", false}});
            Pause(mcp::IntegerArgument(arguments, "wait_ms", reload ? 4000 : 300, 0, 120000) * 1000);
            const uint64 since = uint64(mcp::IntegerArgument(arguments, "since", 0, 0));
            const std::string level = mcp::StringArgument(arguments, "level", "debug");
            const int minimum = level == "error" ? 4 : level == "warning" ? 3 : level == "info" ? 2 : level == "log" ? 1 : 0;
            std::string pattern = mcp::StringArgument(arguments, "filter");
            if (!pattern.empty() && pattern.find('*') == std::string::npos) pattern = "*" + pattern + "*";
            const size_t limit = size_t(mcp::IntegerArgument(arguments, "limit", 200, 1, 10000));
            std::vector<std::string> lines;
            uint64 last = 0;
            size_t total = 0;
            std::string url;
            {
                std::lock_guard lock(sLock);
                auto& model = connection->channel.Model();
                url = model.PageURL();
                for (const auto& entry : model.Entries()) {
                    last = std::max(last, entry.serial);
                    if (entry.serial <= since || int(entry.level) < minimum) continue;
                    if (!pattern.empty() && !mcp::Matches(pattern, entry.text)) continue;
                    ++total;
                    std::string line = "#" + std::to_string(entry.serial) + " [" + LevelName(entry.level) + "]";
                    if (entry.kind == devtools::EntryKind::Navigation) line += " (navigated)";
                    if (!entry.source.empty() && entry.source != "console-api") line += " (" + entry.source + ")";
                    line += " " + entry.text;
                    if (entry.repeat > 1) line += " ×" + std::to_string(entry.repeat);
                    if (!entry.Location().empty()) line += "  — " + entry.Location();
                    if (!entry.stack.empty() && entry.level >= devtools::Level::Warning) line += "\n    " + entry.stack;
                    lines.push_back(line);
                }
                if (mcp::BooleanArgument(arguments, "clear", false)) model.ClearConsole();
            }
            std::string text = "Console of tab " + connection->id + " (" + url + "), collected for "
                + std::to_string((system_time() - connection->attachedAt) / 1000000) + " s:\n";
            const size_t first = lines.size() > limit ? lines.size() - limit : 0;
            if (first) text += "(" + std::to_string(first) + " earlier entries left out)\n";
            for (size_t i = first; i < lines.size(); ++i) text += lines[i] + "\n";
            if (lines.empty()) text += "(nothing" + std::string(since ? " new" : "") + ")\n";
            text += "last=" + std::to_string(last);
            return ToolResult::Text(mcp::Truncate(text, 400000));
        }});

    server.Add({"network",
        "A tab's requests since the first page tool call on it (reload=true to see its load): method, address, "
        "status, type, protocol, sizes, timing, failures and where the answer came from (network, memory or disk "
        "cache); a summary with the slowest and failed ones. request=SERIAL gives one request's headers and body.",
        Schema().String("tab", kTabArgument).String("filter", "Text or glob on the address").Boolean("failed", "Only failed or 4xx/5xx")
            .Integer("limit", "Most requests (default 100), latest last").Boolean("reload", "Reload the page first")
            .Integer("wait_ms", "Wait before reading").Integer("request", "One request's serial, for its headers and body")
            .Integer("max_body", "Most of a body returned (default 20000)"),
        [](const Json& arguments) {
            auto connection = Connect(arguments);
            const bool reload = mcp::BooleanArgument(arguments, "reload", false);
            if (reload) Send(*connection, "Page.reload", {{"ignoreCache", false}});
            Pause(mcp::IntegerArgument(arguments, "wait_ms", reload ? 5000 : 200, 0, 120000) * 1000);
            if (mcp::HasArgument(arguments, "request")) {
                const uint64 serial = uint64(mcp::IntegerArgument(arguments, "request", 0, 1));
                std::vector<std::string> outgoing;
                {
                    std::lock_guard lock(sLock);
                    if (!connection->channel.Model().FindRequest(serial)) throw ToolError("no request " + std::to_string(serial));
                    connection->channel.Model().RequestBody(serial);
                    outgoing = connection->channel.TakeOutgoing();
                }
                Flush(*connection, std::move(outgoing));
                std::unique_lock lock(sLock);
                sChanged.wait_for(lock, std::chrono::seconds(5), [&] {
                    const auto* request = connection->channel.Model().FindRequest(serial);
                    return !request || request->bodyState == devtools::BodyState::Loaded || request->bodyState == devtools::BodyState::Failed;
                });
                const auto* request = connection->channel.Model().FindRequest(serial);
                if (!request) throw ToolError("the request is gone");
                std::string text = devtools::DescribeHeaders(*request).text;
                const size_t limit = size_t(mcp::IntegerArgument(arguments, "max_body", 20000, 0, 16 << 20));
                if (request->bodyState == devtools::BodyState::Loaded) text += "\nBody (" + std::to_string(request->body.size()) + " bytes):\n"
                    + mcp::Truncate(request->body, limit);
                else if (request->bodyState == devtools::BodyState::Failed) text += "\nBody unavailable: " + request->bodyError;
                return ToolResult::Text(text);
            }
            std::string pattern = mcp::StringArgument(arguments, "filter");
            if (!pattern.empty() && pattern.find('*') == std::string::npos) pattern = "*" + pattern + "*";
            const bool failedOnly = mcp::BooleanArgument(arguments, "failed", false);
            const size_t limit = size_t(mcp::IntegerArgument(arguments, "limit", 100, 1, 5000));
            Json list = Json::array();
            std::lock_guard lock(sLock);
            const auto& requests = connection->channel.Model().Requests();
            std::vector<const devtools::Request*> chosen;
            for (const auto& request : requests) {
                if (!pattern.empty() && !mcp::Matches(pattern, request.url)) continue;
                if (failedOnly && !request.IsError()) continue;
                chosen.push_back(&request);
            }
            const size_t first = chosen.size() > limit ? chosen.size() - limit : 0;
            double firstStart = requests.empty() ? 0 : requests.front().started;
            std::vector<std::pair<double, const devtools::Request*>> slowest;
            size_t failures = 0, pending = 0;
            for (size_t i = 0; i < chosen.size(); ++i) {
                const auto& request = *chosen[i];
                if (request.IsError()) ++failures;
                if (request.Pending()) ++pending;
                if (request.Duration() >= 0) slowest.emplace_back(request.Duration(), &request);
                if (i < first) continue;
                Json entry = {{"serial", request.serial}, {"method", request.method}, {"url", request.url.substr(0, 300)},
                    {"status", request.StatusLabel()}, {"type", request.type}, {"mime", request.mimeType}, {"bytes", request.size},
                    {"transferred", request.transferred}, {"startMs", std::round((request.started - firstStart) * 10000) / 10}};
                if (request.Duration() >= 0) entry["durationMs"] = std::round(request.Duration() * 10000) / 10;
                if (!request.protocol.empty()) entry["protocol"] = request.protocol;
                if (!request.source.empty() && request.source != "network") entry["source"] = request.source;
                if (!request.error.empty()) entry["error"] = request.error;
                if (!request.remoteAddress.empty()) entry["remote"] = request.remoteAddress;
                list.push_back(entry);
            }
            std::sort(slowest.begin(), slowest.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            Json slow = Json::array();
            for (size_t i = 0; i < slowest.size() && i < 5; ++i)
                slow.push_back({{"serial", slowest[i].second->serial}, {"url", slowest[i].second->url.substr(0, 200)},
                    {"durationMs", std::round(slowest[i].first * 10000) / 10}});
            return ToolResult::Data({{"tab", connection->id}, {"page", connection->channel.Model().PageURL()},
                {"summary", devtools::SummarizeRequests(requests, chosen.size())}, {"matching", chosen.size()},
                {"failed", failures}, {"pending", pending}, {"slowest", slow}, {"requests", list}});
        }});

    server.Add({"dom",
        "Elements of a tab's page matching a CSS selector: count, and for the first ones their outer HTML, text, "
        "attributes, box on the page (x, y, width, height), visibility and the computed style properties that "
        "matter for layout and compositing (display, position, overflow, transform, opacity, z-index, "
        "will-change, contain, filter).",
        Schema().String("selector", "CSS selector (default html)").String("tab", kTabArgument)
            .Enum("show", "html (default), text, or box (box and style only)", {"html", "text", "box"})
            .Integer("limit", "Most elements described (default 10)").Integer("max_chars", "Most characters of HTML or text each (default 4000)")
            .StringArray("styles", "Other computed style properties to include"),
        [](const Json& arguments) {
            auto connection = Connect(arguments);
            const std::string selector = mcp::StringArgument(arguments, "selector", "html");
            const std::string show = mcp::StringArgument(arguments, "show", "html");
            Json styles = {"display", "position", "overflow", "transform", "opacity", "zIndex", "willChange", "contain", "filter", "visibility"};
            if (mcp::HasArgument(arguments, "styles"))
                for (const auto& style : arguments["styles"]) if (style.is_string()) styles.push_back(style);
            const std::string script = "(() => { const sel = " + Quoted(selector) + "; const all = document.querySelectorAll(sel);"
                " const limit = " + std::to_string(mcp::IntegerArgument(arguments, "limit", 10, 1, 1000)) + ", max = "
                + std::to_string(mcp::IntegerArgument(arguments, "max_chars", 4000, 10, 1000000)) + ", show = " + Quoted(show)
                + ", styles = " + styles.dump() + ";"
                " return { count: all.length, elements: [...all].slice(0, limit).map((e) => { const r = e.getBoundingClientRect();"
                " const cs = getComputedStyle(e); const style = {}; for (const s of styles) style[s] = cs[s];"
                " const attributes = {}; for (const a of e.attributes || []) attributes[a.name] = a.value.slice(0, 300);"
                " const out = { tag: e.tagName.toLowerCase(), attributes, box: { x: Math.round(r.x), y: Math.round(r.y + scrollY),"
                " width: Math.round(r.width), height: Math.round(r.height) }, inViewport: r.bottom > 0 && r.top < innerHeight && r.width > 0,"
                " style, children: e.children.length };"
                " if (show === 'html') out.html = e.outerHTML.slice(0, max); else if (show === 'text') out.text = (e.innerText || e.textContent || '').slice(0, max);"
                " return out; }) }; })()";
            return ToolResult::Data(Script(*connection, script));
        }});

    server.Add({"page_metrics",
        "A tab's page in numbers: navigation timing (DNS, connect, TLS, first byte, DOM ready, load), first and "
        "largest contentful paint, resources by type with the slowest, DOM size (elements, iframes, images, "
        "scripts, canvases, videos, shadow roots), viewport and document size, device pixel ratio, and the "
        "animation frame rate the page gets (requestAnimationFrame callbacks per second over half a second).",
        Schema().String("tab", kTabArgument),
        [](const Json& arguments) {
            auto connection = Connect(arguments);
            return ToolResult::Data(Script(*connection, kMetricsScript, 20000000));
        }});

    server.Add({"page_media",
        "The page's video and audio elements as the page sees them: source (blob: means Media Source "
        "Extensions), ready and network state, paused, time and duration, buffered ranges, size, playback "
        "quality (decoded, dropped and corrupted frames, WebKit's decoded/dropped counters), errors. The engine's "
        "view (which decoder, hardware or not, frames presented) is in the media tool.",
        Schema().String("tab", kTabArgument),
        [](const Json& arguments) {
            auto connection = Connect(arguments);
            return ToolResult::Data({{"tab", connection->id}, {"elements", Script(*connection, kMediaScript)}});
        }});

    server.Add({"timeline",
        "Records a tab's main-thread work for some seconds (default 3) with the inspector's Timeline: time per "
        "kind (script evaluation, function calls, timers, animation frames, style recalculation, layout, paint, "
        "composite), how busy the main thread was, rendering frames (median, 95th percentile, longest, over 16 "
        "ms) and the longest tasks with what was inside them and where they came from. reload=true records the "
        "page's load; scroll=true sends a wheel burst while recording.",
        Schema().String("tab", kTabArgument).Number("seconds", "Recording time (0.2-60, default 3)")
            .Boolean("reload", "Reload the page when recording starts").Boolean("scroll", "Scroll the page while recording")
            .Integer("top", "Longest records listed (default 12)"),
        [](const Json& arguments) {
            auto connection = Connect(arguments);
            const double seconds = std::clamp(mcp::NumberArgument(arguments, "seconds", 3), 0.2, 60.0);
            {
                std::lock_guard lock(sLock);
                connection->channel.Collect("Timeline.", true);
                connection->channel.TakeEvents("Timeline.");
            }
            try {
                Command(*connection, "Timeline.enable", Json::object());
                Command(*connection, "Timeline.setInstruments", {{"instruments", {"Timeline"}}});
                Command(*connection, "Timeline.start", {{"maxCallStackDepth", 5}});
            } catch (...) {
                std::lock_guard lock(sLock);
                connection->channel.Collect("Timeline.", false);
                throw;
            }
            if (mcp::BooleanArgument(arguments, "reload", false)) Send(*connection, "Page.reload", {{"ignoreCache", false}});
            if (mcp::BooleanArgument(arguments, "scroll", false)) {
                try {
                    McpAsk(connection->window, {{"action", "burst"}, {"tab", connection->tab}, {"count", 60}, {"interval_ms", 16}}, 3000000, "The window");
                } catch (const ToolError&) {
                }
            }
            Pause(bigtime_t(seconds * 1e6));
            std::string stopError;
            try {
                Command(*connection, "Timeline.stop", Json::object());
            } catch (const ToolError& error) {
                stopError = error.what();
            }
            Pause(300000);
            std::vector<Json> records;
            {
                std::lock_guard lock(sLock);
                for (auto& event : connection->channel.TakeEvents("Timeline."))
                    if (event.value("method", "") == "Timeline.eventRecorded" && event.contains("params"))
                        records.push_back(event["params"].value("record", Json::object()));
                connection->channel.Collect("Timeline.", false);
            }
            Send(*connection, "Timeline.disable", Json::object());
            Json summary = mcp::SummarizeTimeline(records, size_t(mcp::IntegerArgument(arguments, "top", 12, 1, 200)));
            summary["tab"] = connection->id;
            summary["recordedSeconds"] = seconds;
            if (!stopError.empty()) summary["stopError"] = stopError;
            return ToolResult::Data(summary);
        }});

    server.Add({"resource_usage",
        "Records the inspected WebProcess's CPU and memory timelines (default 3 seconds), with timestamped "
        "samples and per-thread CPU usage. 100% CPU is one full core; the process can exceed 100%. Memory "
        "categories are allocation/resident estimates in bytes: JavaScript covers the page VM, JIT covers "
        "the process, and Other includes unattributed resident areas. The Page allocator category is "
        "unavailable (zero). Shared areas may be counted more than "
        "once; these are not exact private-memory totals. CPU-only recording avoids waiting on a busy page "
        "thread for memory counters. Other tabs in the same process contribute to these measurements.",
        Schema().String("tab", kTabArgument).Number("seconds", "Recording time (1-60, default 3)")
            .Boolean("cpu", "Collect CPU samples (default true)").Boolean("memory", "Collect memory samples (default true)"),
        [](const Json& arguments) {
            auto connection = Connect(arguments);
            const bool cpu = mcp::BooleanArgument(arguments, "cpu", true);
            const bool memory = mcp::BooleanArgument(arguments, "memory", true);
            if (!cpu && !memory) throw ToolError("Select CPU, memory, or both.");
            const double seconds = std::clamp(mcp::NumberArgument(arguments, "seconds", 3), 1.0, 60.0);
            int commits;
            {
                std::lock_guard lock(sLock);
                if (!sResourceRecordings.insert(connection->id).second)
                    throw ToolError("This tab already has a resource recording in progress.");
                commits = connection->channel.Commits();
                for (const char* prefix : {"CPUProfiler.", "Memory."}) {
                    connection->channel.TakeEvents(prefix);
                    connection->channel.Collect(prefix, true);
                }
            }
            struct CollectionGuard {
                PageConnection& connection;
                ~CollectionGuard()
                {
                    std::lock_guard lock(sLock);
                    sResourceRecordings.erase(connection.id);
                    for (const char* prefix : {"CPUProfiler.", "Memory."}) {
                        connection.channel.Collect(prefix, false);
                        connection.channel.TakeEvents(prefix);
                    }
                }
            } guard { *connection };
            bool cpuStarted = false, memoryEnabled = false, memoryStarted = false;
            Json stopErrors = Json::array();
            auto stop = [&] {
                auto command = [&](bool& active, const char* method) {
                    if (!active) return;
                    active = false;
                    try { Command(*connection, method, Json::object(), 5000000); }
                    catch (const ToolError& error) { stopErrors.push_back(error.what()); }
                };
                command(cpuStarted, "CPUProfiler.stopTracking");
                command(memoryStarted, "Memory.stopTracking");
                command(memoryEnabled, "Memory.disable");
            };
            try {
                if (cpu) {
                    cpuStarted = true;
                    Command(*connection, "CPUProfiler.startTracking", Json::object());
                }
                if (memory) {
                    Command(*connection, "Memory.enable", Json::object());
                    memoryEnabled = true;
                    memoryStarted = true;
                    Command(*connection, "Memory.startTracking", Json::object());
                }
                Pause(bigtime_t(seconds * 1e6));
                stop();
            } catch (...) {
                stop();
                throw;
            }
            Json result = {{"tab", connection->id}, {"recordedSeconds", seconds},
                {"scope", "WebProcess"}, {"cpu", Json::array()}, {"memory", Json::array()}};
            {
                std::lock_guard lock(sLock);
                result["processChanged"] = connection->channel.Commits() != commits;
                for (const auto& event : connection->channel.TakeEvents("CPUProfiler."))
                    if (event.value("method", "") == "CPUProfiler.trackingUpdate")
                        result["cpu"].push_back(event.at("params").at("event"));
                for (const auto& event : connection->channel.TakeEvents("Memory."))
                    if (event.value("method", "") == "Memory.trackingUpdate")
                        result["memory"].push_back(event.at("params").at("event"));
            }
            if (!stopErrors.empty()) result["stopErrors"] = std::move(stopErrors);
            return ToolResult::Data(result);
        }});

    server.Add({"layers",
        "A tab's compositing layers (the inspector's LayerTree): how many, their memory and paint counts, and "
        "for the largest the element, size and why they are composited. Too many or too large layers cost GPU "
        "memory and upload time; a layer that repaints every frame shows a high paint count.",
        Schema().String("tab", kTabArgument).Integer("top", "Layers described (default 12)"),
        [](const Json& arguments) {
            auto connection = Connect(arguments);
            const Json document = Command(*connection, "DOM.getDocument", Json::object());
            const int root = document.value("root", Json::object()).value("nodeId", 0);
            Command(*connection, "LayerTree.enable", Json::object());
            const Json layers = Command(*connection, "LayerTree.layersForNode", {{"nodeId", root}});
            const Json& list = layers.value("layers", Json::array());
            std::vector<const Json*> sorted;
            uint64 memory = 0, paints = 0;
            for (const auto& layer : list) {
                sorted.push_back(&layer);
                memory += layer.value("memory", uint64(0));
                paints += layer.value("paintCount", uint64(0));
            }
            std::sort(sorted.begin(), sorted.end(), [](const Json* a, const Json* b) { return a->value("memory", 0) > b->value("memory", 0); });
            Json largest = Json::array();
            const size_t top = size_t(mcp::IntegerArgument(arguments, "top", 12, 1, 500));
            for (size_t i = 0; i < sorted.size() && i < top; ++i) {
                const Json& layer = *sorted[i];
                Json entry = {{"layerId", layer.value("layerId", "")}, {"memoryBytes", layer.value("memory", 0)},
                    {"paintCount", layer.value("paintCount", 0)}, {"bounds", layer.value("bounds", Json::object())}};
                if (layer.contains("pseudoElement")) entry["pseudoElement"] = layer["pseudoElement"];
                if (layer.value("isGeneratedContent", false) || layer.value("isAnonymous", false)) entry["anonymous"] = true;
                if (const int node = layer.value("nodeId", 0)) {
                    entry["nodeId"] = node;
                    try {
                        const Json resolved = Command(*connection, "DOM.resolveNode", {{"nodeId", node}, {"objectGroup", "summit-mcp"}}, 3000000);
                        const Json name = Command(*connection, "Runtime.callFunctionOn", {{"objectId", resolved["object"]["objectId"]},
                            {"functionDeclaration", "function() { return this.nodeName ? this.nodeName.toLowerCase() + (this.id ? '#' + this.id : '')"
                                " + (typeof this.className === 'string' && this.className ? '.' + this.className.trim().split(/\\s+/).join('.') : '') : String(this); }"},
                            {"returnByValue", true}}, 3000000);
                        entry["element"] = name["result"].value("value", "");
                    } catch (const ToolError&) {
                    }
                }
                try {
                    const Json reasons = Command(*connection, "LayerTree.reasonsForCompositingLayer", {{"layerId", layer.value("layerId", "")}}, 3000000);
                    Json why = Json::array();
                    for (const auto& [reason, on] : reasons.value("compositingReasons", Json::object()).items())
                        if (on.is_boolean() && on.get<bool>()) why.push_back(reason);
                    entry["reasons"] = why;
                } catch (const ToolError&) {
                }
                largest.push_back(entry);
            }
            Send(*connection, "Runtime.releaseObjectGroup", {{"objectGroup", "summit-mcp"}});
            Send(*connection, "LayerTree.disable", Json::object());
            return ToolResult::Data({{"tab", connection->id}, {"layers", list.size()}, {"memoryMiB", std::round(memory / 10485.76) / 100},
                {"paintCountTotal", paints}, {"largest", largest}});
        }});

    server.Add({"heap",
        "A tab's JavaScript heap: action summary (default) takes a heap snapshot and lists objects and bytes "
        "per class, the largest first (finds what a leaking page keeps); gc runs a full garbage collection "
        "first (then compare snapshots).",
        Schema().String("tab", kTabArgument).Enum("action", "summary or gc", {"summary", "gc"}).Integer("top", "Classes listed (default 25)"),
        [](const Json& arguments) {
            auto connection = Connect(arguments);
            const std::string action = mcp::StringArgument(arguments, "action", "summary");
            Command(*connection, "Heap.enable", Json::object());
            if (action == "gc") {
                const bigtime_t started = system_time();
                Command(*connection, "Heap.gc", Json::object(), 60000000);
                return ToolResult::Data({{"tab", connection->id}, {"gcMs", (system_time() - started) / 1000}});
            }
            const Json snapshot = Command(*connection, "Heap.snapshot", Json::object(), 120000000);
            Json summary = mcp::SummarizeHeapSnapshot(snapshot.value("snapshotData", ""), size_t(mcp::IntegerArgument(arguments, "top", 25, 1, 1000)));
            summary["tab"] = connection->id;
            summary["snapshotBytes"] = snapshot.value("snapshotData", "").size();
            Send(*connection, "Heap.disable", Json::object());
            return ToolResult::Data(summary);
        }});

    server.Add({"page_render",
        "The tab's page as WebCore paints it (the inspector's Page.snapshotRect, without the compositor or the "
        "screen): the visible viewport, or full_page. Compare with screenshot target page: content wrong in both "
        "is layout or painting, right here but wrong on screen is compositing or presentation.",
        Schema().String("tab", kTabArgument).Boolean("full_page", "The whole document (capped at 8000 px tall)")
            .Integer("max_width", "Scale down to at most this width (default 1600, 0: full size)"),
        [](const Json& arguments) {
            auto connection = Connect(arguments);
            const bool full = mcp::BooleanArgument(arguments, "full_page", false);
            const Json size = Script(*connection, "({ width: innerWidth, height: innerHeight, documentHeight: document.documentElement.scrollHeight, "
                "documentWidth: document.documentElement.scrollWidth })");
            const int width = full ? size.value("documentWidth", 800) : size.value("width", 800);
            const int height = full ? std::min(8000, size.value("documentHeight", 600)) : size.value("height", 600);
            const Json snapshot = Command(*connection, "Page.snapshotRect", {{"x", 0}, {"y", 0}, {"width", width}, {"height", height},
                {"coordinateSystem", full ? "Page" : "Viewport"}}, 30000000);
            std::string url = snapshot.value("dataURL", "");
            const size_t comma = url.find(',');
            if (comma == std::string::npos) throw ToolError("the engine gave no picture");
            std::string data = url.substr(comma + 1);
            const int32 maxWidth = int32(mcp::IntegerArgument(arguments, "max_width", 1600, 0, 16384));
            const std::string caption = "Tab " + connection->id + " rendered by WebCore, " + std::to_string(width) + "×"
                + std::to_string(height) + " CSS px";
            if (maxWidth && width > maxWidth) {
                // Decode, scale, encode again.
                static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                std::string bytes;
                uint32 buffer = 0;
                int bits = 0;
                for (char c : data) {
                    const char* at = std::strchr(table, c);
                    if (!at || !c) continue;
                    buffer = buffer << 6 | uint32(at - table);
                    bits += 6;
                    if (bits >= 8) { bits -= 8; bytes += char(buffer >> bits & 0xff); }
                }
                BMemoryIO input(bytes.data(), bytes.size());
                BBitmap* decoded = BTranslationUtils::GetBitmap(&input);
                if (decoded) {
                    const BRect bounds = decoded->Bounds();
                    const float scale = maxWidth / (bounds.Width() + 1);
                    const BRect target(0, 0, maxWidth - 1, std::max(0.0f, std::round((bounds.Height() + 1) * scale) - 1));
                    auto* scaled = new BBitmap(target, B_BITMAP_ACCEPTS_VIEWS, B_RGB32);
                    auto* view = new BView(target, "scale", B_FOLLOW_NONE, 0);
                    scaled->AddChild(view);
                    if (scaled->Lock()) {
                        view->DrawBitmap(decoded, bounds, target, B_FILTER_BITMAP_BILINEAR);
                        view->Sync();
                        scaled->Unlock();
                    }
                    scaled->RemoveChild(view);
                    delete view;
                    delete decoded;
                    BBitmapStream stream(scaled);
                    BMallocIO output;
                    if (BTranslatorRoster::Default()->Translate(&stream, nullptr, nullptr, &output, B_PNG_FORMAT) == B_OK)
                        data = mcp::Base64(output.Buffer(), output.BufferLength());
                    BBitmap* detached = nullptr;
                    stream.DetachBitmap(&detached);
                    delete detached;
                }
            }
            return ToolResult::Image(data, "image/png", caption);
        }});

    server.Add({"inspector",
        "Sends any Web Inspector protocol command to a tab's page target and returns its result (domains: "
        "Runtime, Page, DOM, CSS, Network, Console, Debugger, DOMStorage, IndexedDB, LayerTree, Timeline, Heap, "
        "Canvas, Animation, Audit, Worker, Memory, CPUProfiler; see WebKit's Source/JavaScriptCore/inspector/protocol). "
        "Events are not returned; resource_usage records CPU/memory events and the other tools cover the common ones.",
        Schema().String("method", "Domain.command", true).Object("params", "The command's parameters").String("tab", kTabArgument)
            .Integer("timeout_ms", "How long to wait (default 15000)").Integer("max_bytes", "Most of the result returned (default 200000)"),
        [](const Json& arguments) {
            auto connection = Connect(arguments);
            const Json parameters = mcp::HasArgument(arguments, "params") && arguments["params"].is_object() ? arguments["params"] : Json::object();
            const Json result = Command(*connection, mcp::StringArgument(arguments, "method"), parameters,
                mcp::IntegerArgument(arguments, "timeout_ms", 15000, 100, 600000) * 1000);
            return ToolResult::Text(mcp::Truncate(result.dump(1, ' ', false, Json::error_handler_t::replace),
                size_t(mcp::IntegerArgument(arguments, "max_bytes", 200000, 100, 32 << 20))));
        }, false});

    server.Add({"page_detach",
        "Ends the inspector session of a tab (or all=true: every tab's), which the page tools keep from their "
        "first use: the page then stops sending console and network events.",
        Schema().String("tab", kTabArgument).Boolean("all", "Every tab's session"),
        [](const Json& arguments) {
            std::vector<std::shared_ptr<PageConnection>> ended;
            {
                std::lock_guard lock(sLock);
                if (mcp::BooleanArgument(arguments, "all", false)) {
                    for (auto& [id, connection] : sByTab) ended.push_back(connection);
                    sByTab.clear();
                    sByIdentifier.clear();
                }
            }
            if (!mcp::BooleanArgument(arguments, "all", false)) {
                McpTabRef ref = McpResolveTab(arguments);
                const std::string id = std::to_string(ref.windowKey) + "." + std::to_string(ConcreteTab(ref));
                std::lock_guard lock(sLock);
                if (auto found = sByTab.find(id); found != sByTab.end()) {
                    ended.push_back(found->second);
                    sByIdentifier.erase(found->second->identifier);
                    sByTab.erase(found);
                }
            }
            Json ids = Json::array();
            for (const auto& connection : ended) ids.push_back(connection->id);
            ended.clear();
            return ToolResult::Data({{"detached", ids}});
        }, false});
}
}
#endif
