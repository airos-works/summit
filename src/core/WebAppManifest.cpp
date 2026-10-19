#include "WebAppManifest.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <vector>

namespace summit {
namespace {
using nlohmann::json;

bool StartsWith(const std::string& text, const char* prefix) { return text.rfind(prefix, 0) == 0; }
bool EndsWith(const std::string& text, const std::string& suffix)
{
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string Text(const json& object, const char* key)
{
    if (!object.is_object()) return {};
    auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

std::string Trimmed(std::string text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

bool Web(const std::string& url) { return StartsWith(url, "https://") || StartsWith(url, "http://"); }

// The largest side an icon offers ("192x192 512x512", "any" counts as big).
int IconSize(const std::string& sizes)
{
    int best = 0;
    size_t position = 0;
    while (position < sizes.size()) {
        const auto end = sizes.find(' ', position);
        const auto token = sizes.substr(position, end == std::string::npos ? std::string::npos : end - position);
        if (token == "any") best = std::max(best, 1024);
        else if (const auto x = token.find_first_of("xX"); x != std::string::npos) best = std::max(best, std::atoi(token.c_str()));
        if (end == std::string::npos) break;
        position = end + 1;
    }
    return best;
}

// Haiku shows icons at 16 to 64 pixels: the smallest one of at least 64
// pixels, else the largest; PNG and ICO before SVG, which needs a renderer.
std::string ChooseIcon(const json& icons, const std::string& base)
{
    std::string chosen;
    int chosenScore = -1;
    if (!icons.is_array()) return chosen;
    for (const auto& icon : icons) {
        const auto src = Text(icon, "src");
        if (src.empty()) continue;
        const auto type = Text(icon, "type");
        const auto purpose = Text(icon, "purpose");
        if (type.find("svg") != std::string::npos || EndsWith(src, ".svg") || purpose == "monochrome") continue;
        const int size = IconSize(Text(icon, "sizes"));
        const int score = size >= 64 ? 100000 - size : size;
        if (score > chosenScore) { chosenScore = score; chosen = ResolveURL(base, src); }
    }
    return chosen;
}
}

std::string ResolveURL(const std::string& base, const std::string& reference)
{
    const auto ref = Trimmed(reference);
    if (ref.empty()) return base;
    const auto colon = ref.find(':');
    if (colon != std::string::npos && ref.find_first_of("/?#") > colon) return ref;
    const auto scheme = base.find("://");
    if (scheme == std::string::npos) return {};
    if (StartsWith(ref, "//")) return base.substr(0, scheme + 1) + ref;
    const auto pathStart = base.find('/', scheme + 3);
    const std::string origin = base.substr(0, pathStart);
    if (StartsWith(ref, "/")) return origin + ref;
    std::string path = pathStart == std::string::npos ? "/" : base.substr(pathStart);
    path = path.substr(0, path.find_first_of("?#"));
    if (StartsWith(ref, "?") || StartsWith(ref, "#")) return origin + path + ref;
    path = path.substr(0, path.rfind('/') + 1) + ref;
    // Remove ./ and ../ segments.
    std::string resolved;
    size_t position = 1;
    std::vector<std::string> segments;
    const auto query = path.find_first_of("?#");
    const std::string tail = query == std::string::npos ? std::string() : path.substr(query);
    path = path.substr(0, query);
    while (position <= path.size()) {
        const auto end = path.find('/', position);
        const auto segment = path.substr(position, end == std::string::npos ? std::string::npos : end - position);
        if (segment == "..") { if (!segments.empty()) segments.pop_back(); }
        else if (segment != ".") segments.push_back(segment);
        if (end == std::string::npos) break;
        position = end + 1;
    }
    for (const auto& segment : segments) resolved += "/" + segment;
    if (resolved.empty()) resolved = "/";
    return origin + resolved + tail;
}

bool InWebAppScope(const std::string& scope, const std::string& url)
{
    return !scope.empty() && url.rfind(scope, 0) == 0;
}

std::string LauncherName(const std::string& name)
{
    std::string result;
    for (char character : name) result += character == '/' || character == ':' || static_cast<unsigned char>(character) < 0x20 ? '-' : character;
    result = Trimmed(result);
    if (result.empty() || result == "." || result == "..") result = "Web App";
    if (result.size() > 64) {
        result.resize(64);
        // Not in the middle of a UTF-8 character.
        while (!result.empty() && (static_cast<unsigned char>(result.back()) & 0xC0) == 0x80) result.pop_back();
        if (!result.empty() && static_cast<unsigned char>(result.back()) >= 0xC0) result.pop_back();
    }
    return result;
}

WebAppPlan PlanWebApp(const std::string& pageInfo)
{
    WebAppPlan plan;
    json info;
    try { info = json::parse(pageInfo); } catch (const std::exception&) { return plan; }
    if (!info.is_object()) return plan;
    const auto pageURL = Text(info, "url");
    if (!Web(pageURL)) return plan;
    const auto& manifest = info.contains("manifest") ? info["manifest"] : json();
    const auto manifestURL = Text(info, "manifestURL");
    const std::string base = Web(manifestURL) ? manifestURL : pageURL;
    plan.name = Trimmed(Text(manifest, "name"));
    if (plan.name.empty()) plan.name = Trimmed(Text(manifest, "short_name"));
    if (plan.name.empty()) plan.name = Trimmed(Text(info, "appName"));
    if (plan.name.empty()) plan.name = Trimmed(Text(info, "title"));
    const auto origin = pageURL.substr(0, pageURL.find('/', pageURL.find("://") + 3));
    if (plan.name.empty()) plan.name = origin.substr(origin.find("://") + 3);
    // A start address or scope on another site is ignored, as in other browsers.
    auto sameOrigin = [&](const std::string& url) { return url.rfind(origin + "/", 0) == 0 || url == origin; };
    plan.startURL = ResolveURL(base, Text(manifest, "start_url"));
    if (Text(manifest, "start_url").empty() || !sameOrigin(plan.startURL)) plan.startURL = pageURL;
    plan.scope = ResolveURL(base, Text(manifest, "scope"));
    if (Text(manifest, "scope").empty() || !sameOrigin(plan.scope) || !InWebAppScope(plan.scope, plan.startURL)) {
        const auto path = plan.startURL.substr(0, plan.startURL.find_first_of("?#"));
        plan.scope = path.substr(0, path.rfind('/') + 1);
    }
    plan.display = Text(manifest, "display");
    plan.iconURL = ChooseIcon(manifest.is_object() && manifest.contains("icons") ? manifest["icons"] : json(), base);
    if (plan.iconURL.empty()) plan.iconURL = ChooseIcon(info.contains("icons") ? info["icons"] : json(), pageURL);
    if (plan.name.size() > 200) plan.name.resize(200);
    return plan;
}
}
