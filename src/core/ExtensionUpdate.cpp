#include "ExtensionUpdate.h"
#include <algorithm>
#include <cctype>
#include <vector>

namespace summit {
namespace {
bool chromeIdentity(const std::string& value)
{
    return value.size() == 32 && std::all_of(value.begin(), value.end(), [](char c) { return c >= 'a' && c <= 'p'; });
}
std::string percentEncode(const std::string& value)
{
    static const char* hex = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') result += char(c);
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}
// The value of attribute name in the start tag text.
std::optional<std::string> attribute(const std::string& tag, const std::string& name)
{
    for (size_t at = tag.find(name + "="); at != std::string::npos; at = tag.find(name + "=", at + 1)) {
        if (at && !std::isspace(static_cast<unsigned char>(tag[at - 1]))) continue;
        const size_t quote = at + name.size() + 1;
        if (quote >= tag.size() || (tag[quote] != '"' && tag[quote] != '\'')) return std::nullopt;
        const auto end = tag.find(tag[quote], quote + 1);
        if (end == std::string::npos) return std::nullopt;
        return tag.substr(quote + 1, end - quote - 1);
    }
    return std::nullopt;
}
}

std::string ExtensionUpdateSource(const nlohmann::json& manifest, const std::string& identifier, const std::string& verifiedCRXIdentity)
{
    if (!manifest.is_object()) return {};
    if (chromeIdentity(identifier) && verifiedCRXIdentity == identifier) {
        const auto url = manifest.value("update_url", std::string());
        if (url == "https://clients2.google.com/service/update2/crx" || url == "http://clients2.google.com/service/update2/crx")
            return "chrome";
        return {};
    }
    for (const char* key : { "browser_specific_settings", "applications" }) {
        if (!manifest.contains(key) || !manifest[key].is_object()) continue;
        const auto& settings = manifest[key];
        if (settings.contains("gecko") && settings["gecko"].is_object() && settings["gecko"].value("id", std::string()) == identifier
            && !identifier.empty())
            return "firefox";
    }
    return {};
}

int CompareExtensionVersions(const std::string& a, const std::string& b)
{
    auto split = [](const std::string& value) {
        std::vector<std::string> parts;
        for (size_t start = 0;;) {
            const auto dot = value.find('.', start);
            parts.push_back(value.substr(start, dot == std::string::npos ? dot : dot - start));
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        return parts;
    };
    const auto left = split(a), right = split(b);
    for (size_t i = 0; i < std::max(left.size(), right.size()); ++i) {
        const std::string x = i < left.size() ? left[i] : "0", y = i < right.size() ? right[i] : "0";
        size_t xDigits = 0, yDigits = 0;
        while (xDigits < x.size() && std::isdigit(static_cast<unsigned char>(x[xDigits]))) ++xDigits;
        while (yDigits < y.size() && std::isdigit(static_cast<unsigned char>(y[yDigits]))) ++yDigits;
        // Numbers without their leading zeros compare by length, then digits.
        auto number = [](const std::string& text, size_t digits) {
            size_t start = 0;
            while (start + 1 < digits && text[start] == '0') ++start;
            return text.substr(start, digits - start);
        };
        const auto xNumber = number(x, xDigits), yNumber = number(y, yDigits);
        if (xNumber.size() != yNumber.size()) return xNumber.size() < yNumber.size() ? -1 : 1;
        if (xNumber != yNumber) return xNumber < yNumber ? -1 : 1;
        const auto xRest = x.substr(xDigits), yRest = y.substr(yDigits);
        if (xRest == yRest) continue;
        // A pre-release ("a1", "b2") comes before its release.
        if (xRest.empty()) return 1;
        if (yRest.empty()) return -1;
        return xRest < yRest ? -1 : 1;
    }
    return 0;
}

std::string ChromeUpdateCheckURL(const std::string& identifier, const std::string& version)
{
    return "https://clients2.google.com/service/update2/crx?response=updatecheck&acceptformat=crx3&prodversion=130.0&x="
        + percentEncode("id=" + identifier + "&v=" + version + "&uc");
}

std::optional<std::string> ChromeUpdateCheckVersion(const std::string& response, const std::string& identifier)
{
    for (size_t at = response.find("<app"); at != std::string::npos; at = response.find("<app", at + 4)) {
        const auto end = response.find('>', at);
        if (end == std::string::npos) return std::nullopt;
        if (attribute(response.substr(at, end - at), "appid") != identifier) continue;
        const auto close = response.find("</app>", end);
        const auto check = response.find("<updatecheck", end);
        if (check == std::string::npos || (close != std::string::npos && check > close)) return std::nullopt;
        const auto checkEnd = response.find('>', check);
        if (checkEnd == std::string::npos) return std::nullopt;
        const auto tag = response.substr(check, checkEnd - check);
        if (attribute(tag, "status") != std::string("ok")) return std::nullopt;
        auto version = attribute(tag, "version");
        if (!version || version->empty() || version->size() > 64) return std::nullopt;
        return version;
    }
    return std::nullopt;
}

std::string FirefoxAddonURL(const std::string& identifier)
{
    return "https://addons.mozilla.org/api/v5/addons/addon/" + percentEncode(identifier) + "/";
}
}
