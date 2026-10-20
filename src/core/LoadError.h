#pragma once
#include <string>

namespace summit {
// Why a page could not be loaded, as the error page in its tab says (#89).
enum class LoadFailure {
    // Not a failure to show a page for: a media document or plug-in that
    // took over the load, a download or policy hand-off, an extension's view.
    None,
    HostNotFound,
    Refused,
    Reset,
    TimedOut,
    Offline,
    Secure,
    Other,
};
// The kind of the engine's error: its domain and code as the engine reports
// them (CurlErrorDomain with libcurl's codes, WebKitErrorDomain, ...).
LoadFailure ClassifyLoadFailure(const std::string& domain, int code);
// A video or audio file opened in a tab: the media document took the load
// over from the page (WebKitErrorDomain 204, "Plug-in handled load"). The
// page loaded; it is not an error (#95).
bool IsMediaTakeover(const std::string& domain, int code);
// What the error page shows: a title ("Can't find example.com") and a
// sentence or two of explanation. url is the address that failed;
// description the engine's own text, shown for failures without a kind.
struct LoadFailureText {
    std::string title, explanation;
};
LoadFailureText DescribeLoadFailure(LoadFailure failure, const std::string& url, const std::string& description);
// The host of an address ("example.com" of "https://example.com:8080/a"),
// or the address itself when it has none.
std::string LoadFailureHost(const std::string& url);
}
