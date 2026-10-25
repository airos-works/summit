#pragma once
#include "DevToolsElements.h"
#include "DevToolsFormat.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The developer tools' model: what a page has requested and what it has
// written to its console, kept up to date from the Web Inspector protocol
// messages of the engine (BWebKitInspectorSession). Nothing here needs Haiku;
// src/ui/DevToolsWindow.cpp shows it and tests/DevToolsTests.cpp covers it.
namespace summit::devtools {

struct Header {
    std::string name, value;
};

enum class BodyState {
    // Not asked for.
    None,
    // Asked for; the page has not answered.
    Waiting,
    Loaded,
    // The page no longer has it, or never had (bodyError says which).
    Failed
};

// One request. A redirect ends its entry and starts another.
struct Request {
    uint64_t serial = 0;
    // The protocol's request, target (page process), loader (document) and frame.
    std::string id, target, loader, frame;
    std::string url, method, documentURL;
    // Document, StyleSheet, Image, Font, Script, XHR, Fetch, Ping, Beacon,
    // WebSocket, EventSource or Other.
    std::string type;
    // "script.js:12" for a request a script made, else what started it.
    std::string initiator;
    std::vector<Header> requestHeaders, responseHeaders;
    bool hasRequestBody = false;
    std::string requestBody;
    int status = 0;
    std::string statusText, mimeType, protocol, remoteAddress;
    // network, memory-cache, disk-cache, service-worker or inspector-override.
    std::string source;
    // Seconds on the page's clock; negative until it happened.
    double started = 0, responded = -1, ended = -1;
    // Seconds since 1970 when the request was made, or 0.
    double wallTime = 0;
    // Bytes of the body as it is used, and bytes that crossed the network.
    uint64_t size = 0, transferred = 0;
    bool hasResponse = false, finished = false, failed = false, canceled = false, redirected = false;
    std::string error;
    BodyState bodyState = BodyState::None;
    // The body's bytes once Loaded.
    std::string body;
    std::string bodyError;

    // What the list calls it: the last part of the path with the query, or the host.
    std::string Name() const;
    std::string Host() const;
    // "200", "(pending)", "(failed)", "(canceled)".
    std::string StatusLabel() const;
    // Seconds from request to end, or to now on the page's clock; negative if unknown.
    double Duration() const { return ended >= 0 ? ended - started : -1; }
    bool Pending() const { return !finished && !failed; }
    // 4xx, 5xx, or no answer.
    bool IsError() const { return failed ? !canceled : status >= 400; }
};

enum class Level { Debug, Log, Info, Warning, Error };
enum class EntryKind {
    // Written by the page or the engine.
    Message,
    // The page went elsewhere and the log was kept.
    Navigation,
    // An expression typed into the console, and what it gave.
    Input,
    Result
};

// An object a console entry shows (an argument, a result) that the
// Console's object tree can look into.
struct ConsoleObject {
    std::string label, objectId;
};

struct ConsoleEntry {
    uint64_t serial = 0;
    EntryKind kind = EntryKind::Message;
    Level level = Level::Log;
    // As shown; may hold several lines.
    std::string text;
    // Lines of "function (file:line:column)", for errors, warnings and traces.
    std::string stack;
    // console-api, javascript, network, security...
    std::string source;
    std::string url;
    int line = 0, column = 0;
    int repeat = 1;
    // Seconds since 1970, or 0.
    double time = 0;
    std::vector<ConsoleObject> objects;
    // "app.js:12:5", or empty.
    std::string Location() const;
};

// An object's properties as the page answered (Runtime.getDisplayableProperties):
// each with its value as the console shows it and, for an object value, the
// object to look into next.
struct ObjectProperty {
    std::string name, value, objectId;
};
struct ObjectProperties {
    bool loaded = false;
    std::string error;
    std::vector<ObjectProperty> items;
};

// Which console levels are shown. console.log() counts as information.
struct LevelFilter {
    bool errors = true, warnings = true, info = true, debug = true;
    bool Shows(const ConsoleEntry& entry) const;
};

// The Storage panel: the local and session storage of the origins of the
// page's frames, and the cookies the page's addresses are sent.
enum class StorageKind { Local, Session, Cookies };
struct StorageItem {
    std::string key, value;
};
struct Cookie {
    std::string name, value, domain, path;
    // "None", "Lax" or "Strict".
    std::string sameSite;
    // Milliseconds since 1970; 0 for a cookie that ends with the session.
    double expires = 0;
    bool session = false, httpOnly = false, secure = false;
    std::string partitionKey;
    std::string ID() const { return name + '\n' + domain + '\n' + path + '\n' + partitionKey; }
    uint64_t Size() const { return name.size() + value.size(); }
    // An address the cookie is sent to. Partitioned deletion also needs its key.
    std::string URL() const;
    // "Session", or the date and time it expires.
    std::string ExpiresLabel() const;
};
struct StorageArea {
    StorageKind kind = StorageKind::Local;
    std::string origin;
    // In the order the page gave them.
    std::vector<StorageItem> items;
    std::vector<Cookie> cookies;
    // Asked for and answered; asked for and not yet answered.
    bool loaded = false, waiting = false;
    std::string error;
    uint64_t Bytes() const;
};

// What changed in the model since the changes were last taken.
struct Changes {
    // The list of requests must be read again from the start.
    bool requestsReset = false;
    // Otherwise: requests that are new or different, oldest first.
    std::vector<uint64_t> requests;
    // Requests whose body arrived or turned out to be unavailable.
    std::vector<uint64_t> bodies;
    bool consoleReset = false;
    // Otherwise: entries added at the end, and whether the entry before
    // them changed (its repeat count).
    size_t consoleAdded = 0;
    bool consoleLastChanged = false;
    bool Any() const
    {
        return requestsReset || consoleReset || consoleAdded || consoleLastChanged || !requests.empty() || !bodies.empty()
            || origins || storage || elements || !properties.empty();
    }
    // The page's origins (StorageOrigins()) changed; a storage area or the
    // cookies changed or were answered.
    bool origins = false;
    bool storage = false;
    // The Elements panel's model changed: DOM().TakeChanges() says how.
    bool elements = false;
    // Objects whose properties arrived (Properties()).
    std::vector<std::string> properties;
};

// Text with the parts of it that are drawn differently.
enum class TextStyle { Plain, Title, Name, Muted, Good, Warning, Error };
struct StyledText {
    struct Run {
        size_t offset, length;
        TextStyle style;
    };
    std::string text;
    std::vector<Run> runs;
    void Add(std::string_view part, TextStyle style = TextStyle::Plain);
};

class Session {
public:
    // Requests and console entries kept; the oldest go first.
    static constexpr size_t kMaximumRequests = 5000, kMaximumEntries = 10000;
    // Bodies of documents and of what scripts fetched are asked for as soon
    // as they are complete, if no larger than this: they are what is looked
    // at, and the page forgets them when it moves on.
    static constexpr uint64_t kEagerBody = 2 * 1024 * 1024;
    // Bodies held at once; beyond it the oldest are given up and asked for
    // again when they are wanted.
    static constexpr uint64_t kBodyBudget = 128ull * 1024 * 1024;

    // Protocol messages for the engine are queued by Start() and by whatever
    // below needs the page to do something; TakeOutgoing() hands them over.
    void Start();
    // The page's target ("page-N"), for a session that is not the page's
    // first: the engine announces its targets to the first frontend only.
    void AssumePageTarget(const std::string& target);
    std::vector<std::string> TakeOutgoing();
    // One protocol message from the engine.
    void Receive(std::string_view message);
    Changes TakeChanges();

    const std::deque<Request>& Requests() const { return fRequests; }
    const Request* FindRequest(uint64_t serial) const;
    const std::deque<ConsoleEntry>& Entries() const { return fEntries; }
    // The address of the page's main frame, once it has been told.
    const std::string& PageURL() const { return fPageURL; }

    // Keeping the log: navigation then clears nothing.
    void SetPreserveRequests(bool preserve);
    void SetPreserveConsole(bool preserve) { fPreserveConsole = preserve; }
    bool PreserveRequests() const { return fPreserveRequests; }
    bool PreserveConsole() const { return fPreserveConsole; }
    void ClearRequests();
    void ClearConsole();
    // Asks the page for the body unless it is loaded or on its way.
    void RequestBody(uint64_t serial);
    // Runs an expression in the page, as typed into the console.
    void Evaluate(const std::string& expression);
    // Asks the page for an object's properties (once; they come with
    // Changes::properties), and what it answered so far, else null.
    void LoadProperties(const std::string& objectId);
    const ObjectProperties* Properties(const std::string& objectId) const;

    // Origins of the page's frames that can keep storage, the page's first.
    const std::vector<std::string>& StorageOrigins() const { return fOrigins; }
    // An area once it has been asked for, else null. Cookies have one area
    // (its origin is empty).
    const StorageArea* FindArea(StorageKind kind, const std::string& origin) const;
    // Asks the page for an area's items (again, if it has them).
    void LoadStorage(StorageKind kind, const std::string& origin);
    // Changes an area, then reads it again. A cookie is named by its name
    // and URL(); clearing the cookies deletes each one of them shown.
    void SetStorageItem(StorageKind kind, const std::string& origin, const std::string& key, const std::string& value);
    void RemoveStorageItem(StorageKind kind, const std::string& origin, const std::string& key);
    void ClearStorage(StorageKind kind, const std::string& origin);
    void DeleteCookie(const Cookie& cookie);
    // Sets a cookie as given (Page.setCookie), then reads the cookies again.
    // A partitioned one stays in its partition.
    void SetCookie(const Cookie& cookie);

    // The Elements panel's model of the page's DOM and styles; its commands
    // go to the page with the session's.
    DOMModel& DOM() { return fDOM; }
    const DOMModel& DOM() const { return fDOM; }

private:
    friend struct ProtocolReader;
    struct Pending {
        enum Kind { Body, Evaluation, Storage, StorageEdit, Frames, Elements, Properties, Other };
        Kind kind;
        uint64_t serial;
        // Storage and StorageEdit: the area; Frames: the target asked;
        // Properties: the object.
        StorageKind storage = StorageKind::Local;
        std::string origin;
    };
    Request* Find(uint64_t serial);
    Request* Find(const std::string& target, const std::string& id);
    Request& Add(const std::string& target, const std::string& id);
    void Remove(const std::function<bool(const Request&)>& gone);
    void Changed(Request& request);
    void BodyChanged(Request& request);
    ConsoleEntry& AddEntry(EntryKind kind, Level level, std::string text);
    void Send(const std::string& method, const std::string& parametersJSON);
    uint64_t SendToTarget(const std::string& target, const std::string& method, const std::string& parametersJSON,
        Pending::Kind kind = Pending::Other, uint64_t serial = 0);
    void TargetCreated(const std::string& target, bool provisional, bool paused);
    void TargetDestroyed(const std::string& target);
    void TargetCommitted(const std::string& previous, const std::string& target);
    void FromTarget(const std::string& target, std::string_view message);
    void Navigated(const std::string& target, const std::string& loader, const std::string& url);
    void ReleaseBodies();
    StorageArea* Area(StorageKind kind, const std::string& origin, bool create = false);
    void StorageEdit(StorageKind kind, const std::string& origin, const std::string& method, const std::string& parametersJSON);
    void SetOrigins(std::vector<std::string> origins);
    void AddOrigin(const std::string& origin);
    std::string StorageID(StorageKind kind, const std::string& origin) const;

    std::deque<Request> fRequests;
    std::deque<ConsoleEntry> fEntries;
    std::map<std::pair<std::string, std::string>, uint64_t> fByID;
    std::map<uint64_t, Pending> fPending;
    // Targets and whether they are provisional.
    std::map<std::string, bool> fTargets;
    std::string fPageTarget, fPageURL;
    std::vector<std::string> fOrigins;
    std::map<std::pair<StorageKind, std::string>, StorageArea> fAreas;
    std::map<std::string, ObjectProperties> fProperties;
    DOMModel fDOM;
    std::vector<DOMModel::Command> fHeldDOM;
    std::vector<std::string> fOutgoing;
    Changes fChanges;
    // The engine gives every connection of a page every answer: each
    // session numbers its commands apart from the others'.
    static uint64_t FirstCommand();
    uint64_t fNextSerial = 1, fNextCommand = FirstCommand(), fBodyBytes = 0;
    int fGroupDepth = 0;
    bool fPreserveRequests = false, fPreserveConsole = false;
};

// General facts, then response and request headers, for the Headers tab.
StyledText DescribeHeaders(const Request& request);
// "12 requests · 1.4 MB transferred · 3.2 MB of resources".
std::string SummarizeRequests(const std::deque<Request>& requests, size_t shown);
// The request as a curl command line (Copy as cURL): its method, headers and
// body, each argument in single quotes.
std::string CurlCommand(const Request& request);
// Requests as an HTTP Archive (HAR 1.2), with the bodies the tools have.
std::string HarArchive(const std::vector<const Request*>& requests, const std::string& version);
// A console message's text from the protocol's message object (its text, or
// its parameters with their previews), exposed for tests.
std::string ConsoleMessageText(std::string_view messageJSON);
}
