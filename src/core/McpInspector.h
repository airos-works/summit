#pragma once
#include "DevTools.h"
#include "Mcp.h"
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// One page's Web Inspector connection as the MCP tools use it. Console and
// network are collected by the developer tools' own model (devtools::Session),
// so they are kept from the moment of connection; the tools' commands
// (Runtime.evaluate, Timeline, LayerTree, Heap…) go to the page target with
// identifiers of their own and their answers are kept until taken. A page
// that moves to another process (a provisional target committed) gets the
// commands that were waiting; the ones already sent to the old target fail.
// Not thread-safe: the caller locks (McpPageTools.cpp).
namespace summit::mcp {

class InspectorChannel {
public:
    // Commands of the tools count from here, out of the developer tools' range.
    static constexpr uint64_t kFirstCommand = 1000000000;

    void Start();
    // Protocol messages for the engine, in order.
    std::vector<std::string> TakeOutgoing();
    // One protocol message from the engine.
    void Receive(std::string_view message);

    // Queues a command for the page; returns its identifier. Without a page
    // target yet it waits for one.
    uint64_t Command(const std::string& method, const Json& parameters = Json::object());
    // {"result": …} or {"error": {"message": …}} once answered; taken once.
    std::optional<Json> TakeAnswer(uint64_t id);
    bool Answered(uint64_t id) const { return fAnswers.count(id) > 0; }

    // Events of the domains asked for ("Timeline.", "Heap.", "LayerTree."),
    // kept until taken (at most 200000).
    void Collect(const std::string& prefix, bool on);
    std::vector<Json> TakeEvents(const std::string& prefix);

    const std::string& PageTarget() const { return fPageTarget; }
    // How many times the page changed process since the connection.
    int Commits() const { return fCommits; }
    devtools::Session& Model() { return fModel; }

private:
    void SendToPage(uint64_t id, const std::string& inner);
    void Fail(uint64_t id, const std::string& message);
    devtools::Session fModel;
    std::vector<std::string> fOutgoing;
    std::string fPageTarget;
    std::set<std::string> fProvisional;
    // Commands waiting for a page target, and commands sent, by target.
    std::vector<std::pair<uint64_t, std::string>> fWaiting;
    std::map<uint64_t, std::string> fSent;
    std::map<uint64_t, Json> fAnswers;
    std::set<std::string> fCollecting;
    std::deque<Json> fEvents;
    uint64_t fNextCommand = kFirstCommand;
    int fCommits = 0;
};

// Summaries of what the protocol gives, shared by the tools and the tests.
// Timeline records (Timeline.eventRecorded "record" objects): time per record
// type, the longest top-level records, rendering frames.
Json SummarizeTimeline(const std::vector<Json>& records, size_t top);
// A JavaScriptCore heap snapshot (Heap.snapshot's snapshotData): objects and
// bytes per class, the largest first.
Json SummarizeHeapSnapshot(const std::string& snapshot, size_t top);
}
