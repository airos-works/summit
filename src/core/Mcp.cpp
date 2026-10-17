#include "Mcp.h"
#include <algorithm>
#include <cctype>
#include <exception>

namespace summit::mcp {
namespace {
// The protocol versions this server speaks, newest first. A client asking for
// another is answered with the newest, as the specification says.
const char* const kVersions[] = { "2025-06-18", "2025-03-26", "2024-11-05" };

Json Error(const Json& id, int code, const std::string& message)
{
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

Json Result(const Json& id, Json result)
{
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

const char* TypeName(const Json& value)
{
    return value.is_string() ? "a string" : value.is_boolean() ? "a boolean" : value.is_number() ? "a number"
        : value.is_object() ? "an object" : value.is_array() ? "an array" : "null";
}
}

ToolResult ToolResult::Text(const std::string& text)
{
    ToolResult result;
    result.AddText(text);
    return result;
}

ToolResult ToolResult::Data(const Json& value)
{
    ToolResult result;
    result.AddText(value.dump(1, ' ', false, Json::error_handler_t::replace));
    if (value.is_object()) result.structured = value;
    return result;
}

ToolResult ToolResult::Error(const std::string& message)
{
    ToolResult result = Text(message);
    result.isError = true;
    return result;
}

ToolResult ToolResult::Image(const std::string& base64, const std::string& mimeType, const std::string& caption)
{
    ToolResult result;
    result.content.push_back({{"type", "image"}, {"data", base64}, {"mimeType", mimeType}});
    if (!caption.empty()) result.AddText(caption);
    return result;
}

ToolResult& ToolResult::AddText(const std::string& text)
{
    content.push_back({{"type", "text"}, {"text", text}});
    return *this;
}

Schema& Schema::Add(const char* name, Json property, bool required)
{
    fProperties[name] = std::move(property);
    if (required) fRequired.push_back(name);
    return *this;
}

Schema& Schema::String(const char* name, const char* description, bool required)
{
    return Add(name, {{"type", "string"}, {"description", description}}, required);
}

Schema& Schema::Enum(const char* name, const char* description, const std::vector<std::string>& values, bool required)
{
    return Add(name, {{"type", "string"}, {"description", description}, {"enum", values}}, required);
}

Schema& Schema::Integer(const char* name, const char* description, bool required)
{
    return Add(name, {{"type", "integer"}, {"description", description}}, required);
}

Schema& Schema::Number(const char* name, const char* description, bool required)
{
    return Add(name, {{"type", "number"}, {"description", description}}, required);
}

Schema& Schema::Boolean(const char* name, const char* description, bool required)
{
    return Add(name, {{"type", "boolean"}, {"description", description}}, required);
}

Schema& Schema::Object(const char* name, const char* description, bool required)
{
    return Add(name, {{"type", "object"}, {"description", description}}, required);
}

Schema& Schema::StringArray(const char* name, const char* description, bool required)
{
    return Add(name, {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", description}}, required);
}

Schema::operator Json() const
{
    Json schema = {{"type", "object"}, {"properties", fProperties}};
    if (!fRequired.empty()) schema["required"] = fRequired;
    return schema;
}

Server::Server(std::string name, std::string version, std::string instructions)
    : fName(std::move(name)), fVersion(std::move(version)), fInstructions(std::move(instructions))
{
}

void Server::Add(Tool tool)
{
    std::lock_guard lock(fLock);
    fTools.push_back(std::move(tool));
}

size_t Server::ToolCount() const
{
    std::lock_guard lock(fLock);
    return fTools.size();
}

std::vector<std::string> Server::ToolNames() const
{
    std::lock_guard lock(fLock);
    std::vector<std::string> names;
    for (const auto& tool : fTools) names.push_back(tool.name);
    return names;
}

Json Server::Initialize(const Json& params) const
{
    std::string version = kVersions[0];
    if (auto asked = params.find("protocolVersion"); asked != params.end() && asked->is_string())
        for (const char* known : kVersions)
            if (*asked == known) version = known;
    Json result = {{"protocolVersion", version}, {"capabilities", {{"tools", {{"listChanged", false}}}}},
        {"serverInfo", {{"name", fName}, {"version", fVersion}}}};
    if (!fInstructions.empty()) result["instructions"] = fInstructions;
    return result;
}

Json Server::ListTools() const
{
    std::lock_guard lock(fLock);
    Json tools = Json::array();
    for (const auto& tool : fTools)
        tools.push_back({{"name", tool.name}, {"description", tool.description}, {"inputSchema", tool.inputSchema},
            {"annotations", {{"readOnlyHint", tool.readOnly}, {"destructiveHint", tool.destructive},
                {"openWorldHint", false}}}});
    return {{"tools", tools}};
}

ToolResult Server::Call(const std::string& name, const Json& arguments)
{
    ToolHandler handler;
    {
        std::lock_guard lock(fLock);
        auto tool = std::find_if(fTools.begin(), fTools.end(), [&](const Tool& tool) { return tool.name == name; });
        if (tool == fTools.end()) throw std::out_of_range("Unknown tool: " + name);
        handler = tool->handler;
    }
    try {
        return handler(arguments.is_object() ? arguments : Json::object());
    } catch (const std::exception& error) {
        return ToolResult::Error(error.what());
    }
}

Json Server::HandleMessage(const Json& message)
{
    if (!message.is_object() || message.value("jsonrpc", "") != "2.0" || !message.contains("method")
        || !message["method"].is_string())
        return Error(message.is_object() && message.contains("id") ? message["id"] : Json(), -32600, "Invalid request");
    const std::string method = message["method"];
    const bool notification = !message.contains("id");
    const Json id = notification ? Json() : message["id"];
    const Json params = message.contains("params") && message["params"].is_object() ? message["params"] : Json::object();
    if (notification) return Json();
    if (method == "initialize") return Result(id, Initialize(params));
    if (method == "ping") return Result(id, Json::object());
    if (method == "tools/list") return Result(id, ListTools());
    if (method == "resources/list") return Result(id, {{"resources", Json::array()}});
    if (method == "resources/templates/list") return Result(id, {{"resourceTemplates", Json::array()}});
    if (method == "prompts/list") return Result(id, {{"prompts", Json::array()}});
    if (method == "tools/call") {
        if (!params.contains("name") || !params["name"].is_string()) return Error(id, -32602, "tools/call needs a tool name");
        ToolResult result;
        try {
            result = Call(params["name"], params.value("arguments", Json::object()));
        } catch (const std::out_of_range& error) {
            return Error(id, -32602, error.what());
        }
        Json answer = {{"content", result.content}, {"isError", result.isError}};
        if (!result.structured.is_null() && !result.isError) answer["structuredContent"] = result.structured;
        return Result(id, std::move(answer));
    }
    return Error(id, -32601, "Method not found: " + method);
}

std::string Server::Handle(const std::string& body)
{
    Json message;
    try {
        message = Json::parse(body);
    } catch (const std::exception& error) {
        return Error(Json(), -32700, std::string("Parse error: ") + error.what()).dump();
    }
    if (message.is_array()) {
        Json responses = Json::array();
        for (const auto& item : message) {
            Json response = HandleMessage(item);
            if (!response.is_null()) responses.push_back(std::move(response));
        }
        return responses.empty() ? std::string() : responses.dump(-1, ' ', false, Json::error_handler_t::replace);
    }
    Json response = HandleMessage(message);
    return response.is_null() ? std::string() : response.dump(-1, ' ', false, Json::error_handler_t::replace);
}

bool HasArgument(const Json& arguments, const char* name)
{
    return arguments.is_object() && arguments.contains(name) && !arguments[name].is_null();
}

std::string StringArgument(const Json& arguments, const char* name, const std::string& fallback)
{
    if (!HasArgument(arguments, name)) return fallback;
    const Json& value = arguments[name];
    if (value.is_string()) return value.get<std::string>();
    // Clients send numbers for identifiers that look like numbers.
    if (value.is_number_integer()) return std::to_string(value.get<int64_t>());
    throw ToolError(std::string("Argument \"") + name + "\" must be a string, not " + TypeName(value));
}

int64_t IntegerArgument(const Json& arguments, const char* name, int64_t fallback, int64_t minimum, int64_t maximum)
{
    if (!HasArgument(arguments, name)) return fallback;
    const Json& value = arguments[name];
    int64_t number = 0;
    if (value.is_number()) number = value.is_number_float() ? static_cast<int64_t>(value.get<double>()) : value.get<int64_t>();
    else if (value.is_string()) {
        try {
            size_t used = 0;
            number = std::stoll(value.get<std::string>(), &used);
            if (used != value.get<std::string>().size()) throw std::invalid_argument("trailing text");
        } catch (const std::exception&) {
            throw ToolError(std::string("Argument \"") + name + "\" must be an integer");
        }
    } else throw ToolError(std::string("Argument \"") + name + "\" must be an integer, not " + TypeName(value));
    if (number < minimum || number > maximum)
        throw ToolError(std::string("Argument \"") + name + "\" must be between " + std::to_string(minimum) + " and "
            + std::to_string(maximum));
    return number;
}

double NumberArgument(const Json& arguments, const char* name, double fallback)
{
    if (!HasArgument(arguments, name)) return fallback;
    const Json& value = arguments[name];
    if (value.is_number()) return value.get<double>();
    if (value.is_string()) {
        try {
            return std::stod(value.get<std::string>());
        } catch (const std::exception&) {
        }
    }
    throw ToolError(std::string("Argument \"") + name + "\" must be a number");
}

bool BooleanArgument(const Json& arguments, const char* name, bool fallback)
{
    if (!HasArgument(arguments, name)) return fallback;
    const Json& value = arguments[name];
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_string()) {
        if (value == "true" || value == "1" || value == "yes") return true;
        if (value == "false" || value == "0" || value == "no") return false;
    }
    if (value.is_number_integer()) return value.get<int64_t>() != 0;
    throw ToolError(std::string("Argument \"") + name + "\" must be true or false");
}

std::string Base64(const void* data, size_t size)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < size; i += 3) {
        const uint32_t triple = bytes[i] << 16 | bytes[i + 1] << 8 | bytes[i + 2];
        out += table[triple >> 18 & 63];
        out += table[triple >> 12 & 63];
        out += table[triple >> 6 & 63];
        out += table[triple & 63];
    }
    if (i < size) {
        const uint32_t triple = bytes[i] << 16 | (i + 1 < size ? bytes[i + 1] << 8 : 0);
        out += table[triple >> 18 & 63];
        out += table[triple >> 12 & 63];
        out += i + 1 < size ? table[triple >> 6 & 63] : '=';
        out += '=';
    }
    return out;
}

std::string Truncate(const std::string& text, size_t limit)
{
    if (text.size() <= limit) return text;
    size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return text.substr(0, cut) + "\n… (" + std::to_string(text.size() - cut) + " more bytes left out)";
}

bool Matches(const std::string& pattern, const std::string& text)
{
    // Iterative glob with backtracking to the last star.
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    auto same = [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); };
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || same(pattern[p], text[t]))) { ++p; ++t; }
        else if (p < pattern.size() && pattern[p] == '*') { star = p++; mark = t; }
        else if (star != std::string::npos) { p = star + 1; t = ++mark; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}
}
