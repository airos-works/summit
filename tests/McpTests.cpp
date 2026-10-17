#include "core/Mcp.h"
#include "core/McpHttp.h"
#include "core/McpInspector.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>
#include <string>

static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " #x "\n"; } } while (0)

using namespace summit::mcp;

static std::shared_ptr<Server> MakeServer()
{
    auto server = std::make_shared<Server>("Summit", "1.0", "Use the tools.");
    server->Add({"echo", "Answers its text.", Schema().String("text", "What to answer", true),
        [](const Json& arguments) { return ToolResult::Text(StringArgument(arguments, "text")); }});
    server->Add({"data", "Answers an object.", Schema().Integer("n", "A number"),
        [](const Json& arguments) { return ToolResult::Data({{"n", IntegerArgument(arguments, "n", 7, 0, 10)}}); }});
    server->Add({"fail", "Throws.", Schema(), [](const Json&) -> ToolResult { throw ToolError("no such tab"); }});
    return server;
}

static void TestProtocol()
{
    auto server = MakeServer();
    Json reply = Json::parse(server->Handle(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}})"));
    CHECK(reply["id"] == 1);
    CHECK(reply["result"]["protocolVersion"] == "2025-03-26");
    CHECK(reply["result"]["serverInfo"]["name"] == "Summit");
    CHECK(reply["result"]["instructions"] == "Use the tools.");
    CHECK(reply["result"]["capabilities"].contains("tools"));
    // Unknown versions get the newest.
    reply = Json::parse(server->Handle(R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":"1999-01-01"}})"));
    CHECK(reply["result"]["protocolVersion"] == "2025-06-18");
    // Notifications are not answered.
    CHECK(server->Handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty());
    reply = Json::parse(server->Handle(R"({"jsonrpc":"2.0","id":"a","method":"tools/list"})"));
    CHECK(reply["id"] == "a");
    CHECK(reply["result"]["tools"].size() == 3);
    CHECK(reply["result"]["tools"][0]["name"] == "echo");
    CHECK(reply["result"]["tools"][0]["inputSchema"]["required"][0] == "text");
    CHECK(reply["result"]["tools"][0]["annotations"]["readOnlyHint"] == true);
    reply = Json::parse(server->Handle(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hi"}}})"));
    CHECK(reply["result"]["content"][0]["text"] == "hi");
    CHECK(reply["result"]["isError"] == false);
    reply = Json::parse(server->Handle(R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"data","arguments":{"n":"3"}}})"));
    CHECK(reply["result"]["structuredContent"]["n"] == 3);
    // Bad arguments and throwing tools are tool errors, not protocol errors.
    reply = Json::parse(server->Handle(R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"data","arguments":{"n":30}}})"));
    CHECK(reply["result"]["isError"] == true);
    CHECK(reply["result"]["content"][0]["text"].get<std::string>().find("between 0 and 10") != std::string::npos);
    reply = Json::parse(server->Handle(R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"fail"}})"));
    CHECK(reply["result"]["isError"] == true);
    CHECK(reply["result"]["content"][0]["text"] == "no such tab");
    reply = Json::parse(server->Handle(R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"nothing"}})"));
    CHECK(reply["error"]["code"] == -32602);
    reply = Json::parse(server->Handle(R"({"jsonrpc":"2.0","id":8,"method":"sampling/createMessage"})"));
    CHECK(reply["error"]["code"] == -32601);
    reply = Json::parse(server->Handle("{not json"));
    CHECK(reply["error"]["code"] == -32700);
    reply = Json::parse(server->Handle(R"([{"jsonrpc":"2.0","id":9,"method":"ping"},{"jsonrpc":"2.0","method":"notifications/x"}])"));
    CHECK(reply.is_array() && reply.size() == 1 && reply[0]["id"] == 9);
}

static void TestHelpers()
{
    CHECK(Base64("", 0).empty());
    CHECK(Base64("f", 1) == "Zg==");
    CHECK(Base64("fo", 2) == "Zm8=");
    CHECK(Base64("foo", 3) == "Zm9v");
    CHECK(Base64("foobar", 6) == "Zm9vYmFy");
    CHECK(Matches("WebProcess*", "webprocess 123"));
    CHECK(Matches("*compositor*", "Summit compositor thread"));
    CHECK(Matches("a?c", "abc"));
    CHECK(!Matches("a?c", "abbc"));
    CHECK(Matches("*", ""));
    CHECK(!Matches("x*", "yx"));
    CHECK(Truncate("hello", 10) == "hello");
    CHECK(Truncate("hello world", 5).rfind("hello\n", 0) == 0);
    // A cut never splits a UTF-8 sequence.
    CHECK(Truncate("ab\xc3\xa9", 3).rfind("ab\n", 0) == 0);
    CHECK(BooleanArgument(Json{{"x", "yes"}}, "x", false));
    CHECK(StringArgument(Json{{"tab", 12}}, "tab") == "12");
    bool threw = false;
    try { StringArgument(Json{{"tab", true}}, "tab"); } catch (const ToolError&) { threw = true; }
    CHECK(threw);
}

static HttpRequest Parsed(std::string text, int expected = 1)
{
    HttpRequest request;
    CHECK(ParseHttpRequest(text, request) == expected);
    return request;
}

static void TestHttpParsing()
{
    HttpRequest request = Parsed("POST /mcp?x=1 HTTP/1.1\r\nContent-Length: 4\r\nAuthorization: Bearer abc\r\n\r\nbody");
    CHECK(request.method == "POST");
    CHECK(request.path == "/mcp");
    CHECK(request.body == "body");
    CHECK(request.headers["authorization"] == "Bearer abc");
    CHECK(request.keepAlive);
    Parsed("POST /mcp HTTP/1.1\r\nContent-Length: 10\r\n\r\nbody", 0);
    Parsed("POST /mcp HTTP/1.1\r\nContent-Length: 4\r\n", 0);
    Parsed("garbage\r\n\r\n", -1);
    request = Parsed("POST /mcp HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n");
    CHECK(request.body == "abcde");
    request = Parsed("GET /health HTTP/1.0\r\n\r\n");
    CHECK(!request.keepAlive);
    // Pipelined requests stay in the buffer.
    std::string two = "POST /mcp HTTP/1.1\r\nContent-Length: 1\r\n\r\naPOST /mcp HTTP/1.1\r\nContent-Length: 1\r\n\r\nb";
    HttpRequest first, second;
    CHECK(ParseHttpRequest(two, first) == 1 && first.body == "a");
    CHECK(ParseHttpRequest(two, second) == 1 && second.body == "b");
    CHECK(two.empty());
    HttpRequest authorized;
    authorized.headers["authorization"] = "Bearer secret";
    CHECK(Authorized(authorized, "secret"));
    CHECK(!Authorized(authorized, "secreT"));
    CHECK(!Authorized(authorized, ""));
    authorized.headers["authorization"] = "bearer   secret ";
    CHECK(Authorized(authorized, "secret"));
}

static void TestHttpResponses()
{
    auto server = MakeServer();
    HttpOptions options;
    options.token = "t0k";
    bool keepAlive = false;
    HttpRequest request;
    request.method = "POST";
    request.path = "/mcp";
    request.body = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
    std::string response = RespondHttp(*server, options, request, keepAlive);
    CHECK(response.rfind("HTTP/1.1 401", 0) == 0);
    request.headers["authorization"] = "Bearer t0k";
    response = RespondHttp(*server, options, request, keepAlive);
    CHECK(response.rfind("HTTP/1.1 200", 0) == 0);
    CHECK(response.find("\"result\":{}") != std::string::npos);
    request.headers["origin"] = "https://evil.example";
    CHECK(RespondHttp(*server, options, request, keepAlive).rfind("HTTP/1.1 403", 0) == 0);
    request.headers.erase("origin");
    request.body = R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
    CHECK(RespondHttp(*server, options, request, keepAlive).rfind("HTTP/1.1 202", 0) == 0);
    request.method = "GET";
    CHECK(RespondHttp(*server, options, request, keepAlive).rfind("HTTP/1.1 405", 0) == 0);
    HttpRequest health;
    health.method = "GET";
    health.path = "/health";
    CHECK(RespondHttp(*server, options, health, keepAlive).find("\"tools\":3") != std::string::npos);
}

static std::string Exchange(uint16_t port, const std::string& text)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { close(fd); return { }; }
    send(fd, text.data(), text.size(), 0);
    std::string reply;
    char chunk[4096];
    ssize_t got;
    while ((got = recv(fd, chunk, sizeof(chunk), 0)) > 0) reply.append(chunk, static_cast<size_t>(got));
    close(fd);
    return reply;
}

static void TestHttpServer()
{
    HttpOptions options;
    options.address = "127.0.0.1";
    options.port = 0;
    options.token = "abc";
    HttpServer http(MakeServer(), options);
    CHECK(http.Start().empty());
    CHECK(http.IsRunning());
    CHECK(http.BoundPort() != 0);
    const std::string body = R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"echo","arguments":{"text":"over the wire"}}})";
    const std::string reply = Exchange(http.BoundPort(), "POST /mcp HTTP/1.1\r\nConnection: close\r\nAuthorization: Bearer abc\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
    CHECK(reply.rfind("HTTP/1.1 200", 0) == 0);
    CHECK(reply.find("over the wire") != std::string::npos);
    CHECK(http.RequestCount() == 1);
    CHECK(http.LastPeer().rfind("127.0.0.1:", 0) == 0);
    // A second server cannot take the same port.
    HttpOptions same = options;
    same.port = http.BoundPort();
    HttpServer other(MakeServer(), same);
    CHECK(other.Start().find("in use") != std::string::npos);
    http.Stop();
    CHECK(!http.IsRunning());
    // Once stopped, the port can be taken again.
    CHECK(other.Start().empty());
    other.Stop();
}


static std::string FromTarget(const std::string& target, const Json& inner)
{
    return Json{{"method", "Target.dispatchMessageFromTarget"}, {"params", {{"targetId", target}, {"message", inner.dump()}}}}.dump();
}

static void TestInspectorChannel()
{
    InspectorChannel channel;
    channel.Start();
    auto outgoing = channel.TakeOutgoing();
    CHECK(!outgoing.empty() && outgoing[0].find("Target.setPauseOnStart") != std::string::npos);
    // A command before the page target exists waits for it.
    const uint64_t early = channel.Command("Runtime.evaluate", {{"expression", "1+1"}});
    CHECK(early >= InspectorChannel::kFirstCommand);
    CHECK(channel.TakeOutgoing().empty());
    channel.Receive(Json{{"method", "Target.targetCreated"}, {"params", {{"targetInfo",
        {{"targetId", "page-1"}, {"type", "page"}, {"isProvisional", false}, {"isPaused", false}}}}}}.dump());
    CHECK(channel.PageTarget() == "page-1");
    outgoing = channel.TakeOutgoing();
    bool sentEarly = false, modelEnabled = false;
    for (const auto& message : outgoing) {
        const Json outer = Json::parse(message);
        if (outer["method"] != "Target.sendMessageToTarget") continue;
        const Json inner = Json::parse(outer["params"]["message"].get<std::string>());
        if (inner["id"] == early) sentEarly = outer["params"]["targetId"] == "page-1" && inner["method"] == "Runtime.evaluate";
        if (inner["method"] == "Console.enable") modelEnabled = true;
    }
    CHECK(sentEarly);
    CHECK(modelEnabled);
    CHECK(!channel.TakeAnswer(early));
    channel.Receive(FromTarget("page-1", {{"id", early}, {"result", {{"result", {{"type", "number"}, {"value", 2}}}}}}));
    auto answer = channel.TakeAnswer(early);
    CHECK(answer && (*answer)["result"]["result"]["value"] == 2);
    CHECK(!channel.TakeAnswer(early));
    // Console messages reach the developer tools' model.
    channel.Receive(FromTarget("page-1", {{"method", "Console.messageAdded"}, {"params", {{"message",
        {{"source", "console-api"}, {"level", "error"}, {"text", "boom"}, {"type", "log"}}}}}}));
    CHECK(!channel.Model().Entries().empty() && channel.Model().Entries().back().text.find("boom") != std::string::npos);
    // Collected events are kept for the tool that asked.
    channel.Collect("Timeline.", true);
    channel.Receive(FromTarget("page-1", {{"method", "Timeline.eventRecorded"}, {"params", {{"record", {{"type", "Layout"}}}}}}));
    channel.Receive(FromTarget("page-1", {{"method", "Heap.garbageCollected"}, {"params", Json::object()}}));
    auto events = channel.TakeEvents("Timeline.");
    CHECK(events.size() == 1 && events[0]["params"]["record"]["type"] == "Layout");
    CHECK(channel.TakeEvents("Heap.").empty());
    // A page that changes process fails what the old one was asked.
    const uint64_t lost = channel.Command("DOM.getDocument");
    channel.Receive(Json{{"method", "Target.targetCreated"}, {"params", {{"targetInfo",
        {{"targetId", "page-2"}, {"type", "page"}, {"isProvisional", true}, {"isPaused", true}}}}}}.dump());
    channel.Receive(Json{{"method", "Target.didCommitProvisionalTarget"}, {"params", {{"oldTargetId", "page-1"}, {"newTargetId", "page-2"}}}}.dump());
    CHECK(channel.PageTarget() == "page-2");
    CHECK(channel.Commits() == 1);
    answer = channel.TakeAnswer(lost);
    CHECK(answer && answer->contains("error"));
    const uint64_t next = channel.Command("DOM.getDocument");
    bool toNew = false;
    for (const auto& message : channel.TakeOutgoing()) {
        const Json outer = Json::parse(message);
        if (outer.value("id", uint64_t(0)) == next) toNew = outer["params"]["targetId"] == "page-2";
    }
    CHECK(toNew);
}

static void TestSummaries()
{
    std::vector<Json> records = {
        {{"type", "RenderingFrame"}, {"startTime", 1.0}, {"endTime", 1.030}, {"children", {
            {{"type", "Layout"}, {"startTime", 1.0}, {"endTime", 1.020}},
            {{"type", "Paint"}, {"startTime", 1.020}, {"endTime", 1.025}}}}},
        {{"type", "TimerFire"}, {"startTime", 1.1}, {"endTime", 1.105}, {"data", {{"timerId", 3}}}},
    };
    const Json summary = SummarizeTimeline(records, 5);
    CHECK(summary["records"] == 2);
    CHECK(summary["byType"][0]["type"] == "RenderingFrame");
    CHECK(summary["longest"][0]["childrenMs"]["Layout"] == 20.0);
    CHECK(summary["renderingFrames"]["count"] == 1);
    CHECK(summary["renderingFrames"]["over16Ms"] == 1);
    CHECK(summary["spanMs"] == 105.0);
    const std::string heap = R"({"version":2,"nodes":[1,32,0,0, 2,64,1,0, 3,16,0,0],"nodeClassNames":["Object","Array"]})";
    const Json classes = SummarizeHeapSnapshot(heap, 10);
    CHECK(classes["objects"] == 3);
    CHECK(classes["bytes"] == 112);
    CHECK(classes["largest"][0]["class"] == "Array");
    CHECK(classes["largest"][1]["objects"] == 2);
    CHECK(SummarizeHeapSnapshot("nope", 1).contains("error"));
}

int main()
{
    TestProtocol();
    TestHelpers();
    TestHttpParsing();
    TestHttpResponses();
    TestHttpServer();
    TestInspectorChannel();
    TestSummaries();
    std::cout << checks - failures << '/' << checks << " MCP checks passed\n";
    return failures ? 1 : 0;
}
