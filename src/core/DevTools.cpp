#include "DevTools.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace summit::devtools {
using nlohmann::json;
namespace {
// Frames of a stack that are shown, and characters of one console value.
constexpr size_t kStackFrames = 50, kValueLength = 10000;

// The protocol is read without trusting its shape: a member that is missing
// or of another type reads as nothing.
const json& Member(const json& object, const char* key)
{
    static const json nothing;
    if (!object.is_object()) return nothing;
    const auto found = object.find(key);
    return found == object.end() ? nothing : *found;
}
std::string Text(const json& value) { return value.is_string() ? value.get<std::string>() : std::string(); }
double Number(const json& value, double otherwise = 0) { return value.is_number() ? value.get<double>() : otherwise; }
bool Flag(const json& value) { return value.is_boolean() && value.get<bool>(); }
uint64_t Count(const json& value)
{
    const double number = Number(value);
    return number > 0 && number < 1e18 ? static_cast<uint64_t>(number) : 0;
}
std::string Dump(const json& value) { return value.dump(-1, ' ', false, json::error_handler_t::replace); }

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }
bool LessNoCase(const std::string& a, const std::string& b)
{
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
        [](char x, char y) { return Lower(x) < Lower(y); });
}

std::vector<Header> Headers(const json& object)
{
    std::vector<Header> result;
    if (!object.is_object()) return result;
    for (auto item = object.begin(); item != object.end(); ++item)
        result.push_back({item.key(), item.value().is_string() ? item.value().get<std::string>() : Dump(item.value())});
    std::stable_sort(result.begin(), result.end(), [](const Header& a, const Header& b) { return LessNoCase(a.name, b.name); });
    return result;
}

// The file of an address: its last path segment, else its host.
std::string ShortURL(const std::string& url)
{
    auto text = std::string_view(url);
    text = text.substr(0, text.find_first_of("?#"));
    while (text.size() > 1 && text.back() == '/') text.remove_suffix(1);
    const auto slash = text.rfind('/');
    if (slash == std::string_view::npos || slash + 1 >= text.size()) return std::string(text);
    return std::string(text.substr(slash + 1));
}

std::string Location(const std::string& url, int line, int column)
{
    if (url.empty()) return { };
    auto result = ShortURL(url);
    if (line > 0) {
        result += ':' + std::to_string(line);
        if (column > 0) result += ':' + std::to_string(column);
    }
    return result;
}

// A frame of the engine's own code has no address, or this one.
bool IsNative(const std::string& url) { return url.empty() || url == "[native code]"; }

std::string Quoted(const std::string& text) { return Dump(json(text)); }

std::string Shortened(std::string text, size_t limit)
{
    if (text.size() <= limit) return text;
    // Not in the middle of a character.
    while (limit > 0 && (static_cast<unsigned char>(text[limit]) & 0xC0) == 0x80) --limit;
    text.resize(limit);
    return text + "…";
}

std::string FirstLine(const std::string& text)
{
    const auto end = text.find('\n');
    return end == std::string::npos ? text : text.substr(0, end) + "…";
}

std::string RenderPreview(const json& preview, int depth);

// A property of a preview, whose value the protocol gives as text.
std::string RenderProperty(const json& property, int depth)
{
    if (const auto& nested = Member(property, "valuePreview"); nested.is_object() && depth < 3)
        return RenderPreview(nested, depth + 1);
    const auto type = Text(Member(property, "type"));
    const auto value = Text(Member(property, "value"));
    if (type == "string") return Quoted(value);
    if (type == "function") return "function";
    if (type == "accessor") return "(…)";
    if (type == "undefined") return "undefined";
    if (type == "object" && Text(Member(property, "subtype")) == "null") return "null";
    return value.empty() ? type : value;
}

std::string RenderPreview(const json& preview, int depth)
{
    const auto type = Text(Member(preview, "type"));
    const auto subtype = Text(Member(preview, "subtype"));
    const auto description = Text(Member(preview, "description"));
    if (type != "object" || subtype == "null" || subtype == "node" || subtype == "regexp" || subtype == "date"
        || subtype == "error")
        return type == "string" ? Quoted(description) : description.empty() ? (subtype == "null" ? "null" : type) : description;
    const bool array = subtype == "array";
    const bool overflow = Flag(Member(preview, "overflow"));
    std::string result;
    if (array) {
        if (overflow && Member(preview, "size").is_number()) result = "(" + std::to_string(Count(Member(preview, "size"))) + ") ";
    } else if (!description.empty() && description != "Object") result = description + " ";
    result += array ? "[" : "{";
    bool first = true;
    const auto separate = [&] {
        if (!first) result += ", ";
        first = false;
    };
    if (const auto& entries = Member(preview, "entries"); entries.is_array()) {
        for (const auto& entry : entries) {
            separate();
            if (const auto& key = Member(entry, "key"); key.is_object()) result += RenderPreview(key, depth + 1) + " => ";
            result += RenderPreview(Member(entry, "value"), depth + 1);
        }
    }
    if (const auto& properties = Member(preview, "properties"); properties.is_array()) {
        for (const auto& property : properties) {
            separate();
            if (!array) result += Text(Member(property, "name")) + ": ";
            result += RenderProperty(property, depth);
        }
    }
    if (overflow) {
        separate();
        result += "…";
    }
    result += array ? "]" : "}";
    return result;
}

// A value as the console shows it. A string that is an argument of its own
// is shown bare; within anything else it is quoted.
std::string Render(const json& object, bool bare)
{
    const auto type = Text(Member(object, "type"));
    const auto subtype = Text(Member(object, "subtype"));
    const auto& value = Member(object, "value");
    auto description = Text(Member(object, "description"));
    if (type == "string") {
        const auto text = value.is_string() ? value.get<std::string>() : description;
        return bare ? text : Quoted(text);
    }
    if (type == "undefined") return "undefined";
    if (type == "object" && subtype == "null") return "null";
    if (type == "function") return Shortened(FirstLine(description.empty() ? "function" : description), 200);
    if (type == "object" && subtype != "error" && subtype != "node" && subtype != "regexp" && subtype != "date") {
        if (const auto& preview = Member(object, "preview"); preview.is_object()) return RenderPreview(preview, 0);
    }
    if (description.empty() && !value.is_null()) description = Dump(value);
    if (description.empty()) description = Text(Member(object, "className"));
    return description.empty() ? type : description;
}

// console.log("%s is %d", name, age): the first argument's substitutions,
// then the arguments it did not use.
std::string RenderArguments(const json& parameters)
{
    std::string result;
    size_t next = 0;
    const size_t count = parameters.size();
    if (count && Text(Member(parameters[0], "type")) == "string") {
        const auto format = Render(parameters[0], true);
        next = 1;
        for (size_t i = 0; i < format.size(); ++i) {
            const char c = format[i];
            if (c != '%' || i + 1 >= format.size()) { result += c; continue; }
            const char kind = format[i + 1];
            if (kind == '%') { result += '%'; ++i; continue; }
            if (std::string_view("sdifoOc").find(kind) == std::string_view::npos || next >= count) { result += c; continue; }
            const auto& argument = parameters[next++];
            ++i;
            if (kind == 'c') continue;
            if (kind == 'd' || kind == 'i') {
                const auto& value = Member(argument, "value");
                if (value.is_number()) {
                    const double number = std::trunc(value.get<double>());
                    char buffer[32];
                    std::snprintf(buffer, sizeof buffer, "%.0f", number);
                    result += std::isfinite(number) ? buffer : "NaN";
                } else result += "NaN";
                continue;
            }
            result += Render(argument, kind == 's');
        }
    }
    for (; next < count; ++next) {
        if (!result.empty() || next) result += ' ';
        result += Render(parameters[next], true);
    }
    return result;
}

std::string MessageText(const json& message)
{
    const auto type = Text(Member(message, "type"));
    const auto& parameters = Member(message, "parameters");
    auto text = parameters.is_array() && !parameters.empty() && type != "image"
        ? RenderArguments(parameters) : Text(Member(message, "text"));
    if (type == "assert") text = text.empty() ? "Assertion failed" : "Assertion failed: " + text;
    if (type == "trace" && text.empty()) text = "console.trace()";
    return Shortened(std::move(text), kValueLength);
}

std::string StackText(const json& stack)
{
    std::string result;
    size_t frames = 0;
    for (const json* trace = &stack; trace->is_object() && frames < kStackFrames; trace = &Member(*trace, "parentStackTrace")) {
        const auto& callFrames = Member(*trace, "callFrames");
        if (!callFrames.is_array()) break;
        if (trace != &stack && !callFrames.empty()) result += "(asynchronous)\n";
        for (const auto& frame : callFrames) {
            if (++frames > kStackFrames) break;
            auto name = Text(Member(frame, "functionName"));
            if (name.empty()) name = "(anonymous)";
            const auto url = Text(Member(frame, "url"));
            const auto where = IsNative(url) ? std::string("native code")
                : Location(url, static_cast<int>(Number(Member(frame, "lineNumber"))), static_cast<int>(Number(Member(frame, "columnNumber"))));
            result += name + " (" + where + ")\n";
        }
    }
    if (!result.empty()) result.pop_back();
    return result;
}

Level LevelOf(const std::string& level)
{
    if (level == "error") return Level::Error;
    if (level == "warning") return Level::Warning;
    if (level == "info") return Level::Info;
    if (level == "debug") return Level::Debug;
    return Level::Log;
}

std::string ClockTime(double secondsSinceEpoch)
{
    if (secondsSinceEpoch <= 0) return { };
    const auto whole = static_cast<std::time_t>(secondsSinceEpoch);
    std::tm local { };
    localtime_r(&whole, &local);
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec,
        static_cast<int>((secondsSinceEpoch - static_cast<double>(whole)) * 1000) % 1000);
    return buffer;
}

void ReadResponse(Request& request, const json& response)
{
    request.hasResponse = true;
    request.status = static_cast<int>(Number(Member(response, "status")));
    request.statusText = Text(Member(response, "statusText"));
    request.mimeType = Text(Member(response, "mimeType"));
    request.source = Text(Member(response, "source"));
    request.responseHeaders = Headers(Member(response, "headers"));
    if (const auto& timing = Member(response, "timing"); timing.is_object()) {
        auto& phases = request.timing;
        phases.known = true;
        const auto read = [&](const char* key) { return Member(timing, key).is_number() ? Member(timing, key).get<double>() : -1.0; };
        phases.dnsStart = read("domainLookupStart");
        phases.dnsEnd = read("domainLookupEnd");
        phases.connectStart = read("connectStart");
        phases.connectEnd = read("connectEnd");
        phases.tlsStart = read("secureConnectionStart");
        phases.requestStart = read("requestStart");
        phases.responseStart = read("responseStart");
        phases.responseEnd = read("responseEnd");
        // WebKit sends 0 for what did not happen: no TLS on http, and no
        // phases at all for what a cache answered.
        if (phases.tlsStart <= 0) phases.tlsStart = -1;
        if (phases.responseStart <= 0) phases.known = false;
    }
    // What went over the network, which has more than the page asked for.
    if (const auto& sent = Member(response, "requestHeaders"); sent.is_object() && !sent.empty())
        request.requestHeaders = Headers(sent);
}
}

// Reads the protocol into the session. It is a friend so that the header
// needs no JSON types.
struct ProtocolReader {
    static void RequestWillBeSent(Session& session, const std::string& target, const json& parameters)
    {
        const auto id = Text(Member(parameters, "requestId"));
        if (id.empty()) return;
        const double timestamp = Number(Member(parameters, "timestamp"));
        std::string type = Text(Member(parameters, "type"));
        if (const auto& redirect = Member(parameters, "redirectResponse"); redirect.is_object()) {
            if (auto* previous = session.Find(target, id)) {
                ReadResponse(*previous, redirect);
                previous->finished = previous->redirected = true;
                previous->responded = previous->ended = timestamp;
                if (type.empty()) type = previous->type;
                session.Changed(*previous);
            }
        }
        auto& request = session.Add(target, id);
        const auto& sent = Member(parameters, "request");
        request.loader = Text(Member(parameters, "loaderId"));
        request.frame = Text(Member(parameters, "frameId"));
        request.documentURL = Text(Member(parameters, "documentURL"));
        request.url = Text(Member(sent, "url"));
        request.method = Text(Member(sent, "method"));
        request.requestHeaders = Headers(Member(sent, "headers"));
        if (const auto& body = Member(sent, "postData"); body.is_string()) {
            request.hasRequestBody = true;
            request.requestBody = body.get<std::string>();
        }
        request.type = type.empty() ? "Other" : type;
        request.started = timestamp;
        request.wallTime = Number(Member(parameters, "walltime"));
        request.initiator = Initiator(Member(parameters, "initiator"));
    }

    static std::string Initiator(const json& initiator)
    {
        const auto type = Text(Member(initiator, "type"));
        if (const auto& frames = Member(Member(initiator, "stackTrace"), "callFrames"); frames.is_array()) {
            for (const auto& frame : frames) {
                // The first frame of a script: fetch() itself is the engine's.
                const auto url = Text(Member(frame, "url"));
                if (!IsNative(url)) return Location(url, static_cast<int>(Number(Member(frame, "lineNumber"))), 0);
            }
        }
        if (const auto url = Text(Member(initiator, "url")); !url.empty())
            return Location(url, static_cast<int>(Number(Member(initiator, "lineNumber"))), 0);
        return type == "parser" ? "document" : type;
    }

    // A request that began before the tools were opened is known from here on.
    static Request& Known(Session& session, const std::string& target, const json& parameters)
    {
        const auto id = Text(Member(parameters, "requestId"));
        if (auto* request = session.Find(target, id)) return *request;
        auto& request = session.Add(target, id);
        request.loader = Text(Member(parameters, "loaderId"));
        request.frame = Text(Member(parameters, "frameId"));
        request.url = Text(Member(Member(parameters, "response"), "url"));
        request.started = Number(Member(parameters, "timestamp"));
        request.type = "Other";
        return request;
    }

    static void ResponseReceived(Session& session, const std::string& target, const json& parameters)
    {
        if (Text(Member(parameters, "requestId")).empty()) return;
        auto& request = Known(session, target, parameters);
        ReadResponse(request, Member(parameters, "response"));
        if (const auto type = Text(Member(parameters, "type")); !type.empty()) request.type = type;
        request.responded = Number(Member(parameters, "timestamp"));
        session.Changed(request);
    }

    static void DataReceived(Session& session, const std::string& target, const json& parameters)
    {
        auto* request = session.Find(target, Text(Member(parameters, "requestId")));
        if (!request) return;
        request->size += Count(Member(parameters, "dataLength"));
        request->transferred += Count(Member(parameters, "encodedDataLength"));
        session.Changed(*request);
    }

    static void LoadingFinished(Session& session, const std::string& target, const json& parameters)
    {
        auto* request = session.Find(target, Text(Member(parameters, "requestId")));
        if (!request) return;
        request->finished = true;
        request->ended = Number(Member(parameters, "timestamp"));
        const auto& metrics = Member(parameters, "metrics");
        if (const auto& sent = Member(metrics, "requestHeaders"); sent.is_object() && !sent.empty())
            request->requestHeaders = Headers(sent);
        if (const auto protocol = Text(Member(metrics, "protocol")); !protocol.empty()) request->protocol = protocol;
        if (const auto address = Text(Member(metrics, "remoteAddress")); !address.empty()) request->remoteAddress = address;
        if (Member(metrics, "responseBodyBytesReceived").is_number())
            request->transferred = Count(Member(metrics, "responseBodyBytesReceived")) + Count(Member(metrics, "responseHeaderBytesReceived"));
        if (const auto decoded = Count(Member(metrics, "responseBodyDecodedSize"))) request->size = decoded;
        session.Changed(*request);
        // Read now, while the page's process has it: a navigation that
        // changes process leaves no one to ask. Kept across navigations
        // (Preserve log), scripts and style sheets are read too.
        const bool kept = session.fPreserveRequests && (request->type == "Script" || request->type == "Stylesheet");
        if ((request->type == "XHR" || request->type == "Fetch" || request->type == "Document" || kept)
            && request->size && request->size <= Session::kEagerBody && !request->redirected)
            session.RequestBody(request->serial);
    }

    static void LoadingFailed(Session& session, const std::string& target, const json& parameters)
    {
        auto* request = session.Find(target, Text(Member(parameters, "requestId")));
        if (!request) return;
        request->failed = true;
        request->canceled = Flag(Member(parameters, "canceled"));
        request->error = Text(Member(parameters, "errorText"));
        request->ended = Number(Member(parameters, "timestamp"));
        session.Changed(*request);
    }

    static void ServedFromMemoryCache(Session& session, const std::string& target, const json& parameters)
    {
        if (Text(Member(parameters, "requestId")).empty()) return;
        auto& request = session.Add(target, Text(Member(parameters, "requestId")));
        const auto& resource = Member(parameters, "resource");
        request.loader = Text(Member(parameters, "loaderId"));
        request.frame = Text(Member(parameters, "frameId"));
        request.documentURL = Text(Member(parameters, "documentURL"));
        request.url = Text(Member(resource, "url"));
        request.method = "GET";
        request.type = Text(Member(resource, "type"));
        if (request.type.empty()) request.type = "Other";
        request.initiator = Initiator(Member(parameters, "initiator"));
        request.started = request.responded = request.ended = Number(Member(parameters, "timestamp"));
        if (const auto& response = Member(resource, "response"); response.is_object()) ReadResponse(request, response);
        request.source = "memory-cache";
        request.size = Count(Member(resource, "bodySize"));
        request.finished = true;
    }

    // A value with properties to look into (not null, not a primitive).
    static bool HasProperties(const json& object)
    {
        const auto type = Text(Member(object, "type"));
        return (type == "object" || type == "function") && Text(Member(object, "subtype")) != "null"
            && !Text(Member(object, "objectId")).empty();
    }

    static void AddObject(ConsoleEntry& entry, const json& object)
    {
        if (!HasProperties(object) || entry.objects.size() >= 16) return;
        entry.objects.push_back({Shortened(Render(object, false), 120), Text(Member(object, "objectId"))});
    }

    // Network.webSocket*: the socket is a request of its own, its messages
    // kept with it.
    static void WebSocket(Session& session, const std::string& target, const std::string& method, const json& parameters)
    {
        const auto id = Text(Member(parameters, "requestId"));
        if (id.empty()) return;
        auto* request = session.Find(target, id);
        if (method == "Network.webSocketCreated") {
            if (!request) request = &session.Add(target, id);
            request->webSocket = true;
            request->type = "WebSocket";
            request->method = "GET";
            request->url = Text(Member(parameters, "url"));
            if (const auto& initiator = Member(parameters, "initiator"); initiator.is_object()) request->initiator = Initiator(initiator);
            session.Changed(*request);
            return;
        }
        if (!request || !request->webSocket) return;
        const double time = Number(Member(parameters, "timestamp"));
        if (method == "Network.webSocketWillSendHandshakeRequest") {
            request->requestHeaders = Headers(Member(Member(parameters, "request"), "headers"));
            request->started = time;
            request->wallTime = Number(Member(parameters, "walltime"));
        } else if (method == "Network.webSocketHandshakeResponseReceived") {
            const auto& response = Member(parameters, "response");
            request->hasResponse = true;
            request->status = static_cast<int>(Number(Member(response, "status")));
            request->statusText = Text(Member(response, "statusText"));
            request->responseHeaders = Headers(Member(response, "headers"));
            request->responded = time;
        } else if (method == "Network.webSocketFrameReceived" || method == "Network.webSocketFrameSent" || method == "Network.webSocketFrameError") {
            SocketFrame frame;
            frame.sent = method == "Network.webSocketFrameSent";
            frame.error = method == "Network.webSocketFrameError";
            frame.time = time;
            if (frame.error) frame.data = Text(Member(parameters, "errorMessage"));
            else {
                const auto& payload = Member(parameters, "response");
                frame.opcode = static_cast<int>(Number(Member(payload, "opcode"), 1));
                frame.data = Shortened(Text(Member(payload, "payloadData")), 64 * 1024);
                frame.length = Count(Member(payload, "payloadLength"));
                request->size += frame.length;
            }
            if (request->frames.size() >= Request::kMaximumFrames) request->frames.pop_front();
            request->frames.push_back(std::move(frame));
            session.fChanges.bodies.push_back(request->serial);
        } else if (method == "Network.webSocketClosed") {
            request->finished = true;
            request->ended = time;
        }
        session.Changed(*request);
    }

    // Debugger.*: the page's scripts and where it is paused.
    static void DebuggerEvent(Session& session, const std::string& target, const std::string& method, const json& parameters)
    {
        if (target != session.fPageTarget) return;
        auto& state = session.fDebugger;
        if (method == "Debugger.scriptParsed") {
            Script script;
            script.id = Text(Member(parameters, "scriptId"));
            script.url = Text(Member(parameters, "sourceURL"));
            if (script.url.empty()) script.url = Text(Member(parameters, "url"));
            script.startLine = static_cast<int>(Number(Member(parameters, "startLine")));
            script.startColumn = static_cast<int>(Number(Member(parameters, "startColumn")));
            script.contentScript = Flag(Member(parameters, "isContentScript"));
            script.sourceMapURL = Text(Member(parameters, "sourceMapURL"));
            if (script.id.empty()) return;
            session.fScripts[script.id] = std::move(script);
            session.fChanges.scripts = true;
        } else if (method == "Debugger.globalObjectCleared") {
            session.fScripts.clear();
            state.paused = false;
            state.frames.clear();
            session.fChanges.scripts = session.fChanges.debugger = true;
        } else if (method == "Debugger.paused") {
            state.paused = true;
            state.reason = Text(Member(parameters, "reason"));
            state.frames.clear();
            state.selectedFrame = 0;
            if (const auto& frames = Member(parameters, "callFrames"); frames.is_array()) {
                for (const auto& item : frames) {
                    DebugFrame frame;
                    frame.callFrameId = Text(Member(item, "callFrameId"));
                    frame.function = Text(Member(item, "functionName"));
                    const auto& location = Member(item, "location");
                    frame.scriptId = Text(Member(location, "scriptId"));
                    frame.line = static_cast<int>(Number(Member(location, "lineNumber")));
                    frame.column = static_cast<int>(Number(Member(location, "columnNumber")));
                    frame.thisObjectId = Text(Member(Member(item, "this"), "objectId"));
                    if (const auto& scopes = Member(item, "scopeChain"); scopes.is_array()) {
                        for (const auto& scope : scopes)
                            frame.scopes.push_back({Text(Member(scope, "type")), Text(Member(scope, "name")), Text(Member(Member(scope, "object"), "objectId"))});
                    }
                    state.frames.push_back(std::move(frame));
                }
            }
            session.fChanges.debugger = true;
        } else if (method == "Debugger.resumed") {
            state.paused = false;
            state.frames.clear();
            session.fChanges.debugger = true;
        } else if (method == "Debugger.breakpointResolved") {
            const auto id = Text(Member(parameters, "breakpointId"));
            for (auto& breakpoint : session.fBreakpoints)
                if (breakpoint.id == id) breakpoint.resolved = true;
            session.fChanges.debugger = true;
        }
    }

    static void MessageAdded(Session& session, const json& message)
    {
        const auto type = Text(Member(message, "type"));
        if (type == "endGroup") {
            session.fGroupDepth = std::max(0, session.fGroupDepth - 1);
            return;
        }
        auto text = MessageText(message);
        if (session.fGroupDepth > 0) {
            // Members of a console.group() stand in from its title.
            const std::string indent(static_cast<size_t>(std::min(session.fGroupDepth, 8)) * 2, ' ');
            std::string indented = indent;
            for (const char c : text) {
                indented += c;
                if (c == '\n') indented += indent;
            }
            text = std::move(indented);
        }
        const auto level = LevelOf(Text(Member(message, "level")));
        auto& entry = session.AddEntry(EntryKind::Message, level, std::move(text));
        entry.source = Text(Member(message, "source"));
        entry.url = Text(Member(message, "url"));
        entry.line = static_cast<int>(Number(Member(message, "line")));
        entry.column = static_cast<int>(Number(Member(message, "column")));
        entry.repeat = std::max(1, static_cast<int>(Number(Member(message, "repeatCount"), 1)));
        entry.time = Number(Member(message, "timestamp"));
        if (const auto& parameters = Member(message, "parameters"); parameters.is_array()) {
            for (const auto& parameter : parameters) AddObject(entry, parameter);
        }
        if (level == Level::Error || level == Level::Warning || type == "trace" || type == "assert")
            entry.stack = StackText(Member(message, "stackTrace"));
        if (entry.url.empty()) {
            // A message of a script has its place in the first frame.
            if (const auto& frames = Member(Member(message, "stackTrace"), "callFrames"); frames.is_array()) {
                for (const auto& frame : frames) {
                    if (IsNative(Text(Member(frame, "url")))) continue;
                    entry.url = Text(Member(frame, "url"));
                    entry.line = static_cast<int>(Number(Member(frame, "lineNumber")));
                    entry.column = static_cast<int>(Number(Member(frame, "columnNumber")));
                    break;
                }
            }
        }
        if (type == "startGroup" || type == "startGroupCollapsed") ++session.fGroupDepth;
    }

    // DOMStorage.domStorageItem*: a change to an area already read.
    static void StorageChanged(Session& session, const std::string& method, const json& parameters)
    {
        const auto& id = Member(parameters, "storageId");
        auto* area = session.Area(Flag(Member(id, "isLocalStorage")) ? StorageKind::Local : StorageKind::Session,
            Text(Member(id, "securityOrigin")));
        if (!area || !area->loaded) return;
        auto& items = area->items;
        const auto key = Text(Member(parameters, "key"));
        const auto found = std::find_if(items.begin(), items.end(), [&](const StorageItem& item) { return item.key == key; });
        if (method == "DOMStorage.domStorageItemsCleared") items.clear();
        else if (method == "DOMStorage.domStorageItemRemoved") {
            if (found != items.end()) items.erase(found);
        } else if (method == "DOMStorage.domStorageItemAdded" || method == "DOMStorage.domStorageItemUpdated") {
            const auto value = Text(Member(parameters, "newValue"));
            if (found != items.end()) found->value = value;
            else items.push_back({key, value});
        } else return;
        session.fChanges.storage = true;
    }

    static void Answered(Session& session, const Session::Pending& pending, const json& message)
    {
        const auto& result = Member(message, "result");
        const auto error = Text(Member(Member(message, "error"), "message"));
        if (pending.kind == Session::Pending::Body) {
            auto* request = session.Find(pending.serial);
            if (!request || request->bodyState != BodyState::Waiting) return;
            const auto& body = Member(result, "body");
            if (!body.is_string() && !request->size) {
                // The page keeps nothing for a response without content.
                request->body.clear();
                request->bodyState = BodyState::Loaded;
            } else if (!body.is_string()) {
                request->bodyState = BodyState::Failed;
                request->bodyError = error.empty() ? "The page gave no content for this request." : error;
            } else if (Flag(Member(result, "base64Encoded"))) {
                if (DecodeBase64(body.get_ref<const std::string&>(), request->body)) request->bodyState = BodyState::Loaded;
                else {
                    request->bodyState = BodyState::Failed;
                    request->bodyError = "The content could not be decoded.";
                }
            } else {
                request->body = body.get<std::string>();
                request->bodyState = BodyState::Loaded;
            }
            if (request->bodyState == BodyState::Loaded) {
                session.fBodyBytes += request->body.size();
                session.ReleaseBodies();
            }
            session.BodyChanged(*request);
        } else if (pending.kind == Session::Pending::CachePromise) {
            const auto objectId = Text(Member(Member(result, "result"), "objectId"));
            if (error.empty() && !Flag(Member(result, "wasThrown")) && !objectId.empty()) {
                session.SendToTarget(session.fPageTarget, "Runtime.awaitPromise", "{\"promiseObjectId\":" + Quoted(objectId)
                    + ",\"returnByValue\":true}", pending.serial ? Session::Pending::StorageEdit : Session::Pending::Storage);
                auto& awaited = session.fPending.rbegin()->second;
                awaited.storage = StorageKind::CacheStorage;
                awaited.origin = pending.origin;
                return;
            }
            auto* area = session.Area(StorageKind::CacheStorage, pending.origin, true);
            area->waiting = false;
            area->loaded = true;
            area->error = !error.empty() ? error : Text(Member(Member(result, "result"), "description"));
            session.fChanges.storage = true;
        } else if (pending.kind == Session::Pending::Storage) {
            auto* area = session.Area(pending.storage, pending.origin);
            if (!area) return;
            area->waiting = false;
            area->loaded = true;
            area->error = error;
            area->items.clear();
            area->cookies.clear();
            if (pending.storage == StorageKind::CacheStorage) {
                // Runtime.awaitPromise: the script's value, or what it threw.
                const auto& value = Member(result, "result");
                if (Flag(Member(result, "wasThrown"))) area->error = Text(Member(value, "description"));
                else if (const auto& entries = Member(value, "value"); entries.is_array()) {
                    for (const auto& entry : entries) {
                        StorageItem item;
                        item.cache = Text(Member(entry, "cache"));
                        item.url = Text(Member(entry, "url"));
                        item.method = Text(Member(entry, "method"));
                        item.key = item.cache + " › " + (item.method == "GET" ? std::string() : item.method + " ") + item.url;
                        const auto status = static_cast<int>(Number(Member(entry, "status")));
                        const auto type = Text(Member(entry, "type"));
                        std::string headers, contentType, length;
                        if (const auto& list = Member(entry, "headers"); list.is_array()) {
                            for (const auto& header : list) {
                                if (!header.is_array() || header.size() < 2) continue;
                                const auto name = Text(header[0]), text = Text(header[1]);
                                if (name == "content-type") contentType = text;
                                if (name == "content-length") length = text;
                                headers += "\n" + name + ": " + text;
                            }
                        }
                        // An opaque response (another origin's, without CORS)
                        // shows nothing of itself.
                        const auto statusText = Text(Member(entry, "statusText"));
                        item.value = type == "opaque" ? std::string("(opaque)")
                            : std::to_string(status) + (statusText.empty() ? std::string() : " " + statusText);
                        if (!contentType.empty()) item.value += " · " + contentType;
                        if (!length.empty()) item.value += " · " + length + " B";
                        item.value += headers;
                        area->items.push_back(std::move(item));
                    }
                    if (area->items.size() >= Session::kCacheEntries)
                        area->error = "The first " + std::to_string(Session::kCacheEntries) + " responses are shown.";
                }
            } else if (pending.storage == StorageKind::Cookies) {
                if (const auto& cookies = Member(result, "cookies"); cookies.is_array()) {
                    for (const auto& item : cookies) {
                        Cookie cookie;
                        cookie.name = Text(Member(item, "name"));
                        cookie.value = Text(Member(item, "value"));
                        cookie.domain = Text(Member(item, "domain"));
                        cookie.path = Text(Member(item, "path"));
                        cookie.sameSite = Text(Member(item, "sameSite"));
                        cookie.partitionKey = Text(Member(item, "partitionKey"));
                        cookie.session = Flag(Member(item, "session"));
                        cookie.expires = cookie.session ? 0 : Number(Member(item, "expires"));
                        cookie.httpOnly = Flag(Member(item, "httpOnly"));
                        cookie.secure = Flag(Member(item, "secure"));
                        area->cookies.push_back(std::move(cookie));
                    }
                }
            } else if (const auto& entries = Member(result, "objectStoreDataEntries"); entries.is_array()) {
                // Keys and values as the console shows them.
                for (const auto& entry : entries) {
                    const auto& key = Member(entry, "key").is_object() ? Member(entry, "key") : Member(entry, "primaryKey");
                    area->items.push_back({Shortened(Render(key, false), kValueLength), Shortened(Render(Member(entry, "value"), false), 64 * 1024)});
                }
                if (Flag(Member(result, "hasMore")))
                    area->error = "The first " + std::to_string(Session::kIndexedDBPage) + " entries are shown.";
            } else if (const auto& entries = Member(result, "entries"); entries.is_array()) {
                for (const auto& entry : entries) {
                    if (!entry.is_array() || entry.size() < 2) continue;
                    area->items.push_back({Text(entry[0]), Text(entry[1])});
                }
            }
            session.fChanges.storage = true;
        } else if (pending.kind == Session::Pending::ScriptSource) {
            const auto found = session.fScripts.find(pending.origin);
            if (found == session.fScripts.end()) return;
            auto& script = found->second;
            script.sourceWaiting = false;
            script.sourceLoaded = true;
            script.source = Text(Member(result, "scriptSource"));
            script.sourceError = error;
            session.fChanges.sources.push_back(script.id);
        } else if (pending.kind == Session::Pending::SourceMap) {
            const auto found = session.fScripts.find(pending.origin);
            if (found == session.fScripts.end()) return;
            const auto status = static_cast<int>(Number(Member(result, "status")));
            if (!error.empty() || (status && (status < 200 || status >= 300)))
                session.MapRead(found->second, { }, !error.empty() ? error : "The server answered " + std::to_string(status) + ".");
            else session.MapRead(found->second, Text(Member(result, "content")), { });
            session.fChanges.sources.push_back(found->first);
        } else if (pending.kind == Session::Pending::SetBreakpoint) {
            for (auto& breakpoint : session.fBreakpoints) {
                if (breakpoint.url + '\n' + std::to_string(breakpoint.line) + ':' + std::to_string(breakpoint.column) != pending.origin) continue;
                if (const auto id = Text(Member(result, "breakpointId")); !id.empty()) breakpoint.id = id;
                if (const auto& locations = Member(result, "locations"); locations.is_array() && !locations.empty()) breakpoint.resolved = true;
            }
            session.fChanges.debugger = true;
        } else if (pending.kind == Session::Pending::DatabaseNames) {
            auto& databases = session.fDatabases[pending.origin];
            databases.waiting = false;
            databases.error = error;
            databases.databases.clear();
            databases.outstanding = 0;
            if (const auto& names = Member(result, "databaseNames"); names.is_array()) {
                for (const auto& name : names) {
                    if (!name.is_string()) continue;
                    databases.databases.push_back({name.get<std::string>(), 0, { }});
                    ++databases.outstanding;
                    session.SendToTarget(session.fPageTarget, "IndexedDB.requestDatabase", "{\"securityOrigin\":" + Quoted(pending.origin)
                        + ",\"databaseName\":" + Quoted(name.get<std::string>()) + "}", Session::Pending::Database);
                    session.fPending.rbegin()->second.origin = pending.origin + '\n' + name.get<std::string>();
                }
            }
            databases.loaded = !databases.outstanding;
            session.fChanges.origins = true;
        } else if (pending.kind == Session::Pending::Database) {
            const auto newline = pending.origin.find('\n');
            const auto origin = pending.origin.substr(0, newline), name = pending.origin.substr(newline + 1);
            auto& databases = session.fDatabases[origin];
            for (auto& database : databases.databases) {
                if (database.name != name) continue;
                const auto& described = Member(result, "databaseWithObjectStores");
                database.version = Number(Member(described, "version"));
                if (const auto& stores = Member(described, "objectStores"); stores.is_array())
                    for (const auto& store : stores) database.stores.push_back(Text(Member(store, "name")));
            }
            if (databases.outstanding && !--databases.outstanding) databases.loaded = true;
            session.fChanges.origins = true;
        } else if (pending.kind == Session::Pending::StorageEdit) {
            // Read the area again: the page may have changed more than was asked.
            if (!error.empty()) {
                if (auto* area = session.Area(pending.storage, pending.origin)) area->error = error;
                session.fChanges.storage = true;
            }
            session.LoadStorage(pending.storage, pending.origin);
        } else if (pending.kind == Session::Pending::Frames) {
            if (pending.origin != session.fPageTarget) return;
            std::vector<std::string> origins;
            std::function<void(const json&)> walk = [&](const json& tree) {
                const auto origin = Text(Member(Member(tree, "frame"), "securityOrigin"));
                if (std::find(origins.begin(), origins.end(), origin) == origins.end()) origins.push_back(origin);
                if (const auto& children = Member(tree, "childFrames"); children.is_array())
                    for (const auto& child : children) walk(child);
            };
            walk(Member(result, "frameTree"));
            session.fMainFrame = Text(Member(Member(Member(result, "frameTree"), "frame"), "id"));
            session.SetOrigins(std::move(origins));
        } else if (pending.kind == Session::Pending::Evaluation) {
            if (!error.empty()) {
                session.AddEntry(EntryKind::Result, Level::Error, error);
                return;
            }
            const bool thrown = Flag(Member(result, "wasThrown"));
            auto& entry = session.AddEntry(EntryKind::Result, thrown ? Level::Error : Level::Log,
                Shortened(Render(Member(result, "result"), false), kValueLength));
            AddObject(entry, Member(result, "result"));
        } else if (pending.kind == Session::Pending::Properties || pending.kind == Session::Pending::OwnProperties) {
            if (!error.empty() && pending.kind == Session::Pending::Properties) {
                // Scope objects have no displayable properties: their own.
                session.SendToTarget(session.fPageTarget, "Runtime.getProperties", "{\"objectId\":" + Quoted(pending.origin)
                    + ",\"ownProperties\":true,\"generatePreview\":true}", Session::Pending::OwnProperties);
                session.fPending.rbegin()->second.origin = pending.origin;
                return;
            }
            auto& properties = session.fProperties[pending.origin];
            properties.loaded = true;
            properties.error = error;
            properties.items.clear();
            const auto add = [&](const json& property, bool internal) {
                ObjectProperty item;
                item.name = Text(Member(property, "name"));
                if (internal) item.name = "[[" + item.name + "]]";
                const auto& value = Member(property, "value");
                if (value.is_object()) {
                    item.value = Shortened(Render(value, false), kValueLength);
                    if (HasProperties(value)) item.objectId = Text(Member(value, "objectId"));
                } else if (Member(property, "get").is_object() || Member(property, "set").is_object())
                    item.value = "(…)";
                properties.items.push_back(std::move(item));
            };
            if (const auto& list = Member(result, "properties"); list.is_array())
                for (const auto& property : list) add(property, false);
            if (const auto& list = Member(result, "internalProperties"); list.is_array())
                for (const auto& property : list) add(property, true);
            session.fChanges.properties.push_back(pending.origin);
        }
    }
};

std::string Request::Name() const
{
    if (url.compare(0, 5, "data:") == 0) return Shortened(url, 48);
    auto text = std::string_view(url);
    text = text.substr(0, text.find('#'));
    const auto query = text.find('?');
    auto path = text.substr(0, query);
    const auto scheme = path.find("://");
    const auto hostStart = scheme == std::string_view::npos ? 0 : scheme + 3;
    while (path.size() > hostStart && path.back() == '/') path.remove_suffix(1);
    const auto slash = path.rfind('/');
    std::string name(slash == std::string_view::npos || slash < hostStart ? path.substr(hostStart) : path.substr(slash + 1));
    if (query != std::string_view::npos) name += text.substr(query);
    return name.empty() ? url : name;
}

std::string Request::Host() const
{
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) return { };
    const auto start = scheme + 3;
    auto end = url.find_first_of("/?#", start);
    if (end == std::string::npos) end = url.size();
    auto host = url.substr(start, end - start);
    if (const auto user = host.rfind('@'); user != std::string::npos) host.erase(0, user + 1);
    return host;
}

std::string Request::StatusLabel() const
{
    if (failed) return canceled ? "(canceled)" : "(failed)";
    if (hasResponse && status > 0) return std::to_string(status);
    if (finished) return source == "memory-cache" ? "(cached)" : "(done)";
    return "(pending)";
}

std::string ConsoleEntry::Location() const { return devtools::Location(url, line, column); }

bool LevelFilter::Shows(const ConsoleEntry& entry) const
{
    if (entry.kind != EntryKind::Message) return true;
    switch (entry.level) {
        case Level::Error: return errors;
        case Level::Warning: return warnings;
        case Level::Debug: return debug;
        default: return info;
    }
}

void StyledText::Add(std::string_view part, TextStyle style)
{
    if (part.empty()) return;
    if (style != TextStyle::Plain) runs.push_back({text.size(), part.size(), style});
    text += part;
}

uint64_t Session::FirstCommand()
{
    // Ten million commands each, below the MCP tools' 10^9.
    static std::atomic<uint64_t> sessions {0};
    return 1 + (sessions++ % 90) * 10000000;
}

void Session::Start()
{
    // A page that moves to another process starts loading in it at once;
    // paused, it waits until its requests are being recorded.
    Send("Target.setPauseOnStart", "{\"pauseOnStart\":true}");
}

std::vector<std::string> Session::TakeOutgoing()
{
    // The Elements panel's commands wait for the page's target.
    for (auto& command : fDOM.TakeCommands()) fHeldDOM.push_back(std::move(command));
    if (!fPageTarget.empty())
        for (const auto& command : std::exchange(fHeldDOM, { }))
            SendToTarget(fPageTarget, command.method, command.parameters, Pending::Elements, command.tag);
    return std::exchange(fOutgoing, { });
}

Changes Session::TakeChanges()
{
    if (fDOM.HasChanges()) fChanges.elements = true;
    return std::exchange(fChanges, { });
}

void Session::Send(const std::string& method, const std::string& parametersJSON)
{
    fOutgoing.push_back("{\"id\":" + std::to_string(fNextCommand++) + ",\"method\":" + Quoted(method)
        + ",\"params\":" + parametersJSON + "}");
}

uint64_t Session::SendToTarget(const std::string& target, const std::string& method, const std::string& parametersJSON,
    Pending::Kind kind, uint64_t serial)
{
    const auto id = fNextCommand++;
    const auto inner = "{\"id\":" + std::to_string(id) + ",\"method\":" + Quoted(method) + ",\"params\":" + parametersJSON + "}";
    if (kind != Pending::Other) fPending[id] = {kind, serial};
    Send("Target.sendMessageToTarget", "{\"targetId\":" + Quoted(target) + ",\"message\":" + Quoted(inner) + "}");
    return id;
}

void Session::Receive(std::string_view text)
{
    const auto message = json::parse(text, nullptr, false);
    if (!message.is_object()) return;
    const auto method = Text(Member(message, "method"));
    const auto& parameters = Member(message, "params");
    if (method == "Target.dispatchMessageFromTarget")
        FromTarget(Text(Member(parameters, "targetId")), Text(Member(parameters, "message")));
    else if (method == "Target.targetCreated") {
        const auto& info = Member(parameters, "targetInfo");
        const auto target = Text(Member(info, "targetId"));
        if (target.empty()) return;
        if (Text(Member(info, "type")) == "page") {
            // A target assumed already is not set up again.
            if (!Flag(Member(info, "isProvisional")) && fTargets.count(target) && !fTargets[target]) return;
            TargetCreated(target, Flag(Member(info, "isProvisional")), Flag(Member(info, "isPaused")));
        }
        else if (Flag(Member(info, "isPaused")))
            Send("Target.resume", "{\"targetId\":" + Quoted(target) + "}");
    } else if (method == "Target.targetDestroyed")
        TargetDestroyed(Text(Member(parameters, "targetId")));
    else if (method == "Target.didCommitProvisionalTarget")
        TargetCommitted(Text(Member(parameters, "oldTargetId")), Text(Member(parameters, "newTargetId")));
}

void Session::AssumePageTarget(const std::string& target)
{
    if (target.empty() || !fPageTarget.empty() || fTargets.count(target)) return;
    TargetCreated(target, false, false);
}

void Session::TargetCreated(const std::string& target, bool provisional, bool paused)
{
    fTargets[target] = provisional;
    if (!provisional) fPageTarget = target;
    SendToTarget(target, "Page.enable", "{}");
    SendToTarget(target, "Network.enable", "{}");
    SendToTarget(target, "Console.enable", "{}");
    SendToTarget(target, "DOMStorage.enable", "{}");
    // The origins of its frames, for the Storage panel.
    SendToTarget(target, "Page.getResourceTree", "{}", Pending::Frames);
    fPending.rbegin()->second.origin = target;
    if (fPreserveRequests) SendToTarget(target, "Network.setClearResourceDataOnNavigate", "{\"clearResourceDataOnNavigate\":false}");
    ApplyNetworkSettings(target);
    if (fDebugger.enabled) {
        SendToTarget(target, "Debugger.enable", "{}");
        SendToTarget(target, "Debugger.setBreakpointsActive", "{\"active\":true}");
        for (auto& breakpoint : fBreakpoints) SendBreakpoint(target, breakpoint);
    }
    if (paused) Send("Target.resume", "{\"targetId\":" + Quoted(target) + "}");
}

std::string Script::Label() const
{
    if (url.empty()) return "(anonymous " + id + ")";
    // A script in a document starts after the document's first line.
    return startLine > 0 ? "(inline) " + Location(url, startLine + 1, 0) : Location(url, 0, 0);
}

std::string Script::Place(int line) const
{
    return url.empty() ? "(anonymous):" + std::to_string(line) : Location(url, line, 0);
}

const Script* Session::FindScript(const std::string& id) const
{
    const auto found = fScripts.find(id);
    return found == fScripts.end() ? nullptr : &found->second;
}

void Session::EnableDebugger()
{
    if (fDebugger.enabled) return;
    fDebugger.enabled = true;
    for (const auto& [target, provisional] : fTargets) {
        SendToTarget(target, "Debugger.enable", "{}");
        // Breakpoints start inactive in WebKit.
        SendToTarget(target, "Debugger.setBreakpointsActive", "{\"active\":true}");
    }
}

void Session::LoadScriptSource(const std::string& id)
{
    auto found = fScripts.find(id);
    if (found == fScripts.end() || found->second.sourceLoaded || found->second.sourceWaiting || fPageTarget.empty()) return;
    found->second.sourceWaiting = true;
    SendToTarget(fPageTarget, "Debugger.getScriptSource", "{\"scriptId\":" + Quoted(id) + "}", Pending::ScriptSource);
    fPending.rbegin()->second.origin = id;
}

void Session::RecordTimeline(bool on)
{
    if (on == fTimelineRecording || fPageTarget.empty()) return;
    fTimelineRecording = on;
    if (on) {
        fTimelineRecords.clear();
        // Five frames of each record's stack say where its work came from.
        SendToTarget(fPageTarget, "Timeline.start", "{\"maxCallStackDepth\":5}");
    } else SendToTarget(fPageTarget, "Timeline.stop", "{}");
    fChanges.timeline = true;
}

void Session::LoadSourceMap(const std::string& id)
{
    auto found = fScripts.find(id);
    if (found == fScripts.end()) return;
    auto& script = found->second;
    if (script.sourceMapURL.empty() || script.mapLoaded || script.mapWaiting || fPageTarget.empty()) return;
    const auto address = ResolveAddress(script.url, script.sourceMapURL);
    if (address.rfind("data:", 0) == 0) {
        // data:application/json;base64,… or percent-encoded JSON.
        const auto comma = address.find(',');
        std::string text;
        if (comma == std::string::npos) MapRead(script, { }, "The map's data: address has no data.");
        else if (address.substr(0, comma).find(";base64") != std::string::npos) {
            if (DecodeBase64(std::string_view(address).substr(comma + 1), text)) MapRead(script, text, { });
            else MapRead(script, { }, "The map's data: address is not base64.");
        } else {
            for (size_t i = comma + 1; i < address.size(); ++i) {
                const auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
                if (address[i] == '%' && i + 2 < address.size() && hex(address[i + 1]) >= 0 && hex(address[i + 2]) >= 0) {
                    text += static_cast<char>(hex(address[i + 1]) * 16 + hex(address[i + 2]));
                    i += 2;
                } else text += address[i];
            }
            MapRead(script, text, { });
        }
        fChanges.sources.push_back(id);
        return;
    }
    if (fMainFrame.empty()) {
        MapRead(script, { }, "The page's frame is not known yet.");
        fChanges.sources.push_back(id);
        return;
    }
    script.mapWaiting = true;
    SendToTarget(fPageTarget, "Network.loadResource", "{\"frameId\":" + Quoted(fMainFrame) + ",\"url\":" + Quoted(address) + "}",
        Pending::SourceMap);
    fPending.rbegin()->second.origin = id;
}

void Session::MapRead(Script& script, const std::string& text, const std::string& error)
{
    script.mapWaiting = false;
    script.mapLoaded = true;
    script.mapError = error;
    script.map.reset();
    if (!error.empty()) return;
    auto map = std::make_shared<SourceMap>();
    std::string parseError;
    // An inline (data:) map's sources are relative to the script.
    auto base = ResolveAddress(script.url, script.sourceMapURL);
    if (base.rfind("data:", 0) == 0) base = script.url;
    if (ParseSourceMap(text, base, *map, parseError)) script.map = std::move(map);
    else script.mapError = parseError;
}

void Session::SendBreakpoint(const std::string& target, Breakpoint& breakpoint)
{
    SendToTarget(target, "Debugger.setBreakpointByUrl", "{\"lineNumber\":" + std::to_string(breakpoint.line)
        + (breakpoint.column ? ",\"columnNumber\":" + std::to_string(breakpoint.column) : std::string()) + ",\"url\":"
        + Quoted(breakpoint.url) + "}", Pending::SetBreakpoint);
    fPending.rbegin()->second.origin = breakpoint.url + '\n' + std::to_string(breakpoint.line) + ':' + std::to_string(breakpoint.column);
}

void Session::SetBreakpoint(const std::string& url, int line, int column)
{
    if (url.empty() || line < 0 || column < 0) return;
    if (std::any_of(fBreakpoints.begin(), fBreakpoints.end(),
            [&](const Breakpoint& known) { return known.url == url && known.line == line && known.column == column; }))
        return;
    fBreakpoints.push_back({url, line, column, { }, false});
    if (fDebugger.enabled)
        for (const auto& [target, provisional] : fTargets) SendBreakpoint(target, fBreakpoints.back());
    fChanges.debugger = true;
}

void Session::RemoveBreakpoint(const std::string& url, int line, int column)
{
    const auto found = std::find_if(fBreakpoints.begin(), fBreakpoints.end(),
        [&](const Breakpoint& known) { return known.url == url && known.line == line && known.column == column; });
    if (found == fBreakpoints.end()) return;
    // WebKit names a breakpoint by address, line and column.
    const auto id = found->id.empty() ? url + ':' + std::to_string(line) + ':' + std::to_string(column) : found->id;
    if (fDebugger.enabled)
        for (const auto& [target, provisional] : fTargets) SendToTarget(target, "Debugger.removeBreakpoint", "{\"breakpointId\":" + Quoted(id) + "}");
    fBreakpoints.erase(found);
    fChanges.debugger = true;
}

void Session::DebuggerCommand(const std::string& command)
{
    static const char* const commands[] = {"pause", "resume", "stepOver", "stepInto", "stepOut"};
    if (fPageTarget.empty() || std::none_of(std::begin(commands), std::end(commands), [&](const char* known) { return command == known; })) return;
    SendToTarget(fPageTarget, "Debugger." + command, "{}");
}

namespace {
// What blocks addresses with pattern in them: WebKit's plain patterns match
// only a whole address, so an escaped regular expression.
std::string Interception(const std::string& pattern)
{
    std::string expression;
    for (const char c : pattern) {
        if (std::strchr("\\^$.|?*+()[]{}/", c)) expression += '\\';
        expression += c;
    }
    return "{\"url\":" + Quoted(expression) + ",\"stage\":\"request\",\"caseSensitive\":false,\"isRegex\":true}";
}
}

void Session::ApplyNetworkSettings(const std::string& target)
{
    if (fCacheDisabled) SendToTarget(target, "Network.setResourceCachingDisabled", "{\"disabled\":true}");
    if (Throttled())
        SendToTarget(target, "Network.setEmulatedConditions", "{\"bandwidth\":" + std::to_string(fThrottleBandwidth) + ",\"latency\":"
            + std::to_string(fThrottleLatency) + "}");
    if (fBlocked.empty()) return;
    SendToTarget(target, "Network.setInterceptionEnabled", "{\"enabled\":true}");
    for (const auto& pattern : fBlocked)
        SendToTarget(target, "Network.addInterception", Interception(pattern));
}

void Session::SetThrottling(uint64_t bytesPerSecond, int latencyMilliseconds)
{
    latencyMilliseconds = std::max(0, latencyMilliseconds);
    if (bytesPerSecond == fThrottleBandwidth && latencyMilliseconds == fThrottleLatency) return;
    fThrottleBandwidth = bytesPerSecond;
    fThrottleLatency = latencyMilliseconds;
    // The page target is enough: the conditions are the network session's.
    if (!fPageTarget.empty())
        SendToTarget(fPageTarget, "Network.setEmulatedConditions", "{\"bandwidth\":" + std::to_string(bytesPerSecond) + ",\"latency\":"
            + std::to_string(latencyMilliseconds) + "}");
}

void Session::SetCacheDisabled(bool disabled)
{
    if (fCacheDisabled == disabled) return;
    fCacheDisabled = disabled;
    for (const auto& [target, provisional] : fTargets)
        SendToTarget(target, "Network.setResourceCachingDisabled", disabled ? "{\"disabled\":true}" : "{\"disabled\":false}");
}

void Session::BlockURL(const std::string& pattern)
{
    if (pattern.empty() || std::find(fBlocked.begin(), fBlocked.end(), pattern) != fBlocked.end()) return;
    fBlocked.push_back(pattern);
    for (const auto& [target, provisional] : fTargets) {
        if (fBlocked.size() == 1) SendToTarget(target, "Network.setInterceptionEnabled", "{\"enabled\":true}");
        SendToTarget(target, "Network.addInterception", Interception(pattern));
    }
}

void Session::UnblockAll()
{
    if (fBlocked.empty()) return;
    for (const auto& [target, provisional] : fTargets) {
        for (const auto& pattern : fBlocked)
            SendToTarget(target, "Network.removeInterception", Interception(pattern));
        SendToTarget(target, "Network.setInterceptionEnabled", "{\"enabled\":false}");
    }
    fBlocked.clear();
}

bool Session::Replay(uint64_t serial)
{
    const auto* request = FindRequest(serial);
    if (!request || fPageTarget.empty() || request->url.rfind("http", 0) != 0 || request->webSocket) return false;
    // The headers a page cannot set (fetch() would refuse or replace them).
    const auto forbidden = [](std::string name) {
        for (auto& c : name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        static const char* const names[] = {"host", "cookie", "cookie2", "content-length", "connection", "origin", "referer", "user-agent",
            "accept-encoding", "accept-charset", "date", "dnt", "expect", "keep-alive", "te", "trailer", "transfer-encoding", "upgrade", "via"};
        return name.empty() || name[0] == ':' || name.rfind("sec-", 0) == 0 || name.rfind("proxy-", 0) == 0
            || std::any_of(std::begin(names), std::end(names), [&](const char* forbidden) { return name == forbidden; });
    };
    json headers = json::object();
    for (const auto& header : request->requestHeaders)
        if (!forbidden(header.name)) headers[header.name] = header.value;
    json options = {{"method", request->method.empty() ? "GET" : request->method}, {"headers", headers}, {"credentials", "include"}};
    if (request->hasRequestBody && request->method != "GET" && request->method != "HEAD") options["body"] = request->requestBody;
    const auto expression = "fetch(" + Quoted(request->url) + ", " + Dump(options) + ").catch(() => { })";
    SendToTarget(fPageTarget, "Runtime.evaluate", "{\"expression\":" + Quoted(expression) + ",\"objectGroup\":\"replay\"}");
    return true;
}

void Session::TargetDestroyed(const std::string& target)
{
    fTargets.erase(target);
    if (fPageTarget == target) {
        fPageTarget.clear();
        // Its scripts went with it (a process that ended sends no
        // Debugger.globalObjectCleared).
        fScripts.clear();
        fDebugger.paused = false;
        fDebugger.frames.clear();
        fChanges.scripts = fChanges.debugger = true;
        // A recording ends with its page.
        if (fTimelineRecording) {
            fTimelineRecording = false;
            fChanges.timeline = true;
        }
    }
    for (auto& request : fRequests) {
        if (request.target != target) continue;
        // What the page had not finished, it never will.
        if (request.Pending()) {
            request.failed = request.canceled = true;
            Changed(request);
        }
        if (request.bodyState == BodyState::Waiting) {
            request.bodyState = BodyState::Failed;
            request.bodyError = "The page that made this request is gone.";
            BodyChanged(request);
        }
    }
    for (auto item = fPending.begin(); item != fPending.end();) {
        const auto* request = item->second.kind == Pending::Body ? FindRequest(item->second.serial) : nullptr;
        item = request && request->bodyState != BodyState::Waiting ? fPending.erase(item) : std::next(item);
    }
}

void Session::TargetCommitted(const std::string& previous, const std::string& target)
{
    fTargets[target] = false;
    fPageTarget = target;
    if (previous == target) return;
    // The DOM the panel knew is the other process's.
    fHeldDOM.clear();
    fDOM.TargetChanged();
    SendToTarget(target, "Page.getResourceTree", "{}", Pending::Frames);
    fPending.rbegin()->second.origin = target;
    // The page is now another process's: a navigation, seen from here.
    if (!fPreserveRequests) {
        const auto before = fRequests.size();
        Remove([&](const Request& request) { return request.target != target; });
        if (fRequests.size() != before) fChanges.requestsReset = true;
    }
    if (!fPreserveConsole && !fEntries.empty()) {
        fEntries.clear();
        fChanges.consoleReset = true;
        fChanges.consoleAdded = 0;
    }
    fGroupDepth = 0;
}

void Session::FromTarget(const std::string& target, std::string_view text)
{
    const auto message = json::parse(text, nullptr, false);
    if (!message.is_object()) return;
    if (const auto& id = Member(message, "id"); id.is_number()) {
        const auto found = fPending.find(Count(id));
        if (found == fPending.end()) return;
        const auto pending = found->second;
        fPending.erase(found);
        if (pending.kind == Pending::Elements) fDOM.Answered(pending.serial, text);
        else ProtocolReader::Answered(*this, pending, message);
        return;
    }
    const auto method = Text(Member(message, "method"));
    const auto& parameters = Member(message, "params");
    if (target == fPageTarget && fDOM.Enabled()
        && (method.rfind("DOM.", 0) == 0 || method.rfind("CSS.", 0) == 0 || method == "Inspector.inspect"
            || method == "Page.domContentEventFired" || method == "Page.loadEventFired"))
        fDOM.Event(text);
    if (method == "Network.dataReceived") ProtocolReader::DataReceived(*this, target, parameters);
    else if (method == "Network.requestWillBeSent") ProtocolReader::RequestWillBeSent(*this, target, parameters);
    else if (method == "Network.responseReceived") ProtocolReader::ResponseReceived(*this, target, parameters);
    else if (method == "Network.loadingFinished") ProtocolReader::LoadingFinished(*this, target, parameters);
    else if (method == "Network.loadingFailed") ProtocolReader::LoadingFailed(*this, target, parameters);
    else if (method == "Network.requestServedFromMemoryCache") ProtocolReader::ServedFromMemoryCache(*this, target, parameters);
    else if (method.rfind("Network.webSocket", 0) == 0) ProtocolReader::WebSocket(*this, target, method, parameters);
    else if (method.rfind("Debugger.", 0) == 0) ProtocolReader::DebuggerEvent(*this, target, method, parameters);
    else if (method == "Network.requestIntercepted") {
        // Blocked if its address has a blocked part; on its way otherwise.
        const auto id = Text(Member(parameters, "requestId"));
        auto url = Text(Member(Member(parameters, "request"), "url"));
        for (auto& c : url) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        const bool blocked = std::any_of(fBlocked.begin(), fBlocked.end(), [&](std::string pattern) {
            for (auto& c : pattern) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
            return url.find(pattern) != std::string::npos;
        });
        if (blocked) SendToTarget(target, "Network.interceptRequestWithError", "{\"requestId\":" + Quoted(id) + ",\"errorType\":\"General\"}");
        else SendToTarget(target, "Network.interceptContinue", "{\"requestId\":" + Quoted(id) + ",\"stage\":\"request\"}");
    }
    else if (method == "Timeline.eventRecorded" && target == fPageTarget && fTimelineRecording) {
        if (fTimelineRecords.size() < kTimelineRecords) fTimelineRecords.push_back(Member(parameters, "record").dump());
        fChanges.timeline = true;
    } else if (method == "Timeline.recordingStopped" && target == fPageTarget && fTimelineRecording) {
        fTimelineRecording = false;
        fChanges.timeline = true;
    }
    else if (method == "Console.messageAdded") ProtocolReader::MessageAdded(*this, Member(parameters, "message"));
    else if (method == "Console.messageRepeatCountUpdated") {
        for (auto entry = fEntries.rbegin(); entry != fEntries.rend(); ++entry) {
            if (entry->kind != EntryKind::Message) continue;
            entry->repeat = std::max(1, static_cast<int>(Number(Member(parameters, "count"), 1)));
            // Only the last entry can be redrawn by itself; one that has
            // not been shown yet needs no redrawing.
            const bool last = entry == fEntries.rbegin();
            if (last && !fChanges.consoleAdded) fChanges.consoleLastChanged = true;
            else if (!last) fChanges.consoleReset = true;
            break;
        }
    } else if (method == "Console.messagesCleared") {
        const auto reason = Text(Member(parameters, "reason"));
        if (reason == "frontend") return;
        if (fPreserveConsole) {
            if (reason == "console-api")
                AddEntry(EntryKind::Message, Level::Info, "console.clear() was not followed: the log is being kept.");
            return;
        }
        ClearConsole();
    } else if (method == "Page.frameNavigated") {
        const auto& frame = Member(parameters, "frame");
        if (Member(frame, "parentId").is_string() && !Text(Member(frame, "parentId")).empty()) {
            if (target == fPageTarget) AddOrigin(Text(Member(frame, "securityOrigin")));
            return;
        }
        Navigated(target, Text(Member(frame, "loaderId")), Text(Member(frame, "url")));
        if (target == fPageTarget || fPageTarget.empty()) SetOrigins({Text(Member(frame, "securityOrigin"))});
    } else if (method.rfind("DOMStorage.", 0) == 0)
        ProtocolReader::StorageChanged(*this, method, parameters);
    else if (method == "Page.loadEventFired" && target == fPageTarget) {
        // The page has run its scripts: read again what is being looked at.
        for (auto& [key, area] : fAreas)
            if (area.loaded && !area.waiting) LoadStorage(key.first, key.second);
    }
}

void Session::Navigated(const std::string& target, const std::string& loader, const std::string& url)
{
    fPageURL = url;
    fGroupDepth = 0;
    if (fPreserveConsole) {
        if (!fEntries.empty()) AddEntry(EntryKind::Navigation, Level::Info, "Navigated to " + url);
    }
    if (fPreserveRequests || loader.empty()) return;
    // The new document's own request was made before it was the document.
    const auto before = fRequests.size();
    Remove([&](const Request& request) {
        if (request.target == target) return request.loader != loader;
        // Another process's requests are the new page's, on its way in.
        return !fTargets.count(request.target);
    });
    if (fRequests.size() != before) fChanges.requestsReset = true;
}

void Session::Remove(const std::function<bool(const Request&)>& gone)
{
    fRequests.erase(std::remove_if(fRequests.begin(), fRequests.end(), [&](const Request& request) {
        if (!gone(request)) return false;
        if (const auto found = fByID.find({request.target, request.id}); found != fByID.end() && found->second == request.serial)
            fByID.erase(found);
        if (request.bodyState == BodyState::Loaded) fBodyBytes -= std::min<uint64_t>(fBodyBytes, request.body.size());
        return true;
    }), fRequests.end());
}

const Request* Session::FindRequest(uint64_t serial) const
{
    const auto found = std::lower_bound(fRequests.begin(), fRequests.end(), serial,
        [](const Request& request, uint64_t wanted) { return request.serial < wanted; });
    return found != fRequests.end() && found->serial == serial ? &*found : nullptr;
}

Request* Session::Find(uint64_t serial) { return const_cast<Request*>(FindRequest(serial)); }

Request* Session::Find(const std::string& target, const std::string& id)
{
    const auto found = fByID.find({target, id});
    return found == fByID.end() ? nullptr : Find(found->second);
}

Request& Session::Add(const std::string& target, const std::string& id)
{
    if (fRequests.size() >= kMaximumRequests) {
        const auto& oldest = fRequests.front();
        if (const auto found = fByID.find({oldest.target, oldest.id}); found != fByID.end() && found->second == oldest.serial)
            fByID.erase(found);
        if (oldest.bodyState == BodyState::Loaded) fBodyBytes -= std::min<uint64_t>(fBodyBytes, oldest.body.size());
        fRequests.pop_front();
        fChanges.requestsReset = true;
    }
    auto& request = fRequests.emplace_back();
    request.serial = fNextSerial++;
    request.target = target;
    request.id = id;
    fByID[{target, id}] = request.serial;
    Changed(request);
    return request;
}

void Session::Changed(Request& request)
{
    if (fChanges.requestsReset) return;
    // Most changes are to the request that changed last.
    auto& changed = fChanges.requests;
    if (!changed.empty() && changed.back() == request.serial) return;
    if (std::find(changed.begin(), changed.end(), request.serial) == changed.end()) changed.push_back(request.serial);
}

void Session::BodyChanged(Request& request)
{
    auto& bodies = fChanges.bodies;
    if (std::find(bodies.begin(), bodies.end(), request.serial) == bodies.end()) bodies.push_back(request.serial);
}

ConsoleEntry& Session::AddEntry(EntryKind kind, Level level, std::string text)
{
    if (fEntries.size() >= kMaximumEntries) {
        fEntries.pop_front();
        fChanges.consoleReset = true;
    }
    auto& entry = fEntries.emplace_back();
    entry.serial = fNextSerial++;
    entry.kind = kind;
    entry.level = level;
    entry.text = std::move(text);
    if (kind != EntryKind::Message) entry.time = static_cast<double>(std::time(nullptr));
    if (!fChanges.consoleReset) ++fChanges.consoleAdded;
    return entry;
}

void Session::SetPreserveRequests(bool preserve)
{
    if (fPreserveRequests == preserve) return;
    fPreserveRequests = preserve;
    // A page that keeps what it loaded can still show it after it has moved on.
    for (const auto& [target, provisional] : fTargets)
        SendToTarget(target, "Network.setClearResourceDataOnNavigate",
            std::string("{\"clearResourceDataOnNavigate\":") + (preserve ? "false" : "true") + "}");
}

void Session::ClearRequests()
{
    fRequests.clear();
    fByID.clear();
    fBodyBytes = 0;
    for (auto item = fPending.begin(); item != fPending.end();)
        item = item->second.kind == Pending::Body ? fPending.erase(item) : std::next(item);
    fChanges.requests.clear();
    fChanges.bodies.clear();
    fChanges.requestsReset = true;
}

void Session::LoadProperties(const std::string& objectId)
{
    if (objectId.empty() || fPageTarget.empty() || fProperties.count(objectId)) return;
    // The page forgets its objects when it moves on; a log kept across pages
    // must not grow this without end.
    if (fProperties.size() >= 2000) fProperties.clear();
    fProperties[objectId];
    SendToTarget(fPageTarget, "Runtime.getDisplayableProperties", "{\"objectId\":" + Quoted(objectId) + ",\"generatePreview\":true}",
        Pending::Properties);
    fPending.rbegin()->second.origin = objectId;
}

const ObjectProperties* Session::Properties(const std::string& objectId) const
{
    const auto found = fProperties.find(objectId);
    return found == fProperties.end() ? nullptr : &found->second;
}

void Session::ClearConsole()
{
    fEntries.clear();
    fProperties.clear();
    fGroupDepth = 0;
    fChanges.consoleReset = true;
    fChanges.consoleAdded = 0;
    fChanges.consoleLastChanged = false;
}

void Session::RequestBody(uint64_t serial)
{
    auto* request = Find(serial);
    if (!request || request->bodyState == BodyState::Waiting || request->bodyState == BodyState::Loaded) return;
    if (request->redirected) {
        request->bodyState = BodyState::Failed;
        request->bodyError = "A redirect has no content.";
    } else if (!fTargets.count(request->target)) {
        request->bodyState = BodyState::Failed;
        request->bodyError = "The page that made this request is gone.";
    } else {
        request->bodyState = BodyState::Waiting;
        request->bodyError.clear();
        SendToTarget(request->target, "Network.getResponseBody", "{\"requestId\":" + Quoted(request->id) + "}",
            Pending::Body, serial);
        return;
    }
    BodyChanged(*request);
}

void Session::ReleaseBodies()
{
    for (auto& request : fRequests) {
        if (fBodyBytes <= kBodyBudget) return;
        if (request.bodyState != BodyState::Loaded) continue;
        fBodyBytes -= std::min<uint64_t>(fBodyBytes, request.body.size());
        request.body = { };
        request.bodyState = BodyState::None;
    }
}

void Session::Evaluate(const std::string& expression)
{
    AddEntry(EntryKind::Input, Level::Log, expression);
    if (fPageTarget.empty()) {
        AddEntry(EntryKind::Result, Level::Error, "The page is not there to run this.");
        return;
    }
    // Paused: in the chosen frame, with its local variables, as in other
    // browsers.
    if (fDebugger.paused && !fDebugger.frames.empty()) {
        const auto index = std::clamp(fDebugger.selectedFrame, 0, static_cast<int>(fDebugger.frames.size()) - 1);
        SendToTarget(fPageTarget, "Debugger.evaluateOnCallFrame", "{\"callFrameId\":" + Quoted(fDebugger.frames[index].callFrameId)
            + ",\"expression\":" + Quoted(expression) + ",\"objectGroup\":\"console\",\"includeCommandLineAPI\":true,\"generatePreview\":true}",
            Pending::Evaluation);
        return;
    }
    SendToTarget(fPageTarget, "Runtime.evaluate", "{\"expression\":" + Quoted(expression)
        + ",\"objectGroup\":\"console\",\"includeCommandLineAPI\":true,\"generatePreview\":true,\"emulateUserGesture\":true}",
        Pending::Evaluation);
}

std::string Cookie::URL() const
{
    auto host = domain;
    if (!host.empty() && host.front() == '.') host.erase(0, 1);
    return (secure ? "https://" : "http://") + host + (path.empty() || path.front() != '/' ? "/" + path : path);
}

std::string Cookie::ExpiresLabel() const
{
    if (session || expires <= 0) return "Session";
    const time_t when = static_cast<time_t>(expires / 1000);
    struct tm local {};
    char text[64] = "";
    if (!localtime_r(&when, &local) || !std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local)) return "?";
    return text;
}

uint64_t StorageArea::Bytes() const
{
    uint64_t bytes = 0;
    for (const auto& item : items) bytes += item.key.size() + item.value.size();
    for (const auto& cookie : cookies) bytes += cookie.Size();
    return bytes;
}

StorageArea* Session::Area(StorageKind kind, const std::string& origin, bool create)
{
    const auto key = std::make_pair(kind, kind == StorageKind::Cookies ? std::string() : origin);
    if (const auto found = fAreas.find(key); found != fAreas.end()) return &found->second;
    if (!create) return nullptr;
    auto& area = fAreas[key];
    area.kind = kind;
    area.origin = key.second;
    return &area;
}

const StorageArea* Session::FindArea(StorageKind kind, const std::string& origin) const
{
    return const_cast<Session*>(this)->Area(kind, origin);
}

std::string Session::StorageID(StorageKind kind, const std::string& origin) const
{
    return "{\"securityOrigin\":" + Quoted(origin) + ",\"isLocalStorage\":" + (kind == StorageKind::Local ? "true" : "false") + "}";
}

std::string IndexedDBStore(const std::string& origin, const std::string& database, const std::string& store)
{
    return origin + '\n' + database + '\n' + store;
}

void Session::LoadDatabases(const std::string& origin)
{
    auto& databases = fDatabases[origin];
    if (databases.waiting || fPageTarget.empty()) return;
    databases.waiting = true;
    SendToTarget(fPageTarget, "IndexedDB.enable", "{}");
    SendToTarget(fPageTarget, "IndexedDB.requestDatabaseNames", "{\"securityOrigin\":" + Quoted(origin) + "}", Pending::DatabaseNames);
    fPending.rbegin()->second.origin = origin;
}

const OriginDatabases* Session::Databases(const std::string& origin) const
{
    const auto found = fDatabases.find(origin);
    return found == fDatabases.end() ? nullptr : &found->second;
}

void Session::LoadStorage(StorageKind kind, const std::string& origin)
{
    auto* area = Area(kind, origin, true);
    if (area->waiting) return;
    if (fPageTarget.empty()) {
        area->error = "The page is not there to ask.";
        fChanges.storage = true;
        return;
    }
    area->waiting = true;
    if (kind == StorageKind::CacheStorage) {
        area->waiting = false;
        if (fOrigins.empty() || origin != fOrigins.front()) {
            area->loaded = true;
            area->error = "Only the page's own origin's caches can be read.";
            fChanges.storage = true;
            return;
        }
        // The first kCacheEntries responses, with their requests' caches.
        CacheScript(origin, "(async () => { if (!self.caches) throw new Error('This page cannot use Cache Storage (it is not a "
            "secure context).'); const out = []; for (const name of await caches.keys()) { const cache = await caches.open(name); "
            "for (const request of (await cache.keys()).slice(0, " + std::to_string(kCacheEntries) + " - out.length)) { "
            "const response = await cache.match(request); out.push({cache: name, url: request.url, method: request.method, "
            "status: response ? response.status : 0, statusText: response ? response.statusText : '', type: response ? "
            "response.type : '', headers: response ? [...response.headers] : []}); } if (out.length >= " + std::to_string(kCacheEntries)
            + ") break; } return out; })()", false);
        area->waiting = true;
        return;
    }
    if (kind == StorageKind::IndexedDB) {
        // "origin\ndatabase\nstore".
        const auto first = origin.find('\n'), second = first == std::string::npos ? first : origin.find('\n', first + 1);
        if (second == std::string::npos) {
            area->waiting = false;
            return;
        }
        SendToTarget(fPageTarget, "IndexedDB.requestData", "{\"securityOrigin\":" + Quoted(origin.substr(0, first))
            + ",\"databaseName\":" + Quoted(origin.substr(first + 1, second - first - 1)) + ",\"objectStoreName\":"
            + Quoted(origin.substr(second + 1)) + ",\"indexName\":\"\",\"skipCount\":0,\"pageSize\":" + std::to_string(kIndexedDBPage) + "}",
            Pending::Storage);
    } else if (kind == StorageKind::Cookies)
        SendToTarget(fPageTarget, "Page.getCookies", "{}", Pending::Storage);
    else
        SendToTarget(fPageTarget, "DOMStorage.getDOMStorageItems", "{\"storageId\":" + StorageID(kind, origin) + "}", Pending::Storage);
    auto& pending = fPending.rbegin()->second;
    pending.storage = kind;
    pending.origin = area->origin;
}

void Session::StorageEdit(StorageKind kind, const std::string& origin, const std::string& method, const std::string& parametersJSON)
{
    if (fPageTarget.empty()) return;
    SendToTarget(fPageTarget, method, parametersJSON, Pending::StorageEdit);
    auto& pending = fPending.rbegin()->second;
    pending.storage = kind;
    pending.origin = kind == StorageKind::Cookies ? std::string() : origin;
}

void Session::CacheScript(const std::string& origin, const std::string& expression, bool edit)
{
    if (fPageTarget.empty()) return;
    SendToTarget(fPageTarget, "Runtime.evaluate", json{{"expression", expression}, {"objectGroup", "summit-storage"},
        {"doNotPauseOnExceptionsAndMuteConsole", true}}.dump(), Pending::CachePromise, edit ? 1 : 0);
    auto& pending = fPending.rbegin()->second;
    pending.storage = StorageKind::CacheStorage;
    pending.origin = origin;
}

void Session::SetStorageItem(StorageKind kind, const std::string& origin, const std::string& key, const std::string& value)
{
    if (kind == StorageKind::Cookies || kind == StorageKind::CacheStorage || kind == StorageKind::IndexedDB) return;
    StorageEdit(kind, origin, "DOMStorage.setDOMStorageItem",
        "{\"storageId\":" + StorageID(kind, origin) + ",\"key\":" + Quoted(key) + ",\"value\":" + Quoted(value) + "}");
}

void Session::RemoveStorageItem(StorageKind kind, const std::string& origin, const std::string& key)
{
    if (kind == StorageKind::Cookies) return;
    if (kind == StorageKind::CacheStorage) {
        const auto* area = FindArea(kind, origin);
        if (!area) return;
        for (const auto& item : area->items) {
            if (item.key != key) continue;
            CacheScript(origin, "caches.open(" + Quoted(item.cache) + ").then(cache => cache.delete(new Request(" + Quoted(item.url)
                + ", {method: " + Quoted(item.method) + "})))", true);
            return;
        }
        return;
    }
    StorageEdit(kind, origin, "DOMStorage.removeDOMStorageItem",
        "{\"storageId\":" + StorageID(kind, origin) + ",\"key\":" + Quoted(key) + "}");
}

void Session::ClearStorage(StorageKind kind, const std::string& origin)
{
    if (kind == StorageKind::CacheStorage) {
        CacheScript(origin, "caches.keys().then(names => Promise.all(names.map(name => caches.delete(name))))", true);
        return;
    }
    if (kind == StorageKind::IndexedDB) {
        const auto first = origin.find('\n'), second = first == std::string::npos ? first : origin.find('\n', first + 1);
        if (second == std::string::npos) return;
        StorageEdit(kind, origin, "IndexedDB.clearObjectStore", "{\"securityOrigin\":" + Quoted(origin.substr(0, first))
            + ",\"databaseName\":" + Quoted(origin.substr(first + 1, second - first - 1)) + ",\"objectStoreName\":"
            + Quoted(origin.substr(second + 1)) + "}");
        return;
    }
    if (kind != StorageKind::Cookies) {
        StorageEdit(kind, origin, "DOMStorage.clearDOMStorageItems", "{\"storageId\":" + StorageID(kind, origin) + "}");
        return;
    }
    if (const auto* area = FindArea(kind, origin))
        for (const auto cookie : area->cookies) DeleteCookie(cookie);
}

void Session::SetCookie(const Cookie& cookie)
{
    if (cookie.name.empty() || cookie.domain.empty()) return;
    json set = {{"name", cookie.name}, {"value", cookie.value}, {"domain", cookie.domain},
        {"path", cookie.path.empty() ? std::string("/") : cookie.path}, {"session", cookie.session || cookie.expires <= 0},
        {"expires", cookie.session ? 0.0 : cookie.expires}, {"httpOnly", cookie.httpOnly}, {"secure", cookie.secure},
        {"sameSite", cookie.sameSite.empty() ? std::string("Lax") : cookie.sameSite}};
    if (!cookie.partitionKey.empty()) set["partitionKey"] = cookie.partitionKey;
    StorageEdit(StorageKind::Cookies, { }, "Page.setCookie", json{{"cookie", set}}.dump());
}

void Session::DeleteCookie(const Cookie& cookie)
{
    if (!cookie.partitionKey.empty()) {
        // Page.deleteCookie has no partition field. Expire the exact structured
        // cookie so deleting this row cannot delete an ordinary same-name one.
        const json expired = {{"name", cookie.name}, {"value", cookie.value}, {"domain", cookie.domain},
            {"path", cookie.path}, {"partitionKey", cookie.partitionKey}, {"session", false}, {"expires", 0},
            {"httpOnly", cookie.httpOnly}, {"secure", cookie.secure}, {"sameSite", cookie.sameSite}};
        StorageEdit(StorageKind::Cookies, { }, "Page.setCookie", json{{"cookie", expired}}.dump());
        return;
    }
    StorageEdit(StorageKind::Cookies, { }, "Page.deleteCookie",
        "{\"cookieName\":" + Quoted(cookie.name) + ",\"url\":" + Quoted(cookie.URL()) + "}");
}

namespace {
// An origin that can keep storage: not opaque ("null") or empty ("://").
bool KeepsStorage(const std::string& origin)
{
    const auto scheme = origin.find("://");
    return scheme != std::string::npos && scheme > 0 && (origin.size() > scheme + 3 || origin.compare(0, scheme, "file") == 0);
}
}

void Session::SetOrigins(std::vector<std::string> origins)
{
    origins.erase(std::remove_if(origins.begin(), origins.end(), [](const std::string& origin) { return !KeepsStorage(origin); }),
        origins.end());
    if (origins == fOrigins) return;
    fOrigins = std::move(origins);
    // Areas of origins the page no longer has are forgotten; the cookies
    // belong to the new page's addresses.
    for (auto area = fAreas.begin(); area != fAreas.end();) {
        const bool gone = area->first.first == StorageKind::Cookies
            || std::find(fOrigins.begin(), fOrigins.end(), area->first.second) == fOrigins.end();
        area = gone ? fAreas.erase(area) : std::next(area);
    }
    // The databases are asked for again when looked at.
    fDatabases.clear();
    for (auto item = fPending.begin(); item != fPending.end();)
        item = item->second.kind == Pending::Storage && !FindArea(item->second.storage, item->second.origin)
            ? fPending.erase(item) : std::next(item);
    fChanges.origins = fChanges.storage = true;
}

void Session::AddOrigin(const std::string& origin)
{
    if (!KeepsStorage(origin) || std::find(fOrigins.begin(), fOrigins.end(), origin) != fOrigins.end()) return;
    fOrigins.push_back(origin);
    fChanges.origins = true;
}

StyledText DescribeFrames(const Request& request)
{
    StyledText text;
    if (request.frames.empty()) {
        text.Add(request.finished ? "The socket closed without messages." : "No messages yet.", TextStyle::Muted);
        return text;
    }
    for (const auto& frame : request.frames) {
        if (!text.text.empty()) text.Add("\n");
        char when[32];
        std::snprintf(when, sizeof when, "+%.3f s", request.started > 0 ? std::max(0.0, frame.time - request.started) : 0.0);
        text.Add(frame.error ? "✕ " : frame.sent ? "↑ " : "↓ ", frame.error ? TextStyle::Error : frame.sent ? TextStyle::Good : TextStyle::Name);
        text.Add(std::string(when) + "  ", TextStyle::Muted);
        if (frame.error) text.Add(frame.data, TextStyle::Error);
        else if (frame.opcode == 1) text.Add(frame.data);
        else {
            const char* kind = frame.opcode == 2 ? "binary" : frame.opcode == 8 ? "close" : frame.opcode == 9 ? "ping" : frame.opcode == 10 ? "pong" : "frame";
            text.Add("(" + std::string(kind) + ", " + std::to_string(frame.length) + (frame.length == 1 ? " byte)" : " bytes)"), TextStyle::Muted);
        }
    }
    return text;
}

StyledText DescribeHeaders(const Request& request)
{
    StyledText out;
    const auto title = [&](const std::string& text) {
        if (!out.text.empty()) out.Add("\n");
        out.Add(text, TextStyle::Title);
        out.Add("\n");
    };
    const auto row = [&](std::string_view name, const std::string& value, TextStyle style = TextStyle::Plain) {
        out.Add("  ");
        out.Add(name, TextStyle::Name);
        out.Add(": ", TextStyle::Name);
        // A header of several lines stays under its name.
        std::string text;
        for (const char c : value) {
            if (c == '\r') continue;
            text += c;
            if (c == '\n') text += "    ";
        }
        out.Add(text, style);
        out.Add("\n");
    };
    title("General");
    row("URL", request.url);
    if (!request.method.empty()) row("Method", request.method);
    if (request.failed)
        row("Status", request.canceled ? "Canceled" : "Failed" + (request.error.empty() ? std::string() : ": " + request.error),
            request.canceled ? TextStyle::Warning : TextStyle::Error);
    else if (request.hasResponse) {
        auto status = std::to_string(request.status);
        if (!request.statusText.empty()) status += " " + request.statusText;
        if (request.redirected) status += " (redirected)";
        row("Status", status, request.status >= 400 ? TextStyle::Error : request.status >= 300 ? TextStyle::Warning : TextStyle::Good);
    } else row("Status", "Pending", TextStyle::Muted);
    if (request.failed && request.canceled && !request.error.empty()) row("Reason", request.error);
    auto type = request.type;
    if (!request.mimeType.empty()) type += " (" + request.mimeType + ")";
    row("Type", type);
    if (!request.source.empty() && request.source != "network" && request.source != "unknown") row("Served from", request.source);
    if (!request.remoteAddress.empty()) row("Remote address", request.remoteAddress);
    if (!request.protocol.empty()) row("Protocol", request.protocol);
    if (!request.initiator.empty()) row("Initiator", request.initiator);
    if (request.hasResponse || request.size) {
        auto size = FormatBytes(request.size);
        if (request.transferred) size += " (" + FormatBytes(request.transferred) + " transferred)";
        row("Size", size);
    }
    if (const auto started = ClockTime(request.wallTime); !started.empty()) row("Started", started);
    if (request.Duration() >= 0) {
        auto time = FormatDuration(request.Duration());
        if (request.responded >= request.started && request.ended > request.responded)
            time += " (" + FormatDuration(request.responded - request.started) + " to the first byte)";
        row("Time", time);
    }
    // Where the time went, as other browsers' timing views show it.
    if (const auto& phases = request.timing; phases.known) {
        const auto span = [](double from, double to) { return from >= 0 && to >= from ? to - from : -1.0; };
        const auto milliseconds = [](double value) {
            char text[32];
            std::snprintf(text, sizeof text, value < 10 ? "%.1f ms" : "%.0f ms", value);
            return std::string(text);
        };
        std::vector<std::pair<const char*, double>> steps;
        steps.push_back({"DNS lookup", span(phases.dnsStart, phases.dnsEnd)});
        if (phases.tlsStart >= 0) {
            steps.push_back({"Connecting", span(phases.connectStart, phases.tlsStart)});
            steps.push_back({"TLS", span(phases.tlsStart, phases.connectEnd)});
        } else steps.push_back({"Connecting", span(phases.connectStart, phases.connectEnd)});
        steps.push_back({"Waiting (to the first byte)", span(phases.requestStart, phases.responseStart)});
        // The body: from the response's start to the end of the request.
        if (request.responded >= 0 && request.ended >= request.responded)
            steps.push_back({"Receiving", (request.ended - request.responded) * 1000});
        if (std::any_of(steps.begin(), steps.end(), [](const auto& step) { return step.second >= 0; })) {
            title("Timing");
            for (const auto& [name, value] : steps)
                if (value >= 0) row(name, milliseconds(value));
        }
    }
    const auto headers = [&](const char* name, const std::vector<Header>& list) {
        if (list.empty()) return;
        title(std::string(name) + " (" + std::to_string(list.size()) + ")");
        for (const auto& header : list) row(header.name, header.value);
    };
    headers("Response Headers", request.responseHeaders);
    headers("Request Headers", request.requestHeaders);
    while (!out.text.empty() && out.text.back() == '\n') out.text.pop_back();
    return out;
}

std::string SummarizeRequests(const std::deque<Request>& requests, size_t shown)
{
    if (requests.empty()) return "No requests yet. Reload the page to record its requests.";
    uint64_t transferred = 0, size = 0;
    size_t failed = 0;
    for (const auto& request : requests) {
        transferred += request.transferred;
        size += request.size;
        if (request.IsError()) ++failed;
    }
    auto text = shown == requests.size() ? std::to_string(shown) : std::to_string(shown) + " of " + std::to_string(requests.size());
    text += requests.size() == 1 ? " request" : " requests";
    text += " · " + FormatBytes(transferred) + " transferred · " + FormatBytes(size) + " of resources";
    if (failed) text += " · " + std::to_string(failed) + " failed";
    return text;
}

std::string ConsoleMessageText(std::string_view messageJSON)
{
    return MessageText(json::parse(messageJSON, nullptr, false));
}

// 'text' for a POSIX shell: a quote ends, is escaped and opens again.
static std::string ShellQuoted(const std::string& text)
{
    std::string quoted = "'";
    for (const char c : text) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    return quoted + "'";
}

std::string CurlCommand(const Request& request)
{
    std::string command = "curl " + ShellQuoted(request.url);
    const bool body = request.hasRequestBody;
    if (!request.method.empty() && request.method != "GET" && !(request.method == "POST" && body))
        command += " \\\n  -X " + ShellQuoted(request.method);
    for (const auto& header : request.requestHeaders) {
        // HTTP/2's pseudo-headers are curl's own; the length follows the body.
        if (header.name.empty() || header.name[0] == ':' || header.name == "Content-Length" || header.name == "content-length") continue;
        command += " \\\n  -H " + ShellQuoted(header.name + ": " + header.value);
    }
    if (body) command += " \\\n  --data-raw " + ShellQuoted(request.requestBody);
    return command;
}

std::string HarArchive(const std::vector<const Request*>& requests, const std::string& version)
{
    const auto headers = [](const std::vector<Header>& list) {
        json array = json::array();
        for (const auto& header : list) array.push_back({{"name", header.name}, {"value", header.value}});
        return array;
    };
    const auto isoTime = [](double seconds) {
        if (seconds <= 0) return std::string("1970-01-01T00:00:00.000Z");
        const auto whole = static_cast<std::time_t>(seconds);
        std::tm utc { };
        gmtime_r(&whole, &utc);
        char buffer[40];
        std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
            utc.tm_hour, utc.tm_min, utc.tm_sec, static_cast<int>((seconds - static_cast<double>(whole)) * 1000) % 1000);
        return std::string(buffer);
    };
    const auto milliseconds = [](double from, double to) { return from >= 0 && to >= from ? std::round((to - from) * 1e5) / 100 : -1.0; };
    json entries = json::array();
    for (const auto* request : requests) {
        if (!request) continue;
        const auto httpVersion = request->protocol.empty() ? std::string("HTTP/1.1")
            : request->protocol == "h2" ? std::string("HTTP/2") : request->protocol;
        json queries = json::array();
        if (const auto question = request->url.find('?'); question != std::string::npos) {
            auto query = request->url.substr(question + 1);
            query = query.substr(0, query.find('#'));
            for (size_t from = 0; from <= query.size() && !query.empty();) {
                const auto end = std::min(query.find('&', from), query.size());
                const auto part = query.substr(from, end - from);
                const auto equals = part.find('=');
                if (!part.empty()) queries.push_back({{"name", part.substr(0, equals)}, {"value", equals == std::string::npos ? "" : part.substr(equals + 1)}});
                from = end + 1;
            }
        }
        json sent = {{"method", request->method.empty() ? "GET" : request->method}, {"url", request->url}, {"httpVersion", httpVersion},
            {"headers", headers(request->requestHeaders)}, {"queryString", queries}, {"cookies", json::array()}, {"headersSize", -1},
            {"bodySize", request->hasRequestBody ? static_cast<int64_t>(request->requestBody.size()) : 0}};
        if (request->hasRequestBody) {
            std::string type;
            for (const auto& header : request->requestHeaders)
                if (header.name == "Content-Type" || header.name == "content-type") type = header.value;
            sent["postData"] = {{"mimeType", type}, {"text", ValidUTF8(request->requestBody)}};
        }
        json content = {{"size", static_cast<int64_t>(request->size)}, {"mimeType", request->mimeType}};
        if (request->bodyState == BodyState::Loaded) content["text"] = ValidUTF8(request->body);
        std::string location;
        for (const auto& header : request->responseHeaders)
            if (header.name == "Location" || header.name == "location") location = header.value;
        json received = {{"status", request->failed ? 0 : request->status}, {"statusText", request->statusText}, {"httpVersion", httpVersion},
            {"headers", headers(request->responseHeaders)}, {"cookies", json::array()}, {"content", content}, {"redirectURL", location},
            {"headersSize", -1}, {"bodySize", request->transferred ? static_cast<int64_t>(request->transferred) : -1}};
        const auto wait = milliseconds(request->started, request->responded);
        const auto receive = milliseconds(request->responded, request->ended);
        const auto total = milliseconds(request->started, request->ended);
        json timings = {{"send", 0}, {"wait", std::max(0.0, wait)}, {"receive", std::max(0.0, receive)}};
        if (const auto& phases = request->timing; phases.known) {
            // HAR: -1 for what did not happen.
            const auto span = [](double from, double to) { return from >= 0 && to >= from ? std::round((to - from) * 100) / 100 : -1.0; };
            timings["dns"] = span(phases.dnsStart, phases.dnsEnd);
            timings["connect"] = span(phases.connectStart, phases.connectEnd);
            timings["ssl"] = span(phases.tlsStart, phases.connectEnd);
            if (const auto waited = span(phases.requestStart, phases.responseStart); waited >= 0) timings["wait"] = waited;
        }
        json entry = {{"startedDateTime", isoTime(request->wallTime)}, {"time", std::max(0.0, total)}, {"request", sent},
            {"response", received}, {"cache", json::object()}, {"timings", timings}};
        if (!request->remoteAddress.empty()) entry["serverIPAddress"] = request->remoteAddress;
        if (request->failed && !request->error.empty()) entry["comment"] = request->error;
        entries.push_back(std::move(entry));
    }
    json archive = {{"log", {{"version", "1.2"}, {"creator", {{"name", "Summit"}, {"version", version}}}, {"pages", json::array()},
        {"entries", entries}}}};
    return archive.dump(2, ' ', false, json::error_handler_t::replace);
}
}
