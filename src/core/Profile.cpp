#include "Profile.h"
#include "HistoryVisits.h"
#include "UserAgent.h"
#include "Address.h"
#include "Zoom.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <functional>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace summit {
using nlohmann::json;
// A newer Summit's profile, which this one leaves as it is.
static constexpr const char* kUnknownVersion = "Unknown profile version";
static json Encode(const std::vector<PageRecord>& pages)
{
    json result = json::array();
    for (const auto& page : pages) {
        json item = {{"url", page.url}, {"title", page.title}};
        if (page.visited) item["visited"] = page.visited;
        if (page.visits > 1 || page.visitCountKnown) item["visits"] = page.visits;
        if (page.visitCountKnown) item["visitCountKnown"] = true;
        if (!page.earlier.empty()) item["earlier"] = page.earlier;
        if (!page.visitTimes.empty()) item["visitTimes"] = page.visitTimes;
        if (!page.visitTransitions.empty()) item["visitTransitions"] = page.visitTransitions;
        if (page.bar) item["bar"] = true;
        result.push_back(std::move(item));
    }
    return result;
}
static std::vector<PageRecord> Decode(const json& value, size_t limit)
{
    if (!value.is_array() || value.size() > limit) throw std::runtime_error("Invalid profile list");
    std::vector<PageRecord> pages;
    for (const auto& item : value) {
        auto url = item.at("url").get<std::string>();
        auto title = item.at("title").get<std::string>();
        if (url.size() > 65536 || title.size() > 65536 || url.find('\0') != std::string::npos)
            throw std::runtime_error("Invalid profile text");
        PageRecord page{url, title};
        if (auto visited = item.find("visited"); visited != item.end()) page.visited = visited->get<int64_t>();
        if (auto bar = item.find("bar"); bar != item.end()) page.bar = bar->get<bool>();
        if (auto visits = item.find("visits"); visits != item.end()) page.visits = std::clamp(visits->get<int>(), 0, 1000000);
        if (auto known = item.find("visitCountKnown"); known != item.end()) page.visitCountKnown = known->get<bool>();
        if (auto earlier = item.find("earlier"); earlier != item.end() && earlier->is_array()) {
            for (const auto& time : *earlier) {
                if (page.earlier.size() >= kMaxEarlierVisits) break;
                if (time.is_number_integer() && time.get<int64_t>() > 0) page.earlier.push_back(time.get<int64_t>());
            }
        }
        if (auto times = item.find("visitTimes"); times != item.end()) {
            if (!times->is_array() || times->size() > kMaxHistoryVisitTimes)
                throw std::runtime_error("Invalid history visit list");
            for (const auto& time : *times) {
                if (!time.is_number_integer() || (time.is_number_unsigned() && time.get<uint64_t>() > uint64_t(INT64_MAX)))
                    throw std::runtime_error("Invalid history visit time");
                page.visitTimes.push_back(time.get<int64_t>());
            }
        }
        if (auto transitions = item.find("visitTransitions"); transitions != item.end()) {
            if (!transitions->is_array() || transitions->size() > kMaxHistoryVisitTimes * 9)
                throw std::runtime_error("Invalid history visit transitions");
            for (const auto& visit : *transitions) {
                if (!visit.is_array() || visit.size() != 2 || !visit[0].is_number_integer() || !visit[1].is_number_integer()
                    || (visit[0].is_number_unsigned() && visit[0].get<uint64_t>() > uint64_t(INT64_MAX))
                    || (!visit[1].is_number_unsigned() && visit[1].get<int64_t>() < 0) || visit[1].get<uint64_t>() > UINT32_MAX)
                    throw std::runtime_error("Invalid history visit transition");
                page.visitTransitions.emplace_back(visit[0].get<int64_t>(), visit[1].get<uint32_t>());
            }
        }
        if (item.contains("visitTimes") || item.contains("visitTransitions")) SetHistoryVisitTimes(page, HistoryVisitTimes(page));
        pages.push_back(std::move(page));
    }
    return pages;
}
// Folders have "children", bookmarks "url", separators "separator": true.
static json EncodeBookmarks(const std::vector<BookmarkNode>& nodes)
{
    json result = json::array();
    for (const auto& node : nodes) {
        json item = {{"title", node.title}};
        if (node.folder) item["children"] = EncodeBookmarks(node.children);
        else if (node.separator) item["separator"] = true;
        else item["url"] = node.url;
        if (node.added) item["added"] = node.added;
        if (node.modified > 0) item["modified"] = node.modified;
        if (!node.origin.Empty()) item["origin"] = {{"scope", node.origin.scope}, {"guid", node.origin.guid}, {"id", node.origin.id}};
        if (!node.metadata.description.empty()) item["description"] = node.metadata.description;
        if (!node.metadata.tags.empty()) item["tags"] = node.metadata.tags;
        if (!node.folder && !node.separator && !node.metadata.keywords.empty()) {
            item["keywords"] = json::array();
            for (const auto& keyword : node.metadata.keywords)
                item["keywords"].push_back({{"name", keyword.name}, {"postData", keyword.postData}, {"charset", keyword.charset}});
        }
        result.push_back(std::move(item));
    }
    return result;
}
// Unusable entries are dropped rather than failing the profile; so are
// folders nested deeper than kMaxBookmarkDepth and anything past the limits.
static void DecodeBookmarks(const json& value, BookmarkNode& folder, size_t depth, size_t& bookmarks, size_t& folders,
    size_t& separators)
{
    if (!value.is_array()) return;
    for (const auto& item : value) {
        if (!item.is_object()) continue;
        auto title = item.find("title");
        if (title == item.end() || !title->is_string() || title->get_ref<const std::string&>().size() > 65536) continue;
        BookmarkNode node;
        node.title = title->get<std::string>();
        if (auto origin = item.find("origin"); origin != item.end() && origin->is_object()
            && origin->contains("scope") && (*origin)["scope"].is_string()
            && origin->contains("guid") && (*origin)["guid"].is_string()
            && origin->contains("id") && (*origin)["id"].is_number_integer())
            node.origin = NormalizeBookmarkOrigin({(*origin)["scope"].get<std::string>(), (*origin)["guid"].get<std::string>(), (*origin)["id"].get<int64_t>()});
        if (auto added = item.find("added"); added != item.end() && added->is_number_integer()) node.added = added->get<int64_t>();
        if (auto modified = item.find("modified"); modified != item.end() && modified->is_number_integer()
            && (!modified->is_number_unsigned() || modified->get<uint64_t>() <= uint64_t(INT64_MAX)))
            node.modified = std::max(int64_t(0), modified->get<int64_t>());
        if (auto children = item.find("children"); children != item.end()) {
            if (depth + 1 > kMaxBookmarkDepth || folders >= kMaxBookmarkFolders) continue;
            node.folder = true;
            ++folders;
            DecodeBookmarks(*children, node, depth + 1, bookmarks, folders, separators);
        } else if (auto separator = item.find("separator"); separator != item.end() && *separator == true) {
            if (separators >= kMaxBookmarkSeparators) continue;
            node.separator = true;
            ++separators;
        } else {
            auto url = item.find("url");
            if (url == item.end() || !url->is_string() || bookmarks >= kMaxBookmarks) continue;
            node.url = url->get<std::string>();
            if (!IsBookmarkableURL(node.url)) continue;
            ++bookmarks;
        }
        if (!node.separator) {
            if (auto description = item.find("description"); description != item.end() && description->is_string())
                node.metadata.description = description->get<std::string>();
            if (auto tags = item.find("tags"); tags != item.end() && tags->is_array()) {
                for (const auto& tag : *tags) {
                    if (tag.is_string()) node.metadata.tags.push_back(tag.get<std::string>());
                    if (node.metadata.tags.size() == kMaxBookmarkTags) break;
                }
            }
            if (!node.folder) {
                if (auto keywords = item.find("keywords"); keywords != item.end() && keywords->is_array()) {
                    for (const auto& keyword : *keywords) {
                        if (!keyword.is_object() || !keyword.contains("name") || !keyword["name"].is_string()) continue;
                        BookmarkKeyword value{keyword["name"].get<std::string>(), {}, {}};
                        if (keyword.contains("postData") && keyword["postData"].is_string()) value.postData = keyword["postData"].get<std::string>();
                        if (keyword.contains("charset") && keyword["charset"].is_string()) value.charset = keyword["charset"].get<std::string>();
                        node.metadata.keywords.push_back(std::move(value));
                        if (node.metadata.keywords.size() == kMaxBookmarkKeywords) break;
                    }
                }
            }
            node.metadata = NormalizeBookmarkMetadata(std::move(node.metadata));
        }
        folder.children.push_back(std::move(node));
    }
}
Profile Profile::Load(const std::filesystem::path& path, std::string& error)
{
    error.clear();
    Profile profile;
    try {
        if (!std::filesystem::exists(path)) return profile;
        if (std::filesystem::file_size(path) > 16 * 1024 * 1024) throw std::runtime_error("Profile is too large");
        std::ifstream stream(path);
        auto j = json::parse(stream);
        if (j.at("version") != 1) throw std::runtime_error(kUnknownVersion);
        if (auto windows = j.find("windows"); windows != j.end()) {
            if (!windows->is_array() || windows->size() > 64) throw std::runtime_error("Invalid window list");
            for (const auto& item : *windows) {
                WindowSession window;
                window.tabs = Decode(item.at("tabs"), 512);
                window.selected = item.at("selected").get<size_t>();
                if (window.selected >= window.tabs.size()) window.selected = 0;
                if (auto pinned = item.find("pinned"); pinned != item.end() && pinned->is_number_unsigned())
                    window.pinned = std::min(pinned->get<size_t>(), window.tabs.size());
                if (auto frame = item.find("frame"); frame != item.end()) {
                    if (!frame->is_array() || frame->size() != 4) throw std::runtime_error("Invalid window frame");
                    for (size_t i = 0; i < 4; ++i) window.frame[i] = (*frame)[i].get<float>();
                }
                if (!window.tabs.empty()) profile.windows.push_back(std::move(window));
            }
        } else {
            WindowSession window;
            window.tabs = Decode(j.at("tabs"), 512);
            window.selected = j.at("selected").get<size_t>();
            if (window.selected >= window.tabs.size()) window.selected = 0;
            if (!window.tabs.empty()) profile.windows.push_back(std::move(window));
        }
        const auto pages = Decode(j.at("bookmarks"), kMaxBookmarks);
        // Builds since the folders came keep them in "bookmarkTree"; older
        // ones (which leave it out when they save) only have the flat list.
        if (auto tree = j.find("bookmarkTree"); tree != j.end() && tree->is_object()) {
            BookmarkNode bar, other;
            for (auto pair : {std::pair{"barModified", &bar}, std::pair{"otherModified", &other}})
                if (auto value = tree->find(pair.first); value != tree->end() && value->is_number_integer()
                    && (!value->is_number_unsigned() || value->get<uint64_t>() <= uint64_t(INT64_MAX)))
                    pair.second->modified = std::max(int64_t(0), value->get<int64_t>());
            size_t bookmarks = 0, folders = 0, separators = 0;
            if (auto items = tree->find("bar"); items != tree->end()) DecodeBookmarks(*items, bar, 0, bookmarks, folders, separators);
            if (auto items = tree->find("other"); items != tree->end()) DecodeBookmarks(*items, other, 0, bookmarks, folders, separators);
            profile.bookmarks.Assign(std::move(bar), std::move(other));
        } else profile.bookmarks.AssignPages(pages);
        profile.history = Decode(j.at("history"), 2000);
        if (auto home = j.find("homeURL"); home != j.end()) {
            profile.homeURL = home->get<std::string>();
            if (profile.homeURL.size() > 65536 || profile.homeURL.find('\0') != std::string::npos)
                throw std::runtime_error("Invalid home page");
        }
        if (auto bar = j.find("showBookmarksBar"); bar != j.end()) profile.showBookmarksBar = bar->get<bool>();
        if (auto agent = j.find("userAgent"); agent != j.end() && agent->is_string())
            profile.userAgent = FindUserAgentPreset(agent->get<std::string>()).id;
        if (auto text = j.find("zoomTextOnly"); text != j.end() && text->is_boolean()) profile.zoomTextOnly = text->get<bool>();
        if (auto language = j.find("spellingLanguage"); language != j.end() && language->is_string() && language->get<std::string>().size() < 64)
            profile.spellingLanguage = language->get<std::string>();
        if (auto restore = j.find("restoreSession"); restore != j.end()) profile.restoreSession = restore->get<bool>();
        if (auto save = j.find("savePasswords"); save != j.end() && save->is_boolean()) profile.savePasswords = *save;
        if (auto never = j.find("passwordNever"); never != j.end() && never->is_array() && never->size() <= 10000)
            for (const auto& origin : *never) if (origin.is_string()) profile.passwordNever.insert(origin.get<std::string>());
        if (auto spelling = j.find("spellChecking"); spelling != j.end() && spelling->is_boolean()) profile.spellChecking = *spelling;
        if (auto apps = j.find("webApps"); apps != j.end() && apps->is_array() && apps->size() <= 500) {
            for (const auto& item : *apps) {
                if (!item.is_object()) continue;
                WebApp app { item.value("id", ""), item.value("name", ""), item.value("startURL", ""),
                    item.value("scope", ""), item.value("launcher", "") };
                const bool valid = !app.id.empty() && app.id.size() <= 64
                    && std::all_of(app.id.begin(), app.id.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)); })
                    && (app.startURL.rfind("https://", 0) == 0 || app.startURL.rfind("http://", 0) == 0);
                if (valid && !profile.FindWebApp(app.id)) profile.webApps.push_back(std::move(app));
            }
        }
        if (auto autoplay = j.find("autoplay"); autoplay != j.end() && autoplay->is_string()) {
            const auto value = autoplay->get<std::string>();
            if (value == "allow" || value == "block-audio" || value == "block") profile.autoplay = value;
        }
        if (auto style = j.find("interfaceStyle"); style != j.end()) {
            profile.interfaceStyle = style->get<std::string>();
            if (profile.interfaceStyle != "haiku" && profile.interfaceStyle != "safari") profile.interfaceStyle = "haiku";
        }
        if (auto engine = j.find("searchEngine"); engine != j.end()) {
            const auto id = engine->get<std::string>();
            for (const auto& known : SearchEngines())
                if (id == known.id) profile.searchEngine = id;
        }
        if (auto zoom = j.find("siteZoom"); zoom != j.end()) {
            if (!zoom->is_object() || zoom->size() > 5000) throw std::runtime_error("Invalid site zoom list");
            for (const auto& [site, value] : zoom->items()) {
                const double factor = value.get<double>();
                // Unusable entries are dropped rather than failing the profile.
                if (site.empty() || site.size() > 255 || !(factor >= kMinimumZoom && factor <= kMaximumZoom)) continue;
                profile.siteZoom[site] = factor;
            }
        }
        if (auto unpinned = j.find("unpinnedExtensions"); unpinned != j.end()) {
            if (!unpinned->is_array() || unpinned->size() > 1000) throw std::runtime_error("Invalid unpinned extension list");
            for (const auto& identifier : *unpinned) {
                const auto text = identifier.get<std::string>();
                if (!text.empty() && text.size() <= 255) profile.unpinnedExtensions.insert(text);
            }
        }
        if (auto permissions = j.find("sitePermissions"); permissions != j.end()) {
            if (!permissions->is_object()) throw std::runtime_error("Invalid site permissions");
            for (const auto& [permission, origins] : permissions->items()) {
                if (!origins.is_object() || origins.size() > 10000) throw std::runtime_error("Invalid site permissions");
                // Unusable entries are dropped rather than failing the profile.
                for (const auto& [origin, allowed] : origins.items())
                    if (allowed.is_boolean()) profile.SetSitePermission(permission, origin, allowed.get<bool>());
            }
        }
        if (auto mcp = j.find("mcp"); mcp != j.end() && mcp->is_object()) {
            // Unusable values keep their defaults rather than failing the profile.
            if (auto enabled = mcp->find("enabled"); enabled != mcp->end() && enabled->is_boolean()) profile.mcpEnabled = *enabled;
            if (auto port = mcp->find("port"); port != mcp->end() && port->is_number_integer() && *port > 0 && *port < 65536)
                profile.mcpPort = *port;
            if (auto local = mcp->find("localOnly"); local != mcp->end() && local->is_boolean()) profile.mcpLocalOnly = *local;
            if (auto token = mcp->find("token"); token != mcp->end() && token->is_string() && token->get<std::string>().size() <= 256)
                profile.mcpToken = *token;
        }
        if (auto updates = j.find("updates"); updates != j.end() && updates->is_object()) {
            if (auto check = updates->find("check"); check != updates->end() && check->is_boolean()) profile.checkForUpdates = *check;
            if (auto last = updates->find("lastCheck"); last != updates->end() && last->is_number_integer() && *last >= 0)
                profile.lastUpdateCheck = *last;
        }
        if (auto handlers = j.find("protocolHandlers"); handlers != j.end() && handlers->is_object() && handlers->size() <= 1000) {
            for (const auto& [scheme, item] : handlers->items()) {
                if (!item.is_object()) continue;
                auto kind = item.find("kind"), target = item.find("target"), origin = item.find("origin");
                if (kind == item.end() || target == item.end() || origin == item.end()
                    || !kind->is_string() || !target->is_string() || !origin->is_string()) continue;
                profile.SetProtocolHandler(scheme, {kind->get<std::string>(), target->get<std::string>(), origin->get<std::string>()});
            }
        }
        if (auto denied = j.find("declinedProtocolHandlers"); denied != j.end() && denied->is_object() && denied->size() <= 1000) {
            for (const auto& [scheme, targets] : denied->items()) {
                if (!IsWebHandlerScheme(scheme) || !targets.is_array() || targets.size() > 100) continue;
                for (const auto& target : targets) {
                    if (!target.is_string()) continue;
                    const auto url = target.get<std::string>();
                    if (ValidProtocolHandler(scheme, {"web", url, ProtocolOrigin(url)}))
                        profile.declinedProtocolHandlers[scheme].insert(url);
                }
            }
        }
        if (auto trusted = j.find("trustedCertificates"); trusted != j.end()) {
            if (!trusted->is_array() || trusted->size() > 1000) throw std::runtime_error("Invalid trusted certificate list");
            for (const auto& item : *trusted) {
                TrustedCertificate certificate;
                certificate.host = item.at("host").get<std::string>();
                certificate.sha256 = item.at("sha256").get<std::string>();
                if (auto subject = item.find("subject"); subject != item.end()) certificate.subject = subject->get<std::string>();
                if (auto added = item.find("added"); added != item.end()) certificate.added = added->get<int64_t>();
                // Unusable entries are dropped rather than failing the profile.
                const bool hex = certificate.sha256.size() == 64
                    && certificate.sha256.find_first_not_of("0123456789abcdef") == std::string::npos;
                if (certificate.host.empty() || certificate.host.size() > 255 || !hex || certificate.subject.size() > 1024)
                    continue;
                profile.TrustCertificate(certificate);
            }
        }
    } catch (const std::exception& e) { error = e.what(); return {}; }
    return profile;
}

const WebApp* Profile::FindWebApp(const std::string& id) const
{
    for (const auto& app : webApps) if (app.id == id) return &app;
    return nullptr;
}

const std::vector<std::string>& SitePermissionNames()
{
    static const std::vector<std::string> names = {"notifications", "geolocation", "camera", "microphone", "popups", "autoplay"};
    return names;
}

bool Profile::SetSitePermission(const std::string& permission, const std::string& origin, std::optional<bool> allowed)
{
    const auto& names = SitePermissionNames();
    if (std::find(names.begin(), names.end(), permission) == names.end()) return false;
    // Only web origins: scheme://host[:port], nothing after.
    const bool web = origin.rfind("https://", 0) == 0 || origin.rfind("http://", 0) == 0;
    const size_t host = origin.find("://") + 3;
    if (!web || origin.size() <= host || origin.size() > 300 || origin.find_first_of(std::string_view("/?#\\ \0", 6), host) != std::string::npos)
        return false;
    auto& origins = sitePermissions[permission];
    if (!allowed) {
        const bool erased = origins.erase(origin) > 0;
        if (origins.empty()) sitePermissions.erase(permission);
        return erased;
    }
    auto [entry, inserted] = origins.try_emplace(origin, *allowed);
    if (!inserted && entry->second == *allowed) return false;
    entry->second = *allowed;
    return true;
}

bool Profile::SetProtocolHandler(const std::string& scheme, const ProtocolHandler& handler)
{
    if (!ValidProtocolHandler(scheme, handler) || (protocolHandlers.find(scheme) == protocolHandlers.end() && protocolHandlers.size() >= 1000)) return false;
    if (auto found = protocolHandlers.find(scheme); found != protocolHandlers.end() && found->second == handler) return false;
    protocolHandlers[scheme] = handler;
    return true;
}
bool Profile::RemoveProtocolHandler(const std::string& scheme)
{
    const bool removed = protocolHandlers.erase(scheme) != 0;
    return declinedProtocolHandlers.erase(scheme) != 0 || removed;
}

bool Profile::TrustCertificate(const TrustedCertificate& certificate)
{
    auto lower = [](std::string text) {
        for (auto& character : text) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        return text;
    };
    TrustedCertificate stored = certificate;
    stored.host = lower(stored.host);
    stored.sha256 = lower(stored.sha256);
    for (const auto& existing : trustedCertificates)
        if (existing.host == stored.host && existing.sha256 == stored.sha256) return false;
    trustedCertificates.push_back(std::move(stored));
    return true;
}
// Local date (and time) for backup names: 2026-10-20 (-131502).
static std::string Stamp(int64_t seconds, bool withTime)
{
    const std::time_t time = static_cast<std::time_t>(seconds);
    std::tm local { };
    char text[32] = "";
    if (localtime_r(&time, &local)) std::strftime(text, sizeof text, withTime ? "%Y-%m-%d-%H%M%S" : "%Y-%m-%d", &local);
    return text;
}
static std::filesystem::path PreviousCopy(const std::filesystem::path& path) { return path.string() + ".1"; }
static std::string DailyPrefix(const std::filesystem::path& path) { return path.stem().string() + "-backup-"; }
// Writes data to a new file beside path, on the disk before it returns, and
// renames it to path.
static void WriteFileAtomically(const std::filesystem::path& path, const std::string& data,
    const std::function<void()>& beforeRename = { })
{
    std::string temporary = path.string() + ".XXXXXX";
    int fd = mkstemp(temporary.data());
    if (fd < 0) throw std::runtime_error(std::strerror(errno));
    try {
        size_t written = 0;
        while (written < data.size()) {
            ssize_t count = write(fd, data.data() + written, data.size() - written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error(std::strerror(errno));
            written += count;
        }
        if (fsync(fd) != 0) throw std::runtime_error(std::strerror(errno));
        const int closed = close(fd);
        fd = -1;
        if (closed != 0) throw std::runtime_error(std::strerror(errno));
        if (beforeRename) beforeRename();
        if (rename(temporary.c_str(), path.c_str()) != 0) throw std::runtime_error(std::strerror(errno));
    } catch (...) {
        if (fd >= 0) close(fd);
        unlink(temporary.c_str());
        throw;
    }
}
std::vector<std::filesystem::path> Profile::Backups(const std::filesystem::path& path)
{
    std::vector<std::filesystem::path> result;
    std::error_code error;
    if (std::filesystem::is_regular_file(PreviousCopy(path), error)) result.push_back(PreviousCopy(path));
    const std::string prefix = DailyPrefix(path), extension = path.extension().string();
    std::vector<std::string> daily;
    for (auto entry = std::filesystem::directory_iterator(path.parent_path(), error);
        !error && entry != std::filesystem::directory_iterator(); entry.increment(error)) {
        const auto name = entry->path().filename().string();
        if (name.size() > prefix.size() + extension.size() && name.compare(0, prefix.size(), prefix) == 0
            && name.compare(name.size() - extension.size(), extension.size(), extension) == 0)
            daily.push_back(name);
    }
    // The dates sort as text.
    std::sort(daily.rbegin(), daily.rend());
    for (const auto& name : daily) result.push_back(path.parent_path() / name);
    return result;
}
ProfileLoad Profile::LoadOrRestore(const std::filesystem::path& path, int64_t now)
{
    ProfileLoad result;
    std::error_code ignored;
    const bool present = std::filesystem::exists(path, ignored);
    result.profile = Load(path, result.error);
    if (present && result.error.empty()) return result;
    if (result.error == kUnknownVersion) {
        result.writable = false;
        return result;
    }
    const auto backups = Backups(path);
    // A first start.
    if (!present && backups.empty()) return result;
    // Gone, with copies left: a save cut short between its two renames.
    if (!present) result.error = "The profile file is missing";
    for (const auto& backup : backups) {
        std::string failure;
        auto restored = Load(backup, failure);
        if (!failure.empty()) continue;
        result.profile = std::move(restored);
        result.restoredFrom = backup;
        struct stat status { };
        if (stat(backup.c_str(), &status) == 0) result.restoredTime = status.st_mtime;
        break;
    }
    if (present) {
        // Kept for whoever wants to look, out of the way of the next save.
        const auto damaged = path.parent_path() / (path.stem().string() + ".damaged-" + Stamp(now, true) + path.extension().string());
        if (rename(path.c_str(), damaged.c_str()) == 0) result.damagedCopy = damaged;
        else result.writable = false;
    }
    return result;
}
bool Profile::Save(const std::filesystem::path& path, std::string& error, int64_t now) const
{
    error.clear();
    std::string data;
    try {
        std::filesystem::create_directories(path.parent_path());
        json sessions = json::array();
        for (const auto& window : windows) {
            json item = {{"tabs", Encode(window.tabs)}, {"selected", window.selected}};
            if (window.pinned) item["pinned"] = window.pinned;
            if (window.HasFrame()) item["frame"] = {window.frame[0], window.frame[1], window.frame[2], window.frame[3]};
            sessions.push_back(std::move(item));
        }
        json trusted = json::array();
        for (const auto& certificate : trustedCertificates)
            trusted.push_back({{"host", certificate.host}, {"sha256", certificate.sha256},
                {"subject", certificate.subject}, {"added", certificate.added}});
        json handlers = json::object();
        for (const auto& [scheme, handler] : protocolHandlers)
            handlers[scheme] = {{"kind", handler.kind}, {"target", handler.target}, {"origin", handler.origin}};
        const WindowSession first = windows.empty() ? WindowSession() : windows.front();
        data = json{{"version", 1}, {"tabs", Encode(first.tabs)}, {"selected", first.selected},
            {"windows", std::move(sessions)},
            {"bookmarks", Encode(bookmarks.Pages())},
            {"bookmarkTree", {{"bar", EncodeBookmarks(bookmarks.Bar().children)}, {"other", EncodeBookmarks(bookmarks.Other().children)},
                {"barModified", bookmarks.Bar().modified}, {"otherModified", bookmarks.Other().modified}}},
            {"history", Encode(history)},
            {"homeURL", homeURL}, {"showBookmarksBar", showBookmarksBar}, {"zoomTextOnly", zoomTextOnly}, {"spellingLanguage", spellingLanguage}, {"restoreSession", restoreSession},
            {"savePasswords", savePasswords}, {"passwordNever", passwordNever}, {"spellChecking", spellChecking},
            {"autoplay", autoplay}, {"userAgent", FindUserAgentPreset(userAgent).id}, {"webApps", [&] {
                json apps = json::array();
                for (const auto& app : webApps)
                    apps.push_back({{"id", app.id}, {"name", app.name}, {"startURL", app.startURL},
                        {"scope", app.scope}, {"launcher", app.launcher}});
                return apps;
            }()},
            {"interfaceStyle", interfaceStyle}, {"searchEngine", searchEngine},
            {"siteZoom", siteZoom}, {"unpinnedExtensions", unpinnedExtensions},
            {"trustedCertificates", trusted}, {"sitePermissions", sitePermissions},
            {"protocolHandlers", handlers}, {"declinedProtocolHandlers", declinedProtocolHandlers},
            {"mcp", {{"enabled", mcpEnabled}, {"port", mcpPort}, {"localOnly", mcpLocalOnly}, {"token", mcpToken}}},
            {"updates", {{"check", checkForUpdates}, {"lastCheck", lastUpdateCheck}}}}.dump(2);
        if (data.size() > 16 * 1024 * 1024) throw std::runtime_error("Profile is too large");
        // The file this one replaces stays as the newest copy (#93).
        WriteFileAtomically(path, data, [&] { rename(path.c_str(), PreviousCopy(path).c_str()); });
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    // The day's first save is kept for a few days too: a file written
    // seconds before a power cut can be lost with the newest one. A copy
    // that cannot be written does not fail the save.
    try {
        const auto daily = path.parent_path() / (DailyPrefix(path) + Stamp(now, false) + path.extension().string());
        std::error_code ignored;
        if (!Stamp(now, false).empty() && !std::filesystem::exists(daily, ignored)) {
            WriteFileAtomically(daily, data);
            auto copies = Backups(path);
            size_t kept = 0;
            for (const auto& copy : copies) {
                if (copy == PreviousCopy(path)) continue;
                if (++kept > kDailyBackups) std::filesystem::remove(copy, ignored);
            }
        }
    } catch (const std::exception&) { }
    return true;
}
static bool RecordVisit(Profile& profile, const PageRecord& page, int64_t micros, bool download = false)
{
    if (page.url.rfind("https://", 0) != 0 && page.url.rfind("http://", 0) != 0) return false;
    if (page.url.size() > 65536 || page.title.size() > 65536 || page.url.find('\0') != std::string::npos) return false;
    int visits = 0;
    bool countKnown = false;
    std::string oldTitle;
    std::vector<int64_t> times;
    std::vector<std::pair<int64_t, uint32_t>> transitions;
    auto& history = profile.history;
    history.erase(std::remove_if(history.begin(), history.end(), [&](const auto& old) {
        if (old.url != page.url) return false;
        visits = std::max(visits, HistoryVisitCount(old));
        countKnown |= old.visitCountKnown;
        if (oldTitle.empty()) oldTitle = old.title;
        auto oldTimes = HistoryVisitTimes(old);
        times.insert(times.end(), oldTimes.begin(), oldTimes.end());
        transitions.insert(transitions.end(), old.visitTransitions.begin(), old.visitTransitions.end());
        return true;
    }), history.end());
    times.push_back(micros);
    history.insert(history.begin(), page);
    auto& record = history.front();
    if (download && !oldTitle.empty()) record.title = std::move(oldTitle);
    if (download) transitions.emplace_back(micros, 7);
    record.visitTransitions = std::move(transitions);
    SetHistoryVisitTimes(record, std::move(times));
    record.visits = std::min(visits + (download ? 0 : 1), 1000000);
    record.visitCountKnown = countKnown || download;
    record.bar = false;
    std::stable_sort(history.begin(), history.end(), [](const auto& a, const auto& b) { return a.visited > b.visited; });
    if (history.size() > 2000) history.resize(2000);
    return true;
}
bool Profile::Visit(const PageRecord& page)
{
    return RecordVisit(*this, page, HistoryNowMicroseconds());
}
bool Profile::Visit(const PageRecord& page, int64_t now)
{
    return RecordVisit(*this, page, HistoryMicroseconds(now));
}
bool Profile::VisitDownload(const PageRecord& page, int64_t startedMicros)
{
    return startedMicros > 0 && RecordVisit(*this, page, startedMicros, true);
}
bool Profile::SetDownloadTitle(const std::string& url, int64_t startedMicros, const std::string& filename)
{
    if (filename.empty() || filename.size() > 65536) return false;
    for (auto& page : history) {
        if (page.url != url || !page.title.empty()) continue;
        if (std::find(page.visitTransitions.begin(), page.visitTransitions.end(), std::pair<int64_t, uint32_t>{startedMicros, 7})
            == page.visitTransitions.end()) continue;
        page.title = filename;
        return true;
    }
    return false;
}
size_t Profile::ForgetVisitsSince(int64_t since)
{
    size_t forgotten = 0;
    history.erase(std::remove_if(history.begin(), history.end(), [&](PageRecord& page) {
        auto times = HistoryVisitTimes(page);
        const size_t before = times.size();
        int counted = 0;
        for (auto time : times) if (time / 1000000 >= since) {
            bool known = false, counts = false;
            for (const auto& visit : page.visitTransitions) if (visit.first == time) {
                known = true;
                counts |= visit.second != 4 && visit.second != 7 && visit.second != 8 && visit.second != 9;
            }
            if (!known || counts) ++counted;
        }
        times.erase(std::remove_if(times.begin(), times.end(), [&](int64_t time) { return time / 1000000 >= since; }), times.end());
        const int removed = static_cast<int>(before - times.size());
        if (!removed) return false; // Unknown legacy dates remain unknown.
        forgotten += removed;
        if (times.empty()) return true;
        SetHistoryVisitTimes(page, std::move(times));
        page.visits = std::max(page.visitCountKnown ? 0 : 1, page.visits - counted);
        return false;
    }), history.end());
    std::stable_sort(history.begin(), history.end(), [](const PageRecord& a, const PageRecord& b) { return a.visited > b.visited; });
    return forgotten;
}
void Profile::AddBookmark(const PageRecord& page, bool bar)
{
    if (const auto* existing = bookmarks.FindURL(page.url)) {
        if (bar && !bookmarks.OnBar(page.url)) bookmarks.Move(existing->id, kBookmarksBarFolder, kBookmarkEnd);
        return;
    }
    bookmarks.Add(page, bar ? kBookmarksBarFolder : kOtherBookmarksFolder, kBookmarkEnd, std::time(nullptr));
}
size_t Profile::ImportHistory(const std::vector<PageRecord>& pages, bool* changed)
{
    if (changed) *changed = false;
    std::map<std::string, size_t> known;
    for (size_t i = 0; i < history.size(); ++i) known.emplace(history[i].url, i);
    std::set<std::string> addedURLs;
    bool updated = false;
    for (const auto& page : pages) {
        if (page.url.rfind("https://", 0) != 0 && page.url.rfind("http://", 0) != 0) continue;
        if (page.url.size() > 65536 || page.title.size() > 65536 || page.url.find('\0') != std::string::npos) continue;
        if (auto found = known.find(page.url); found != known.end()) {
            // A page visited in both keeps the later visit and the larger count.
            auto& existing = history[found->second];
            const auto before = existing;
            auto times = HistoryVisitTimes(existing);
            auto importedTimes = HistoryVisitTimes(page);
            times.insert(times.end(), importedTimes.begin(), importedTimes.end());
            existing.visitTransitions.insert(existing.visitTransitions.end(), page.visitTransitions.begin(), page.visitTransitions.end());
            SetHistoryVisitTimes(existing, std::move(times));
            if (existing.visitCountKnown || page.visitCountKnown) {
                existing.visits = std::max(HistoryVisitCount(existing), HistoryVisitCount(page));
                existing.visitCountKnown = true;
            } else existing.visits = std::clamp(std::max(existing.visits, page.visits), 0, 1000000);
            if (existing.title.empty()) existing.title = page.title;
            updated |= before.visited != existing.visited || before.visits != existing.visits
                || before.title != existing.title || before.earlier != existing.earlier || before.visitTimes != existing.visitTimes
                || before.visitTransitions != existing.visitTransitions || before.visitCountKnown != existing.visitCountKnown;
            continue;
        }
        PageRecord record{page.url, page.title, page.visited, false, std::clamp(page.visits, 0, 1000000)};
        record.visitTransitions = page.visitTransitions;
        record.visitCountKnown = page.visitCountKnown;
        SetHistoryVisitTimes(record, HistoryVisitTimes(page));
        known.emplace(record.url, history.size());
        addedURLs.insert(record.url);
        history.push_back(std::move(record));
    }
    if (!updated && addedURLs.empty()) return 0;
    std::stable_sort(history.begin(), history.end(), [](const PageRecord& a, const PageRecord& b) { return a.visited > b.visited; });
    if (history.size() > 2000) history.resize(2000);
    // Count only the imported pages that were kept.
    size_t kept = 0;
    for (const auto& page : history) kept += addedURLs.count(page.url);
    if (changed) *changed = updated || kept;
    return kept;
}
}
