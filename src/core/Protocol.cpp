#include "Protocol.h"
#include "Address.h"
#include <algorithm>
#include <array>

namespace summit {
namespace {
bool Alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool Controls(std::string_view s)
{
    return std::any_of(s.begin(), s.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
std::string Lower(std::string_view s)
{
    std::string result(s);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return result;
}
}
std::string URLScheme(std::string_view url)
{
    const auto colon = url.find(':');
    if (colon == std::string_view::npos || !colon || colon > 64 || !Alpha(url.front())
        || url.size() > 65536 || Controls(url)) return {};
    for (char c : url.substr(0, colon))
        if (!Alpha(c) && !(c >= '0' && c <= '9') && c != '+' && c != '-' && c != '.') return {};
    return Lower(url.substr(0, colon));
}
bool IsExternalScheme(std::string_view scheme)
{
    if (URLScheme(std::string(scheme) + ':') != scheme) return false;
    constexpr std::array blocked {"http", "https", "file", "about", "data", "blob", "javascript",
        "webkit-extension", "chrome-extension", "summit", "filesystem", "view-source"};
    return std::find(blocked.begin(), blocked.end(), scheme) == blocked.end();
}
bool IsExtensionPageURL(std::string_view url)
{
    return url.starts_with("webkit-extension:") || url.starts_with("chrome-extension:");
}
bool IsWebHandlerScheme(std::string_view scheme)
{
    constexpr std::array allowed {"bitcoin", "ftp", "ftps", "geo", "im", "irc", "ircs", "magnet",
        "mailto", "matrix", "mms", "news", "nntp", "openpgp4fpr", "sftp", "sip", "sms", "smsto",
        "ssh", "tel", "urn", "webcal", "wtai", "xmpp"};
    if (std::find(allowed.begin(), allowed.end(), scheme) != allowed.end()) return true;
    return scheme.substr(0, 4) == "web+" && scheme.size() > 4
        && std::all_of(scheme.begin() + 4, scheme.end(), [](char c) { return c >= 'a' && c <= 'z'; });
}
std::string ProtocolOrigin(std::string_view url)
{
    const auto scheme = URLScheme(url);
    if ((scheme != "https" && scheme != "http") || url.substr(scheme.size(), 3) != "://") return {};
    auto authority = url.substr(scheme.size() + 3);
    authority = authority.substr(0, authority.find_first_of("/?#"));
    if (authority.empty() || authority.find_first_of("@\\ %<>^`|{}\"") != std::string_view::npos) return {};
    std::string_view host = authority, port;
    if (authority.front() == '[') {
        const auto end = authority.find(']');
        if (end == std::string_view::npos || end <= 2) return {};
        host = authority.substr(0, end + 1);
        const auto ip = host.substr(1, host.size() - 2);
        if (ip.find(':') == std::string_view::npos || !std::all_of(ip.begin(), ip.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == ':' || c == '.';
        })) return {};
        if (end + 1 < authority.size()) {
            if (authority[end + 1] != ':') return {};
            port = authority.substr(end + 2);
        }
    } else {
        const auto colon = authority.find(':');
        host = authority.substr(0, colon);
        if (colon != std::string_view::npos) port = authority.substr(colon + 1);
        if (host.find_first_of("[]") != std::string_view::npos) return {};
    }
    if (host.empty()) return {};
    auto normalized = Lower(host);
    if (!port.empty()) {
        unsigned number = 0;
        for (char c : port) {
            if (c < '0' || c > '9' || number > 6553) return {};
            number = number * 10 + c - '0';
            if (number > 65535) return {};
        }
        if (number != (scheme == "http" ? 80u : 443u)) normalized += ':' + std::to_string(number);
    }
    return scheme + "://" + normalized;
}
bool ValidProtocolHandler(std::string_view scheme, const ProtocolHandler& handler)
{
    if (!IsExternalScheme(scheme) || handler.target.empty() || handler.target.size() > 65536
        || Controls(handler.target)) return false;
    if (handler.kind == "app") return handler.target.front() == '/' && handler.target.size() <= 4096 && handler.origin.empty();
    if (handler.kind != "web" || !IsWebHandlerScheme(scheme) || handler.target.find("%s") == std::string::npos) return false;
    const auto origin = ProtocolOrigin(handler.target);
    return !origin.empty() && origin == handler.origin;
}
std::string ProtocolHandlerURL(const ProtocolHandler& handler, std::string_view input)
{
    const auto scheme = URLScheme(input);
    if (handler.kind != "web" || !ValidProtocolHandler(scheme, handler)) return {};
    std::string url(input);
    // URL credentials must not be passed on to a registered website.
    if (url.substr(scheme.size(), 3) == "://") {
        const auto start = scheme.size() + 3;
        const auto end = url.find_first_of("/?#", start);
        const auto at = url.rfind('@', end == std::string::npos ? url.size() : end);
        if (at != std::string::npos && at >= start) url.erase(start, at - start + 1);
    }
    auto result = handler.target;
    result.replace(result.find("%s"), 2, PercentEncode(url));
    return result;
}
}
