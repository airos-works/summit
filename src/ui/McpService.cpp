#if SUMMIT_MODERN_WEBKIT
#include "McpService.h"
#include "core/BuildTarget.h"
#include <Message.h>
#include <NetworkAddress.h>
#include <NetworkInterface.h>
#include <NetworkRoster.h>
#include <image.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace summit {
namespace {
std::mutex sWindowsLock;
std::vector<McpWindowEntry> sWindows;

const char* kInstructions =
    "Summit's own debugging server: tools that look inside this running browser (the native WebKit browser of "
    "air/OS and Haiku) on the machine that shows a problem.\n"
    "- Start with summit_info (versions, processes, GPU, settings) and tabs (every tab with its id, address, "
    "web process and load state). Tabs are named \"WINDOW.TAB\" (\"2.5\"); omit tab for the front window's "
    "selected tab.\n"
    "- Pages: evaluate (JavaScript), console, network, dom, page_metrics, timeline (layout/style/paint/script "
    "time), layers, heap, inspector (any Web Inspector protocol command). Console and network collect from the "
    "first call; pass reload=true to see a page's load.\n"
    "- Processes: processes (CPU and memory of Summit and its web/network processes), threads (states, what "
    "they wait on, CPU per thread), memory (areas by kind), cpu (whole system), stacks (Debugger report of a "
    "team), profile (sampling profiler: where a process spends CPU).\n"
    "- Engine: engine_report (a process's pages, JS heap, caches, media players and decoders, compositor and GL "
    "renderer, network transfers; answered even when its main thread hangs), media, graphics, frame_stats.\n"
    "- logs: stderr of Summit and its engine processes (traces, warnings); environment / set_environment set "
    "SUMMIT_* trace switches for processes started afterwards; restart_process starts a tab's web process again.\n"
    "- Input and views: screenshot (screen, window or the page's own frame), scroll, click, type_text, key, "
    "navigate, open_tab, close_tab, reload.\n"
    "A looper that does not answer within its timeout is reported as busy or hung: that is itself a finding "
    "(check threads and stacks of that team).";

std::string ReadToken()
{
    unsigned char bytes[32] = { };
    const int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t got = 0;
        while (got < sizeof(bytes)) {
            const ssize_t count = read(fd, bytes + got, sizeof(bytes) - got);
            if (count <= 0) break;
            got += size_t(count);
        }
        close(fd);
    }
    std::string token;
    char hex[3];
    for (unsigned char byte : bytes) {
        std::snprintf(hex, sizeof(hex), "%02x", byte);
        token += hex;
    }
    return token;
}

// The addresses of the interfaces that are up, not the loopback (wired
// and wireless both, on a laptop).
std::vector<std::string> NetworkAddresses()
{
    std::vector<std::string> addresses;
    BNetworkRoster& roster = BNetworkRoster::Default();
    BNetworkInterface interface;
    uint32 cookie = 0;
    while (roster.GetNextInterface(&cookie, interface) == B_OK) {
        if ((interface.Flags() & IFF_LOOPBACK) || !(interface.Flags() & IFF_UP)) continue;
        for (int32 i = 0; i < interface.CountAddresses(); ++i) {
            BNetworkInterfaceAddress address;
            if (interface.GetAddressAt(i, address) != B_OK || address.Address().Family() != AF_INET) continue;
            const BString text = address.Address().ToString(false);
            if (text.Length() && text != "0.0.0.0") addresses.push_back(text.String());
        }
    }
    if (addresses.empty()) addresses.push_back("127.0.0.1");
    return addresses;
}

std::string NetworkAddress()
{
    return NetworkAddresses().front();
}
}

Json McpAsk(const BMessenger& target, const Json& request, bigtime_t timeout, const char* who)
{
    if (!target.IsValid()) throw mcp::ToolError(std::string(who) + " is not there any more");
    BMessage message(kMcpRequest);
    message.AddString("request", request.dump().c_str());
    BMessage reply;
    const bigtime_t started = system_time();
    const status_t status = target.SendMessage(&message, &reply, timeout, timeout);
    if (status == B_TIMED_OUT || status == B_WOULD_BLOCK)
        throw mcp::ToolError(std::string(who) + " did not answer within " + std::to_string(timeout / 1000)
            + " ms: its thread is busy or hung (see the threads and stacks tools)");
    if (status != B_OK) throw mcp::ToolError(std::string(who) + " could not be asked: " + std::strerror(status));
    if (const char* error = nullptr; reply.FindString("error", &error) == B_OK) throw mcp::ToolError(error);
    const char* text = nullptr;
    if (reply.FindString("reply", &text) != B_OK) {
        if (reply.what == B_NO_REPLY) throw mcp::ToolError(std::string(who) + " dropped the request");
        throw mcp::ToolError(std::string(who) + " gave no answer (an older Summit?)");
    }
    Json answer = Json::parse(text, nullptr, false);
    if (answer.is_discarded()) throw mcp::ToolError(std::string(who) + " answered something that is not JSON");
    if (answer.is_object() && !answer.contains("answeredInMs"))
        answer["answeredInMs"] = double(system_time() - started) / 1000.0;
    return answer;
}

void McpReply(BMessage* request, const Json& reply)
{
    BMessage answer(B_REPLY);
    answer.AddString("reply", reply.dump(-1, ' ', false, Json::error_handler_t::replace).c_str());
    request->SendReply(&answer);
}

void McpReplyError(BMessage* request, const std::string& error)
{
    BMessage answer(B_REPLY);
    answer.AddString("error", error.c_str());
    request->SendReply(&answer);
}

std::vector<McpWindowEntry> McpWindows()
{
    std::lock_guard lock(sWindowsLock);
    return sWindows;
}

void McpRegisterWindow(const McpWindowEntry& entry)
{
    std::lock_guard lock(sWindowsLock);
    sWindows.push_back(entry);
}

void McpUnregisterWindow(uint64 key)
{
    std::lock_guard lock(sWindowsLock);
    std::erase_if(sWindows, [&](const McpWindowEntry& entry) { return entry.key == key; });
}

McpTabRef McpResolveTab(const Json& arguments)
{
    const std::string id = mcp::StringArgument(arguments, "tab");
    const auto windows = McpWindows();
    if (windows.empty()) throw mcp::ToolError("Summit has no browser window open");
    McpTabRef ref;
    uint64 windowKey = 0;
    int64 tab = 0;
    if (!id.empty()) {
        const size_t dot = id.find('.');
        try {
            if (dot == std::string::npos) tab = std::stoll(id);
            else {
                windowKey = std::stoull(id.substr(0, dot));
                tab = std::stoll(id.substr(dot + 1));
            }
        } catch (const std::exception&) {
            throw mcp::ToolError("A tab is named \"WINDOW.TAB\" as the tabs tool lists it, not \"" + id + "\"");
        }
    }
    // Without a window: the front window (the application's most recently
    // active one, which the application knows best).
    const McpWindowEntry* window = nullptr;
    if (windowKey) {
        for (const auto& entry : windows) if (entry.key == windowKey) window = &entry;
        if (!window) throw mcp::ToolError("There is no window " + std::to_string(windowKey) + " (see the tabs tool)");
    }
    std::vector<const McpWindowEntry*> candidates;
    if (window) candidates.push_back(window);
    else {
        // The most recently activated window answers "front"; ask each.
        bigtime_t newest = -1;
        for (const auto& entry : windows) {
            Json state;
            try {
                state = McpAsk(entry.messenger, {{"action", "summary"}}, 2000000, "A browser window");
            } catch (const mcp::ToolError&) {
                continue;
            }
            const bigtime_t activated = state.value("activatedAt", int64(0));
            if (tab) {
                for (const auto& item : state.value("tabIds", Json::array()))
                    if (item == tab) candidates.push_back(&entry);
            } else if (activated > newest) {
                newest = activated;
                candidates.assign(1, &entry);
            }
        }
        if (candidates.empty())
            throw mcp::ToolError(tab ? "No window has tab " + std::to_string(tab) + " (see the tabs tool)"
                : std::string("No browser window answered"));
        if (candidates.size() > 1)
            throw mcp::ToolError("Several windows have a tab " + std::to_string(tab) + "; name it as WINDOW.TAB");
    }
    ref.window = candidates.front()->messenger;
    ref.windowKey = candidates.front()->key;
    ref.tab = tab;
    ref.id = std::to_string(ref.windowKey) + "." + std::to_string(tab);
    return ref;
}

std::vector<team_id> McpSummitTeams()
{
    // This team and its descendants (the engine's web, network and
    // extension processes are started by it).
    std::vector<team_id> teams { getpid() };
    std::vector<team_info> all;
    int32 cookie = 0;
    team_info info;
    while (get_next_team_info(&cookie, &info) == B_OK) all.push_back(info);
    for (size_t i = 0; i < teams.size(); ++i)
        for (const auto& candidate : all)
            if (candidate.parent == teams[i] && std::find(teams.begin(), teams.end(), candidate.team) == teams.end())
                teams.push_back(candidate.team);
    return teams;
}

std::string McpTeamRole(team_id team)
{
    if (team == getpid()) return "browser";
    int32 cookie = 0;
    image_info image;
    while (get_next_image_info(team, &cookie, &image) == B_OK) {
        if (image.type != B_APP_IMAGE) continue;
        const char* slash = std::strrchr(image.name, '/');
        const std::string name = slash ? slash + 1 : image.name;
        if (name == "WebProcess") return "web";
        if (name == "NetworkProcess") return "network";
        if (name == "GPUProcess") return "gpu";
        return name;
    }
    return "unknown";
}

McpService::McpService(BMessenger app, std::filesystem::path profile)
    : fContext(std::make_shared<McpContext>())
{
    fContext->app = app;
    fContext->profile = std::move(profile);
    fContext->startedAt = system_time();
    fServer = std::make_shared<mcp::Server>("Summit", "0.1 (" SUMMIT_TARGET_NAME ")", kInstructions);
    RegisterMcpBrowserTools(*fServer, fContext);
    RegisterMcpSystemTools(*fServer, fContext);
    RegisterMcpPageTools(*fServer, fContext);
    RegisterMcpEngineTools(*fServer, fContext);
}

McpService::~McpService()
{
    if (fHttp) fHttp->Stop();
}

std::string McpService::NewToken()
{
    return ReadToken();
}

void McpService::Apply(const McpSettings& requested)
{
    McpSettings settings = requested;
    if (const char* value = std::getenv("SUMMIT_MCP"); value && *value) settings.enabled = !std::strcmp(value, "1");
    if (const char* value = std::getenv("SUMMIT_MCP_PORT"); value && *value) {
        const long port = std::strtol(value, nullptr, 10);
        if (port > 0 && port < 65536) settings.port = uint16(port);
    }
    if (const char* value = std::getenv("SUMMIT_MCP_TOKEN"); value && *value) settings.token = value;
    if (const char* value = std::getenv("SUMMIT_MCP_LOCAL"); value && *value) settings.localOnly = !std::strcmp(value, "1");
    std::lock_guard lock(fLock);
    if (fHttp && fHttp->IsRunning() && settings == fEffective) return;
    if (fHttp) {
        fHttp->Stop();
        fHttp.reset();
        mcplog::Note("Summit MCP: server stopped");
    }
    fEffective = settings;
    if (!settings.enabled) {
        fStatus = "Off";
        return;
    }
    if (settings.token.empty()) {
        fStatus = "No access token";
        return;
    }
    // The log is kept from the moment the server is first turned on, and
    // extension pages' console messages go to it (read when an extension's
    // background page or popup is made; SUMMIT_TRACE_EXTENSION_CONSOLE=0 keeps
    // them out).
    mcplog::Start();
    setenv("SUMMIT_TRACE_EXTENSION_CONSOLE", "1", 0);
    mcp::HttpOptions options;
    options.address = settings.localOnly ? "127.0.0.1" : "0.0.0.0";
    options.port = settings.port;
    options.token = settings.token;
    auto http = std::make_unique<mcp::HttpServer>(fServer, options);
    const std::string error = http->Start();
    if (!error.empty()) {
        fStatus = "Could not start: " + error;
        mcplog::Note("Summit MCP: " + fStatus);
        std::fprintf(stderr, "Summit MCP: could not start: %s\n", error.c_str());
        return;
    }
    fHttp = std::move(http);
    std::string addresses;
    for (const auto& address : settings.localOnly ? std::vector<std::string> { "127.0.0.1" } : NetworkAddresses())
        addresses += (addresses.empty() ? "" : ", ") + address;
    fStatus = "Listening on " + addresses + " port " + std::to_string(settings.port) + " ("
        + std::to_string(fServer->ToolCount()) + " tools)";
    mcplog::Note("Summit MCP: " + fStatus);
    std::fprintf(stderr, "Summit MCP: %s\n", fStatus.c_str());
}

McpSettings McpService::Effective() const
{
    std::lock_guard lock(fLock);
    return fEffective;
}

bool McpService::IsRunning() const
{
    std::lock_guard lock(fLock);
    return fHttp && fHttp->IsRunning();
}

std::string McpService::Status() const
{
    std::lock_guard lock(fLock);
    if (fHttp && fHttp->IsRunning() && fHttp->RequestCount())
        return fStatus + ", " + std::to_string(fHttp->RequestCount()) + " requests, last from " + fHttp->LastPeer();
    return fStatus;
}

std::string McpService::Endpoint() const
{
    std::lock_guard lock(fLock);
    return "http://" + std::string(fEffective.localOnly ? "127.0.0.1" : NetworkAddress()) + ":"
        + std::to_string(fEffective.port) + "/mcp";
}

std::string McpService::ClientConfiguration() const
{
    const std::string endpoint = Endpoint();
    const std::string token = Effective().token;
    const Json configuration = {{"mcpServers", {{"summit", {{"type", "http"}, {"url", endpoint},
        {"headers", {{"Authorization", "Bearer " + token}}}}}}}};
    return configuration.dump(2) + "\n\nclaude mcp add --transport http summit " + endpoint
        + " --header \"Authorization: Bearer " + token + "\"\n";
}
}
#endif
