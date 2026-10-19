#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// View Page Source: the page's document as the server sent it, which the Web
// Inspector protocol keeps as the main frame's resource
// (Page.getResourceContent), not the DOM that scripts may have rewritten.
// One question, asked through an inspector session (BWebKitInspectorSession):
// the page's frame and address, then that resource. Nothing here needs Haiku;
// tests/DevToolsTests.cpp covers it.
namespace summit::devtools {

class PageSourceRequest {
public:
    PageSourceRequest();
    enum class State { Waiting, Done, Failed };
    // The page's target ("page-N"), when the session is not the page's
    // first: the engine announces its targets to the first frontend only.
    void AssumeTarget(const std::string& target);
    // Protocol messages for the engine.
    std::vector<std::string> TakeOutgoing();
    // One protocol message from the engine.
    void Receive(std::string_view message);
    // Gives up waiting, with what to tell the reader.
    void Fail(const std::string& error);
    State Status() const { return fState; }
    // The document's address and type, once the page has said.
    const std::string& URL() const { return fURL; }
    const std::string& MimeType() const { return fMimeType; }
    // The source, once Done; what went wrong, once Failed.
    const std::string& Text() const { return fText; }
    const std::string& Error() const { return fError; }

private:
    void SendToPage(int64_t id, const std::string& method, const std::string& parametersJSON);
    State fState = State::Waiting;
    std::string fTarget, fFrame, fURL, fMimeType, fText, fError;
    std::vector<std::string> fOutgoing;
    // The engine gives every connection of the page every answer: the
    // request's commands are numbered apart from the other tools'.
    int64_t fFirstCommand, fNextCommand;
};
}
