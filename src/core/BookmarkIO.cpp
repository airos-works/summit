#include "BookmarkIO.h"
#include "Address.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <fstream>
#include <map>
#include <random>
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
        if (tag.name == "dl") {
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
            node.added = AttributeTime(tag, "add_date");
            if (auto name = tag.attributes.find("name"); name != tag.attributes.end()) node.title = name->second;
            current().children.push_back(std::move(node));
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
        BookmarkNode node;
        node.title = text;
        node.added = AttributeTime(tag, "add_date");
        if (link) {
            auto href = tag.attributes.find("href");
            if (href == tag.attributes.end()) continue;
            node.url = Trim(href->second);
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
        any = true;
    }
    // Later ones first, so that the indices of earlier ones still hold.
    std::sort(roots.rbegin(), roots.rend());
    for (const auto& path : roots) {
        BookmarkNode* parent = &folder;
        for (size_t i = 0; i + 1 < path.size() && parent; ++i)
            parent = path[i] < parent->children.size() ? &parent->children[path[i]] : nullptr;
        if (parent && path.back() < parent->children.size() && parent->children[path.back()].folder
            && parent->children[path.back()].children.empty())
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
void ChromeNode(const nlohmann::json& item, BookmarkNode& into, size_t depth, size_t& nodes)
{
    if (!item.is_object() || ++nodes > kMaxImportNodes) return;
    const auto type = item.value("type", std::string());
    BookmarkNode node;
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
};

void PlacesFolder(const std::map<int64_t, std::vector<const PlacesRow*>>& children, int64_t id, BookmarkNode& into,
    size_t depth)
{
    auto found = children.find(id);
    if (found == children.end()) return;
    for (const auto* row : found->second) {
        if (row->type == 1 && !row->url.empty()) into.children.push_back({0, row->title, row->url, false, row->added, {}});
        else if (row->type == 3) into.children.push_back({0, row->title, "", false, row->added, {}, true});
        else if (row->type == 2) {
            BookmarkNode* target = &into;
            if (depth + 1 <= kMaxBookmarkDepth) {
                into.children.push_back({0, row->title, "", true, row->added, {}});
                target = &into.children.back();
            }
            if (depth < 64) PlacesFolder(children, row->id, *target, depth + 1);
        }
    }
}

int64_t Seconds(int64_t microseconds) { return microseconds > 0 ? microseconds / 1000000 : 0; }
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
        // The toolbar first, then the Bookmarks Menu's own, then the rest.
        for (auto [guid, title] : {std::pair{"toolbar_____", "Bookmarks Toolbar"}, {"menu________", ""},
                {"unfiled_____", "Other Bookmarks"}, {"mobile______", "Mobile Bookmarks"}}) {
            auto root = roots.find(guid);
            if (root == roots.end()) continue;
            if (!*title) { PlacesFolder(children, root->second, *folder, 0); continue; }
            BookmarkNode node{0, title, "", true, 0, {}};
            PlacesFolder(children, root->second, node, 1);
            if (!node.children.empty()) folder->children.push_back(std::move(node));
        }
    }
    if (history) {
        history->clear();
        if (!database.Query("SELECT url, IFNULL(title, ''), visit_count, IFNULL(last_visit_date, 0) FROM moz_places "
                "WHERE visit_count > 0 AND hidden = 0 AND last_visit_date IS NOT NULL ORDER BY last_visit_date DESC LIMIT 2000",
                [&](void* row) {
                    history->push_back({database.Text(row, 0), database.Text(row, 1), Seconds(database.Int(row, 3)), false,
                        static_cast<int>(std::clamp<int64_t>(database.Int(row, 2), 0, 1000000))});
                }, error)) {
            error = "The history in this places.sqlite could not be read: " + error;
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
    if (!database.Query("SELECT url, IFNULL(title, ''), visit_count, last_visit_time FROM urls "
            "WHERE hidden = 0 AND visit_count > 0 ORDER BY last_visit_time DESC LIMIT 2000",
            [&](void* row) {
                const int64_t seconds = database.Int(row, 3) / 1000000 - kWindowsEpochSeconds;
                history.push_back({database.Text(row, 0), database.Text(row, 1), seconds > 0 ? seconds : 0, false,
                    static_cast<int>(std::clamp<int64_t>(database.Int(row, 2), 0, 1000000))});
            }, error)) {
        error = "This History database could not be read: " + error;
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
void WriteNodes(const std::vector<BookmarkNode>& nodes, int depth, std::string& html)
{
    const std::string indent(4 * depth, ' ');
    for (const auto& node : nodes) {
        const std::string added = node.added ? " ADD_DATE=\"" + std::to_string(node.added) + "\"" : "";
        if (node.folder) {
            html += indent + "<DT><H3" + added + ">" + EscapeHTML(node.title) + "</H3>\n";
            html += indent + "<DL><p>\n";
            WriteNodes(node.children, depth + 1, html);
            html += indent + "</DL><p>\n";
        } else if (node.separator)
            html += indent + "<HR" + (node.title.empty() ? "" : " NAME=\"" + EscapeHTML(node.title) + "\"") + added + ">\n";
        else
            html += indent + "<DT><A HREF=\"" + EscapeHTML(node.url) + "\"" + added + ">" + EscapeHTML(node.title) + "</A>\n";
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
    html += "    <DT><H3 PERSONAL_TOOLBAR_FOLDER=\"true\">" + EscapeHTML(bookmarks.Bar().title) + "</H3>\n    <DL><p>\n";
    WriteNodes(bookmarks.Bar().children, 2, html);
    html += "    </DL><p>\n";
    WriteNodes(bookmarks.Other().children, 1, html);
    html += "</DL><p>\n";
    return html;
}
}
