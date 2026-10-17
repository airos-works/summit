#include "McpInspector.h"
#include <algorithm>
#include <cmath>

namespace summit::mcp {
namespace {
constexpr size_t kMaximumEvents = 200000;

std::string Text(const Json& object, const char* name)
{
    if (!object.is_object()) return { };
    auto found = object.find(name);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

double Round(double value, double scale = 10)
{
    return std::round(value * scale) / scale;
}
}

void InspectorChannel::Start()
{
    fModel.Start();
}

std::vector<std::string> InspectorChannel::TakeOutgoing()
{
    for (auto& message : fModel.TakeOutgoing()) fOutgoing.push_back(std::move(message));
    return std::exchange(fOutgoing, { });
}

void InspectorChannel::SendToPage(uint64_t id, const std::string& inner)
{
    fSent[id] = fPageTarget;
    const Json outer = {{"id", id}, {"method", "Target.sendMessageToTarget"},
        {"params", {{"targetId", fPageTarget}, {"message", inner}}}};
    fOutgoing.push_back(outer.dump());
}

uint64_t InspectorChannel::Command(const std::string& method, const Json& parameters)
{
    const uint64_t id = fNextCommand++;
    const std::string inner = Json{{"id", id}, {"method", method}, {"params", parameters}}.dump();
    if (fPageTarget.empty()) fWaiting.emplace_back(id, inner);
    else SendToPage(id, inner);
    return id;
}

void InspectorChannel::Fail(uint64_t id, const std::string& message)
{
    fAnswers[id] = {{"error", {{"message", message}}}};
}

std::optional<Json> InspectorChannel::TakeAnswer(uint64_t id)
{
    auto found = fAnswers.find(id);
    if (found == fAnswers.end()) return std::nullopt;
    Json answer = std::move(found->second);
    fAnswers.erase(found);
    return answer;
}

void InspectorChannel::Collect(const std::string& prefix, bool on)
{
    if (on) fCollecting.insert(prefix);
    else fCollecting.erase(prefix);
}

std::vector<Json> InspectorChannel::TakeEvents(const std::string& prefix)
{
    std::vector<Json> taken;
    std::deque<Json> kept;
    for (auto& event : fEvents) {
        if (Text(event, "method").rfind(prefix, 0) == 0) taken.push_back(std::move(event));
        else kept.push_back(std::move(event));
    }
    fEvents = std::move(kept);
    return taken;
}

void InspectorChannel::Receive(std::string_view text)
{
    const Json message = Json::parse(text, nullptr, false);
    if (!message.is_object()) return;
    // Answers to the tools' own outer commands carry nothing to keep.
    if (message.contains("id") && message["id"].is_number_unsigned() && message["id"].get<uint64_t>() >= kFirstCommand) {
        if (message.contains("error")) {
            const uint64_t id = message["id"];
            // The outer command failed: the target is gone.
            if (fSent.erase(id)) Fail(id, "the page target refused the command: " + message["error"].dump());
        }
        return;
    }
    const std::string method = Text(message, "method");
    const Json parameters = message.value("params", Json::object());
    if (method == "Target.dispatchMessageFromTarget") {
        const Json inner = Json::parse(Text(parameters, "message"), nullptr, false);
        if (inner.is_object() && inner.contains("id") && inner["id"].is_number_unsigned()
            && inner["id"].get<uint64_t>() >= kFirstCommand) {
            const uint64_t id = inner["id"];
            fSent.erase(id);
            if (inner.contains("error")) fAnswers[id] = {{"error", inner["error"]}};
            else fAnswers[id] = {{"result", inner.value("result", Json::object())}};
            return;
        }
        if (inner.is_object() && inner.contains("method") && !fCollecting.empty()) {
            const std::string event = Text(inner, "method");
            for (const auto& prefix : fCollecting)
                if (event.rfind(prefix, 0) == 0) {
                    fEvents.push_back(inner);
                    if (fEvents.size() > kMaximumEvents) fEvents.pop_front();
                    break;
                }
        }
    } else if (method == "Target.targetCreated") {
        const Json& info = parameters.value("targetInfo", Json::object());
        const std::string target = Text(info, "targetId");
        if (Text(info, "type") == "page" && !target.empty()) {
            if (info.value("isProvisional", false)) fProvisional.insert(target);
            else if (fPageTarget.empty()) fPageTarget = target;
        }
    } else if (method == "Target.didCommitProvisionalTarget") {
        const std::string previous = Text(parameters, "oldTargetId"), target = Text(parameters, "newTargetId");
        fProvisional.erase(target);
        if (previous == fPageTarget || fPageTarget.empty()) {
            fPageTarget = target;
            ++fCommits;
            for (auto sent = fSent.begin(); sent != fSent.end();) {
                if (sent->second == previous) {
                    Fail(sent->first, "the page moved to another process before answering");
                    sent = fSent.erase(sent);
                } else ++sent;
            }
        }
    } else if (method == "Target.targetDestroyed") {
        const std::string target = Text(parameters, "targetId");
        fProvisional.erase(target);
        if (target == fPageTarget) {
            fPageTarget.clear();
            for (auto sent = fSent.begin(); sent != fSent.end();) {
                if (sent->second == target) {
                    Fail(sent->first, "the page closed before answering");
                    sent = fSent.erase(sent);
                } else ++sent;
            }
        }
    }
    fModel.Receive(text);
    if (!fPageTarget.empty() && !fWaiting.empty())
        for (auto& [id, inner] : std::exchange(fWaiting, { })) SendToPage(id, inner);
}

Json SummarizeTimeline(const std::vector<Json>& records, size_t top)
{
    struct Totals {
        size_t count = 0;
        double total = 0, longest = 0;
    };
    std::map<std::string, Totals> byType;
    struct TopRecord {
        double duration;
        const Json* record;
    };
    std::vector<TopRecord> topLevel;
    double first = INFINITY, last = -INFINITY, busy = 0;
    std::vector<double> frames;
    // Time of each type includes the records nested in it.
    auto walk = [&](auto&& self, const Json& record) -> void {
        if (!record.is_object()) return;
        const std::string type = Text(record, "type");
        const double start = record.value("startTime", 0.0), end = record.value("endTime", start);
        auto& totals = byType[type.empty() ? "?" : type];
        ++totals.count;
        totals.total += std::max(0.0, end - start);
        totals.longest = std::max(totals.longest, end - start);
        if (type == "RenderingFrame") frames.push_back(end - start);
        for (const auto& child : record.value("children", Json::array())) self(self, child);
    };
    for (const auto& record : records) {
        if (!record.is_object()) continue;
        walk(walk, record);
        const double start = record.value("startTime", 0.0), end = record.value("endTime", start);
        first = std::min(first, start);
        last = std::max(last, end);
        busy += std::max(0.0, end - start);
        topLevel.push_back({end - start, &record});
    }
    Json types = Json::array();
    std::vector<std::pair<std::string, Totals>> sorted(byType.begin(), byType.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.total > b.second.total; });
    for (const auto& [type, totals] : sorted)
        types.push_back({{"type", type}, {"count", totals.count}, {"totalMs", Round(totals.total * 1000)},
            {"longestMs", Round(totals.longest * 1000)}});
    std::sort(topLevel.begin(), topLevel.end(), [](const TopRecord& a, const TopRecord& b) { return a.duration > b.duration; });
    Json longest = Json::array();
    for (size_t i = 0; i < topLevel.size() && i < top; ++i) {
        const Json& record = *topLevel[i].record;
        Json entry = {{"type", Text(record, "type")}, {"durationMs", Round(topLevel[i].duration * 1000)},
            {"atMs", Round((record.value("startTime", 0.0) - first) * 1000)}};
        if (record.contains("data") && !record["data"].empty()) entry["data"] = record["data"];
        if (record.contains("stackTrace")) {
            // The first frames say where the work came from.
            Json frames = Json::array();
            const Json& trace = record["stackTrace"];
            const Json& callFrames = trace.is_object() ? trace.value("callFrames", Json::array()) : trace;
            for (size_t f = 0; f < callFrames.size() && f < 4; ++f)
                frames.push_back(Text(callFrames[f], "functionName") + " " + Text(callFrames[f], "url") + ":"
                    + std::to_string(callFrames[f].value("lineNumber", 0)));
            if (!frames.empty()) entry["stack"] = frames;
        }
        // What was inside it, by type.
        std::map<std::string, double> inside;
        for (const auto& child : record.value("children", Json::array()))
            if (child.is_object())
                inside[Text(child, "type")] += child.value("endTime", 0.0) - child.value("startTime", 0.0);
        if (!inside.empty()) {
            Json parts = Json::object();
            for (const auto& [type, time] : inside) parts[type] = Round(time * 1000);
            entry["childrenMs"] = parts;
        }
        longest.push_back(entry);
    }
    Json summary = {{"records", records.size()}, {"byType", types}, {"longest", longest}};
    if (std::isfinite(first) && last > first) {
        summary["spanMs"] = Round((last - first) * 1000);
        summary["mainThreadBusyMs"] = Round(busy * 1000);
        summary["mainThreadBusyPercent"] = Round(100 * busy / (last - first));
    }
    if (!frames.empty()) {
        std::sort(frames.begin(), frames.end());
        summary["renderingFrames"] = {{"count", frames.size()}, {"medianMs", Round(frames[frames.size() / 2] * 1000)},
            {"p95Ms", Round(frames[std::min(frames.size() - 1, frames.size() * 95 / 100)] * 1000)},
            {"longestMs", Round(frames.back() * 1000)},
            {"over16Ms", std::count_if(frames.begin(), frames.end(), [](double frame) { return frame > 0.0167; })}};
    }
    return summary;
}

Json SummarizeHeapSnapshot(const std::string& text, size_t top)
{
    const Json snapshot = Json::parse(text, nullptr, false);
    if (!snapshot.is_object()) return {{"error", "the snapshot is not JSON"}};
    const Json& nodes = snapshot.value("nodes", Json::array());
    const Json& names = snapshot.value("nodeClassNames", Json::array());
    // Version 1 and 2 nodes: id, size, class name index, flags.
    const size_t fields = 4;
    if (!nodes.is_array() || nodes.size() % fields) return {{"error", "unknown snapshot layout"}};
    std::map<std::string, std::pair<uint64_t, uint64_t>> byClass;
    uint64_t total = 0;
    for (size_t i = 0; i + fields <= nodes.size(); i += fields) {
        const uint64_t size = nodes[i + 1].is_number() ? nodes[i + 1].get<uint64_t>() : 0;
        const size_t index = nodes[i + 2].is_number() ? nodes[i + 2].get<size_t>() : 0;
        const std::string name = index < names.size() && names[index].is_string() ? names[index].get<std::string>() : "?";
        auto& entry = byClass[name];
        ++entry.first;
        entry.second += size;
        total += size;
    }
    std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> sorted(byClass.begin(), byClass.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
    Json classes = Json::array();
    for (size_t i = 0; i < sorted.size() && i < top; ++i)
        classes.push_back({{"class", sorted[i].first}, {"objects", sorted[i].second.first}, {"bytes", sorted[i].second.second}});
    return {{"objects", nodes.size() / fields}, {"bytes", total}, {"classes", byClass.size()}, {"largest", classes}};
}
}
