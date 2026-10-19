#include "PageSource.h"
#include "DevToolsFormat.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <utility>

namespace summit::devtools {
using nlohmann::json;
namespace {
// Above the developer tools' and the MCP tools' (from 10^9) numbers.
constexpr int64_t kFirstCommand = 2000000000;
// The two questions, after the request's first number.
constexpr int64_t kTree = 0, kContent = 1;
std::atomic<int64_t> sNextRequest {0};

const json& Member(const json& object, const char* key)
{
    static const json nothing;
    if (!object.is_object()) return nothing;
    const auto found = object.find(key);
    return found == object.end() ? nothing : *found;
}
std::string String(const json& value) { return value.is_string() ? value.get<std::string>() : std::string(); }
}

PageSourceRequest::PageSourceRequest()
    : fFirstCommand(kFirstCommand + 16 * sNextRequest++)
    , fNextCommand(fFirstCommand + 2)
{
}

std::vector<std::string> PageSourceRequest::TakeOutgoing() { return std::exchange(fOutgoing, { }); }

void PageSourceRequest::SendToPage(int64_t id, const std::string& method, const std::string& parametersJSON)
{
    const auto inner = json {{"id", fFirstCommand + id}, {"method", method}, {"params", json::parse(parametersJSON)}}.dump();
    fOutgoing.push_back(json {{"id", fNextCommand++}, {"method", "Target.sendMessageToTarget"},
        {"params", {{"targetId", fTarget}, {"message", inner}}}}.dump());
}

void PageSourceRequest::AssumeTarget(const std::string& target)
{
    if (target.empty() || !fTarget.empty() || fState != State::Waiting) return;
    fTarget = target;
    SendToPage(kTree, "Page.getResourceTree", "{}");
}

void PageSourceRequest::Fail(const std::string& error)
{
    if (fState != State::Waiting) return;
    fState = State::Failed;
    fError = error;
}

void PageSourceRequest::Receive(std::string_view text)
{
    if (fState != State::Waiting) return;
    const auto message = json::parse(text, nullptr, false);
    const auto method = String(Member(message, "method"));
    const auto& parameters = Member(message, "params");
    if (method == "Target.targetCreated") {
        const auto& info = Member(parameters, "targetInfo");
        // Only the page that is shown; one that is still loading elsewhere
        // (a provisional target) is not the one the reader asked about.
        if (String(Member(info, "type")) != "page" || Member(info, "isProvisional") == true || !fTarget.empty()) return;
        fTarget = String(Member(info, "targetId"));
        SendToPage(kTree, "Page.getResourceTree", "{}");
        return;
    }
    // The outer command failed: the target assumed is not the page's.
    if (const auto& outer = Member(message, "id"); outer.is_number() && outer.get<int64_t>() > fFirstCommand + 1
        && outer.get<int64_t>() < fNextCommand && Member(message, "error").is_object()) {
        Fail("The page is changing; ask for its source again once it has loaded.");
        return;
    }
    if (method == "Target.targetDestroyed" && String(Member(parameters, "targetId")) == fTarget) {
        Fail("The page closed before it gave its source.");
        return;
    }
    if (method != "Target.dispatchMessageFromTarget" || String(Member(parameters, "targetId")) != fTarget) return;
    const auto inner = json::parse(String(Member(parameters, "message")), nullptr, false);
    const auto& number = Member(inner, "id");
    if (!number.is_number()) return;
    // Answers to other connections' questions come too.
    const auto id = number.get<int64_t>() - fFirstCommand;
    if (id != kTree && id != kContent) return;
    const auto& result = Member(inner, "result");
    if (const auto& error = Member(inner, "error"); error.is_object()) {
        Fail("The page did not give its source: " + String(Member(error, "message")));
        return;
    }
    if (id == kTree) {
        const auto& frame = Member(Member(result, "frameTree"), "frame");
        fFrame = String(Member(frame, "id"));
        fURL = String(Member(frame, "url"));
        fMimeType = String(Member(frame, "mimeType"));
        if (fFrame.empty() || fURL.empty()) {
            Fail("The page has no document.");
            return;
        }
        SendToPage(kContent, "Page.getResourceContent", json {{"frameId", fFrame}, {"url", fURL}}.dump());
    } else if (id == kContent) {
        const auto content = String(Member(result, "content"));
        if (Member(result, "base64Encoded") == true) {
            std::string bytes;
            if (!DecodeBase64(content, bytes)) {
                Fail("The page's source could not be decoded.");
                return;
            }
            fText = ValidUTF8(bytes);
        } else fText = content;
        if (fText.empty()) {
            Fail("The page keeps no source for this document.");
            return;
        }
        fState = State::Done;
    }
}
}
