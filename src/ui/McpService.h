#pragma once
#include "core/Mcp.h"
#include "core/McpHttp.h"
#include <Messenger.h>
#include <OS.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Summit's built-in MCP server (Preferences › Developer, off by default): the
// tools a debugging client such as Claude Code uses to look inside this
// browser on the machine that shows a problem. Tabs, pages (through the Web
// Inspector protocol), processes, threads, CPU, memory, rendering, media,
// extensions and the captured log. docs/mcp.md lists the tools.
//
// The tools run on the server's connection threads. What belongs to a window
// or to the application is asked of its looper with a timeout
// (McpAsk), so a hung WebKit main thread is reported, not waited on; what the
// operating system knows (teams, threads, areas) needs no looper at all.
namespace summit {

using mcp::Json;

struct McpSettings {
    bool enabled = false;
    uint16 port = 7790;
    // Only connections from this computer (127.0.0.1); otherwise every
    // network interface, protected by the token.
    bool localOnly = false;
    std::string token;
    bool operator==(const McpSettings&) const = default;
};

// A request to a looper: kMcpRequest with "request" (JSON text), answered
// with "reply" (JSON text) or "error".
constexpr uint32 kMcpRequest = 'mcpq';
// To the Preferences window: "status", "endpoint", "token", "running".
constexpr uint32 kMcpStatus = 'mcps';

// Asks a looper (the application, a browser window) and waits at most
// timeout. Throws mcp::ToolError naming the looper when it does not answer.
Json McpAsk(const BMessenger& target, const Json& request, bigtime_t timeout, const char* who);
// The reply a looper sends for kMcpRequest.
void McpReply(BMessage* request, const Json& reply);
void McpReplyError(BMessage* request, const std::string& error);

// Open browser windows, registered by BrowserWindow itself (any thread).
struct McpWindowEntry {
    BMessenger messenger;
    uint64 key = 0;
    bool privateBrowsing = false;
};
std::vector<McpWindowEntry> McpWindows();
void McpRegisterWindow(const McpWindowEntry&);
void McpUnregisterWindow(uint64 key);

// The stderr of Summit and of the engine processes it starts, kept in memory
// for the "logs" tool (McpLog.cpp). Started with the server; processes that
// were already running keep writing where they did.
namespace mcplog {
void Start();
bool Capturing();
bigtime_t Since();
struct Line {
    uint64 sequence = 0;
    bigtime_t when = 0;
    std::string text;
};
// Lines after sequence `after` matching the glob (all when empty), the last
// `limit` of them.
std::vector<Line> Read(uint64 after, const std::string& pattern, size_t limit, uint64& last);
// Appends a line of Summit's own (the server's notes).
void Note(const std::string& text);
}

// What the tools are given.
struct McpContext {
    BMessenger app;
    std::filesystem::path profile;
    bigtime_t startedAt = 0;
};

void RegisterMcpBrowserTools(mcp::Server&, const std::shared_ptr<McpContext>&);
void RegisterMcpSystemTools(mcp::Server&, const std::shared_ptr<McpContext>&);
void RegisterMcpPageTools(mcp::Server&, const std::shared_ptr<McpContext>&);
void RegisterMcpEngineTools(mcp::Server&, const std::shared_ptr<McpContext>&);

// Helpers shared by the tool files.
// A tab named by the client ("2.5": window key 2, tab 5; "5" in the front
// window; empty: the front window's selected tab), as its window resolves it.
struct McpTabRef {
    BMessenger window;
    uint64 windowKey = 0;
    int64 tab = 0;
    std::string id;
};
McpTabRef McpResolveTab(const Json& arguments);
// The team ids of this Summit and of every engine process it started.
std::vector<team_id> McpSummitTeams();
std::string McpTeamRole(team_id);

class McpService {
public:
    McpService(BMessenger app, std::filesystem::path profile);
    ~McpService();
    // Starts, restarts or stops the server for these settings. The
    // environment overrides them: SUMMIT_MCP=1 turns the server on (=0 off),
    // SUMMIT_MCP_PORT, SUMMIT_MCP_TOKEN and SUMMIT_MCP_LOCAL=1 replace the
    // saved values, for test copies started by a script.
    void Apply(const McpSettings& settings);
    // The settings in effect after the environment's overrides.
    McpSettings Effective() const;
    bool IsRunning() const;
    // "Listening on …", "Off" or what went wrong; for Preferences.
    std::string Status() const;
    // The URL a client connects to: this machine's network address (or
    // 127.0.0.1), the port and /mcp.
    std::string Endpoint() const;
    // A client configuration (mcpServers JSON) to paste.
    std::string ClientConfiguration() const;
    static std::string NewToken();
    std::shared_ptr<mcp::Server> Server() const { return fServer; }

private:
    std::shared_ptr<McpContext> fContext;
    std::shared_ptr<mcp::Server> fServer;
    std::unique_ptr<mcp::HttpServer> fHttp;
    McpSettings fEffective;
    std::string fStatus = "Off";
    mutable std::mutex fLock;
};
}
