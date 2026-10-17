#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

// A Model Context Protocol server (JSON-RPC 2.0 over streamable HTTP, see
// McpHttp.h): the tools a debugging client such as Claude Code calls to look
// inside the running browser. The protocol and the transport need nothing of
// Haiku; the tools themselves are in src/ui/Mcp*.cpp and the transport is
// covered by tests/McpTests.cpp. docs/mcp.md describes the tools.
namespace summit::mcp {

using Json = nlohmann::json;

// What a tool answers: content blocks (text, images) and, for data, the same
// value as structuredContent.
struct ToolResult {
    Json content = Json::array();
    Json structured;
    bool isError = false;

    static ToolResult Text(const std::string& text);
    // Pretty-printed JSON text; objects are also given as structuredContent.
    static ToolResult Data(const Json& value);
    static ToolResult Error(const std::string& message);
    // base64 is the encoded file (a PNG for screenshots).
    static ToolResult Image(const std::string& base64, const std::string& mimeType, const std::string& caption = { });
    ToolResult& AddText(const std::string& text);
};

// Thrown by tools (and argument helpers) for a request that cannot be done;
// the client sees the message as the tool's error result.
struct ToolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

using ToolHandler = std::function<ToolResult(const Json& arguments)>;

struct Tool {
    std::string name;
    std::string description;
    Json inputSchema;
    ToolHandler handler;
    // Hints for clients: the tool only reads, or it changes the browser.
    bool readOnly = true;
    bool destructive = false;
};

// Builds a tool's input schema: an object with typed, described properties.
class Schema {
public:
    Schema& String(const char* name, const char* description, bool required = false);
    Schema& Enum(const char* name, const char* description, const std::vector<std::string>& values, bool required = false);
    Schema& Integer(const char* name, const char* description, bool required = false);
    Schema& Number(const char* name, const char* description, bool required = false);
    Schema& Boolean(const char* name, const char* description, bool required = false);
    // Free-form JSON (an object of protocol parameters).
    Schema& Object(const char* name, const char* description, bool required = false);
    Schema& StringArray(const char* name, const char* description, bool required = false);
    operator Json() const;

private:
    Schema& Add(const char* name, Json property, bool required);
    Json fProperties = Json::object();
    std::vector<std::string> fRequired;
};

class Server {
public:
    Server(std::string name, std::string version, std::string instructions);
    void Add(Tool tool);
    size_t ToolCount() const;
    std::vector<std::string> ToolNames() const;
    // One JSON-RPC message or batch, as text; the response text, or an empty
    // string when nothing is answered (notifications). Any thread: tools run
    // on the caller's thread and must look after their own locking.
    std::string Handle(const std::string& body);
    Json HandleMessage(const Json& message);
    // Calls a tool directly (tests, the browser's own command line).
    ToolResult Call(const std::string& name, const Json& arguments);

private:
    Json Initialize(const Json& params) const;
    Json ListTools() const;
    std::string fName, fVersion, fInstructions;
    mutable std::mutex fLock;
    std::vector<Tool> fTools;
};

// Arguments, with a default when absent. A present value of the wrong type
// throws ToolError naming the argument.
std::string StringArgument(const Json& arguments, const char* name, const std::string& fallback = { });
int64_t IntegerArgument(const Json& arguments, const char* name, int64_t fallback, int64_t minimum = INT64_MIN,
    int64_t maximum = INT64_MAX);
double NumberArgument(const Json& arguments, const char* name, double fallback);
bool BooleanArgument(const Json& arguments, const char* name, bool fallback);
bool HasArgument(const Json& arguments, const char* name);

// Text helpers for tool output.
std::string Base64(const void* data, size_t size);
// At most limit bytes of text, cut at a UTF-8 boundary, with a note of what was left out.
std::string Truncate(const std::string& text, size_t limit);
// Glob match with * and ?, case-insensitive (thread and area names).
bool Matches(const std::string& pattern, const std::string& text);
}
