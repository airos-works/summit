#include "BookmarkIO.h"
#include "HistoryVisits.h"
#include "Address.h"
#include "BookmarkQueries.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <fstream>
#include <map>
#include <random>
#include <openssl/sha.h>
#include <sys/stat.h>
#include <type_traits>

namespace summit {
namespace {
// Imports of more than this are cut short; the profile keeps at most
// kMaxBookmarks anyway.
constexpr size_t kMaxImportNodes = 100000;
constexpr uintmax_t kMaxImportFile = 256 * 1024 * 1024;
// Chrome counts microseconds from 1601.
constexpr int64_t kWindowsEpochSeconds = 11644473600;

std::string Digest(const std::string& source)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(source.data()), source.size(), digest);
    std::string value;
    for (auto byte : digest) { value += "0123456789abcdef"[byte >> 4]; value += "0123456789abcdef"[byte & 15]; }
    return value;
}

std::string Lower(std::string_view text)
{
    std::string result(text);
    for (auto& character : result) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return result;
}

bool ReadFile(const std::filesystem::path& path, std::string& data, std::string& error)
{
    std::error_code code;
    const auto size = std::filesystem::file_size(path, code);
    if (code) { error = "Could not read " + path.filename().string() + ": " + code.message(); return false; }
    if (size > kMaxImportFile) { error = path.filename().string() + " is too large to import."; return false; }
    std::ifstream stream(path, std::ios::binary);
    data.assign(static_cast<size_t>(size), '\0');
    if (!stream.read(data.data(), static_cast<std::streamsize>(size))) {
        error = "Could not read " + path.filename().string() + ".";
        return false;
    }
    return true;
}

void AppendUTF8(std::string& text, uint32_t code)
{
    if (code == 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) code = 0xFFFD;
    if (code < 0x80) text += char(code);
    else if (code < 0x800) { text += char(0xC0 | (code >> 6)); text += char(0x80 | (code & 0x3F)); }
    else if (code < 0x10000) {
        text += char(0xE0 | (code >> 12)); text += char(0x80 | ((code >> 6) & 0x3F)); text += char(0x80 | (code & 0x3F));
    } else {
        text += char(0xF0 | (code >> 18)); text += char(0x80 | ((code >> 12) & 0x3F));
        text += char(0x80 | ((code >> 6) & 0x3F)); text += char(0x80 | (code & 0x3F));
    }
}

std::string DecodeEntities(std::string_view text)
{
    static const std::map<std::string, uint32_t> named = {{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'},
        {"apos", '\''}, {"nbsp", 0xA0}, {"ndash", 0x2013}, {"mdash", 0x2014}, {"hellip", 0x2026}, {"laquo", 0xAB},
        {"raquo", 0xBB}, {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122}};
    std::string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') { result += text[i]; continue; }
        const size_t end = text.find(';', i + 1);
        if (end == std::string_view::npos || end - i > 12) { result += '&'; continue; }
        const std::string name(text.substr(i + 1, end - i - 1));
        uint32_t code = 0;
        if (name.size() > 1 && name[0] == '#') {
            const bool hex = name[1] == 'x' || name[1] == 'X';
            const std::string digits = name.substr(hex ? 2 : 1);
            if (digits.empty() || digits.find_first_not_of(hex ? "0123456789abcdefABCDEF" : "0123456789") != std::string::npos) {
                result += '&'; continue;
            }
            code = static_cast<uint32_t>(std::min<unsigned long>(std::strtoul(digits.c_str(), nullptr, hex ? 16 : 10), 0x110000));
        } else if (auto found = named.find(Lower(name)); found != named.end()) code = found->second;
        else { result += '&'; continue; }
        AppendUTF8(result, code);
        i = end;
    }
    return result;
}

// Text between tags: entities decoded, inner tags left out, white space collapsed.
std::string ElementText(std::string_view html)
{
    std::string text;
    bool inTag = false, space = false;
    for (char c : html) {
        if (inTag) { inTag = c != '>'; continue; }
        if (c == '<') { inTag = true; continue; }
        if (std::isspace(static_cast<unsigned char>(c))) { space = !text.empty(); continue; }
        if (space) text += ' ';
        space = false;
        text += c;
    }
    return DecodeEntities(text);
}

struct Tag {
    std::string name; // lower case
    bool closing = false;
    std::map<std::string, std::string> attributes; // names lower case
};

// Reads the tag at html[position] ('<') and moves past it. Comments and
// declarations are skipped (name empty).
bool ReadTag(std::string_view html, size_t& position, Tag& tag)
{
    tag = Tag();
    size_t i = position + 1;
    if (i < html.size() && html[i] == '!') {
        const bool comment = html.substr(i, 3) == "!--";
        const size_t end = html.find(comment ? "-->" : ">", i);
        position = end == std::string_view::npos ? html.size() : end + (comment ? 3 : 1);
        return true;
    }
    if (i < html.size() && html[i] == '/') { tag.closing = true; ++i; }
    const size_t nameStart = i;
    while (i < html.size() && std::isalnum(static_cast<unsigned char>(html[i]))) ++i;
    if (i == nameStart) { position += 1; return false; }
    tag.name = Lower(html.substr(nameStart, i - nameStart));
    while (i < html.size() && html[i] != '>') {
        if (std::isspace(static_cast<unsigned char>(html[i])) || html[i] == '/') { ++i; continue; }
        const size_t attributeStart = i;
        while (i < html.size() && html[i] != '=' && html[i] != '>' && !std::isspace(static_cast<unsigned char>(html[i]))) ++i;
        std::string name = Lower(html.substr(attributeStart, i - attributeStart));
        while (i < html.size() && std::isspace(static_cast<unsigned char>(html[i]))) ++i;
        std::string value;
        if (i < html.size() && html[i] == '=') {
            ++i;
            while (i < html.size() && std::isspace(static_cast<unsigned char>(html[i]))) ++i;
            if (i < html.size() && (html[i] == '"' || html[i] == '\'')) {
                const char quote = html[i++];
                const size_t end = html.find(quote, i);
                const size_t stop = end == std::string_view::npos ? html.size() : end;
                value = DecodeEntities(html.substr(i, stop - i));
                i = stop + 1;
            } else {
                const size_t valueStart = i;
                while (i < html.size() && html[i] != '>' && !std::isspace(static_cast<unsigned char>(html[i]))) ++i;
                value = DecodeEntities(html.substr(valueStart, i - valueStart));
            }
        }
        if (!name.empty() && !tag.attributes.count(name)) tag.attributes.emplace(std::move(name), std::move(value));
    }
    position = std::min(html.size(), i + 1);
    return true;
}

size_t FindClose(std::string_view html, size_t from, const char* closing)
{
    // Case-insensitive search for "</a" or "</h3".
    const size_t length = std::strlen(closing);
    for (size_t i = html.find('<', from); i != std::string_view::npos; i = html.find('<', i + 1))
        if (i + length <= html.size() && Lower(html.substr(i, length)) == closing) return i;
    return std::string_view::npos;
}

int64_t AttributeTime(const Tag& tag, const char* name)
{
    auto found = tag.attributes.find(name);
    if (found == tag.attributes.end() || found->second.empty()
        || found->second.find_first_not_of("0123456789") != std::string::npos || found->second.size() > 18) return 0;
    int64_t value = std::stoll(found->second);
    // Some exports count microseconds or milliseconds.
    while (value > 100000000000) value /= 1000;
    return value;
}

int64_t PositiveTime(std::string_view text)
{
    int64_t time = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), time);
    return result.ec == std::errc() && result.ptr == text.data() + text.size() && time > 0 ? time : 0;
}

int64_t AttributeModified(const Tag& tag)
{
    if (auto value = tag.attributes.find("summit_last_modified_us"); value != tag.attributes.end())
        if (auto time = PositiveTime(value->second)) return time;
    return HistoryMicroseconds(AttributeTime(tag, "last_modified"));
}

BookmarkMetadata AttributeMetadata(const Tag& tag)
{
    BookmarkMetadata metadata;
    if (tag.name == "a") {
        if (auto keyword = tag.attributes.find("shortcuturl"); keyword != tag.attributes.end()) {
            BookmarkKeyword item{keyword->second, {}, {}};
            if (auto post = tag.attributes.find("post_data"); post != tag.attributes.end()) item.postData = post->second;
            if (auto charset = tag.attributes.find("last_charset"); charset != tag.attributes.end()) item.charset = charset->second;
            metadata.keywords.push_back(std::move(item));
        }
        // Firefox's standard attributes hold one shortcut. Preserve all aliases
        // and their distinct request templates in a Summit export/reimport.
        if (auto keywords = tag.attributes.find("summit_keywords"); keywords != tag.attributes.end()) {
            auto values = nlohmann::json::parse(keywords->second, nullptr, false);
            if (values.is_array()) {
                metadata.keywords.clear();
                for (const auto& value : values) {
                    if (!value.is_object() || !value.contains("name") || !value["name"].is_string()) continue;
                    BookmarkKeyword item{value["name"].get<std::string>(), {}, {}};
                    if (value.contains("postData") && value["postData"].is_string()) item.postData = value["postData"].get<std::string>();
                    if (value.contains("charset") && value["charset"].is_string()) item.charset = value["charset"].get<std::string>();
                    metadata.keywords.push_back(std::move(item));
                    if (metadata.keywords.size() == kMaxBookmarkKeywords) break;
                }
            }
        }
    }
    // TAGS is a plain comma-separated list in Firefox's format. Our extra
    // attribute preserves tag names containing commas during a round trip.
    if (auto tags = tag.attributes.find("summit_tags"); tags != tag.attributes.end()) {
        auto values = nlohmann::json::parse(tags->second, nullptr, false);
        if (values.is_array()) {
            for (const auto& value : values)
                if (value.is_string() && metadata.tags.size() < kMaxBookmarkTags)
                    metadata.tags.push_back(value.get<std::string>());
            return NormalizeBookmarkMetadata(std::move(metadata));
        }
    }
    if (auto tags = tag.attributes.find("tags"); tags != tag.attributes.end()) {
        size_t start = 0;
        do {
            const size_t end = tags->second.find(',', start);
            metadata.tags.push_back(tags->second.substr(start, end == std::string::npos ? end : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        } while (metadata.tags.size() < kMaxBookmarkTags);
    }
    return NormalizeBookmarkMetadata(std::move(metadata));
}

BookmarkOrigin AttributeOrigin(const Tag& tag)
{
    auto found = tag.attributes.find("summit_origin");
    if (found == tag.attributes.end() || found->second.size() > 1024) return {};
    const auto value = nlohmann::json::parse(found->second, nullptr, false);
    if (!value.is_object() || !value.contains("scope") || !value["scope"].is_string()
        || !value.contains("guid") || !value["guid"].is_string()
        || !value.contains("id") || !value["id"].is_number_integer()) return {};
    return NormalizeBookmarkOrigin({value["scope"].get<std::string>(), value["guid"].get<std::string>(), value["id"].get<int64_t>()});
}

size_t DescriptionEnd(std::string_view html, size_t position)
{
    for (size_t next = html.find('<', position); next != std::string_view::npos; next = html.find('<', position)) {
        position = next;
        Tag tag;
        ReadTag(html, position, tag);
        if (tag.name == "dt" || tag.name == "dd" || tag.name == "dl" || tag.name == "h3" || tag.name == "hr") return next;
    }
    return html.size();
}
}

bool ParseNetscapeBookmarks(std::string_view html, BookmarkNode& folder, std::string& error)
{
    folder.folder = true;
    folder.children.clear();
    // The folders being filled; each <DL> opens the folder whose <H3> came
    // just before it. Pointers stay valid: a folder's own vector only grows
    // while it is on top, and the folders below are not changed meanwhile.
    std::vector<BookmarkNode*> open;
    BookmarkNode* pending = nullptr;
    BookmarkNode* previous = nullptr;
    // Where each open folder and the pending one are, as child indices from
    // the top, and the browsers' own roots (Chrome's bar, Firefox's toolbar
    // and Other Bookmarks): empty, they are no folder of the user's, and an
    // import would make an empty folder of them.
    std::vector<std::vector<size_t>> openPaths;
    std::vector<size_t> pendingPath;
    std::vector<std::vector<size_t>> roots;
    size_t nodes = 0;
    bool any = false;
    auto current = [&]() -> BookmarkNode& { return open.empty() ? folder : *open.back(); };
    auto currentPath = [&]() { return openPaths.empty() ? std::vector<size_t>{} : openPaths.back(); };
    for (size_t position = html.find('<'); position != std::string_view::npos && position < html.size();
        position = html.find('<', position)) {
        Tag tag;
        if (!ReadTag(html, position, tag) || tag.name.empty()) continue;
        if (tag.name == "dd" && !tag.closing) {
            const size_t end = DescriptionEnd(html, position);
            if (previous && !previous->separator) {
                previous->metadata.description = ElementText(html.substr(position, end - position));
                previous->metadata = NormalizeBookmarkMetadata(std::move(previous->metadata));
            }
            position = end;
            continue;
        }
        if (tag.name == "dl") {
            previous = nullptr;
            if (tag.closing) {
                if (!open.empty()) open.pop_back();
                if (!openPaths.empty()) openPaths.pop_back();
                pending = nullptr;
                continue;
            }
            // The first list is the file's top level; past the depth limit
            // folders' contents go into the folder above.
            BookmarkNode* opened = pending ? pending : open.empty() ? &folder : open.back();
            auto openedPath = pending ? pendingPath : currentPath();
            if (open.size() > kMaxBookmarkDepth) {
                opened = open.back();
                openedPath = openPaths.back();
            }
            open.push_back(opened);
            openPaths.push_back(std::move(openedPath));
            pending = nullptr;
            any = true;
            continue;
        }
        if (!tag.closing && tag.name == "hr") {
            if (++nodes > kMaxImportNodes) break;
            BookmarkNode node;
            node.separator = true;
            node.origin = AttributeOrigin(tag);
            node.added = AttributeTime(tag, "add_date");
            node.modified = AttributeModified(tag);
            if (auto name = tag.attributes.find("name"); name != tag.attributes.end()) node.title = name->second;
            current().children.push_back(std::move(node));
            previous = nullptr;
            pending = nullptr;
            any = true;
            continue;
        }
        if (tag.closing || (tag.name != "a" && tag.name != "h3")) continue;
        const bool link = tag.name == "a";
        const size_t end = FindClose(html, position, link ? "</a" : "</h3");
        const std::string text = ElementText(html.substr(position, (end == std::string_view::npos ? html.size() : end) - position));
        if (end != std::string_view::npos) position = end;
        if (++nodes > kMaxImportNodes) break;
        pending = nullptr;
        previous = nullptr;
        BookmarkNode node;
        node.title = text;
        node.added = AttributeTime(tag, "add_date");
        node.modified = AttributeModified(tag);
        node.metadata = AttributeMetadata(tag);
        node.origin = AttributeOrigin(tag);
        if (link) {
            auto href = tag.attributes.find("href");
            if (href == tag.attributes.end()) continue;
            node.url = Trim(href->second);
            // Standard HTML exports omit the database's folder identities.
            // Never reinterpret their references as Summit's native roots.
            if (IsBookmarkQueryURL(node.url) && node.origin.Empty()) node.origin = {"unresolved-html", Digest(node.url), 0};
            current().children.push_back(std::move(node));
        } else {
            node.folder = true;
            current().children.push_back(std::move(node));
            pending = &current().children.back();
            pendingPath = currentPath();
            pendingPath.push_back(current().children.size() - 1);
            if (tag.attributes.count("personal_toolbar_folder") || tag.attributes.count("unfiled_bookmarks_folder"))
                roots.push_back(pendingPath);
        }
        previous = &current().children.back();
        any = true;
    }
    // Later ones first, so that the indices of earlier ones still hold.
    std::sort(roots.rbegin(), roots.rend());
    for (const auto& path : roots) {
        BookmarkNode* parent = &folder;
        for (size_t i = 0; i + 1 < path.size() && parent; ++i)
            parent = path[i] < parent->children.size() ? &parent->children[path[i]] : nullptr;
        if (parent && path.back() < parent->children.size() && parent->children[path.back()].folder
            && parent->children[path.back()].children.empty() && parent->children[path.back()].metadata.Empty()
            && parent->children[path.back()].origin.Empty())
            parent->children.erase(parent->children.begin() + path.back());
    }
    if (!any) {
        error = "This file has no bookmarks in the Netscape bookmarks format.";
        return false;
    }
    return true;
}

std::string NetscapeBookmarksSource(std::string_view html)
{
    const auto lower = Lower(html.substr(0, std::min<size_t>(html.size(), 1 << 20)));
    if (lower.find("<!-- exported by summit") != std::string::npos) return "Summit";
    if (lower.find("<h1>bookmarks menu</h1>") != std::string::npos || lower.find("unfiled_bookmarks_folder") != std::string::npos)
        return "Firefox";
    if (lower.find("personal_toolbar_folder=\"true\">bookmarks bar</h3>") != std::string::npos) return "Chrome";
    return {};
}

namespace {
int64_t ChromeModified(const nlohmann::json& item)
{
    if (auto value = item.find("date_modified"); value != item.end() && value->is_string()) {
        const auto time = PositiveTime(value->get_ref<const std::string&>());
        if (time > kWindowsEpochSeconds * 1000000) return time - kWindowsEpochSeconds * 1000000;
    }
    return 0;
}
void ChromeNode(const nlohmann::json& item, BookmarkNode& into, size_t depth, size_t& nodes)
{
    if (!item.is_object() || ++nodes > kMaxImportNodes) return;
    const auto type = item.value("type", std::string());
    BookmarkNode node;
    node.modified = ChromeModified(item);
    if (auto name = item.find("name"); name != item.end() && name->is_string()) node.title = name->get<std::string>();
    if (auto added = item.find("date_added"); added != item.end() && added->is_string()) {
        const auto& text = added->get_ref<const std::string&>();
        if (!text.empty() && text.size() < 19 && text.find_first_not_of("0123456789") == std::string::npos) {
            const int64_t seconds = std::stoll(text) / 1000000 - kWindowsEpochSeconds;
            if (seconds > 0) node.added = seconds;
        }
    }
    if (type == "url") {
        auto url = item.find("url");
        if (url == item.end() || !url->is_string()) return;
        node.url = url->get<std::string>();
        into.children.push_back(std::move(node));
    } else if (type == "folder") {
        auto children = item.find("children");
        if (children == item.end() || !children->is_array()) return;
        // Past the depth limit a folder's contents go into the folder above.
        BookmarkNode* target = &into;
        if (depth + 1 <= kMaxBookmarkDepth) {
            node.folder = true;
            into.children.push_back(std::move(node));
            target = &into.children.back();
        }
        for (const auto& child : *children) ChromeNode(child, *target, depth + 1, nodes);
    }
}
}

bool ParseChromeBookmarks(std::string_view text, BookmarkNode& folder, std::string& error)
{
    folder.folder = true;
    folder.children.clear();
    const auto json = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    auto roots = json.is_object() ? json.find("roots") : json.end();
    if (!json.is_object() || roots == json.end() || !roots->is_object()) {
        error = "This is not a Chrome or Chromium Bookmarks file.";
        return false;
    }
    // The bar becomes a folder, Other Bookmarks' contents come first-level,
    // and any other root (mobile bookmarks, Falkon's menu) a folder.
    std::vector<std::string> keys = {"bookmark_bar", "other"};
    for (const auto& [key, value] : roots->items())
        if (key != "bookmark_bar" && key != "other" && value.is_object()) keys.push_back(key);
    static const std::map<std::string, std::string> names = {{"bookmark_bar", "Bookmarks Bar"},
        {"other", "Other Bookmarks"}, {"synced", "Mobile Bookmarks"}};
    size_t nodes = 0;
    for (const auto& key : keys) {
        auto root = roots->find(key);
        if (root == roots->end() || !root->is_object()) continue;
        auto children = root->find("children");
        if (children == root->end() || !children->is_array() || children->empty()) continue;
        BookmarkNode* target = &folder;
        if (key != "other") {
            BookmarkNode node;
            node.folder = true;
            node.modified = ChromeModified(*root);
            auto name = root->find("name");
            node.title = name != root->end() && name->is_string() && !name->get_ref<const std::string&>().empty()
                ? name->get<std::string>() : names.count(key) ? names.at(key) : key;
            folder.children.push_back(std::move(node));
            target = &folder.children.back();
        }
        for (const auto& child : *children) ChromeNode(child, *target, 1, nodes);
    }
    return true;
}

namespace {
void ReadDirectory(const std::filesystem::path& directory, const BookmarkFileReader& read, BookmarkNode& folder,
    size_t depth, size_t& nodes)
{
    std::vector<std::filesystem::directory_entry> entries;
    std::error_code code;
    for (auto entry = std::filesystem::directory_iterator(directory, code);
        !code && entry != std::filesystem::directory_iterator(); entry.increment(code)) {
        if (entries.size() >= 20000) break;
        entries.push_back(*entry);
    }
    // Sorted by name, as Tracker and WebPositive's menus show them.
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return Lower(a.path().filename().string()) < Lower(b.path().filename().string());
    });
    for (const auto& entry : entries) {
        if (++nodes > kMaxImportNodes) return;
        const auto name = entry.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        std::error_code ignored;
        // Linked folders are not followed: they can loop.
        if (entry.is_directory(ignored) && !entry.is_symlink(ignored)) {
            BookmarkNode* target = &folder;
            if (depth + 1 <= kMaxBookmarkDepth) {
                folder.children.push_back({0, name, "", true, 0, {}});
                target = &folder.children.back();
            }
            ReadDirectory(entry.path(), read, *target, depth + 1, nodes);
            continue;
        }
        if (!entry.is_regular_file(ignored)) continue;
        std::string url, title;
        if (!read || !read(entry.path(), url, title) || url.empty()) continue;
        int64_t added = 0;
        struct stat info;
        if (stat(entry.path().c_str(), &info) == 0) added = info.st_mtime;
        folder.children.push_back({0, title.empty() ? name : title, url, false, added, {}});
    }
}
}

bool ReadBookmarkDirectory(const std::filesystem::path& directory, const BookmarkFileReader& read, BookmarkNode& folder,
    std::string& error)
{
    folder.folder = true;
    folder.children.clear();
    std::error_code code;
    if (!std::filesystem::is_directory(directory, code)) {
        error = directory.string() + " is not a folder.";
        return false;
    }
    size_t nodes = 0;
    ReadDirectory(directory, read, folder, 0, nodes);
    return true;
}

namespace {
// The few SQLite functions the importers need, from the system's library.
class SQLite {
public:
    explicit SQLite(const std::string& library)
    {
        fLibrary = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!fLibrary) return;
        auto load = [this](auto& function, const char* name) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(dlsym(fLibrary, name));
            return function != nullptr;
        };
        fLoaded = load(fOpen, "sqlite3_open_v2") && load(fClose, "sqlite3_close") && load(fPrepare, "sqlite3_prepare_v2")
            && load(fStep, "sqlite3_step") && load(fFinalize, "sqlite3_finalize") && load(fInt64, "sqlite3_column_int64")
            && load(fText, "sqlite3_column_text") && load(fMessage, "sqlite3_errmsg");
    }
    ~SQLite()
    {
        if (fDatabase) fClose(fDatabase);
        if (fLibrary) dlclose(fLibrary);
    }
    SQLite(const SQLite&) = delete;
    SQLite& operator=(const SQLite&) = delete;

    bool Open(const std::filesystem::path& path, std::string& error)
    {
        if (!fLoaded) { error = "SQLite (libsqlite3) is not installed, so this database cannot be read."; return false; }
        constexpr int kReadWrite = 0x2;
        if (fOpen(path.c_str(), &fDatabase, kReadWrite, nullptr) != 0) {
            error = fDatabase ? fMessage(fDatabase) : "could not open the database";
            return false;
        }
        return true;
    }
    // Calls row(statement) for each row; columns are read with Int and Text.
    bool Query(const char* sql, const std::function<void(void*)>& row, std::string& error)
    {
        void* statement = nullptr;
        if (fPrepare(fDatabase, sql, -1, &statement, nullptr) != 0) { error = fMessage(fDatabase); return false; }
        constexpr int kRow = 100, kDone = 101;
        int status;
        while ((status = fStep(statement)) == kRow) row(statement);
        fFinalize(statement);
        if (status != kDone) { error = fMessage(fDatabase); return false; }
        return true;
    }
    int64_t Int(void* statement, int column) { return fInt64(statement, column); }
    std::string Text(void* statement, int column)
    {
        const auto* text = fText(statement, column);
        return text ? reinterpret_cast<const char*>(text) : "";
    }

private:
    void* fLibrary = nullptr;
    void* fDatabase = nullptr;
    bool fLoaded = false;
    int (*fOpen)(const char*, void**, int, const char*) = nullptr;
    int (*fClose)(void*) = nullptr;
    int (*fPrepare)(void*, const char*, int, void**, const char**) = nullptr;
    int (*fStep)(void*) = nullptr;
    int (*fFinalize)(void*) = nullptr;
    long long (*fInt64)(void*, int) = nullptr;
    const unsigned char* (*fText)(void*, int) = nullptr;
    const char* (*fMessage)(void*) = nullptr;
};

// The browser that owns a database may be running and holding it, with its
// latest changes in the write-ahead log beside it: a copy of both is read.
class DatabaseCopy {
public:
    bool Make(const std::filesystem::path& source, std::string& error)
    {
        std::random_device random;
        char name[40];
        std::snprintf(name, sizeof name, "summit-import-%08x%08x", random(), random());
        std::error_code code;
        fDirectory = std::filesystem::temp_directory_path(code) / name;
        if (!code) std::filesystem::create_directories(fDirectory, code);
        if (!code) std::filesystem::copy_file(source, Path(), code);
        if (code) { error = "Could not copy " + source.filename().string() + ": " + code.message(); return false; }
        for (const char* suffix : {"-wal", "-journal"}) {
            const auto side = source.string() + suffix;
            std::error_code ignored;
            if (std::filesystem::exists(side, ignored))
                std::filesystem::copy_file(side, Path().string() + suffix, ignored);
        }
        return true;
    }
    ~DatabaseCopy()
    {
        std::error_code ignored;
        if (!fDirectory.empty()) std::filesystem::remove_all(fDirectory, ignored);
    }
    std::filesystem::path Path() const { return fDirectory / "copy.sqlite"; }

private:
    std::filesystem::path fDirectory;
};

struct PlacesRow {
    int64_t id, parent, type, added;
    std::string title, url, guid;
    BookmarkMetadata metadata = {};
    int64_t modified = 0;
};

void PlacesFolder(const std::map<int64_t, std::vector<const PlacesRow*>>& children, int64_t id, BookmarkNode& into,
    size_t depth, const std::string& scope)
{
    auto found = children.find(id);
    if (found == children.end()) return;
    for (const auto* row : found->second) {
        if (row->type == 1 && !row->url.empty()) {
            into.children.push_back({0, row->title, row->url, false, row->added, {}});
            into.children.back().metadata = row->metadata;
            into.children.back().modified = row->modified;
            into.children.back().origin = NormalizeBookmarkOrigin({scope, row->guid, row->id});
        }
        else if (row->type == 3) {
            into.children.push_back({0, row->title, "", false, row->added, {}, true});
            into.children.back().modified = row->modified;
            into.children.back().origin = NormalizeBookmarkOrigin({scope, row->guid, row->id});
        }
        else if (row->type == 2) {
            BookmarkNode* target = &into;
            if (depth + 1 <= kMaxBookmarkDepth) {
                into.children.push_back({0, row->title, "", true, row->added, {}});
                target = &into.children.back();
                target->metadata = row->metadata;
                target->modified = row->modified;
                target->origin = NormalizeBookmarkOrigin({scope, row->guid, row->id});
            }
            if (depth < 64) PlacesFolder(children, row->id, *target, depth + 1, scope);
        }
    }
}

int64_t Seconds(int64_t microseconds) { return microseconds > 0 ? microseconds / 1000000 : 0; }

// Exported/older databases may omit visit rows. Their last-visit summary still
// survives; complete databases supply exact recent times and older daily samples.
// Chrome's page transition (ui/base/page_transition_types.h: a core type in
// the low byte, qualifiers above) as Firefox's visit type, for the same
// smart-folder transition filters and frecency: a redirect's target is a
// temporary redirect (Chrome does not say which kind); link, typed (also
// generated suggestions and keywords), bookmark, embedded frame, followed
// frame and reload map to theirs; an automatic top-level load and a form
// submission count as links. 0 for none.
uint32_t FirefoxTransitionForChrome(int64_t transition)
{
    constexpr int64_t clientRedirect = 0x40000000, serverRedirect = 0x80000000;
    if (transition & (clientRedirect | serverRedirect)) return 6;
    switch (transition & 0xff) {
        case 0: case 6: case 7: return 1;
        case 1: case 5: case 9: case 10: return 2;
        case 2: return 3;
        case 3: return 4;
        case 4: return 8;
        case 8: return 9;
        default: return 0;
    }
}

bool ReadVisitTimes(SQLite& database, bool firefox, const std::map<int64_t, size_t>& pages,
    std::vector<PageRecord>& history, std::string& error)
{
    const std::string table = firefox ? "moz_historyvisits" : "visits";
    bool available = false;
    if (!database.Query(("SELECT 1 FROM sqlite_master WHERE type='table' AND name='" + table + "'").c_str(),
            [&](void*) { available = true; }, error)) return false;
    bool transitionTypes = false;
    if (available && !database.Query(("PRAGMA table_info(" + table + ")").c_str(), [&](void* row) {
            if (database.Text(row, 1) == (firefox ? "visit_type" : "transition")) transitionTypes = true;
        }, error)) return false;
    for (auto [id, index] : pages) {
        auto times = HistoryVisitTimes(history[index]);
        if (available) {
            const std::string column = firefox ? "visit_date" : "visit_time";
            const std::string where = " FROM " + table + " WHERE " + (firefox ? "place_id" : "url") + "=" + std::to_string(id);
            auto read = [&](void* row) {
                auto time = database.Int(row, 0);
                if (!firefox) time = time > kWindowsEpochSeconds * 1000000 ? time - kWindowsEpochSeconds * 1000000 : 0;
                if (time > 0) times.push_back(time);
            };
            const auto recent = "SELECT " + column + where + " ORDER BY " + column + " DESC LIMIT " + std::to_string(kRecentHistoryVisits);
            const auto epochSeconds = column + "/1000000" + (firefox ? "" : "-" + std::to_string(kWindowsEpochSeconds));
            const auto daily = "SELECT MAX(" + column + ")" + where + " GROUP BY date(" + epochSeconds
                + ", 'unixepoch', 'localtime') ORDER BY 1 DESC LIMIT " + std::to_string(kMaxEarlierVisits + 1);
            if (!database.Query(recent.c_str(), read, error) || !database.Query(daily.c_str(), read, error)) return false;
        }
        SetHistoryVisitTimes(history[index], std::move(times));
        if (transitionTypes && !history[index].visitTimes.empty()) {
            std::string retained;
            for (auto time : history[index].visitTimes) {
                if (!retained.empty()) retained += ',';
                retained += std::to_string(time);
            }
            // Select types for the actual retained rows, including the daily
            // samples. Do not associate an arbitrary type with a MAX() group.
            if (firefox) {
                const auto query = "SELECT visit_date, visit_type FROM moz_historyvisits WHERE place_id=" + std::to_string(id)
                    + " AND visit_date IN (" + retained + ") AND visit_type BETWEEN 1 AND 9 GROUP BY visit_date, visit_type";
                if (!database.Query(query.c_str(), [&](void* row) {
                        history[index].visitTransitions.emplace_back(database.Int(row, 0), uint32_t(database.Int(row, 1)));
                    }, error)) return false;
            } else {
                std::string chromeTimes;
                for (auto time : history[index].visitTimes) {
                    if (!chromeTimes.empty()) chromeTimes += ',';
                    chromeTimes += std::to_string(time + kWindowsEpochSeconds * 1000000);
                }
                const auto query = "SELECT visit_time, transition FROM visits WHERE url=" + std::to_string(id)
                    + " AND visit_time IN (" + chromeTimes + ") GROUP BY visit_time, transition";
                if (!database.Query(query.c_str(), [&](void* row) {
                        if (auto type = FirefoxTransitionForChrome(database.Int(row, 1)))
                            history[index].visitTransitions.emplace_back(database.Int(row, 0) - kWindowsEpochSeconds * 1000000, type);
                    }, error)) return false;
            }
            SetHistoryVisitTimes(history[index], history[index].visitTimes);
        }
    }
    return true;
}
}

bool ReadFirefoxPlaces(const std::filesystem::path& places, const std::string& sqliteLibrary, BookmarkNode* folder,
    std::vector<PageRecord>* history, std::string& error)
{
    DatabaseCopy copy;
    SQLite database(sqliteLibrary);
    if (!copy.Make(places, error) || !database.Open(copy.Path(), error)) return false;
    if (folder) {
        folder->folder = true;
        folder->children.clear();
        // Stable across reimports of this database; exported metadata carries
        // the namespace so moving a Summit export does not change its links.
        const auto scope = "firefox-" + Digest(std::filesystem::weakly_canonical(places).string());
        std::vector<PlacesRow> rows;
        if (!database.Query("SELECT b.id, b.parent, b.type, IFNULL(b.title, ''), IFNULL(p.url, ''), IFNULL(b.dateAdded, 0), "
                "IFNULL(b.guid, '') FROM moz_bookmarks b LEFT JOIN moz_places p ON p.id = b.fk ORDER BY b.parent, b.position",
                [&](void* row) {
                    if (rows.size() < kMaxImportNodes)
                        rows.push_back({database.Int(row, 0), database.Int(row, 1), database.Int(row, 2),
                            Seconds(database.Int(row, 5)), database.Text(row, 3), database.Text(row, 4), database.Text(row, 6)});
                }, error)) {
            error = "This places.sqlite could not be read: " + error;
            return false;
        }
        std::map<int64_t, std::vector<const PlacesRow*>> children;
        std::map<std::string, int64_t> roots;
        for (const auto& row : rows) {
            children[row.parent].push_back(&row);
            roots[row.guid] = row.id;
        }
        std::map<std::string, std::vector<std::string>> tags;
        if (auto root = roots.find("tags________"); root != roots.end()) {
            for (const auto* tag : children[root->second]) {
                if (tag->type != 2 || tag->title.empty()) continue;
                for (const auto* item : children[tag->id]) {
                    if (item->type != 1 || item->url.empty()) continue;
                    auto& names = tags[item->url];
                    if (names.size() < kMaxBookmarkTags) names.push_back(tag->title);
                }
            }
        }
        std::map<int64_t, PlacesRow*> byID;
        std::map<std::string, std::vector<BookmarkKeyword>> keywords;
        size_t keywordTables = 0;
        if (!database.Query("SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'moz_keywords'",
                [&](void*) { ++keywordTables; }, error)) return false;
        if (keywordTables && !database.Query("SELECT k.keyword, p.url, IFNULL(k.post_data, '') FROM moz_keywords k "
                "JOIN moz_places p ON p.id = k.place_id ORDER BY k.id LIMIT 100000", [&](void* item) {
                    auto& aliases = keywords[database.Text(item, 1)];
                    if (aliases.size() < kMaxBookmarkKeywords)
                        aliases.push_back({database.Text(item, 0), database.Text(item, 2), {}});
                }, error)) return false;
        size_t charsetTables = 0;
        if (!database.Query("SELECT name FROM sqlite_master WHERE type = 'table' AND name IN ('moz_annos', 'moz_anno_attributes')",
                [&](void*) { ++charsetTables; }, error)) return false;
        if (charsetTables == 2 && !database.Query("SELECT p.url, IFNULL(a.content, '') FROM moz_annos a "
                "JOIN moz_anno_attributes n ON n.id = a.anno_attribute_id JOIN moz_places p ON p.id = a.place_id "
                "WHERE n.name = 'URIProperties/characterSet' ORDER BY a.id LIMIT 100000", [&](void* item) {
                    if (auto aliases = keywords.find(database.Text(item, 0)); aliases != keywords.end())
                        for (auto& keyword : aliases->second) keyword.charset = database.Text(item, 1);
                }, error)) return false;
        for (auto& row : rows) {
            byID[row.id] = &row;
            if (auto found = tags.find(row.url); found != tags.end()) row.metadata.tags = found->second;
            if (row.type == 1)
                if (auto found = keywords.find(row.url); found != keywords.end()) row.metadata.keywords = found->second;
        }
        // Descriptions were item annotations in older Firefox profiles. Newer
        // profiles may omit these tables altogether; neither is required.
        size_t annotationTables = 0;
        if (!database.Query("SELECT name FROM sqlite_master WHERE type = 'table' AND name IN "
                "('moz_items_annos', 'moz_anno_attributes')", [&](void*) { ++annotationTables; }, error)) return false;
        if (annotationTables == 2 && !database.Query("SELECT a.item_id, IFNULL(a.content, '') FROM moz_items_annos a "
                "JOIN moz_anno_attributes n ON n.id = a.anno_attribute_id WHERE n.name = 'bookmarkProperties/description' "
                "ORDER BY a.id LIMIT 100000", [&](void* item) {
                    if (auto row = byID.find(database.Int(item, 0)); row != byID.end())
                        row->second->metadata.description = database.Text(item, 1);
                }, error)) return false;
        bool hasModified = false;
        if (!database.Query("PRAGMA table_info(moz_bookmarks)", [&](void* row) {
                if (database.Text(row, 1) == "lastModified") hasModified = true;
            }, error)) return false;
        if (hasModified && !database.Query("SELECT id, lastModified FROM moz_bookmarks", [&](void* row) {
                if (auto found = byID.find(database.Int(row, 0)); found != byID.end())
                    found->second->modified = std::max(int64_t(0), database.Int(row, 1));
            }, error)) return false;
        for (auto& row : rows) row.metadata = NormalizeBookmarkMetadata(std::move(row.metadata));
        if (auto root = roots.find("root________"); root != roots.end()) folder->origin = {scope, root->first, root->second};
        // Keep every root as a real folder, including empty roots. Folder
        // shortcuts must not accidentally include siblings of a flattened root.
        for (auto [guid, title] : {std::pair{"toolbar_____", "Bookmarks Toolbar"}, {"menu________", "Bookmarks Menu"},
                {"unfiled_____", "Other Bookmarks"}, {"mobile______", "Mobile Bookmarks"}}) {
            auto root = roots.find(guid);
            if (root == roots.end()) continue;
            BookmarkNode node{0, title, "", true, 0, {}};
            node.origin = {scope, guid, root->second};
            if (auto row = byID.find(root->second); row != byID.end()) {
                node.metadata = row->second->metadata;
                node.added = row->second->added;
                node.modified = row->second->modified;
            }
            PlacesFolder(children, root->second, node, 1, scope);
            folder->children.push_back(std::move(node));
        }
        if (auto root = byID.find(folder->origin.id); root != byID.end()) folder->modified = root->second->modified;
    }
    if (history) {
        history->clear();
        std::map<int64_t, size_t> pages;
        if (!database.Query("SELECT url, IFNULL(title, ''), visit_count, IFNULL(last_visit_date, 0), id FROM moz_places "
                "WHERE (visit_count > 0 OR last_visit_date > 0) AND hidden = 0 AND last_visit_date IS NOT NULL ORDER BY last_visit_date DESC LIMIT 2000",
                [&](void* row) {
                    pages.emplace(database.Int(row, 4), history->size());
                    history->push_back({database.Text(row, 0), database.Text(row, 1), Seconds(database.Int(row, 3)), false,
                        static_cast<int>(std::clamp<int64_t>(database.Int(row, 2), 0, 1000000))});
                    history->back().visitTimes = {database.Int(row, 3)};
                    history->back().visitCountKnown = true;
                }, error)) {
            error = "The history in this places.sqlite could not be read: " + error;
            return false;
        }
        if (!ReadVisitTimes(database, true, pages, *history, error)) {
            error = "The visits in this places.sqlite could not be read: " + error;
            return false;
        }
    }
    return true;
}

bool ReadChromeHistory(const std::filesystem::path& historyFile, const std::string& sqliteLibrary,
    std::vector<PageRecord>& history, std::string& error)
{
    DatabaseCopy copy;
    SQLite database(sqliteLibrary);
    if (!copy.Make(historyFile, error) || !database.Open(copy.Path(), error)) return false;
    history.clear();
    std::map<int64_t, size_t> pages;
    if (!database.Query("SELECT url, IFNULL(title, ''), visit_count, last_visit_time, id FROM urls "
            "WHERE hidden = 0 AND visit_count > 0 ORDER BY last_visit_time DESC LIMIT 2000",
            [&](void* row) {
                const int64_t raw = database.Int(row, 3);
                const int64_t micros = raw > kWindowsEpochSeconds * 1000000 ? raw - kWindowsEpochSeconds * 1000000 : 0;
                const int64_t seconds = micros / 1000000;
                pages.emplace(database.Int(row, 4), history.size());
                history.push_back({database.Text(row, 0), database.Text(row, 1), seconds > 0 ? seconds : 0, false,
                    static_cast<int>(std::clamp<int64_t>(database.Int(row, 2), 0, 1000000))});
                history.back().visitTimes = {micros};
                history.back().visitCountKnown = true;
            }, error)) {
        error = "This History database could not be read: " + error;
        return false;
    }
    if (!ReadVisitTimes(database, false, pages, history, error)) {
        error = "The visits in this History database could not be read: " + error;
        return false;
    }
    return true;
}

namespace {
bool HasTable(const std::filesystem::path& path, const std::string& library, const char* table, std::string& error)
{
    DatabaseCopy copy;
    SQLite database(library);
    if (!copy.Make(path, error) || !database.Open(copy.Path(), error)) return false;
    bool found = false;
    const std::string sql = std::string("SELECT name FROM sqlite_master WHERE type = 'table' AND name = '") + table + "'";
    if (!database.Query(sql.c_str(), [&](void*) { found = true; }, error)) return false;
    return found;
}
}

bool ReadImport(const std::filesystem::path& path, const BookmarkImportOptions& options, ImportedBookmarks& imported,
    std::string& error)
{
    error.clear();
    imported = ImportedBookmarks();
    std::error_code code;
    if (std::filesystem::is_directory(path, code)) {
        // WebPositive keeps each bookmark as a file in
        // ~/config/settings/WebPositive/Bookmarks, in folders.
        const auto name = path.filename().string();
        const auto parent = path.parent_path().filename().string();
        imported.source = parent == "WebPositive" ? "WebPositive" : name.empty() ? "a folder" : name;
        return ReadBookmarkDirectory(path, options.readBookmarkFile, imported.bookmarks, error);
    }
    std::string data;
    if (!ReadFile(path, data, error)) return false;
    const auto name = path.filename().string();
    if (data.compare(0, 16, std::string("SQLite format 3\0", 16)) == 0) {
        data.clear();
        std::string tableError;
        if (HasTable(path, options.sqliteLibrary, "moz_bookmarks", tableError)) {
            imported.source = "Firefox";
            return ReadFirefoxPlaces(path, options.sqliteLibrary, &imported.bookmarks,
                options.history ? &imported.history : nullptr, error);
        }
        if (!tableError.empty()) { error = tableError; return false; }
        if (HasTable(path, options.sqliteLibrary, "urls", tableError)) {
            imported.source = "Chrome";
            return ReadChromeHistory(path, options.sqliteLibrary, imported.history, error);
        }
        error = tableError.empty() ? name + " is a database Summit cannot import from (not places.sqlite or History)." : tableError;
        return false;
    }
    const size_t start = data.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    if (start != std::string::npos && data[start] == '{') {
        imported.source = "Chrome";
        if (!ParseChromeBookmarks(data, imported.bookmarks, error)) return false;
        // The History database sits beside the Bookmarks file in a profile folder.
        const auto historyFile = path.parent_path() / "History";
        std::error_code ignored;
        if (options.history && std::filesystem::is_regular_file(historyFile, ignored)) {
            std::string historyError;
            if (!ReadChromeHistory(historyFile, options.sqliteLibrary, imported.history, historyError))
                std::fprintf(stderr, "Summit: could not import Chrome history: %s\n", historyError.c_str());
        }
        return true;
    }
    const auto source = NetscapeBookmarksSource(data);
    imported.source = source.empty() ? name : source;
    return ParseNetscapeBookmarks(data, imported.bookmarks, error);
}

namespace {
std::string WriteMetadata(const BookmarkNode& node)
{
    std::string attributes;
    if (!node.origin.Empty()) attributes = " SUMMIT_ORIGIN=\"" + EscapeHTML(nlohmann::json{
        {"scope", node.origin.scope}, {"guid", node.origin.guid}, {"id", node.origin.id}}.dump()) + "\"";
    if (!node.folder && !node.metadata.keywords.empty()) {
        const auto& first = node.metadata.keywords.front();
        attributes += " SHORTCUTURL=\"" + EscapeHTML(first.name) + "\"";
        if (!first.postData.empty()) attributes += " POST_DATA=\"" + EscapeHTML(first.postData) + "\"";
        if (!first.charset.empty()) attributes += " LAST_CHARSET=\"" + EscapeHTML(first.charset) + "\"";
        auto aliases = nlohmann::json::array();
        for (const auto& keyword : node.metadata.keywords)
            aliases.push_back({{"name", keyword.name}, {"postData", keyword.postData}, {"charset", keyword.charset}});
        attributes += " SUMMIT_KEYWORDS=\"" + EscapeHTML(aliases.dump()) + "\"";
    }
    if (node.metadata.tags.empty()) return attributes;
    std::string tags;
    for (const auto& tag : node.metadata.tags) tags += (tags.empty() ? "" : ",") + tag;
    return attributes + " TAGS=\"" + EscapeHTML(tags) + "\" SUMMIT_TAGS=\""
        + EscapeHTML(nlohmann::json(node.metadata.tags).dump()) + "\"";
}

std::string WriteDescription(const BookmarkNode& node, const std::string& indent)
{
    if (node.metadata.description.empty()) return {};
    std::string text;
    // Encode whitespace so the tolerant reader can distinguish the saved
    // description's spacing from indentation in a hand-formatted HTML file.
    for (char c : EscapeHTML(node.metadata.description)) {
        if (c == ' ') text += "&#32;";
        else if (c == '\n') text += "&#10;";
        else if (c == '\r') text += "&#13;";
        else if (c == '\t') text += "&#9;";
        else text += c;
    }
    return indent + "<DD>" + text + "\n";
}

std::string ModifiedAttributes(const BookmarkNode& node)
{
    return node.modified > 0 ? " LAST_MODIFIED=\"" + std::to_string(node.modified / 1000000)
        + "\" SUMMIT_LAST_MODIFIED_US=\"" + std::to_string(node.modified) + "\"" : "";
}

void WriteNodes(const std::vector<BookmarkNode>& nodes, int depth, std::string& html)
{
    const std::string indent(4 * depth, ' ');
    for (const auto& node : nodes) {
        const std::string added = (node.added ? " ADD_DATE=\"" + std::to_string(node.added) + "\"" : "") + ModifiedAttributes(node);
        if (node.folder) {
            html += indent + "<DT><H3" + added + WriteMetadata(node) + ">" + EscapeHTML(node.title) + "</H3>\n";
            html += WriteDescription(node, indent);
            html += indent + "<DL><p>\n";
            WriteNodes(node.children, depth + 1, html);
            html += indent + "</DL><p>\n";
        } else if (node.separator)
            html += indent + "<HR" + (node.title.empty() ? "" : " NAME=\"" + EscapeHTML(node.title) + "\"") + added + WriteMetadata(node) + ">\n";
        else {
            html += indent + "<DT><A HREF=\"" + EscapeHTML(node.url) + "\"" + added + WriteMetadata(node) + ">" + EscapeHTML(node.title) + "</A>\n";
            html += WriteDescription(node, indent);
        }
    }
}
}

std::string WriteNetscapeBookmarks(const Bookmarks& bookmarks)
{
    std::string html = "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
        "<!-- This is an automatically generated file.\n"
        "     It will be read and overwritten.\n"
        "     DO NOT EDIT! -->\n"
        "<!-- Exported by Summit -->\n"
        "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n"
        "<TITLE>Bookmarks</TITLE>\n"
        "<H1>Bookmarks</H1>\n"
        "<DL><p>\n";
    html += "    <DT><H3 PERSONAL_TOOLBAR_FOLDER=\"true\"" + ModifiedAttributes(bookmarks.Bar()) + ">" + EscapeHTML(bookmarks.Bar().title) + "</H3>\n    <DL><p>\n";
    WriteNodes(bookmarks.Bar().children, 2, html);
    html += "    </DL><p>\n";
    WriteNodes(bookmarks.Other().children, 1, html);
    html += "</DL><p>\n";
    return html;
}
}
