#include "core/DevTools.h"
#include "core/DevToolsFormat.h"
#include "core/PageSource.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>

static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " #x "\n"; } } while (0)
#define CHECK_TEXT(actual, expected) do { ++checks; const std::string a = (actual), e = (expected); if (a != e) { ++failures; \
    std::cerr << __FILE__ << ':' << __LINE__ << ": expected\n" << e << "\n---- got\n" << a << "\n----\n"; } } while (0)

using namespace summit::devtools;

static void TestKinds()
{
    CHECK(DetectBodyKind("application/json", "") == BodyKind::JSON);
    CHECK(DetectBodyKind("Application/JSON; charset=utf-8", "") == BodyKind::JSON);
    CHECK(DetectBodyKind("application/vnd.api+json", "") == BodyKind::JSON);
    CHECK(DetectBodyKind("text/html; charset=UTF-8", "") == BodyKind::HTML);
    CHECK(DetectBodyKind("application/xhtml+xml", "") == BodyKind::HTML);
    CHECK(DetectBodyKind("text/xml", "") == BodyKind::XML);
    CHECK(DetectBodyKind("application/rss+xml", "") == BodyKind::XML);
    CHECK(DetectBodyKind("image/svg+xml", "") == BodyKind::XML);
    CHECK(DetectBodyKind("image/png", "") == BodyKind::Image);
    CHECK(DetectBodyKind("text/javascript", "") == BodyKind::JavaScript);
    CHECK(DetectBodyKind("text/css", "") == BodyKind::CSS);
    CHECK(DetectBodyKind("font/woff2", "") == BodyKind::Binary);
    CHECK(DetectBodyKind("text/plain", "hello") == BodyKind::Text);
    // What the type does not say, the body may.
    CHECK(DetectBodyKind("text/plain", " {\"a\": [1, 2]}\n") == BodyKind::JSON);
    CHECK(DetectBodyKind("text/plain", "{not json}") == BodyKind::Text);
    CHECK(DetectBodyKind("", "[1,2,3]") == BodyKind::JSON);
    CHECK(DetectBodyKind("", "<?xml version=\"1.0\"?><a/>") == BodyKind::XML);
    CHECK(DetectBodyKind("application/octet-stream", "<!DOCTYPE HTML><html></html>") == BodyKind::HTML);
    CHECK(DetectBodyKind("application/octet-stream", std::string("\x89PNG\0\0", 6)) == BodyKind::Binary);
    CHECK(DetectBodyKind("text/plain", std::string("a\0b", 3)) == BodyKind::Text);
    CHECK(std::string(BodyKindLexer(BodyKind::HTML)) == "hypertext" && std::string(BodyKindLexer(BodyKind::Image)) == "null");
    CHECK(CanFormat(BodyKind::JSON) && CanFormat(BodyKind::XML) && CanFormat(BodyKind::HTML) && !CanFormat(BodyKind::CSS));
}

static void TestJSON()
{
    std::string out;
    CHECK(FormatJSON("{\"a\":1,\"b\":[true,null,\"x\"],\"c\":{},\"d\":[ ]}", out));
    CHECK_TEXT(out, "{\n  \"a\": 1,\n  \"b\": [\n    true,\n    null,\n    \"x\"\n  ],\n  \"c\": {},\n  \"d\": []\n}");
    // Tokens are copied: no number or escape is rewritten.
    CHECK(FormatJSON("[12345678901234567890123, 1.0, 1e3, -0, \"\\u00e9\\n\\\"{[,:]}\"]", out));
    CHECK_TEXT(out, "[\n  12345678901234567890123,\n  1.0,\n  1e3,\n  -0,\n  \"\\u00e9\\n\\\"{[,:]}\"\n]");
    CHECK(FormatJSON("  \"text\" ", out) && out == "\"text\"");
    CHECK(FormatJSON("\xEF\xBB\xBF[\r\n1\r\n]", out) && out == "[\n  1\n]");
    CHECK(FormatJSON(")]}'\n{\"a\":1}", out));
    CHECK_TEXT(out, ")]}'\n{\n  \"a\": 1\n}");
    CHECK(FormatJSON("while(1);[1]", out) && out == "while(1);\n[\n  1\n]");
    out = "kept";
    CHECK(!FormatJSON("{\"a\":1,}", out) && out == "kept");
    CHECK(!FormatJSON("", out) && !FormatJSON("{", out) && !FormatJSON("[1] [2]", out) && !FormatJSON("{'a':1}", out));
    std::string deep;
    for (int i = 0; i < 200; ++i) deep += '[';
    for (int i = 0; i < 200; ++i) deep += ']';
    CHECK(FormatJSON(deep, out) && out.find("[]") != std::string::npos);
    const auto result = FormatBody(BodyKind::JSON, "{broken");
    CHECK(!result.formatted && result.text == "{broken" && !result.note.empty());
    CHECK(FormatBody(BodyKind::CSS, "a{}").text == "a{}" && !FormatBody(BodyKind::CSS, "a{}").formatted);
}

static void TestXML()
{
    CHECK_TEXT(FormatXML("<?xml version=\"1.0\"?><root><item id=\"1\">One</item><item id=\"2\"/><empty></empty>"
        "<group><a>1</a><!-- note --><b><![CDATA[x < y]]></b></group></root>"),
        "<?xml version=\"1.0\"?>\n<root>\n  <item id=\"1\">One</item>\n  <item id=\"2\"/>\n  <empty></empty>\n"
        "  <group>\n    <a>1</a>\n    <!-- note -->\n    <b><![CDATA[x < y]]></b>\n  </group>\n</root>");
    // Formatting what is formatted already changes nothing.
    const std::string formatted = "<a>\n  <b>text</b>\n  <c>\n    <d/>\n  </c>\n</a>";
    CHECK_TEXT(FormatXML(formatted), formatted);
    CHECK_TEXT(FormatXML("<a x=\"1 > 0\"\n   y='2'><b>two\nlines</b></a>"),
        "<a x=\"1 > 0\" y='2'>\n  <b>\n    two\n    lines\n  </b>\n</a>");
    CHECK_TEXT(FormatXML("<!DOCTYPE r [ <!ENTITY e \"v\"> ]><r>text <i>mixed</i> more</r>"),
        "<!DOCTYPE r [ <!ENTITY e \"v\"> ]>\n<r>\n  text\n  <i>mixed</i>\n  more\n</r>");
    // Not well formed: laid out all the same, nothing lost.
    CHECK_TEXT(FormatXML("</a><b>1 < 2</b><c"), "</a>\n<b>1 < 2</b>\n<c");
    CHECK_TEXT(FormatXML("plain"), "plain");
    CHECK(FormatXML("").empty());
    CHECK_TEXT(FormatXML("<Case><case>x</case></Case>"), "<Case>\n  <case>x</case>\n</Case>");
}

static void TestHTML()
{
    CHECK_TEXT(FormatHTML("<!DOCTYPE html><html><head><title>A page</title><meta charset=utf-8>"
        "<link rel=stylesheet href=a.css></head><body><h1>Title</h1><p>Some <b>bold</b> and <a href=\"/x\">linked</a> text."
        "<p>Second<br>line</p><ul><li>One<li>Two</ul><img src=a.png></body></html>"),
        "<!DOCTYPE html>\n<html>\n  <head>\n    <title>A page</title>\n    <meta charset=utf-8>\n"
        "    <link rel=stylesheet href=a.css>\n  </head>\n  <body>\n    <h1>Title</h1>\n    <p>\n"
        "      Some <b>bold</b> and <a href=\"/x\">linked</a> text.\n    <p>\n      Second<br>\n      line\n    </p>\n"
        "    <ul>\n      <li>\n        One\n      <li>\n        Two\n    </ul>\n    <img src=a.png>\n  </body>\n</html>");
    // Scripts and styles keep their text; only the indentation they share changes.
    CHECK_TEXT(FormatHTML("<div><script>\n        if (a < b) {\n            go('</div>');\n        }\n    </script>"
        "<style>p{color:red}</style></div>"),
        "<div>\n  <script>\n    if (a < b) {\n        go('</div>');\n    }\n  </script>\n  <style>\n    p{color:red}\n  </style>\n</div>");
    CHECK_TEXT(FormatHTML("<DIV CLASS=a><PRE>  keep\n   this </PRE><textarea>\n x </textarea></DIV>"),
        "<DIV CLASS=a>\n  <PRE>  keep\n   this </PRE>\n  <textarea>\n x </textarea>\n</DIV>");
    CHECK_TEXT(FormatHTML("<table><tr><td>1<td>2<tr><td>3</table>"),
        "<table>\n  <tr>\n    <td>\n      1\n    <td>\n      2\n  <tr>\n    <td>\n      3\n</table>");
    CHECK_TEXT(FormatHTML("<div><div><span>a</span> <span>b</span></div><!-- c --></div>"),
        "<div>\n  <div><span>a</span> <span>b</span></div>\n  <!-- c -->\n</div>");
    CHECK_TEXT(FormatHTML("<p>a < b and c > d</p>"), "<p>a < b and c > d</p>");
    CHECK_TEXT(FormatHTML("</div><p>text"), "</div>\n<p>\n  text");
    CHECK_TEXT(FormatHTML("<svg viewBox=\"0 0 1 1\"><path d=\"M0 0\"/><g><circle r=\"1\"/></g></svg>"),
        "<svg viewBox=\"0 0 1 1\">\n  <path d=\"M0 0\"/>\n  <g>\n    <circle r=\"1\"/>\n  </g>\n</svg>");
    CHECK_TEXT(FormatHTML("<select><option>a<option>b</select>"),
        "<select>\n  <option>\n    a\n  <option>\n    b\n</select>");
    CHECK_TEXT(FormatHTML("just text\n  over lines"), "just text over lines");
    CHECK(FormatHTML("").empty());
    // An element too long for a line is opened out.
    const std::string words(150, 'w');
    CHECK_TEXT(FormatHTML("<p>" + words + "</p>"), "<p>\n  " + words + "\n</p>");
    std::string nested;
    for (int i = 0; i < 100; ++i) nested += "<div>";
    nested += "x";
    CHECK(FormatHTML(nested).find(std::string(80, ' ') + "x") != std::string::npos);
    CHECK(FormatHTML(nested).find(std::string(81, ' ')) == std::string::npos);
}

static void TestHelpers()
{
    CHECK(FormatBytes(0) == "0 B" && FormatBytes(1023) == "1023 B" && FormatBytes(1536) == "1.5 KB");
    CHECK(FormatBytes(5 * 1024 * 1024) == "5.00 MB" && FormatBytes(3ull * 1024 * 1024 * 1024) == "3.00 GB");
    CHECK(FormatDuration(0.0123) == "12 ms" && FormatDuration(1.234) == "1.23 s" && FormatDuration(150) == "2.5 min");
    CHECK(FormatDuration(-1).empty() && FormatDuration(0.9996) == "1.00 s");
    std::string out;
    CHECK(DecodeBase64("aGVsbG8=", out) && out == "hello");
    CHECK(DecodeBase64("aGVs\nbG8h", out) && out == "hello!");
    CHECK(DecodeBase64("YQ==", out) && out == "a" && DecodeBase64("YQ", out) && out == "a");
    CHECK(DecodeBase64("", out) && out.empty());
    CHECK(DecodeBase64("_-8=", out) && out == "\xff\xef");
    CHECK(!DecodeBase64("a", out) && !DecodeBase64("a$==", out) && !DecodeBase64("YQ==YQ", out));
    CHECK_TEXT(HexDump(std::string("\x00\x01" "ABCDEFGHIJKLMNOP", 18)),
        "00000000  00 01 41 42 43 44 45 46  47 48 49 4a 4b 4c 4d 4e  ..ABCDEFGHIJKLMN\n"
        "00000010  4f 50                                             OP");
    CHECK(HexDump(std::string(100, 'a'), 32).find("… 68 B more") != std::string::npos);
    CHECK(HexDump("").empty());
    CHECK(ValidUTF8("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80") == "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80");
    CHECK(ValidUTF8("a\xFF" "b") == "a\xEF\xBF\xBD" "b");
    CHECK(ValidUTF8(std::string("a\0b", 3)) == "a\xEF\xBF\xBD" "b");
    CHECK(ValidUTF8("\xC3") == "\xEF\xBF\xBD" && ValidUTF8("\xED\xA0\x80") == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
    CHECK(ValidUTF8("\xC0\xAF") == "\xEF\xBF\xBD\xEF\xBF\xBD");
}

using nlohmann::json;

// A message of a page's agents as the engine sends it: inside one of its target.
static std::string FromTarget(const std::string& target, const json& message)
{
    return json{{"method", "Target.dispatchMessageFromTarget"},
        {"params", {{"targetId", target}, {"message", message.dump()}}}}.dump();
}
static std::string Event(const std::string& target, const std::string& method, const json& parameters)
{
    return FromTarget(target, json{{"method", method}, {"params", parameters}});
}
static std::string TargetCreated(const std::string& target, bool provisional = false, bool paused = false)
{
    json info = {{"targetId", target}, {"type", "page"}};
    if (provisional) info["isProvisional"] = true;
    if (paused) info["isPaused"] = true;
    return json{{"method", "Target.targetCreated"}, {"params", {{"targetInfo", info}}}}.dump();
}
// The commands sent to a target since the last call: (id, method, parameters).
struct Command { uint64_t id; std::string method; json parameters; std::string target; };
static std::vector<Command> Sent(Session& session)
{
    std::vector<Command> commands;
    for (const auto& text : session.TakeOutgoing()) {
        const auto outer = json::parse(text);
        if (outer["method"] != "Target.sendMessageToTarget") {
            commands.push_back({outer["id"], outer["method"], outer["params"], ""});
            continue;
        }
        const auto inner = json::parse(outer["params"]["message"].get<std::string>());
        commands.push_back({inner["id"], inner["method"], inner["params"], outer["params"]["targetId"]});
    }
    return commands;
}
static const Command* Find(const std::vector<Command>& commands, const std::string& method)
{
    for (const auto& command : commands) if (command.method == method) return &command;
    return nullptr;
}
static void Ask(Session& session, const std::string& target, const std::string& id, const std::string& url,
    const std::string& loader, const std::string& type, double time, const json& more = json::object())
{
    json parameters = {{"requestId", id}, {"frameId", "f1"}, {"loaderId", loader}, {"documentURL", "https://example.com/"},
        {"request", {{"url", url}, {"method", "GET"}, {"headers", {{"Accept", "*/*"}}}}}, {"timestamp", time},
        {"walltime", 1790000000.5}, {"initiator", {{"type", "other"}}}, {"type", type}};
    parameters.update(more);
    session.Receive(Event(target, "Network.requestWillBeSent", parameters));
}
static void Respond(Session& session, const std::string& target, const std::string& id, const std::string& loader,
    const std::string& type, int status, const std::string& mime, double time)
{
    session.Receive(Event(target, "Network.responseReceived", {{"requestId", id}, {"frameId", "f1"}, {"loaderId", loader},
        {"timestamp", time}, {"type", type}, {"response", {{"url", "ignored"}, {"status", status}, {"statusText", status == 200 ? "OK" : "Not Found"},
            {"headers", {{"content-type", mime}, {"Cache-Control", "no-store"}}}, {"mimeType", mime}, {"source", "network"},
            {"requestHeaders", {{"User-Agent", "Summit"}, {"Accept", "*/*"}, {"cookie", "a=b"}}}}}}));
}
static void Finish(Session& session, const std::string& target, const std::string& id, double time, uint64_t bytes)
{
    session.Receive(Event(target, "Network.dataReceived", {{"requestId", id}, {"timestamp", time}, {"dataLength", bytes}, {"encodedDataLength", bytes / 2}}));
    session.Receive(Event(target, "Network.loadingFinished", {{"requestId", id}, {"timestamp", time},
        {"metrics", {{"protocol", "h2"}, {"remoteAddress", "93.184.216.34:443"}, {"responseBodyBytesReceived", bytes / 2},
            {"responseHeaderBytesReceived", 100}, {"responseBodyDecodedSize", bytes}}}}));
}

static void TestSession()
{
    Session session;
    session.Start();
    auto commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].method == "Target.setPauseOnStart" && commands[0].parameters["pauseOnStart"] == true);
    session.Receive(TargetCreated("page-1"));
    commands = Sent(session);
    CHECK(Find(commands, "Network.enable") && Find(commands, "Console.enable") && Find(commands, "Page.enable"));
    CHECK(Find(commands, "Network.enable")->target == "page-1" && !Find(commands, "Target.resume"));
    CHECK(!session.TakeChanges().Any());

    // A document, a script and a request a script made.
    Ask(session, "page-1", "0.1", "https://example.com/", "L1", "Document", 10);
    Respond(session, "page-1", "0.1", "L1", "Document", 200, "text/html", 10.1);
    Finish(session, "page-1", "0.1", 10.2, 4000);
    Ask(session, "page-1", "0.2", "https://example.com/js/app.js?v=3#x", "L1", "Script", 10.3,
        {{"initiator", {{"type", "parser"}, {"url", "https://example.com/"}, {"lineNumber", 12}}}});
    Ask(session, "page-1", "0.3", "https://api.example.com/v1/items/", "L1", "Fetch", 10.4,
        {{"request", {{"url", "https://api.example.com/v1/items/"}, {"method", "POST"}, {"headers", {{"Content-Type", "application/json"}}},
            {"postData", "{\"q\":1}"}}},
         {"initiator", {{"type", "script"}, {"stackTrace", {{"callFrames", {
            {{"functionName", "fetch"}, {"url", "[native code]"}, {"lineNumber", 0}, {"columnNumber", 0}, {"scriptId", "0"}},
            {{"functionName", "load"}, {"url", "https://example.com/js/app.js"}, {"lineNumber", 40}, {"columnNumber", 9}, {"scriptId", "1"}}}}}}}}});
    auto changes = session.TakeChanges();
    CHECK(!changes.requestsReset && changes.requests.size() == 3);
    CHECK(session.Requests().size() == 3);
    const auto& document = session.Requests()[0];
    CHECK(document.Name() == "example.com" && document.Host() == "example.com" && document.StatusLabel() == "200");
    CHECK(document.finished && !document.Pending() && !document.IsError() && document.size == 4000 && document.transferred == 2100);
    CHECK(document.protocol == "h2" && document.remoteAddress == "93.184.216.34:443" && document.mimeType == "text/html");
    CHECK(std::abs(document.Duration() - 0.2) < 1e-9);
    // Headers are in the order of their names; the request's are what was sent.
    CHECK(document.requestHeaders.size() == 3 && document.requestHeaders[0].name == "Accept"
        && document.requestHeaders[1].name == "cookie" && document.requestHeaders[2].name == "User-Agent");
    CHECK(document.responseHeaders.size() == 2 && document.responseHeaders[0].name == "Cache-Control");
    const auto& script = session.Requests()[1];
    CHECK(script.Name() == "app.js?v=3" && script.StatusLabel() == "(pending)" && script.Pending() && script.initiator == "example.com:12");
    const auto& fetch = session.Requests()[2];
    CHECK(fetch.Name() == "items" && fetch.Host() == "api.example.com" && fetch.method == "POST");
    CHECK(fetch.hasRequestBody && fetch.requestBody == "{\"q\":1}" && fetch.initiator == "app.js:40");

    // The document's body was asked for when it was complete.
    commands = Sent(session);
    const auto* body = Find(commands, "Network.getResponseBody");
    CHECK(body && body->parameters["requestId"] == "0.1" && body->target == "page-1");
    CHECK(session.Requests()[0].bodyState == BodyState::Waiting);
    session.Receive(FromTarget("page-1", {{"id", body->id}, {"result", {{"body", "<html></html>"}, {"base64Encoded", false}}}}));
    changes = session.TakeChanges();
    CHECK(changes.bodies.size() == 1 && changes.bodies[0] == document.serial);
    CHECK(document.bodyState == BodyState::Loaded && document.body == "<html></html>");
    session.RequestBody(document.serial);
    CHECK(Sent(session).empty());

    // An image's is asked for when it is wanted, and may come in base64.
    Ask(session, "page-1", "0.4", "https://example.com/a.png", "L1", "Image", 10.5);
    Respond(session, "page-1", "0.4", "L1", "Image", 200, "image/png", 10.6);
    Finish(session, "page-1", "0.4", 10.7, 10);
    CHECK(Sent(session).empty());
    const auto image = session.Requests()[3].serial;
    session.RequestBody(image);
    commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].method == "Network.getResponseBody");
    session.Receive(FromTarget("page-1", {{"id", commands[0].id}, {"result", {{"body", "iVBORw=="}, {"base64Encoded", true}}}}));
    CHECK(session.FindRequest(image)->body == "\x89PNG" && session.FindRequest(image)->bodyState == BodyState::Loaded);

    // Failures.
    Respond(session, "page-1", "0.3", "L1", "Fetch", 404, "application/json", 10.8);
    Finish(session, "page-1", "0.3", 10.9, 20);
    commands = Sent(session);
    CHECK(commands.size() == 1);
    session.Receive(FromTarget("page-1", {{"id", commands[0].id}, {"error", {{"code", -32000}, {"message", "No resource with given identifier found"}}}}));
    CHECK(session.Requests()[2].bodyState == BodyState::Failed && session.Requests()[2].bodyError == "No resource with given identifier found");
    CHECK(session.Requests()[2].IsError() && session.Requests()[2].StatusLabel() == "404");
    session.Receive(Event("page-1", "Network.loadingFailed", {{"requestId", "0.2"}, {"timestamp", 11}, {"errorText", "Load cancelled"}, {"canceled", true}}));
    CHECK(script.failed && script.canceled && script.StatusLabel() == "(canceled)" && !script.IsError() && !script.Pending());
    session.TakeChanges();

    // A redirect ends one entry and starts the next.
    Ask(session, "page-1", "0.5", "http://example.com/old", "L1", "XHR", 12);
    Ask(session, "page-1", "0.5", "https://example.com/new", "L1", "", 12.1,
        {{"redirectResponse", {{"url", "http://example.com/old"}, {"status", 301}, {"statusText", "Moved Permanently"},
            {"headers", {{"Location", "https://example.com/new"}}}, {"mimeType", "text/html"}, {"source", "network"}}}, {"type", nullptr}});
    CHECK(session.Requests().size() == 6);
    const auto& redirect = session.Requests()[4];
    CHECK(redirect.redirected && redirect.finished && redirect.status == 301 && redirect.responseHeaders[0].value == "https://example.com/new");
    CHECK(session.Requests()[5].url == "https://example.com/new" && session.Requests()[5].type == "XHR" && session.Requests()[5].Pending());
    session.RequestBody(redirect.serial);
    CHECK(redirect.bodyState == BodyState::Failed && Sent(session).empty());
    Respond(session, "page-1", "0.5", "L1", "XHR", 200, "application/json", 12.2);
    CHECK(session.Requests()[5].status == 200 && redirect.status == 301);
    changes = session.TakeChanges();
    CHECK(changes.requests.size() == 2 && changes.requests[0] == redirect.serial);

    // What the memory cache served, and a response to a request never seen.
    session.Receive(Event("page-1", "Network.requestServedFromMemoryCache", {{"requestId", "0.6"}, {"frameId", "f1"}, {"loaderId", "L1"},
        {"documentURL", "https://example.com/"}, {"timestamp", 13}, {"initiator", {{"type", "parser"}}},
        {"resource", {{"url", "https://example.com/style.css"}, {"type", "StyleSheet"}, {"bodySize", 900},
            {"response", {{"url", "https://example.com/style.css"}, {"status", 200}, {"statusText", "OK"}, {"headers", json::object()},
                {"mimeType", "text/css"}, {"source", "memory-cache"}}}}}}));
    CHECK(session.Requests().back().finished && session.Requests().back().source == "memory-cache" && session.Requests().back().size == 900);
    Respond(session, "page-1", "0.99", "L1", "Image", 200, "image/gif", 13.5);
    CHECK(session.Requests().back().url == "ignored" && session.Requests().back().status == 200);
    CHECK(session.Requests().size() == 8);
    CHECK(SummarizeRequests(session.Requests(), 8).find("8 requests") == 0);
    CHECK(SummarizeRequests(session.Requests(), 2).find("2 of 8 requests") == 0);
    CHECK(SummarizeRequests(session.Requests(), 8).find("1 failed") != std::string::npos);
    session.TakeChanges();
    Sent(session);

    // Navigation in the same process: the new document's requests remain.
    Ask(session, "page-1", "0.7", "https://example.com/next", "L2", "Document", 20);
    session.Receive(Event("page-1", "Page.frameNavigated", {{"frame", {{"id", "f1"}, {"loaderId", "L2"}, {"url", "https://example.com/next"},
        {"securityOrigin", "https://example.com"}, {"mimeType", "text/html"}}}}));
    changes = session.TakeChanges();
    CHECK(changes.requestsReset && session.Requests().size() == 1 && session.Requests()[0].url == "https://example.com/next");
    CHECK(session.PageURL() == "https://example.com/next");
    // A frame inside the page is not the page.
    session.Receive(Event("page-1", "Page.frameNavigated", {{"frame", {{"id", "f2"}, {"parentId", "f1"}, {"loaderId", "L3"}, {"url", "https://ads.example/"}}}}));
    CHECK(session.Requests().size() == 1 && session.PageURL() == "https://example.com/next");
    Respond(session, "page-1", "0.7", "L2", "Document", 200, "text/html", 20.1);
    CHECK(session.Requests()[0].status == 200);

    // Kept logs.
    session.SetPreserveRequests(true);
    commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].method == "Network.setClearResourceDataOnNavigate"
        && commands[0].parameters["clearResourceDataOnNavigate"] == false);
    Ask(session, "page-1", "0.8", "https://example.com/third", "L4", "Document", 30);
    session.Receive(Event("page-1", "Page.frameNavigated", {{"frame", {{"id", "f1"}, {"loaderId", "L4"}, {"url", "https://example.com/third"}}}}));
    CHECK(session.Requests().size() == 2 && !session.TakeChanges().requestsReset);

    // A navigation to another process: a provisional target that is paused
    // until its requests are recorded, then takes the page over.
    session.Receive(TargetCreated("page-2", true, true));
    commands = Sent(session);
    CHECK(Find(commands, "Network.enable") && Find(commands, "Network.enable")->target == "page-2");
    CHECK(Find(commands, "Network.setClearResourceDataOnNavigate") && Find(commands, "Target.resume"));
    CHECK(Find(commands, "Target.resume")->parameters["targetId"] == "page-2" && commands.back().method == "Target.resume");
    Ask(session, "page-2", "0.1", "https://other.example/", "L1", "Document", 1);
    session.Receive(json{{"method", "Target.didCommitProvisionalTarget"}, {"params", {{"oldTargetId", "page-1"}, {"newTargetId", "page-2"}}}}.dump());
    session.Receive(json{{"method", "Target.targetDestroyed"}, {"params", {{"targetId", "page-1"}}}}.dump());
    CHECK(session.Requests().size() == 3);
    // The old page's unfinished request will never finish, and its bodies are gone.
    CHECK(session.Requests()[1].failed && session.Requests()[1].canceled);
    // The new page's frames are asked for, for the Storage panel.
    commands = Sent(session);
    CHECK(Find(commands, "Page.getResourceTree") && Find(commands, "Page.getResourceTree")->target == "page-2");
    session.RequestBody(session.Requests()[0].serial);
    CHECK(session.Requests()[0].bodyState == BodyState::Failed && Sent(session).empty());
    session.SetPreserveRequests(false);
    Sent(session);
    session.TakeChanges();
    session.Receive(TargetCreated("page-3", true, true));
    Ask(session, "page-3", "0.1", "https://third.example/", "L1", "Document", 1);
    Ask(session, "page-2", "0.2", "https://other.example/late.js", "L1", "Script", 2);
    session.Receive(json{{"method", "Target.didCommitProvisionalTarget"}, {"params", {{"oldTargetId", "page-2"}, {"newTargetId", "page-3"}}}}.dump());
    CHECK(session.TakeChanges().requestsReset && session.Requests().size() == 1 && session.Requests()[0].url == "https://third.example/");
    // The same request identifier in another process is another request.
    Respond(session, "page-3", "0.1", "L1", "Document", 200, "text/html", 1.5);
    CHECK(session.Requests()[0].status == 200);

    session.ClearRequests();
    CHECK(session.Requests().empty() && session.TakeChanges().requestsReset);
    CHECK(SummarizeRequests(session.Requests(), 0).find("No requests") == 0);
    Finish(session, "page-3", "0.1", 3, 10);
    CHECK(session.Requests().empty());

    // Nothing that is not the protocol does harm.
    session.Receive("");
    session.Receive("not json");
    session.Receive("[1,2]");
    session.Receive("{\"method\":5,\"params\":\"x\"}");
    session.Receive(Event("page-3", "Network.requestWillBeSent", "text"));
    session.Receive(Event("page-3", "Network.responseReceived", {{"requestId", 5}, {"response", 7}}));
    session.Receive(json{{"method", "Target.dispatchMessageFromTarget"}, {"params", {{"targetId", "page-3"}, {"message", "{broken"}}}}.dump());
    session.Receive(FromTarget("page-3", {{"id", 99999}, {"result", json::object()}}));
    CHECK(session.Requests().empty());

    // More requests than are kept: the oldest go.
    Session many;
    many.Receive(TargetCreated("p"));
    for (size_t i = 0; i < Session::kMaximumRequests + 10; ++i)
        Ask(many, "p", "r" + std::to_string(i), "https://example.com/" + std::to_string(i), "L", "Image", 1);
    CHECK(many.Requests().size() == Session::kMaximumRequests && many.Requests().front().url == "https://example.com/10");
    CHECK(many.TakeChanges().requestsReset);
    CHECK(many.FindRequest(many.Requests()[100].serial) == &many.Requests()[100] && !many.FindRequest(1));
}

static json Message(const std::string& level, const std::string& text, const json& more = json::object())
{
    json message = {{"source", "console-api"}, {"level", level}, {"text", text}, {"type", "log"}, {"timestamp", 1790000000.25}};
    message.update(more);
    return message;
}
static json Value(const json& value)
{
    if (value.is_string()) return {{"type", "string"}, {"value", value}};
    if (value.is_number()) return {{"type", "number"}, {"value", value}, {"description", value.dump()}};
    if (value.is_boolean()) return {{"type", "boolean"}, {"value", value}};
    return {{"type", "object"}, {"subtype", "null"}, {"value", nullptr}};
}

static void TestConsole()
{
    CHECK(ConsoleMessageText(Message("log", "plain").dump()) == "plain");
    CHECK(ConsoleMessageText(Message("log", "a", {{"parameters", {Value("a"), Value(1), Value(true), Value(nullptr), {{"type", "undefined"}}}}}).dump())
        == "a 1 true null undefined");
    CHECK(ConsoleMessageText(Message("log", "x", {{"parameters", {Value("%s is %d years, %i%% %o %c styled %f"), Value("Ann"), Value(41.9),
        Value(7), Value("q"), Value("color: red"), Value(1.5), Value("extra")}}}).dump()) == "Ann is 41 years, 7% \"q\"  styled 1.5 extra");
    CHECK(ConsoleMessageText(Message("log", "x", {{"parameters", {Value("100% done %s %d")}}}).dump()) == "100% done %s %d");
    CHECK(ConsoleMessageText(Message("log", "x", {{"parameters", {Value(5), Value("five")}}}).dump()) == "5 five");
    const json object = {{"type", "object"}, {"className", "Object"}, {"description", "Object"}, {"objectId", "{}"}, {"preview",
        {{"type", "object"}, {"description", "Object"}, {"lossless", false}, {"overflow", true}, {"properties", {
            {{"name", "id"}, {"type", "number"}, {"value", "7"}},
            {{"name", "name"}, {"type", "string"}, {"value", "Ann \"A\""}},
            {{"name", "tags"}, {"type", "object"}, {"subtype", "array"}, {"value", "Array"}, {"valuePreview",
                {{"type", "object"}, {"subtype", "array"}, {"description", "Array"}, {"lossless", true}, {"size", 2}, {"properties", {
                    {{"name", "0"}, {"type", "string"}, {"value", "a"}}, {{"name", "1"}, {"type", "number"}, {"value", "2"}}}}}}},
            {{"name", "none"}, {"type", "object"}, {"subtype", "null"}, {"value", "null"}},
            {{"name", "run"}, {"type", "function"}, {"value", ""}},
            {{"name", "child"}, {"type", "object"}, {"value", "Point"}}}}}}};
    CHECK_TEXT(ConsoleMessageText(Message("log", "x", {{"parameters", {Value("user"), object}}}).dump()),
        "user {id: 7, name: \"Ann \\\"A\\\"\", tags: [\"a\", 2], none: null, run: function, child: Point, …}");
    const json array = {{"type", "object"}, {"subtype", "array"}, {"description", "Array"}, {"size", 100}, {"preview",
        {{"type", "object"}, {"subtype", "array"}, {"description", "Array"}, {"lossless", false}, {"overflow", true}, {"size", 100},
            {"properties", {{{"name", "0"}, {"type", "number"}, {"value", "1"}}, {{"name", "1"}, {"type", "number"}, {"value", "2"}}}}}}};
    CHECK_TEXT(ConsoleMessageText(Message("log", "x", {{"parameters", {array}}}).dump()), "(100) [1, 2, …]");
    const json map = {{"type", "object"}, {"subtype", "map"}, {"description", "Map"}, {"preview",
        {{"type", "object"}, {"subtype", "map"}, {"description", "Map"}, {"lossless", true}, {"entries", {
            {{"key", {{"type", "string"}, {"description", "k"}, {"lossless", true}}}, {"value", {{"type", "number"}, {"description", "1"}, {"lossless", true}}}}}}}}};
    CHECK_TEXT(ConsoleMessageText(Message("log", "x", {{"parameters", {map}}}).dump()), "Map {\"k\" => 1}");
    const json error = {{"type", "object"}, {"subtype", "error"}, {"className", "TypeError"}, {"description", "TypeError: x is not a function"},
        {"preview", {{"type", "object"}, {"subtype", "error"}, {"description", "TypeError: x is not a function"}, {"lossless", false}}}};
    CHECK_TEXT(ConsoleMessageText(Message("error", "x", {{"parameters", {error}}}).dump()), "TypeError: x is not a function");
    CHECK(ConsoleMessageText(Message("log", "x", {{"parameters", {{{"type", "function"}, {"description", "function f() {\n  return 1;\n}"}}}}}).dump())
        == "function f() {…");
    CHECK(ConsoleMessageText(Message("error", "was false", {{"type", "assert"}}).dump()) == "Assertion failed: was false");
    CHECK(ConsoleMessageText("{broken").empty());

    Session session;
    session.Start();
    session.Receive(TargetCreated("page-1"));
    Sent(session);
    const json stack = {{"callFrames", {
        {{"functionName", "inner"}, {"url", "https://example.com/js/app.js"}, {"scriptId", "1"}, {"lineNumber", 12}, {"columnNumber", 5}},
        {{"functionName", ""}, {"url", "https://example.com/js/app.js"}, {"scriptId", "1"}, {"lineNumber", 30}, {"columnNumber", 1}},
        {{"functionName", "forEach"}, {"url", "[native code]"}, {"scriptId", "0"}, {"lineNumber", 0}, {"columnNumber", 0}}}}};
    session.Receive(Event("page-1", "Console.messageAdded", {{"message", Message("debug", "fine detail")}}));
    session.Receive(Event("page-1", "Console.messageAdded", {{"message", Message("log", "hello", {{"url", "https://example.com/js/app.js"}, {"line", 3}, {"column", 9}})}}));
    session.Receive(Event("page-1", "Console.messageAdded", {{"message", Message("info", "note")}}));
    session.Receive(Event("page-1", "Console.messageAdded", {{"message", Message("warning", "careful", {{"stackTrace", stack}})}}));
    session.Receive(Event("page-1", "Console.messageAdded", {{"message", Message("error", "TypeError: no", {{"source", "javascript"}, {"stackTrace", stack}})}}));
    auto changes = session.TakeChanges();
    CHECK(changes.consoleAdded == 5 && !changes.consoleReset && !changes.consoleLastChanged);
    const auto& entries = session.Entries();
    CHECK(entries.size() == 5 && entries[0].level == Level::Debug && entries[1].level == Level::Log && entries[2].level == Level::Info
        && entries[3].level == Level::Warning && entries[4].level == Level::Error);
    CHECK(entries[1].Location() == "app.js:3:9" && entries[1].stack.empty() && entries[1].time == 1790000000.25);
    CHECK_TEXT(entries[4].stack, "inner (app.js:12:5)\n(anonymous) (app.js:30:1)\nforEach (native code)");
    // Without a place of its own, a message is where its first frame is.
    CHECK(entries[3].Location() == "app.js:12:5" && entries[4].source == "javascript" && entries[0].Location().empty());

    LevelFilter filter;
    CHECK(filter.Shows(entries[0]) && filter.Shows(entries[4]));
    filter.info = false;
    CHECK(!filter.Shows(entries[1]) && !filter.Shows(entries[2]) && filter.Shows(entries[0]) && filter.Shows(entries[3]));
    filter = { };
    filter.debug = filter.errors = filter.warnings = false;
    CHECK(!filter.Shows(entries[0]) && filter.Shows(entries[1]) && !filter.Shows(entries[3]) && !filter.Shows(entries[4]));

    // The same message again is counted, not repeated.
    session.Receive(Event("page-1", "Console.messageRepeatCountUpdated", {{"count", 3}, {"timestamp", 1790000001}}));
    changes = session.TakeChanges();
    CHECK(entries.back().repeat == 3 && changes.consoleLastChanged && !changes.consoleAdded && !changes.consoleReset);
    session.Receive(Event("page-1", "Console.messageAdded", {{"message", Message("log", "again")}}));
    session.Receive(Event("page-1", "Console.messageRepeatCountUpdated", {{"count", 2}}));
    changes = session.TakeChanges();
    CHECK(entries.back().repeat == 2 && changes.consoleAdded == 1 && !changes.consoleLastChanged && !changes.consoleReset);

    // Groups stand in.
    session.Receive(Event("page-1", "Console.messageAdded", {{"message", Message("log", "Group", {{"type", "startGroup"}})}}));
    session.Receive(Event("page-1", "Console.messageAdded", {{"message", Message("log", "two\nlines")}}));
    session.Receive(Event("page-1", "Console.messageAdded", {{"message", Message("log", "", {{"type", "endGroup"}})}}));
    session.Receive(Event("page-1", "Console.messageAdded", {{"message", Message("log", "after")}}));
    CHECK(entries.size() == 9 && entries[6].text == "Group" && entries[7].text == "  two\n  lines" && entries[8].text == "after");

    // An expression typed into the console.
    session.TakeChanges();
    session.Evaluate("1 + 1");
    auto commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].method == "Runtime.evaluate" && commands[0].parameters["expression"] == "1 + 1"
        && commands[0].target == "page-1");
    session.Receive(FromTarget("page-1", {{"id", commands[0].id}, {"result", {{"result", Value(2)}, {"wasThrown", false}}}}));
    CHECK(entries.size() == 11 && entries[9].kind == EntryKind::Input && entries[9].text == "1 + 1"
        && entries[10].kind == EntryKind::Result && entries[10].text == "2" && entries[10].level == Level::Log);
    session.Evaluate("'a' +");
    commands = Sent(session);
    session.Receive(FromTarget("page-1", {{"id", commands[0].id}, {"result", {{"result", error}, {"wasThrown", true}}}}));
    CHECK(entries.back().level == Level::Error && entries.back().text == "TypeError: x is not a function");
    session.Evaluate("'text'");
    commands = Sent(session);
    session.Receive(FromTarget("page-1", {{"id", commands[0].id}, {"result", {{"result", Value("text")}, {"wasThrown", false}}}}));
    CHECK(entries.back().text == "\"text\"");
    // Results are shown whatever levels are.
    filter = { };
    filter.info = filter.errors = false;
    CHECK(filter.Shows(entries.back()) && filter.Shows(entries[9]));

    // Navigation clears the console, unless it is kept.
    session.TakeChanges();
    session.Receive(Event("page-1", "Console.messagesCleared", {{"reason", "main-frame-navigation"}}));
    changes = session.TakeChanges();
    CHECK(entries.empty() && changes.consoleReset);
    session.Receive(Event("page-1", "Page.frameNavigated", {{"frame", {{"id", "f1"}, {"loaderId", "L2"}, {"url", "https://example.com/two"}}}}));
    CHECK(entries.empty());
    session.Receive(Event("page-1", "Console.messageAdded", {{"message", Message("log", "kept")}}));
    session.SetPreserveConsole(true);
    session.Receive(Event("page-1", "Console.messagesCleared", {{"reason", "main-frame-navigation"}}));
    session.Receive(Event("page-1", "Page.frameNavigated", {{"frame", {{"id", "f1"}, {"loaderId", "L3"}, {"url", "https://example.com/three"}}}}));
    CHECK(entries.size() == 2 && entries[0].text == "kept" && entries[1].kind == EntryKind::Navigation
        && entries[1].text == "Navigated to https://example.com/three");
    session.Receive(Event("page-1", "Console.messagesCleared", {{"reason", "console-api"}}));
    CHECK(entries.size() == 3 && entries[2].level == Level::Info);
    session.Receive(TargetCreated("page-2", true, true));
    session.Receive(json{{"method", "Target.didCommitProvisionalTarget"}, {"params", {{"oldTargetId", "page-1"}, {"newTargetId", "page-2"}}}}.dump());
    CHECK(entries.size() == 3);
    Sent(session);
    session.Evaluate("2");
    commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].target == "page-2");
    session.SetPreserveConsole(false);
    session.Receive(Event("page-2", "Console.messagesCleared", {{"reason", "console-api"}}));
    CHECK(entries.empty());
    session.Receive(Event("page-2", "Console.messageAdded", {{"message", Message("log", "x")}}));
    session.Receive(TargetCreated("page-3", true, false));
    session.Receive(json{{"method", "Target.didCommitProvisionalTarget"}, {"params", {{"oldTargetId", "page-2"}, {"newTargetId", "page-3"}}}}.dump());
    CHECK(entries.empty());
    session.Receive(Event("page-3", "Console.messageAdded", {{"message", Message("log", "y")}}));
    session.TakeChanges();
    session.ClearConsole();
    CHECK(entries.empty() && session.TakeChanges().consoleReset);
    session.Receive(Event("page-3", "Console.messagesCleared", {{"reason", "frontend"}}));
    CHECK(!session.TakeChanges().Any());

    // Without a page, an expression is answered at once.
    Session empty;
    empty.Evaluate("1");
    CHECK(empty.Entries().size() == 2 && empty.Entries()[1].level == Level::Error && empty.TakeOutgoing().empty());

    Session many;
    many.Receive(TargetCreated("p"));
    for (size_t i = 0; i < Session::kMaximumEntries + 5; ++i)
        many.Receive(Event("p", "Console.messageAdded", {{"message", Message("log", std::to_string(i))}}));
    CHECK(many.Entries().size() == Session::kMaximumEntries && many.Entries().front().text == "5" && many.TakeChanges().consoleReset);
}

// The Console's object tree: objects of messages and results, and their
// properties as the page answers.
static void TestObjectTree()
{
    Session session;
    session.Start();
    session.Receive(TargetCreated("page-1"));
    Sent(session);
    const json point = {{"type", "object"}, {"className", "Object"}, {"description", "Object"}, {"objectId", "{\"id\":7}"}};
    session.Receive(Event("page-1", "Console.messageAdded", {{"message", Message("log", "x",
        {{"parameters", {Value("point"), point, Value(nullptr), {{"type", "object"}, {"subtype", "null"}, {"objectId", "{\"id\":8}"}}}}})}}));
    const auto& entries = session.Entries();
    CHECK(entries.size() == 1 && entries[0].objects.size() == 1 && entries[0].objects[0].objectId == "{\"id\":7}"
        && entries[0].objects[0].label == "Object");
    session.TakeChanges();

    session.LoadProperties("{\"id\":7}");
    auto commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].method == "Runtime.getDisplayableProperties"
        && commands[0].parameters["objectId"] == "{\"id\":7}" && commands[0].target == "page-1");
    // Asked once.
    session.LoadProperties("{\"id\":7}");
    CHECK(Sent(session).empty());
    const auto* properties = session.Properties("{\"id\":7}");
    CHECK(properties && !properties->loaded);
    session.Receive(FromTarget("page-1", {{"id", commands[0].id}, {"result", {
        {"properties", {
            {{"name", "x"}, {"value", Value(3)}, {"isOwn", true}},
            {{"name", "child"}, {"value", {{"type", "object"}, {"description", "Point"}, {"objectId", "{\"id\":9}"}}}, {"isOwn", true}},
            {{"name", "size"}, {"get", {{"type", "function"}, {"objectId", "{\"id\":10}"}}}}}},
        {"internalProperties", {{{"name", "Prototype"}, {"value", {{"type", "object"}, {"description", "Object"}, {"objectId", "{\"id\":11}"}}}}}}}}}));
    auto changes = session.TakeChanges();
    CHECK(changes.properties.size() == 1 && changes.properties[0] == "{\"id\":7}" && changes.Any());
    properties = session.Properties("{\"id\":7}");
    CHECK(properties && properties->loaded && properties->error.empty() && properties->items.size() == 4);
    CHECK(properties->items[0].name == "x" && properties->items[0].value == "3" && properties->items[0].objectId.empty());
    CHECK(properties->items[1].name == "child" && properties->items[1].value == "Point" && properties->items[1].objectId == "{\"id\":9}");
    CHECK(properties->items[2].name == "size" && properties->items[2].value == "(…)" && properties->items[2].objectId.empty());
    CHECK(properties->items[3].name == "[[Prototype]]" && properties->items[3].objectId == "{\"id\":11}");

    // An object the page no longer has.
    session.LoadProperties("{\"id\":99}");
    commands = Sent(session);
    session.Receive(FromTarget("page-1", {{"id", commands[0].id}, {"error", {{"message", "Could not find object with given id"}}}}));
    // Asked again for its own properties (what a scope object answers).
    commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].method == "Runtime.getProperties");
    properties = session.Properties("{\"id\":99}");
    CHECK(properties && !properties->loaded);
    session.Receive(FromTarget("page-1", {{"id", commands[0].id}, {"error", {{"message", "Could not find object with given id"}}}}));
    properties = session.Properties("{\"id\":99}");
    CHECK(properties && properties->loaded && !properties->error.empty() && properties->items.empty());

    // A result of the console's input has its object too.
    session.Evaluate("({a: 1})");
    commands = Sent(session);
    session.Receive(FromTarget("page-1", {{"id", commands[0].id}, {"result", {{"result", point}, {"wasThrown", false}}}}));
    CHECK(entries.back().kind == EntryKind::Result && entries.back().objects.size() == 1);
    // Clearing the console forgets what was looked into.
    session.ClearConsole();
    CHECK(!session.Properties("{\"id\":7}"));

    // Storage: a cookie is set as given; a session one has no expiry.
    Sent(session);
    Cookie cookie;
    cookie.name = "theme";
    cookie.value = "dark";
    cookie.domain = "example.com";
    cookie.session = true;
    cookie.secure = true;
    session.SetCookie(cookie);
    commands = Sent(session);
    const auto* set = Find(commands, "Page.setCookie");
    CHECK(set && set->parameters["cookie"]["name"] == "theme" && set->parameters["cookie"]["value"] == "dark"
        && set->parameters["cookie"]["path"] == "/" && set->parameters["cookie"]["session"] == true
        && set->parameters["cookie"]["expires"] == 0 && set->parameters["cookie"]["sameSite"] == "Lax"
        && !set->parameters["cookie"].contains("partitionKey"));
    cookie.name.clear();
    session.SetCookie(cookie);
    CHECK(!Find(Sent(session), "Page.setCookie"));

    // A WebSocket and its messages.
    session.TakeChanges();
    session.Receive(Event("page-1", "Network.webSocketCreated", {{"requestId", "ws1"}, {"url", "wss://example.com/live"}}));
    session.Receive(Event("page-1", "Network.webSocketWillSendHandshakeRequest", {{"requestId", "ws1"}, {"timestamp", 100.0},
        {"walltime", 1790000000.0}, {"request", {{"headers", {{"Upgrade", "websocket"}}}}}}));
    session.Receive(Event("page-1", "Network.webSocketHandshakeResponseReceived", {{"requestId", "ws1"}, {"timestamp", 100.1},
        {"response", {{"status", 101}, {"statusText", "Switching Protocols"}, {"headers", {{"Upgrade", "websocket"}}}}}}));
    session.Receive(Event("page-1", "Network.webSocketFrameSent", {{"requestId", "ws1"}, {"timestamp", 100.5},
        {"response", {{"opcode", 1}, {"mask", true}, {"payloadData", "hello"}, {"payloadLength", 5}}}}));
    session.Receive(Event("page-1", "Network.webSocketFrameReceived", {{"requestId", "ws1"}, {"timestamp", 101.25},
        {"response", {{"opcode", 2}, {"mask", false}, {"payloadData", "AAEC"}, {"payloadLength", 3}}}}));
    session.Receive(Event("page-1", "Network.webSocketFrameError", {{"requestId", "ws1"}, {"timestamp", 102.0}, {"errorMessage", "bad frame"}}));
    session.Receive(Event("page-1", "Network.webSocketClosed", {{"requestId", "ws1"}, {"timestamp", 103.0}}));
    {
        const auto changes = session.TakeChanges();
        CHECK(!changes.requests.empty() && changes.bodies.size() == 3);
        const auto& socket = session.Requests().back();
        CHECK(socket.webSocket && socket.type == "WebSocket" && socket.url == "wss://example.com/live" && socket.status == 101
            && socket.finished && socket.frames.size() == 3 && socket.size == 8 && socket.requestHeaders.size() == 1);
        CHECK_TEXT(DescribeFrames(socket).text, "↑ +0.500 s  hello\n↓ +1.250 s  (binary, 3 bytes)\n✕ +2.000 s  bad frame");
    }
    // IndexedDB: an origin's databases, their stores, then a store's entries.
    Sent(session);
    session.LoadDatabases("https://example.com");
    commands = Sent(session);
    const auto* names = Find(commands, "IndexedDB.requestDatabaseNames");
    CHECK(Find(commands, "IndexedDB.enable") && names && names->parameters["securityOrigin"] == "https://example.com");
    session.Receive(FromTarget("page-1", {{"id", names->id}, {"result", {{"databaseNames", {"app", "cache"}}}}}));
    commands = Sent(session);
    CHECK(commands.size() == 2 && commands[0].method == "IndexedDB.requestDatabase" && commands[0].parameters["databaseName"] == "app");
    CHECK(session.Databases("https://example.com") && !session.Databases("https://example.com")->loaded);
    session.Receive(FromTarget("page-1", {{"id", commands[0].id}, {"result", {{"databaseWithObjectStores", {{"name", "app"}, {"version", 3},
        {"objectStores", {{{"name", "notes"}, {"keyPath", {{"type", "string"}, {"string", "id"}}}, {"autoIncrement", false}, {"indexes", json::array()}}}}}}}}}));
    session.Receive(FromTarget("page-1", {{"id", commands[1].id}, {"result", {{"databaseWithObjectStores", {{"name", "cache"}, {"version", 1},
        {"objectStores", json::array()}}}}}}));
    {
        const auto* databases = session.Databases("https://example.com");
        CHECK(databases && databases->loaded && databases->databases.size() == 2 && databases->databases[0].stores == std::vector<std::string> {"notes"}
            && databases->databases[0].version == 3 && session.TakeChanges().origins);
    }
    const auto store = IndexedDBStore("https://example.com", "app", "notes");
    session.LoadStorage(StorageKind::IndexedDB, store);
    commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].method == "IndexedDB.requestData" && commands[0].parameters["objectStoreName"] == "notes"
        && commands[0].parameters["databaseName"] == "app" && commands[0].parameters["pageSize"] == Session::kIndexedDBPage);
    session.Receive(FromTarget("page-1", {{"id", commands[0].id}, {"result", {{"objectStoreDataEntries", {
        {{"key", Value(1)}, {"primaryKey", Value(1)}, {"value", Value("first note")}}}}, {"hasMore", true}}}}));
    {
        const auto* area = session.FindArea(StorageKind::IndexedDB, store);
        CHECK(area && area->loaded && area->items.size() == 1 && area->items[0].key == "1" && area->items[0].value == "\"first note\""
            && !area->error.empty());
    }
    session.ClearStorage(StorageKind::IndexedDB, store);
    commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].method == "IndexedDB.clearObjectStore" && commands[0].parameters["objectStoreName"] == "notes");

    // Network: Disable cache, blocking and replay.
    Sent(session);
    session.SetCacheDisabled(true);
    commands = Sent(session);
    CHECK(Find(commands, "Network.setResourceCachingDisabled") && Find(commands, "Network.setResourceCachingDisabled")->parameters["disabled"] == true);
    session.BlockURL("ads.example");
    commands = Sent(session);
    CHECK(Find(commands, "Network.setInterceptionEnabled") && Find(commands, "Network.addInterception")
        && Find(commands, "Network.addInterception")->parameters["url"] == "ads.example" && session.BlockedURLs().size() == 1);
    session.Receive(Event("page-1", "Network.requestIntercepted", {{"requestId", "r9"}, {"request", {{"url", "https://ADS.example/x.js"}}}}));
    session.Receive(Event("page-1", "Network.requestIntercepted", {{"requestId", "r10"}, {"request", {{"url", "https://example.com/app.js"}}}}));
    commands = Sent(session);
    CHECK(Find(commands, "Network.interceptRequestWithError") && Find(commands, "Network.interceptRequestWithError")->parameters["requestId"] == "r9"
        && Find(commands, "Network.interceptContinue") && Find(commands, "Network.interceptContinue")->parameters["requestId"] == "r10");
    session.UnblockAll();
    commands = Sent(session);
    CHECK(Find(commands, "Network.removeInterception") && session.BlockedURLs().empty());
    // A new process of the page is set up the same way.
    session.Receive(TargetCreated("page-2"));
    commands = Sent(session);
    CHECK(Find(commands, "Network.setResourceCachingDisabled") && !Find(commands, "Network.addInterception"));
    session.SetCacheDisabled(false);
    Sent(session);
    // Replay: the request again with fetch() from the page, without the headers a page cannot set.
    session.Receive(Event("page-2", "Network.requestWillBeSent", {{"requestId", "p1"}, {"loaderId", "l"}, {"frameId", "f"},
        {"documentURL", "https://example.com/"}, {"timestamp", 1.0}, {"type", "Fetch"},
        {"request", {{"url", "https://example.com/api"}, {"method", "POST"}, {"postData", "{\"q\":1}"},
            {"headers", {{"Content-Type", "application/json"}, {"Cookie", "a=b"}, {"X-Token", "t"}}}}}}));
    session.TakeChanges();
    const auto serial = session.Requests().back().serial;
    CHECK(session.Replay(serial));
    commands = Sent(session);
    {
        const auto* replay = Find(commands, "Runtime.evaluate");
        const auto expression = replay ? replay->parameters["expression"].get<std::string>() : std::string();
        CHECK(replay && replay->target == "page-2" && expression.find("fetch(\"https://example.com/api\"") == 0
            && expression.find("\"X-Token\":\"t\"") != std::string::npos && expression.find("Cookie") == std::string::npos
            && expression.find("\"method\":\"POST\"") != std::string::npos && expression.find("\"body\"") != std::string::npos);
    }
    CHECK(!session.Replay(999999));

    // Messages of a socket the tools never saw open are not news.
    session.Receive(Event("page-1", "Network.webSocketFrameReceived", {{"requestId", "ws9"}, {"timestamp", 1.0},
        {"response", {{"opcode", 1}, {"payloadData", "x"}, {"payloadLength", 1}}}}));
    CHECK(session.TakeChanges().bodies.empty());
}

// Copy as cURL and HAR export.
static void TestExports()
{
    Request get;
    get.url = "https://example.com/a?x=1&y=it's";
    get.method = "GET";
    get.requestHeaders = {{"Accept", "text/html"}, {":authority", "example.com"}, {"Cookie", "a=b'c"}};
    CHECK_TEXT(CurlCommand(get), "curl 'https://example.com/a?x=1&y=it'\\''s' \\\n  -H 'Accept: text/html' \\\n  -H 'Cookie: a=b'\\''c'");
    Request post;
    post.url = "https://example.com/api";
    post.method = "POST";
    post.hasRequestBody = true;
    post.requestBody = "{\"q\":1}";
    post.requestHeaders = {{"Content-Type", "application/json"}, {"Content-Length", "7"}};
    CHECK_TEXT(CurlCommand(post), "curl 'https://example.com/api' \\\n  -H 'Content-Type: application/json' \\\n  --data-raw '{\"q\":1}'");
    Request put = post;
    put.method = "PUT";
    CHECK(CurlCommand(put).find("-X 'PUT'") != std::string::npos);

    get.status = 200;
    get.statusText = "OK";
    get.protocol = "h2";
    get.mimeType = "text/html";
    get.responseHeaders = {{"Content-Type", "text/html"}};
    get.started = 10;
    get.responded = 10.25;
    get.ended = 10.5;
    get.wallTime = 1790000000.5;
    get.size = 5;
    get.transferred = 120;
    get.bodyState = BodyState::Loaded;
    get.body = "hello";
    get.remoteAddress = "93.184.216.34:443";
    get.timing.known = true;
    get.timing.dnsStart = 1;
    get.timing.dnsEnd = 6;
    get.timing.connectStart = 6;
    get.timing.tlsStart = 20;
    get.timing.connectEnd = 50;
    get.timing.requestStart = 50.5;
    get.timing.responseStart = 200;
    {
        const auto described = DescribeHeaders(get).text;
        CHECK(described.find("Timing") != std::string::npos && described.find("DNS lookup: 5.0 ms") != std::string::npos
            && described.find("Connecting: 14 ms") != std::string::npos && described.find("TLS: 30 ms") != std::string::npos
            && described.find("Waiting (to the first byte): 150 ms") != std::string::npos
            && described.find("Receiving: 250 ms") != std::string::npos);
    }
    const auto har = json::parse(HarArchive({&get, &post, nullptr}, "0.1.0"));
    CHECK(har["log"]["version"] == "1.2" && har["log"]["creator"]["name"] == "Summit" && har["log"]["entries"].size() == 2);
    const auto& first = har["log"]["entries"][0];
    CHECK(first["startedDateTime"] == "2026-09-21T14:13:20.500Z" && first["time"] == 500 && first["timings"]["wait"] == 149.5
        && first["timings"]["receive"] == 250 && first["timings"]["dns"] == 5 && first["timings"]["connect"] == 44 && first["timings"]["ssl"] == 30);
    CHECK(first["request"]["httpVersion"] == "HTTP/2" && first["request"]["queryString"].size() == 2
        && first["request"]["queryString"][1]["value"] == "it's" && first["request"]["headers"].size() == 3);
    CHECK(first["response"]["status"] == 200 && first["response"]["content"]["text"] == "hello" && first["response"]["bodySize"] == 120
        && first["serverIPAddress"] == "93.184.216.34:443");
    const auto& second = har["log"]["entries"][1];
    CHECK(second["request"]["postData"]["mimeType"] == "application/json" && second["request"]["postData"]["text"] == "{\"q\":1}"
        && !second["response"]["content"].contains("text"));
}

// The Sources panel's debugger.
static void TestDebugger()
{
    Session session;
    session.Start();
    session.Receive(TargetCreated("page-1"));
    Sent(session);
    // Breakpoints wait for the debugger; then go to every process.
    session.SetBreakpoint("https://example.com/app.js", 11);
    CHECK(!Find(Sent(session), "Debugger.setBreakpointByUrl") && session.Breakpoints().size() == 1);
    session.EnableDebugger();
    auto commands = Sent(session);
    CHECK(Find(commands, "Debugger.enable") && !Find(commands, "Debugger.setBreakpointByUrl"));
    session.EnableDebugger();
    CHECK(Sent(session).empty());
    session.SetBreakpoint("https://example.com/app.js", 20);
    commands = Sent(session);
    const auto* set = Find(commands, "Debugger.setBreakpointByUrl");
    CHECK(set && set->parameters["lineNumber"] == 20 && set->parameters["url"] == "https://example.com/app.js");
    session.Receive(FromTarget("page-1", {{"id", set->id}, {"result", {{"breakpointId", "https://example.com/app.js:20:0"},
        {"locations", {{{"scriptId", "5"}, {"lineNumber", 20}, {"columnNumber", 4}}}}}}}));
    CHECK(session.Breakpoints()[1].resolved && session.Breakpoints()[1].id == "https://example.com/app.js:20:0");
    // A new process of the page gets the debugger and every breakpoint.
    session.Receive(TargetCreated("page-2"));
    commands = Sent(session);
    CHECK(Find(commands, "Debugger.enable") && std::count_if(commands.begin(), commands.end(),
        [](const Command& command) { return command.method == "Debugger.setBreakpointByUrl"; }) == 2);

    // Scripts as they are parsed, then their source.
    session.TakeChanges();
    session.Receive(Event("page-2", "Debugger.scriptParsed", {{"scriptId", "5"}, {"url", "https://example.com/app.js"}, {"startLine", 0}}));
    session.Receive(Event("page-2", "Debugger.scriptParsed", {{"scriptId", "6"}, {"url", "https://example.com/"}, {"startLine", 9}}));
    session.Receive(Event("page-2", "Debugger.scriptParsed", {{"scriptId", "7"}, {"url", ""}, {"isContentScript", true}}));
    CHECK(session.TakeChanges().scripts && session.Scripts().size() == 3);
    CHECK(session.FindScript("5")->Label() == "app.js" && session.FindScript("6")->Label().rfind("(inline) ", 0) == 0
        && session.FindScript("6")->Label().find(":10") != std::string::npos && session.FindScript("7")->contentScript);
    session.LoadScriptSource("5");
    commands = Sent(session);
    const auto* source = Find(commands, "Debugger.getScriptSource");
    CHECK(source && source->parameters["scriptId"] == "5" && source->target == "page-2");
    session.LoadScriptSource("5");
    CHECK(Sent(session).empty());
    session.Receive(FromTarget("page-2", {{"id", source->id}, {"result", {{"scriptSource", "function tick() {\n  return 1;\n}"}}}}));
    {
        const auto changes = session.TakeChanges();
        CHECK(changes.sources.size() == 1 && changes.sources[0] == "5" && session.FindScript("5")->sourceLoaded
            && session.FindScript("5")->source.find("tick") != std::string::npos);
    }

    // Paused: the frames with their scopes; stepping; resumed.
    session.Receive(Event("page-2", "Debugger.paused", {{"reason", "Breakpoint"}, {"callFrames", {
        {{"callFrameId", "f1"}, {"functionName", "tick"}, {"location", {{"scriptId", "5"}, {"lineNumber", 1}, {"columnNumber", 2}}},
            {"scopeChain", {{{"type", "local"}, {"object", {{"type", "object"}, {"objectId", "s1"}}}},
                {{"type", "global"}, {"object", {{"type", "object"}, {"objectId", "s2"}}}}}}, {"this", {{"type", "object"}, {"objectId", "t1"}}}},
        {{"callFrameId", "f2"}, {"functionName", ""}, {"location", {{"scriptId", "6"}, {"lineNumber", 12}, {"columnNumber", 0}}}, {"scopeChain", json::array()}}}}}));
    {
        const auto& state = session.Debugger();
        CHECK(session.TakeChanges().debugger && state.paused && state.reason == "Breakpoint" && state.frames.size() == 2
            && state.frames[0].function == "tick" && state.frames[0].line == 1 && state.frames[0].scopes.size() == 2
            && state.frames[0].scopes[0].type == "local" && state.frames[0].scopes[0].objectId == "s1" && state.frames[0].thisObjectId == "t1");
    }
    session.DebuggerCommand("stepOver");
    session.DebuggerCommand("evaluate");
    commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].method == "Debugger.stepOver" && commands[0].target == "page-2");
    session.Receive(Event("page-2", "Debugger.resumed", json::object()));
    CHECK(!session.Debugger().paused && session.Debugger().frames.empty());

    // Removing a breakpoint uses its id; a new page forgets its scripts.
    session.RemoveBreakpoint("https://example.com/app.js", 20);
    commands = Sent(session);
    CHECK(Find(commands, "Debugger.removeBreakpoint") && Find(commands, "Debugger.removeBreakpoint")->parameters["breakpointId"] == "https://example.com/app.js:20:0"
        && session.Breakpoints().size() == 1);
    session.Receive(Event("page-2", "Debugger.globalObjectCleared", json::object()));
    CHECK(session.Scripts().empty() && session.TakeChanges().scripts);
}

static void TestHeaders()
{
    Session session;
    session.Receive(TargetCreated("p"));
    Ask(session, "p", "1", "https://example.com/api?x=1", "L", "XHR", 5);
    Respond(session, "p", "1", "L", "XHR", 200, "application/json", 5.25);
    Finish(session, "p", "1", 5.5, 2048);
    const auto described = DescribeHeaders(session.Requests()[0]);
    const auto& text = described.text;
    CHECK(text.find("General\n  URL: https://example.com/api?x=1\n  Method: GET\n  Status: 200 OK\n  Type: XHR (application/json)\n") == 0);
    CHECK(text.find("  Remote address: 93.184.216.34:443\n  Protocol: h2\n") != std::string::npos);
    CHECK(text.find("  Size: 2.0 KB (1.1 KB transferred)\n") != std::string::npos);
    CHECK(text.find("  Time: 500 ms (250 ms to the first byte)\n") != std::string::npos);
    CHECK(text.find("\n\nResponse Headers (2)\n  Cache-Control: no-store\n  content-type: application/json\n\nRequest Headers (3)\n  Accept: */*\n") != std::string::npos);
    CHECK(text.back() != '\n');
    bool title = false, good = false;
    for (const auto& run : described.runs) {
        CHECK(run.offset + run.length <= text.size());
        const auto part = text.substr(run.offset, run.length);
        if (run.style == TextStyle::Title && part == "Response Headers (2)") title = true;
        if (run.style == TextStyle::Good && part == "200 OK") good = true;
    }
    CHECK(title && good);
    session.Receive(Event("p", "Network.requestWillBeSent", {{"requestId", "2"}, {"loaderId", "L"}, {"request", {{"url", "https://example.com/x"}, {"method", "GET"}, {"headers", json::object()}}}, {"timestamp", 6}}));
    CHECK(DescribeHeaders(session.Requests()[1]).text.find("  Status: Pending\n") != std::string::npos);
    session.Receive(Event("p", "Network.loadingFailed", {{"requestId", "2"}, {"timestamp", 7}, {"errorText", "Could not resolve host"}}));
    CHECK(DescribeHeaders(session.Requests()[1]).text.find("  Status: Failed: Could not resolve host\n") != std::string::npos);
    CHECK(session.Requests()[1].IsError() && session.Requests()[1].StatusLabel() == "(failed)" && session.Requests()[1].type == "Other");
    // A response without content has an empty body, whatever the page says.
    Ask(session, "p", "3", "https://example.com/beacon", "L", "Fetch", 8);
    Respond(session, "p", "3", "L", "Fetch", 204, "", 8.1);
    session.Receive(Event("p", "Network.loadingFinished", {{"requestId", "3"}, {"timestamp", 8.2}}));
    Sent(session);
    session.RequestBody(session.Requests()[2].serial);
    const auto commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].method == "Network.getResponseBody");
    session.Receive(FromTarget("p", {{"id", commands[0].id}, {"error", {{"message", "Missing content of resource for given requestId"}}}}));
    CHECK(session.Requests()[2].bodyState == BodyState::Loaded && session.Requests()[2].body.empty());
    summit::devtools::Request data;
    data.url = "data:image/png;base64," + std::string(200, 'A');
    CHECK(data.Name().size() < 60 && data.Host().empty());
    data.url = "https://user:pw@example.com:8443/a/b/?q#f";
    CHECK(data.Host() == "example.com:8443" && data.Name() == "b?q");
    data.url = "about:blank";
    CHECK(data.Name() == "about:blank");
}

static std::string Reply(const std::string& target, uint64_t id, const json& result)
{
    return FromTarget(target, json{{"id", id}, {"result", result}});
}

static void TestStorage()
{
    Session session;
    session.Start();
    Sent(session);
    session.Receive(TargetCreated("page-1"));
    auto commands = Sent(session);
    CHECK(Find(commands, "DOMStorage.enable") && Find(commands, "DOMStorage.enable")->target == "page-1");
    const auto* tree = Find(commands, "Page.getResourceTree");
    CHECK(tree && tree->target == "page-1");
    // The page and its frames; an opaque frame keeps nothing and is left out.
    session.Receive(Reply("page-1", tree->id, {{"frameTree", {{"frame", {{"id", "f1"}, {"securityOrigin", "https://example.com"}}},
        {"childFrames", json::array({{{"frame", {{"id", "f2"}, {"securityOrigin", "https://ads.example"}}}},
            {{"frame", {{"id", "f3"}, {"securityOrigin", "null"}}}},
            {{"frame", {{"id", "f4"}, {"securityOrigin", "https://example.com"}}}}})}}}}));
    auto changes = session.TakeChanges();
    CHECK(changes.origins && session.StorageOrigins() == std::vector<std::string>({"https://example.com", "https://ads.example"}));

    // Local storage, read.
    CHECK(!session.FindArea(StorageKind::Local, "https://example.com"));
    session.LoadStorage(StorageKind::Local, "https://example.com");
    session.LoadStorage(StorageKind::Local, "https://example.com");
    commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].method == "DOMStorage.getDOMStorageItems"
        && commands[0].parameters["storageId"]["securityOrigin"] == "https://example.com"
        && commands[0].parameters["storageId"]["isLocalStorage"] == true);
    const auto* area = session.FindArea(StorageKind::Local, "https://example.com");
    CHECK(area && area->waiting && !area->loaded);
    session.Receive(Reply("page-1", commands[0].id, {{"entries", json::array({json::array({"theme", "dark"}),
        json::array({"cart", "{\"items\":[1,2]}"})})}}));
    area = session.FindArea(StorageKind::Local, "https://example.com");
    CHECK(area && area->loaded && !area->waiting && area->items.size() == 2 && area->items[1].key == "cart"
        && area->items[1].value == "{\"items\":[1,2]}" && area->Bytes() == 5 + 4 + 4 + 15);
    CHECK(session.TakeChanges().storage);

    // The page changes it: the model follows without asking again.
    const json local = {{"securityOrigin", "https://example.com"}, {"isLocalStorage", true}};
    session.Receive(Event("page-1", "DOMStorage.domStorageItemAdded", {{"storageId", local}, {"key", "lang"}, {"newValue", "en"}}));
    session.Receive(Event("page-1", "DOMStorage.domStorageItemUpdated", {{"storageId", local}, {"key", "theme"}, {"oldValue", "dark"}, {"newValue", "light"}}));
    session.Receive(Event("page-1", "DOMStorage.domStorageItemRemoved", {{"storageId", local}, {"key", "cart"}}));
    area = session.FindArea(StorageKind::Local, "https://example.com");
    CHECK(area->items.size() == 2 && area->items[0].value == "light" && area->items[1].key == "lang" && Sent(session).empty());
    // Session storage of the same origin is another area, not yet read.
    session.Receive(Event("page-1", "DOMStorage.domStorageItemAdded", {{"storageId", {{"securityOrigin", "https://example.com"},
        {"isLocalStorage", false}}}, {"key", "x"}, {"newValue", "1"}}));
    CHECK(!session.FindArea(StorageKind::Session, "https://example.com"));
    session.Receive(Event("page-1", "DOMStorage.domStorageItemsCleared", {{"storageId", local}}));
    CHECK(session.FindArea(StorageKind::Local, "https://example.com")->items.empty());
    session.TakeChanges();

    // Changing an area asks the page, then reads it again.
    session.SetStorageItem(StorageKind::Session, "https://ads.example", "k", "v");
    commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].method == "DOMStorage.setDOMStorageItem" && commands[0].parameters["key"] == "k"
        && commands[0].parameters["value"] == "v" && commands[0].parameters["storageId"]["isLocalStorage"] == false);
    session.Receive(Reply("page-1", commands[0].id, json::object()));
    commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].method == "DOMStorage.getDOMStorageItems"
        && commands[0].parameters["storageId"]["securityOrigin"] == "https://ads.example");
    session.Receive(Reply("page-1", commands[0].id, {{"entries", json::array({json::array({"k", "v"})})}}));
    CHECK(session.FindArea(StorageKind::Session, "https://ads.example")->items.size() == 1);
    session.RemoveStorageItem(StorageKind::Session, "https://ads.example", "k");
    commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].method == "DOMStorage.removeDOMStorageItem" && commands[0].parameters["key"] == "k");
    session.ClearStorage(StorageKind::Local, "https://example.com");
    commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].method == "DOMStorage.clearDOMStorageItems");
    // A failure is shown with the area.
    session.Receive(FromTarget("page-1", json{{"id", commands[0].id}, {"error", {{"message", "No storage"}}}}));
    CHECK(session.FindArea(StorageKind::Local, "https://example.com")->error == "No storage");
    Sent(session);

    // Cookies: one area, of all the page's addresses, HttpOnly ones included.
    session.LoadStorage(StorageKind::Cookies, "ignored");
    commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].method == "Page.getCookies");
    session.Receive(Reply("page-1", commands[0].id, {{"cookies", json::array({
        {{"name", "sid"}, {"value", "abc"}, {"domain", ".example.com"}, {"path", "/"}, {"expires", 1790000000000.0},
            {"session", false}, {"httpOnly", true}, {"secure", true}, {"sameSite", "Lax"}},
        {{"name", "pref"}, {"value", "1"}, {"domain", "example.com"}, {"path", "/app"}, {"expires", 0},
            {"session", true}, {"httpOnly", false}, {"secure", false}, {"sameSite", "None"}},
        {{"name", "sid"}, {"value", "partitioned"}, {"domain", ".example.com"}, {"path", "/"},
            {"session", true}, {"httpOnly", true}, {"secure", true}, {"sameSite", "None"}, {"partitionKey", "https://shop.test"}}})}}));
    const auto* cookies = session.FindArea(StorageKind::Cookies, "");
    CHECK(cookies && cookies->loaded && cookies->cookies.size() == 3 && cookies->cookies[0].httpOnly && cookies->cookies[0].secure);
    CHECK(cookies->cookies[0].URL() == "https://example.com/" && cookies->cookies[1].URL() == "http://example.com/app");
    CHECK(cookies->cookies[1].ExpiresLabel() == "Session" && cookies->cookies[0].ExpiresLabel().rfind("2026-", 0) == 0);
    CHECK(cookies->cookies[0].Size() == 6 && cookies->Bytes() == 6 + 5 + 14);
    auto partitioned = cookies->cookies[2];
    CHECK(partitioned.partitionKey == "https://shop.test" && partitioned.ID() != cookies->cookies[0].ID());
    session.DeleteCookie(partitioned);
    commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].method == "Page.setCookie"
        && commands[0].parameters["cookie"]["partitionKey"] == "https://shop.test"
        && commands[0].parameters["cookie"]["expires"] == 0
        && commands[0].parameters["cookie"]["session"] == false
        && commands[0].parameters["cookie"]["name"] == "sid"
        && commands[0].parameters["cookie"]["domain"] == ".example.com"
        && commands[0].parameters["cookie"]["path"] == "/");
    session.DeleteCookie(cookies->cookies[0]);
    commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].method == "Page.deleteCookie" && commands[0].parameters["cookieName"] == "sid"
        && commands[0].parameters["url"] == "https://example.com/");
    session.Receive(Reply("page-1", commands[0].id, json::object()));
    commands = Sent(session);
    CHECK(commands.size() == 1 && commands[0].method == "Page.getCookies");
    session.Receive(Reply("page-1", commands[0].id, {{"cookies", json::array()}}));
    CHECK(session.FindArea(StorageKind::Cookies, "")->cookies.empty());
    // Clearing deletes each cookie shown.
    session.LoadStorage(StorageKind::Cookies, "");
    commands = Sent(session);
    session.Receive(Reply("page-1", commands[0].id, {{"cookies", json::array({{{"name", "a"}, {"domain", "x.test"}, {"path", "/"}},
        {{"name", "b"}, {"domain", "x.test"}, {"path", "/"}}})}}));
    session.ClearStorage(StorageKind::Cookies, "");
    commands = Sent(session);
    CHECK(commands.size() == 2 && commands[0].method == "Page.deleteCookie" && commands[1].parameters["cookieName"] == "b");
    session.TakeChanges();

    // The page's load reads again what was read.
    session.Receive(Event("page-1", "Page.loadEventFired", {{"timestamp", 3}}));
    commands = Sent(session);
    CHECK(Find(commands, "DOMStorage.getDOMStorageItems") && Find(commands, "Page.getCookies"));

    // A subframe adds its origin; the page going elsewhere replaces them and
    // forgets the areas of origins it no longer has, and the cookies.
    session.Receive(Event("page-1", "Page.frameNavigated", {{"frame", {{"id", "f5"}, {"parentId", "f1"}, {"loaderId", "L9"},
        {"url", "https://widgets.example/w"}, {"securityOrigin", "https://widgets.example"}}}}));
    CHECK(session.StorageOrigins().size() == 3 && session.TakeChanges().origins);
    session.Receive(Event("page-1", "Page.frameNavigated", {{"frame", {{"id", "f1"}, {"loaderId", "L10"},
        {"url", "https://example.com/other"}, {"securityOrigin", "https://example.com"}}}}));
    changes = session.TakeChanges();
    CHECK(changes.origins && session.StorageOrigins() == std::vector<std::string>({"https://example.com"}));
    CHECK(session.FindArea(StorageKind::Local, "https://example.com") && !session.FindArea(StorageKind::Session, "https://ads.example")
        && !session.FindArea(StorageKind::Cookies, ""));
    // A file page keeps storage too; about:blank does not.
    session.Receive(Event("page-1", "Page.frameNavigated", {{"frame", {{"id", "f1"}, {"loaderId", "L11"},
        {"url", "file:///boot/home/a.html"}, {"securityOrigin", "file://"}}}}));
    CHECK(session.StorageOrigins() == std::vector<std::string>({"file://"}));
    session.Receive(Event("page-1", "Page.frameNavigated", {{"frame", {{"id", "f1"}, {"loaderId", "L12"},
        {"url", "about:blank"}, {"securityOrigin", "://"}}}}));
    CHECK(session.StorageOrigins().empty());

    // Without a page there is nothing to ask.
    Session empty;
    empty.LoadStorage(StorageKind::Local, "https://example.com");
    CHECK(Sent(empty).empty() && !empty.FindArea(StorageKind::Local, "https://example.com")->error.empty());
}

// A node as the protocol describes it.
static json Node(int id, int type, const std::string& name, json more = json::object())
{
    std::string local;
    if (type == 1)
        for (const char c : name) local += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    json node = {{"nodeId", id}, {"nodeType", type}, {"nodeName", name}, {"localName", local}, {"nodeValue", ""}};
    if (type == 1) node["attributes"] = json::array();
    node.update(more);
    return node;
}
static std::string Joined(const std::vector<RowPart>& parts)
{
    std::string text;
    for (const auto& part : parts) text += part.text;
    return text;
}
// A document: doctype, html with head and body (base + 1 to base + 4).
static json Document(int base)
{
    return Node(base, 9, "#document", {{"documentURL", "https://example.com/"}, {"childNodeCount", 2}, {"children", json::array({
        Node(base + 1, 10, "html", {{"publicId", ""}, {"systemId", ""}}),
        Node(base + 2, 1, "HTML", {{"attributes", {"lang", "en"}}, {"childNodeCount", 2}, {"children", json::array({
            Node(base + 3, 1, "HEAD", {{"childNodeCount", 1}}),
            Node(base + 4, 1, "BODY", {{"attributes", {"class", "home page"}}, {"childNodeCount", 3}})})}})})}});
}

static void TestElements()
{
    Session session;
    session.Start();
    Sent(session);
    session.Receive(TargetCreated("page-1"));
    Sent(session);
    auto& dom = session.DOM();
    // Until the panel is shown nothing is asked or followed.
    CHECK(!dom.Enabled());
    session.Receive(Event("page-1", "DOM.documentUpdated", json::object()));
    CHECK(Sent(session).empty() && !session.TakeChanges().Any());

    dom.Enable();
    auto commands = Sent(session);
    CHECK(Find(commands, "Inspector.enable") && Find(commands, "CSS.enable") && Find(commands, "CSS.getSupportedCSSProperties"));
    const auto* get = Find(commands, "DOM.getDocument");
    CHECK(get && get->target == "page-1");
    session.Receive(Reply("page-1", Find(commands, "CSS.getSupportedCSSProperties")->id, {{"cssProperties", json::array({
        {{"name", "color"}, {"inherited", true}}, {{"name", "font-size"}, {"inherited", true}}, {{"name", "display"}},
        {{"name", "margin"}, {"longhands", {"margin-top", "margin-right", "margin-bottom", "margin-left"}}}})}}));
    session.Receive(Reply("page-1", get->id, {{"root", Document(1)}}));
    // The body is selected and expanded, as in other browsers.
    CHECK(dom.Loaded() && dom.Document() == 1 && dom.Selected() == 5 && dom.IsExpanded(3) && dom.IsExpanded(5));
    CHECK(session.TakeChanges().elements);
    auto changes = dom.TakeChanges();
    CHECK(changes.tree && changes.selection);
    commands = Sent(session);
    const auto* children = Find(commands, "DOM.requestChildNodes");
    CHECK(children && children->parameters["nodeId"] == 5);
    CHECK(Find(commands, "DOM.setInspectedNode") && Find(commands, "CSS.getMatchedStylesForNode")
        && Find(commands, "CSS.getInlineStylesForNode") && Find(commands, "CSS.getComputedStyleForNode") && Find(commands, "DOM.resolveNode"));

    // The doctype, html, head, body (its children on their way), </html>.
    auto rows = dom.Rows();
    CHECK(rows.size() == 5 && rows[0].node == 2 && rows[1].node == 3 && rows[2].node == 4 && rows[2].depth == 1 && rows[3].node == 5
        && rows[4].closing && rows[4].node == 3);
    CHECK_TEXT(Joined(dom.DescribeRow(rows[0])), "<!DOCTYPE html>");
    CHECK_TEXT(Joined(dom.DescribeRow(rows[1])), "<html lang=\"en\">");
    CHECK_TEXT(Joined(dom.DescribeRow(rows[2])), "<head>…</head>");
    CHECK_TEXT(Joined(dom.DescribeRow(rows[3])), "<body class=\"home page\">…</body>");
    CHECK(dom.DescribeRow(rows[1])[1].kind == Piece::Tag && dom.DescribeRow(rows[1])[3].kind == Piece::AttributeName
        && dom.DescribeRow(rows[1])[5].kind == Piece::AttributeValue);

    session.Receive(Event("page-1", "DOM.setChildNodes", {{"parentId", 5}, {"nodes", json::array({
        Node(6, 1, "H1", {{"attributes", {"id", "title"}}, {"childNodeCount", 1}, {"children", json::array({
            Node(7, 3, "#text", {{"nodeValue", "  Hello\n  world "}})})}}),
        Node(8, 8, "#comment", {{"nodeValue", " note "}}),
        Node(9, 1, "DIV", {{"childNodeCount", 2}})})}}));
    rows = dom.Rows();
    CHECK(rows.size() == 9 && rows[7].closing && rows[7].node == 5);
    CHECK_TEXT(Joined(dom.DescribeRow(rows[4])), "<h1 id=\"title\">Hello world</h1>");
    CHECK(rows[4].depth == 2 && !dom.Expandable(6));
    CHECK_TEXT(Joined(dom.DescribeRow(rows[5])), "<!-- note -->");
    CHECK_TEXT(Joined(dom.DescribeRow(rows[6])), "<div>…</div>");
    CHECK_TEXT(dom.Breadcrumbs(6), "html › body.home.page › h1#title");
    CHECK_TEXT(dom.Selector(6), "#title");
    CHECK_TEXT(dom.Selector(9), "body > div");
    CHECK_TEXT(dom.PathOf(9), "1,HTML,1,BODY,2,DIV");
    CHECK_TEXT(dom.AttributesText(5), "class=\"home page\"");

    // The page changes: a node inserted, attributes and text changed.
    session.Receive(Event("page-1", "DOM.childNodeInserted", {{"parentNodeId", 5}, {"previousNodeId", 6},
        {"node", Node(10, 1, "P", {{"childNodeCount", 0}})}}));
    CHECK(dom.Find(5)->children == std::vector<int>({6, 10, 8, 9}) && dom.Find(5)->childCount == 4);
    CHECK_TEXT(Joined(dom.DescribeRow({10, 2, false})), "<p></p>");
    session.Receive(Event("page-1", "DOM.attributeModified", {{"nodeId", 6}, {"name", "class"}, {"value", "big \"one\" & two"}}));
    CHECK_TEXT(dom.AttributesText(6), "id=\"title\" class=\"big &quot;one&quot; &amp; two\"");
    session.Receive(Event("page-1", "DOM.attributeRemoved", {{"nodeId", 6}, {"name", "class"}}));
    session.Receive(Event("page-1", "DOM.characterDataModified", {{"nodeId", 7}, {"characterData", "Bye"}}));
    std::string text;
    CHECK(dom.InlineText(6, &text) && text == "Bye");
    CHECK(dom.TakeChanges().nodes);
    session.Receive(Event("page-1", "DOM.childNodeCountUpdated", {{"nodeId", 4}, {"childNodeCount", 0}}));
    CHECK(!dom.Expandable(4));
    CHECK_TEXT(Joined(dom.DescribeRow({4, 1, false})), "<head></head>");
    // Children it has not given come back (an undo puts a node's children back).
    session.Receive(Event("page-1", "DOM.childNodeCountUpdated", {{"nodeId", 4}, {"childNodeCount", 2}}));
    CHECK(dom.Expandable(4) && !dom.Find(4)->childrenKnown);
    CHECK_TEXT(Joined(dom.DescribeRow({4, 1, false})), "<head>…</head>");

    // A node removed: the selection in it moves to the node after it.
    dom.Select(10);
    Sent(session);
    dom.Remove(10);
    commands = Sent(session);
    CHECK(Find(commands, "DOM.removeNode") && Find(commands, "DOM.removeNode")->parameters["nodeId"] == 10
        && Find(commands, "DOM.markUndoableState"));
    session.Receive(Event("page-1", "DOM.childNodeRemoved", {{"parentNodeId", 5}, {"nodeId", 10}}));
    CHECK(!dom.Find(10) && dom.Selected() == 8);
    // A subtree removed with the selection deep in it: the parent is selected.
    dom.SetExpanded(9, true);
    commands = Sent(session);
    CHECK(Find(commands, "DOM.requestChildNodes") && Find(commands, "DOM.requestChildNodes")->parameters["nodeId"] == 9);
    session.Receive(Event("page-1", "DOM.setChildNodes", {{"parentId", 9}, {"nodes", json::array({
        Node(11, 3, "#text", {{"nodeValue", "a"}}), Node(12, 1, "SPAN", {{"childNodeCount", 0}})})}}));
    CHECK(dom.Rows().size() == 12);
    dom.Select(12);
    session.Receive(Event("page-1", "DOM.childNodeRemoved", {{"parentNodeId", 5}, {"nodeId", 9}}));
    CHECK(!dom.Find(9) && !dom.Find(11) && !dom.Find(12) && dom.Selected() == 5 && !dom.IsExpanded(9));

    // Attributes edited as text: those left out are removed.
    Sent(session);
    dom.SetAttributesText(6, "id=\"main\" data-x='1 2' hidden");
    commands = Sent(session);
    CHECK(!Find(commands, "DOM.removeAttribute") && Find(commands, "DOM.setAttributesAsText")
        && Find(commands, "DOM.setAttributesAsText")->parameters["text"] == "id=\"main\" data-x='1 2' hidden");
    dom.SetAttributesText(6, "");
    commands = Sent(session);
    CHECK(Find(commands, "DOM.removeAttribute") && Find(commands, "DOM.removeAttribute")->parameters["name"] == "id"
        && !Find(commands, "DOM.setAttributesAsText"));
    CHECK(AttributeNames("a=1 B='x y' c=\"q > r\" d  e = f/g /h") == std::vector<std::string>({"a", "b", "c", "d", "e", "h"}));
    dom.SetNodeText(7, "Hi");
    dom.SetTagName(6, "h2");
    commands = Sent(session);
    CHECK(Find(commands, "DOM.setNodeValue") && Find(commands, "DOM.setNodeValue")->parameters["value"] == "Hi");
    const auto* rename = Find(commands, "DOM.setNodeName");
    CHECK(rename && rename->parameters["name"] == "h2");
    session.Receive(Event("page-1", "DOM.childNodeRemoved", {{"parentNodeId", 5}, {"nodeId", 6}}));
    session.Receive(Event("page-1", "DOM.childNodeInserted", {{"parentNodeId", 5}, {"previousNodeId", 0},
        {"node", Node(13, 1, "H2", {{"childNodeCount", 0}})}}));
    session.Receive(Reply("page-1", rename->id, {{"nodeId", 13}}));
    CHECK(dom.Selected() == 13 && dom.Find(5)->children.front() == 13);
    // An error is told.
    dom.Remove(8);
    commands = Sent(session);
    session.Receive(FromTarget("page-1", {{"id", Find(commands, "DOM.removeNode")->id}, {"error", {{"message", "Node is in a shadow tree"}}}}));
    CHECK(dom.TakeChanges().notice == "Node is in a shadow tree" && dom.Find(8));

    // Search: the results are selected in turn.
    dom.Search("note");
    commands = Sent(session);
    const auto* search = Find(commands, "DOM.performSearch");
    CHECK(search && search->parameters["query"] == "note" && dom.Searching());
    session.Receive(Reply("page-1", search->id, {{"searchId", "s1"}, {"resultCount", 2}}));
    commands = Sent(session);
    const auto* results = Find(commands, "DOM.getSearchResults");
    CHECK(results && results->parameters["searchId"] == "s1" && results->parameters["toIndex"] == 2);
    session.Receive(Reply("page-1", results->id, {{"nodeIds", {8, 13}}}));
    CHECK(dom.SearchCount() == 2 && dom.Selected() == 8 && dom.SearchIndex() == 0 && !dom.Searching());
    dom.SearchStep(1);
    CHECK(dom.Selected() == 13 && dom.SearchIndex() == 1);
    dom.SearchStep(1);
    CHECK(dom.Selected() == 8);
    dom.SearchStep(-1);
    CHECK(dom.Selected() == 13);
    dom.Search("");
    commands = Sent(session);
    CHECK(Find(commands, "DOM.discardSearchResults") && dom.SearchCount() == 0);

    // The picker: the page tells what was clicked with Inspector.inspect.
    dom.SetPicking(true);
    commands = Sent(session);
    const auto* pick = Find(commands, "DOM.setInspectModeEnabled");
    CHECK(pick && pick->parameters["enabled"] == true && pick->parameters["highlightConfig"].is_object() && dom.Picking());
    session.Receive(Event("page-1", "Inspector.inspect", {{"object", {{"type", "object"}, {"subtype", "node"}, {"objectId", "o1"}}},
        {"hints", json::object()}}));
    CHECK(!dom.Picking());
    commands = Sent(session);
    const auto* request = Find(commands, "DOM.requestNode");
    CHECK(request && request->parameters["objectId"] == "o1" && Find(commands, "Runtime.releaseObject"));
    // The path to the node arrives first.
    session.Receive(Event("page-1", "DOM.setChildNodes", {{"parentId", 4}, {"nodes", json::array({Node(14, 1, "TITLE", {{"childNodeCount", 0}})})}}));
    session.Receive(Reply("page-1", request->id, {{"nodeId", 14}}));
    CHECK(dom.Selected() == 14 && dom.IsExpanded(4));
    changes = dom.TakeChanges();
    CHECK(changes.picked && changes.picking);
    // Inspect Element: the element at a point of the page.
    dom.InspectAt(10, 20.5);
    commands = Sent(session);
    const auto* evaluate = Find(commands, "Runtime.evaluate");
    CHECK(evaluate && evaluate->parameters["expression"].get<std::string>().find("(10.00, 20.50)") != std::string::npos);
    session.Receive(Reply("page-1", evaluate->id, {{"result", {{"type", "object"}, {"subtype", "node"}, {"objectId", "o2"}}}}));
    commands = Sent(session);
    request = Find(commands, "DOM.requestNode");
    CHECK(request && request->parameters["objectId"] == "o2");
    session.Receive(Reply("page-1", request->id, {{"nodeId", 8}}));
    CHECK(dom.Selected() == 8 && dom.TakeChanges().picked);
    dom.Highlight(13);
    dom.Highlight(13);
    dom.HideHighlight();
    commands = Sent(session);
    CHECK(Find(commands, "DOM.highlightNode") && Find(commands, "DOM.highlightNode")->parameters["nodeId"] == 13
        && Find(commands, "DOM.hideHighlight"));
    size_t highlights = 0;
    for (const auto& command : commands) highlights += command.method == "DOM.highlightNode";
    CHECK(highlights == 1);

    // A reload: the document is read again and the selection found by its path.
    dom.Select(13);
    session.Receive(Event("page-1", "DOM.documentUpdated", json::object()));
    CHECK(!dom.Loaded() && dom.Selected() == 0);
    commands = Sent(session);
    get = Find(commands, "DOM.getDocument");
    CHECK(get && !Find(commands, "CSS.enable"));
    session.Receive(Reply("page-1", get->id, {{"root", Document(21)}}));
    commands = Sent(session);
    const auto* path = Find(commands, "DOM.pushNodeByPathToFrontend");
    CHECK(path && path->parameters["path"] == "1,HTML,1,BODY,0,H2" && dom.Selected() == 25);
    session.Receive(Event("page-1", "DOM.setChildNodes", {{"parentId", 25}, {"nodes", json::array({Node(30, 1, "H2", {{"childNodeCount", 0}})})}}));
    session.Receive(Reply("page-1", path->id, {{"nodeId", 30}}));
    CHECK(dom.Selected() == 30);

    // Another process: the document is read from the new target.
    session.Receive(TargetCreated("page-2", true));
    Sent(session);
    session.Receive(json{{"method", "Target.didCommitProvisionalTarget"}, {"params", {{"oldTargetId", "page-1"}, {"newTargetId", "page-2"}}}}.dump());
    CHECK(!dom.Loaded());
    commands = Sent(session);
    CHECK(Find(commands, "DOM.getDocument") && Find(commands, "DOM.getDocument")->target == "page-2" && Find(commands, "CSS.enable"));
    // A search asked before the document is there runs once it is.
    dom.Search("x");
    CHECK(!Find(Sent(session), "DOM.performSearch"));
}

static void TestElementStyles()
{
    // Colours, for the swatches and the picker.
    using Color = std::array<uint8_t, 4>;
    CHECK(ParseCSSColor("#fff") == Color({255, 255, 255, 255}) && ParseCSSColor(" #FF000080 ") == Color({255, 0, 0, 128}));
    CHECK(ParseCSSColor("rgb(51, 51, 51)") == Color({51, 51, 51, 255}) && ParseCSSColor("rgba(0,0,0,0.5)") == Color({0, 0, 0, 128}));
    CHECK(ParseCSSColor("rgb(100% 0% 0% / 50%)") == Color({255, 0, 0, 128}) && ParseCSSColor("Navy") == Color({0, 0, 128, 255}));
    CHECK(!ParseCSSColor("var(--x)") && !ParseCSSColor("#ggg") && !ParseCSSColor("12px") && !ParseCSSColor("rgb(1,2)"));
    CHECK(CSSColorText({255, 0, 0, 255}) == "#ff0000" && CSSColorText({0, 0, 0, 128}) == "#00000080");

    // Blocks as shown: the element's own from the strongest, then inherited.
    StyleBlock inlined, rule, weak, agent, parent;
    inlined.kind = StyleBlock::Inline;
    inlined.declarations = {{"color", "red", "color: red;"}};
    rule.declarations = {{"color", "blue", "color: blue !important;"}, {"margin", "0", "margin: 0;"}, {"margin", "1px", "margin: 1px;"}};
    rule.declarations[0].important = true;
    // WebKit marks a property overridden later in the same rule.
    rule.declarations[1].overridden = true;
    weak.declarations = {{"margin", "2px", "margin: 2px;"}, {"padding", "1px", "padding: 1px"}, {"display", "flex", "/* display: flex; */"}};
    weak.declarations[2].enabled = false;
    agent.origin = "user-agent";
    agent.declarations = {{"display", "block"}};
    parent.inheritedFrom = 7;
    parent.declarations = {{"color", "green", "color: green;"}, {"font-size", "12px", "font-size: 12px;"}};
    std::vector<StyleBlock> blocks = {inlined, rule, weak, agent, parent};
    MarkOverridden(blocks);
    CHECK(blocks[0].declarations[0].overridden);
    CHECK(!blocks[1].declarations[0].overridden && blocks[1].declarations[1].overridden && !blocks[1].declarations[2].overridden);
    CHECK(blocks[2].declarations[0].overridden && !blocks[2].declarations[1].overridden && !blocks[2].declarations[2].overridden);
    CHECK(!blocks[3].declarations[0].overridden);
    CHECK(blocks[4].declarations[0].overridden && !blocks[4].declarations[1].overridden);
    // Shorthands and their parts.
    StyleBlock strong, weaker;
    strong.declarations = {{"padding", "0", "padding: 0;"}};
    weaker.declarations = {{"padding-top", "1em", "padding-top: 1em;"}, {"margin", "1px", "margin: 1px;"}};
    std::vector<StyleBlock> shorthands = {weaker, strong};
    shorthands[0].declarations.insert(shorthands[0].declarations.begin(), {"margin-top", "2px", "margin-top: 2px;"});
    const std::map<std::string, std::vector<std::string>> longhands = {{"padding", {"padding-top", "padding-right"}},
        {"margin", {"margin-top", "margin-bottom"}}};
    // margin-top: 2px, padding-top: 1em, margin: 1px (top taken; bottom its own); then padding: 0.
    MarkOverridden(shorthands, longhands);
    CHECK(!shorthands[0].declarations[0].overridden && !shorthands[0].declarations[1].overridden
        && !shorthands[0].declarations[2].overridden && !shorthands[1].declarations[0].overridden);
    std::vector<StyleBlock> reversed = {strong, weaker};
    MarkOverridden(reversed, longhands);
    CHECK(reversed[1].declarations[0].overridden && !reversed[1].declarations[1].overridden);

    // Changing a style's text: switched off and on, replaced, removed, added.
    CHECK_TEXT(EditStyleText(weak, 1, StyleEdit::Toggle), "margin: 2px; /* padding: 1px; */ /* display: flex; */");
    CHECK_TEXT(EditStyleText(weak, 2, StyleEdit::Toggle), "margin: 2px; padding: 1px; display: flex;");
    CHECK_TEXT(EditStyleText(weak, 0, StyleEdit::Replace, " margin : 3px "), "margin : 3px; padding: 1px; /* display: flex; */");
    CHECK_TEXT(EditStyleText(weak, 0, StyleEdit::Replace, ""), "padding: 1px; /* display: flex; */");
    CHECK_TEXT(EditStyleText(agent, 0, StyleEdit::Add, "color: red"), "display: block; color: red;");
    CHECK(agent.Location() == "user agent style sheet");
    rule.sourceURL = "https://example.com/css/site.css?v=2";
    rule.line = 12;
    CHECK(rule.Location() == "site.css:12" && inlined.Location().empty());

    // The protocol's answers make the panel's blocks.
    DOMModel dom;
    dom.Enable();
    const auto answer = [&](uint64_t tag, const json& result) { dom.Answered(tag, json {{"id", 1}, {"result", result}}.dump()); };
    std::map<std::string, uint64_t> tags;
    const auto take = [&] {
        tags.clear();
        const auto commands = dom.TakeCommands();
        for (const auto& command : commands) tags[command.method] = command.tag;
        return commands;
    };
    take();
    answer(tags["CSS.getSupportedCSSProperties"], {{"cssProperties", json::array({{{"name", "color"}, {"inherited", true}},
        {{"name", "margin"}, {"longhands", {"margin-top", "margin-right", "margin-bottom", "margin-left"}}}})}});
    // The author's rules are in a <style> of the document, from its fifth line.
    dom.Event(json {{"method", "CSS.styleSheetAdded"}, {"params", {{"header", {{"styleSheetId", "1"},
        {"sourceURL", "https://example.com/"}, {"origin", "author"}, {"isInline", true}, {"startLine", 4}}}}}}.dump());
    answer(tags["DOM.getDocument"], {{"root", Document(1)}});
    CHECK(dom.Selected() == 5 && dom.StylesLoading());
    take();
    const json userAgent = {{"origin", "user-agent"}, {"selectorList", {{"text", "body"}}}, {"sourceLine", 0},
        {"style", {{"styleId", {{"styleSheetId", "3"}, {"ordinal", 2}}}, {"cssProperties", json::array({
            {{"name", "display"}, {"value", "block"}}, {{"name", "margin-top"}, {"value", "8px"}},
            {{"name", "margin-right"}, {"value", "8px"}, {"implicit", true}}, {{"name", "margin-bottom"}, {"value", "8px"}, {"implicit", true}},
            {{"name", "margin-left"}, {"value", "8px"}, {"implicit", true}}})}, {"shorthandEntries", json::array({{{"name", "margin"}, {"value", "8px"}}})}}}};
    const json author = {{"origin", "author"}, {"ruleId", {{"styleSheetId", "1"}, {"ordinal", 1}}}, {"selectorList", {{"text", "body"}}},
        {"sourceURL", "https://example.com/"}, {"sourceLine", 4}, {"groupings", json::array({{{"type", "media-rule"}, {"text", "screen"}}})},
        {"style", {{"styleId", {{"styleSheetId", "1"}, {"ordinal", 1}}}, {"cssProperties", json::array({
            {{"name", "margin"}, {"value", "auto"}, {"text", "margin:auto;"}, {"status", "active"}},
            {{"name", "color"}, {"value", "#333"}, {"text", "color:#333"}, {"status", "active"}},
            {{"name", "margin-top"}, {"value", "auto"}, {"implicit", true}}})}}}};
    const json html = {{"origin", "author"}, {"selectorList", {{"text", "html"}}}, {"sourceURL", "https://example.com/"}, {"sourceLine", 1},
        {"style", {{"styleId", {{"styleSheetId", "1"}, {"ordinal", 0}}}, {"cssProperties", json::array({
            {{"name", "color"}, {"value", "black"}, {"text", "color: black;"}}, {{"name", "background"}, {"value", "#eee"}, {"text", "background: #eee;"}}})}}}};
    answer(tags["CSS.getMatchedStylesForNode"], {{"matchedCSSRules", json::array({{{"rule", userAgent}}, {{"rule", author}}})},
        {"inherited", json::array({{{"matchedCSSRules", json::array({{{"rule", html}}})}}})}, {"pseudoElements", json::array()}});
    answer(tags["CSS.getInlineStylesForNode"], {{"inlineStyle", {{"styleId", {{"styleSheetId", "5"}, {"ordinal", 0}}}, {"cssText", ""},
        {"cssProperties", json::array()}}}, {"attributesStyle", {{"cssProperties", json::array()}}}});
    answer(tags["CSS.getComputedStyleForNode"], {{"computedStyle", json::array({{{"name", "width"}, {"value", "480px"}},
        {{"name", "height"}, {"value", "auto"}}, {{"name", "margin-top"}, {"value", "8px"}}, {{"name", "padding-left"}, {"value", "2px"}},
        {{"name", "padding-right"}, {"value", "2px"}}, {{"name", "border-left-width"}, {"value", "1px"}}, {{"name", "display"}, {"value", "block"}},
        {{"name", "box-sizing"}, {"value", "border-box"}}, {{"name", "color"}, {"value", "rgb(51, 51, 51)"}}})}});
    answer(tags["DOM.resolveNode"], {{"object", {{"objectId", "box1"}}}});
    CHECK(dom.StylesLoading());
    take();
    CHECK(tags.count("Runtime.callFunctionOn") && tags.count("Runtime.releaseObject"));
    answer(tags["Runtime.callFunctionOn"], {{"result", {{"type", "object"}, {"value", {480, 300}}}}});
    CHECK(!dom.StylesLoading() && dom.TakeChanges().styles);
    const auto& styles = dom.Styles();
    CHECK(styles.node == 5 && styles.blocks.size() == 4);
    CHECK(styles.blocks[0].kind == StyleBlock::Inline && styles.blocks[0].Editable() && styles.blocks[0].declarations.empty());
    // Accessibility (#118): asked for the selected element only when wanted;
    // role and label first, then its states, arrays as node counts.
    take();
    dom.RequestAccessibility();
    take();
    CHECK(tags.count("DOM.getAccessibilityPropertiesForNode"));
    answer(tags["DOM.getAccessibilityPropertiesForNode"], {{"properties", {{"exists", true}, {"label", "Main"}, {"role", "main"},
        {"ignored", false}, {"childNodeIds", json::array({6, 7})}, {"headingLevel", 2}}}});
    {
        const auto changes = dom.TakeChanges();
        CHECK(changes.accessibility && changes.Any());
    }
    const auto& accessibility = dom.Accessibility().properties;
    CHECK(dom.Accessibility().node == 5 && dom.Accessibility().error.empty() && accessibility.size() == 6
        && accessibility[0] == std::make_pair(std::string("role"), std::string("main"))
        && accessibility[1] == std::make_pair(std::string("label"), std::string("Main"))
        && std::count(accessibility.begin(), accessibility.end(), std::make_pair(std::string("childNodeIds"), std::string("2 nodes")))
        && std::count(accessibility.begin(), accessibility.end(), std::make_pair(std::string("headingLevel"), std::string("2"))));
    // The author's rule: what it says, and where.
    const auto& own = styles.blocks[1];
    CHECK(own.Editable() && own.declarations.size() == 2 && own.declarations[1].text == "color:#333" && own.Location() == "example.com:9"
        && own.groupings == std::vector<std::string>({"@media screen"}));
    // The browser's rule shows the shorthand it was given; it cannot be edited.
    const auto& browser = styles.blocks[2];
    CHECK(!browser.Editable() && browser.declarations.size() == 2 && browser.declarations[1].name == "margin"
        && browser.declarations[1].value == "8px" && browser.declarations[1].overridden && !browser.declarations[0].overridden);
    // From the parent only what is inherited, overridden by the element's own.
    const auto& inherited = styles.blocks[3];
    CHECK(inherited.inheritedFrom == 3 && inherited.declarations.size() == 1 && inherited.declarations[0].overridden);
    // The content box of a border-box element: its width less padding and border.
    CHECK(styles.box.known && styles.box.width == 480 - 4 - 1 && styles.box.height == 300 && styles.box.margin[0] == 8);
    CHECK(dom.Declared("margin-left") && dom.Declared("color") && !dom.Declared("background"));
    // The style sheet is opened at the rule's line in it.
    dom.RequestSheetSource(1);
    take();
    CHECK(tags.count("CSS.getStyleSheetText"));
    answer(tags["CSS.getStyleSheetText"], {{"text", "a {}\n\n\n\nbody { margin:auto; color:#333 }"}});
    CHECK(dom.TakeChanges().source && dom.Source().line == 5 && dom.Source().title == "Style sheet in https://example.com/"
        && dom.Source().error.empty());
    // Switching a declaration off: shown at once, then the styles are read again.
    dom.ToggleDeclaration(1, 1);
    CHECK(!dom.Styles().blocks[1].declarations[1].enabled);
    auto commands = take();
    CHECK(commands.size() == 1 && commands[0].method == "CSS.setStyleText");
    const auto set = json::parse(commands[0].parameters);
    CHECK(set["styleId"]["styleSheetId"] == "1" && set["styleId"]["ordinal"] == 1 && set["text"] == "margin:auto; /* color:#333; */");
    answer(commands[0].tag, {{"style", json::object()}});
    commands = take();
    CHECK(commands.size() == 4 && dom.StylesLoading());
    // element.style takes a new declaration.
    dom.AddDeclaration(0, "color: red");
    commands = take();
    CHECK(commands.size() == 1 && json::parse(commands[0].parameters)["text"] == "color: red;"
        && json::parse(commands[0].parameters)["styleId"]["styleSheetId"] == "5");
    // A property's computed value, for what var() comes to.
    CHECK(dom.ComputedValue("width") == "480px" && dom.ComputedValue("color") == "rgb(51, 51, 51)" && dom.ComputedValue("--none").empty());
    // :hov forces states on the selected element and reads its styles again.
    take();
    dom.SetForcedPseudoClasses(5, {"hover", "focus", "hover"});
    {
        const auto commands = take();
        const auto* forced = [&]() -> const DOMModel::Command* {
            for (const auto& command : commands) if (command.method == "CSS.forcePseudoState") return &command;
            return nullptr;
        }();
        CHECK(forced && json::parse(forced->parameters) == json({{"nodeId", 5}, {"forcedPseudoClasses", {"focus", "hover"}}}));
        CHECK(tags.count("CSS.getMatchedStylesForNode") || dom.StylesLoading());
    }
    CHECK((dom.ForcedPseudoClasses(5) == std::vector<std::string> {"focus", "hover"}) && dom.ForcedPseudoClasses(6).empty());
    dom.SetForcedPseudoClasses(99, {"hover"});
    CHECK(dom.ForcedPseudoClasses(99).empty());
    dom.SetForcedPseudoClasses(5, {});
    take();
    CHECK(dom.ForcedPseudoClasses(5).empty());
    // The author's rule's selector is changed; the browser's cannot be.
    {
        const auto& blocks = dom.Styles().blocks;
        size_t authorBlock = blocks.size(), agentBlock = blocks.size();
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (blocks[i].origin == "author" && blocks[i].SelectorEditable()) authorBlock = std::min(authorBlock, i);
            if (blocks[i].origin == "user-agent") agentBlock = i;
        }
        CHECK(authorBlock < blocks.size() && agentBlock < blocks.size() && !blocks[agentBlock].SelectorEditable());
        take();
        dom.SetRuleSelector(authorBlock, "  body.page  ");
        dom.SetRuleSelector(agentBlock, "html");
        const auto commands = take();
        CHECK(commands.size() == 1 && commands[0].method == "CSS.setRuleSelector"
            && json::parse(commands[0].parameters) == json({{"ruleId", {{"styleSheetId", "1"}, {"ordinal", 1}}}, {"selector", "body.page"}}));
    }
    // New Style Rule: the main frame, its inspector style sheet, then the rule;
    // the sheet is made once.
    dom.AddRule("div#main");
    take();
    CHECK(tags.count("Page.getResourceTree"));
    answer(tags["Page.getResourceTree"], {{"frameTree", {{"frame", {{"id", "frame-1"}}}}}});
    {
        const auto commands = take();
        CHECK(commands.size() == 1 && commands[0].method == "CSS.createStyleSheet"
            && json::parse(commands[0].parameters)["frameId"] == "frame-1");
        answer(commands[0].tag, {{"styleSheetId", "9"}});
    }
    {
        const auto commands = take();
        CHECK(commands.size() == 1 && commands[0].method == "CSS.addRule"
            && json::parse(commands[0].parameters) == json({{"styleSheetId", "9"}, {"selector", "div#main"}}));
    }
    dom.AddRule("p");
    {
        const auto commands = take();
        CHECK(commands.size() == 1 && commands[0].method == "CSS.addRule" && json::parse(commands[0].parameters)["styleSheetId"] == "9");
    }
    // Event listeners of the selected element and above it.
    dom.RequestListeners();
    take();
    CHECK(tags.count("DOM.getEventListenersForNode"));
    answer(tags["DOM.getEventListenersForNode"], {{"listeners", json::array({
        {{"type", "click"}, {"useCapture", false}, {"isAttribute", false}, {"nodeId", 5}, {"handlerName", "onClick"},
            {"location", {{"scriptId", "7"}, {"lineNumber", 11}, {"columnNumber", 2}}}, {"once", true}},
        {{"type", "load"}, {"useCapture", true}, {"isAttribute", true}, {"onWindow", true}, {"passive", true}}})}});
    CHECK(dom.TakeChanges().listeners);
    const auto& listeners = dom.Listeners();
    CHECK(listeners.node == 5 && listeners.error.empty() && listeners.listeners.size() == 2);
    CHECK(listeners.listeners[0].type == "click" && listeners.listeners[0].handler == "onClick" && listeners.listeners[0].once
        && !listeners.listeners[0].capture && listeners.listeners[0].location == "script 7, line 12" && !listeners.listeners[0].node.empty());
    CHECK(listeners.listeners[1].node == "window" && listeners.listeners[1].capture && listeners.listeners[1].attribute
        && listeners.listeners[1].passive && listeners.listeners[1].handler == "(anonymous)");
}

static void TestPageSource()
{
    const auto inner = [](const std::string& outer) {
        return json::parse(json::parse(outer)["params"]["message"].get<std::string>());
    };
    PageSourceRequest request;
    CHECK(request.TakeOutgoing().empty());
    // A provisional page is not the one shown.
    request.Receive(TargetCreated("page-2", true));
    CHECK(request.TakeOutgoing().empty());
    request.Receive(TargetCreated("page-1"));
    auto sent = request.TakeOutgoing();
    CHECK(sent.size() == 1);
    const auto outer = json::parse(sent.at(0));
    auto command = inner(sent.at(0));
    CHECK(outer["method"] == "Target.sendMessageToTarget" && outer["params"]["targetId"] == "page-1" && command["method"] == "Page.getResourceTree");
    request.Receive(Reply("page-1", command["id"], {{"frameTree", {{"frame", {{"id", "0.1"}, {"url", "https://example.com/a#b"},
        {"mimeType", "text/html"}}}, {"resources", json::array()}}}}));
    command = inner(request.TakeOutgoing().at(0));
    CHECK(command["method"] == "Page.getResourceContent" && command["params"]["frameId"] == "0.1"
        && command["params"]["url"] == "https://example.com/a#b");
    CHECK(request.Status() == PageSourceRequest::State::Waiting);
    request.Receive(Reply("page-1", command["id"], {{"content", "<!doctype html><p>Hi"}, {"base64Encoded", false}}));
    CHECK(request.Status() == PageSourceRequest::State::Done && request.Text() == "<!doctype html><p>Hi"
        && request.URL() == "https://example.com/a#b" && request.MimeType() == "text/html");

    PageSourceRequest encoded;
    encoded.Receive(TargetCreated("page-1"));
    command = inner(encoded.TakeOutgoing().at(0));
    encoded.Receive(Reply("page-1", command["id"], {{"frameTree", {{"frame", {{"id", "f"}, {"url", "https://example.com/"}}}}}}));
    command = inner(encoded.TakeOutgoing().at(0));
    encoded.Receive(Reply("page-1", command["id"], {{"content", "PGI+eDwvYj4="}, {"base64Encoded", true}}));
    CHECK(encoded.Status() == PageSourceRequest::State::Done && encoded.Text() == "<b>x</b>");

    PageSourceRequest empty;
    empty.Receive(TargetCreated("page-1"));
    command = inner(empty.TakeOutgoing().at(0));
    empty.Receive(Reply("page-1", command["id"], {{"frameTree", {{"frame", {{"id", "f"}, {"url", "about:blank"}}}}}}));
    command = inner(empty.TakeOutgoing().at(0));
    empty.Receive(Reply("page-1", command["id"], {{"content", ""}, {"base64Encoded", false}}));
    CHECK(empty.Status() == PageSourceRequest::State::Failed && !empty.Error().empty());

    PageSourceRequest closed;
    closed.Receive(TargetCreated("page-1"));
    closed.Receive(json{{"method", "Target.targetDestroyed"}, {"params", {{"targetId", "page-1"}}}}.dump());
    CHECK(closed.Status() == PageSourceRequest::State::Failed);

    // Not the page's first connection: the target is assumed, the page's
    // answers to other connections are passed over, and a target announced
    // later changes nothing.
    PageSourceRequest second;
    second.AssumeTarget("page-7");
    command = inner(second.TakeOutgoing().at(0));
    CHECK(command["method"] == "Page.getResourceTree" && command["id"].get<int64_t>() >= 2000000000);
    second.Receive(Reply("page-7", 3, {{"frameTree", {{"frame", {{"id", "x"}, {"url", "https://other.example/"}}}}}}));
    CHECK(second.TakeOutgoing().empty() && second.Status() == PageSourceRequest::State::Waiting);
    second.Receive(TargetCreated("page-7"));
    CHECK(second.TakeOutgoing().empty());
    // A target that is not the page's.
    PageSourceRequest wrong;
    wrong.AssumeTarget("page-9");
    const auto outerID = json::parse(wrong.TakeOutgoing().at(0))["id"];
    wrong.Receive(json{{"id", outerID}, {"error", {{"message", "Missing target for given targetId"}}}}.dump());
    CHECK(wrong.Status() == PageSourceRequest::State::Failed);
    // Two requests number their commands apart.
    PageSourceRequest third;
    third.AssumeTarget("page-7");
    CHECK(inner(third.TakeOutgoing().at(0))["id"] != command["id"]);

    // A session that is not the page's first assumes its target too.
    Session session;
    session.Start();
    session.AssumePageTarget("page-7");
    const auto commands = Sent(session);
    CHECK(Find(commands, "Network.enable") && Find(commands, "Network.enable")->target == "page-7");
    session.Receive(TargetCreated("page-7"));
    CHECK(Sent(session).empty());
}

int main()
{
    TestSession();
    TestStorage();
    TestElements();
    TestElementStyles();
    TestPageSource();
    TestConsole();
    TestObjectTree();
    TestExports();
    TestDebugger();
    TestHeaders();
    TestKinds();
    TestJSON();
    TestXML();
    TestHTML();
    TestHelpers();
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
