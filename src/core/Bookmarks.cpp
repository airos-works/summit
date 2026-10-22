#include "Bookmarks.h"
#include "BookmarkQueries.h"
#include "Address.h"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <functional>
#include <map>
#include <set>

namespace summit {
namespace {
std::string MetadataText(std::string text, size_t limit)
{
    text.erase(std::remove(text.begin(), text.end(), '\0'), text.end());
    if (text.size() > limit) {
        while (limit && (static_cast<unsigned char>(text[limit]) & 0xc0) == 0x80) --limit;
        text.resize(limit);
    }
    return text;
}
}

BookmarkOrigin NormalizeBookmarkOrigin(BookmarkOrigin origin)
{
    const auto valid = [](const std::string& text) {
        return text.size() <= 128 && text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string::npos;
    };
    if (origin.scope.empty() || !valid(origin.scope) || !valid(origin.guid) || origin.id < 0
        || (origin.guid.empty() && !origin.id)) return {};
    return origin;
}

std::string BookmarkOriginKey(const BookmarkOrigin& origin)
{
    const auto value = NormalizeBookmarkOrigin(origin);
    return value.Empty() ? std::string() : value.scope + ":" + (value.guid.empty() ? "#" + std::to_string(value.id) : value.guid);
}

BookmarkMetadata NormalizeBookmarkMetadata(BookmarkMetadata metadata)
{
    metadata.description = MetadataText(std::move(metadata.description), kMaxBookmarkDescription);
    std::vector<std::string> tags;
    std::set<std::string> seen;
    for (auto tag : metadata.tags) {
        for (char& c : tag) if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        const auto start = tag.find_first_not_of(" ");
        if (start == std::string::npos) continue;
        tag = MetadataText(tag.substr(start, tag.find_last_not_of(" ") - start + 1), kMaxBookmarkTagLength);
        if (!tag.empty() && seen.insert(tag).second) tags.push_back(std::move(tag));
        if (tags.size() == kMaxBookmarkTags) break;
    }
    metadata.tags = std::move(tags);
    std::vector<BookmarkKeyword> keywords;
    seen.clear();
    for (auto keyword : metadata.keywords) {
        while (!keyword.name.empty() && std::isspace(static_cast<unsigned char>(keyword.name.back()))) keyword.name.pop_back();
        const auto start = keyword.name.find_first_not_of(" \t\r\n\f\v");
        if (start == std::string::npos) continue;
        keyword.name.erase(0, start);
        // Do not truncate names or request templates into a different shortcut.
        if (keyword.name.size() > kMaxBookmarkKeywordLength || keyword.postData.size() > kMaxBookmarkPostData
            || keyword.charset.size() > 128 || keyword.postData.find('\0') != std::string::npos
            || std::any_of(keyword.name.begin(), keyword.name.end(), [](unsigned char c) { return c <= 32 || c == 127; })
            || std::any_of(keyword.charset.begin(), keyword.charset.end(), [](unsigned char c) { return c <= 32 || c == 127; }))
            continue;
        for (char& c : keyword.name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (seen.insert(keyword.name).second) keywords.push_back(std::move(keyword));
        if (keywords.size() == kMaxBookmarkKeywords) break;
    }
    metadata.keywords = std::move(keywords);
    return metadata;
}

static std::vector<std::string> CommaList(std::string_view text)
{
    BookmarkMetadata metadata;
    std::string tag;
    bool quoted = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '"' && (quoted || tag.find_first_not_of(" \t") == std::string::npos)) {
            if (quoted && i + 1 < text.size() && text[i + 1] == '"') { tag += '"'; ++i; }
            else quoted = !quoted;
        } else if (c == ',' && !quoted) {
            metadata.tags.push_back(std::move(tag));
            tag.clear();
        } else tag += c;
    }
    metadata.tags.push_back(std::move(tag));
    return metadata.tags;
}

std::vector<std::string> ParseBookmarkTags(std::string_view text)
{
    return NormalizeBookmarkMetadata({{}, CommaList(text)}).tags;
}

std::string BookmarkTagsText(const std::vector<std::string>& tags)
{
    std::string text;
    for (const auto& tag : tags) {
        if (!text.empty()) text += ", ";
        if (tag.find_first_of(",\"") == std::string::npos) { text += tag; continue; }
        text += '"';
        for (char c : tag) { text += c; if (c == '"') text += '"'; }
        text += '"';
    }
    return text;
}

std::string BookmarkKeywordsText(const std::vector<BookmarkKeyword>& keywords)
{
    std::vector<std::string> names;
    for (const auto& keyword : keywords) names.push_back(keyword.name);
    return BookmarkTagsText(names);
}

std::vector<BookmarkKeyword> EditBookmarkKeywords(const std::vector<BookmarkKeyword>& existing, std::string_view text)
{
    BookmarkMetadata metadata;
    for (const auto& name : CommaList(text)) metadata.keywords.push_back({name, {}, {}});
    metadata = NormalizeBookmarkMetadata(std::move(metadata));
    for (auto& keyword : metadata.keywords) {
        const auto found = std::find_if(existing.begin(), existing.end(), [&](const auto& saved) { return saved.name == keyword.name; });
        if (found != existing.end()) keyword = *found;
        else if (!existing.empty()) {
            keyword.postData = existing.front().postData;
            keyword.charset = existing.front().charset;
        }
    }
    return metadata.keywords;
}

static bool StartsWithLower(const std::string& text, const char* prefix)
{
    for (size_t i = 0; prefix[i]; ++i)
        if (i >= text.size() || std::tolower(static_cast<unsigned char>(text[i])) != prefix[i]) return false;
    return true;
}

bool IsBookmarkableURL(const std::string& url)
{
    if (url.empty() || url.size() > 65536 || url.find('\0') != std::string::npos) return false;
    return StartsWithLower(url, "https://") || StartsWithLower(url, "http://") || StartsWithLower(url, "file://")
        || IsBookmarkletURL(url) || IsBookmarkQueryURL(url);
}

bool IsBookmarkletURL(const std::string& url)
{
    return url.size() <= 65536 && url.find('\0') == std::string::npos && StartsWithLower(url, "javascript:");
}

std::string NormalizeBookmarkAddress(std::string_view text)
{
    auto url = Trim(text);
    if (!url.empty() && url.find("://") == std::string::npos && !StartsWithLower(url, "file:") && !StartsWithLower(url, "javascript:") && !IsBookmarkQueryURL(url))
        url = "https://" + url;
    return url;
}

size_t CountBookmarks(const BookmarkNode& folder)
{
    size_t count = 0;
    for (const auto& child : folder.children) count += child.folder ? CountBookmarks(child) : !child.separator;
    return count;
}

namespace {
const BookmarkNode* FindIn(const BookmarkNode& node, int64_t id)
{
    if (node.id == id) return &node;
    for (const auto& child : node.children)
        if (child.folder || child.id == id)
            if (auto* found = FindIn(child, id)) return found;
    return nullptr;
}

const BookmarkNode* ParentIn(const BookmarkNode& node, int64_t id, size_t* index)
{
    for (size_t i = 0; i < node.children.size(); ++i) {
        if (node.children[i].id == id) {
            if (index) *index = i;
            return &node;
        }
        if (node.children[i].folder)
            if (auto* found = ParentIn(node.children[i], id, index)) return found;
    }
    return nullptr;
}

const BookmarkNode* FindURLIn(const BookmarkNode& node, const std::string& url)
{
    for (const auto& child : node.children) {
        if (!child.folder && !child.separator && child.url == url) return &child;
        if (child.folder)
            if (auto* found = FindURLIn(child, url)) return found;
    }
    return nullptr;
}

// How many levels of folders a node holds below itself.
size_t Height(const BookmarkNode& node)
{
    size_t height = 0;
    for (const auto& child : node.children)
        if (child.folder) height = std::max(height, 1 + Height(child));
    return height;
}

bool PathTo(const BookmarkNode& node, int64_t id, std::vector<const BookmarkNode*>& path)
{
    path.push_back(&node);
    if (node.id == id) return true;
    for (const auto& child : node.children)
        if (child.folder && PathTo(child, id, path)) return true;
    path.pop_back();
    return false;
}

void Flatten(const BookmarkNode& folder, bool bar, std::vector<PageRecord>& pages, size_t& folders, size_t& separators)
{
    for (const auto& child : folder.children) {
        if (child.folder) {
            ++folders;
            Flatten(child, false, pages, folders, separators);
            continue;
        }
        if (child.separator) { ++separators; continue; }
        PageRecord page{child.url, child.title};
        page.bar = bar;
        pages.push_back(std::move(page));
    }
}

void ListFolders(const BookmarkNode& folder, size_t depth, int64_t except, std::vector<BookmarkFolderChoice>& list)
{
    if (folder.id == except) return;
    list.push_back({folder.id, depth, folder.title});
    for (const auto& child : folder.children)
        if (child.folder) ListFolders(child, depth + 1, except, list);
}

}

Bookmarks::Bookmarks()
{
    fBar = {kBookmarksBarFolder, "Bookmarks Bar", "", true, 0, {}};
    fOther = {kOtherBookmarksFolder, "Other Bookmarks", "", true, 0, {}};
}

size_t Bookmarks::Count() const { return fPages.size(); }
bool Bookmarks::Empty() const { return fBar.children.empty() && fOther.children.empty(); }

void Bookmarks::Number(BookmarkNode& node)
{
    node.id = fNextID++;
    node.origin = NormalizeBookmarkOrigin(std::move(node.origin));
    for (auto& child : node.children) Number(child);
}

void Bookmarks::Assign(BookmarkNode bar, BookmarkNode other)
{
    fNextID = kOtherBookmarksFolder + 1;
    fBar.children = std::move(bar.children);
    fOther.children = std::move(other.children);
    for (auto* root : {&fBar, &fOther})
        for (auto& child : root->children) Number(child);
    Reindex();
}

void Bookmarks::AssignPages(const std::vector<PageRecord>& pages)
{
    BookmarkNode bar, other;
    for (const auto& page : pages) {
        if (CountBookmarks(bar) + CountBookmarks(other) >= kMaxBookmarks) break;
        (page.bar ? bar : other).children.push_back({0, page.title, page.url, false, 0, {}});
    }
    Assign(std::move(bar), std::move(other));
}

void Bookmarks::Reindex()
{
    fPages.clear();
    fFolders = 0;
    fSeparators = 0;
    Flatten(fBar, true, fPages, fFolders, fSeparators);
    Flatten(fOther, false, fPages, fFolders, fSeparators);
}

const BookmarkNode* Bookmarks::Find(int64_t id) const
{
    if (id <= 0) return nullptr;
    if (auto* found = FindIn(fBar, id)) return found;
    return FindIn(fOther, id);
}

BookmarkNode* Bookmarks::FindNode(int64_t id) { return const_cast<BookmarkNode*>(Find(id)); }

const BookmarkNode* Bookmarks::Parent(int64_t id, size_t* index) const
{
    if (auto* found = ParentIn(fBar, id, index)) return found;
    return ParentIn(fOther, id, index);
}

BookmarkNode* Bookmarks::ParentNode(int64_t id, size_t* index) { return const_cast<BookmarkNode*>(Parent(id, index)); }

const BookmarkNode* Bookmarks::FindURL(const std::string& url) const
{
    if (url.empty()) return nullptr;
    if (auto* found = FindURLIn(fBar, url)) return found;
    return FindURLIn(fOther, url);
}

bool Bookmarks::OnBar(const std::string& url) const
{
    return std::any_of(fBar.children.begin(), fBar.children.end(),
        [&](const BookmarkNode& child) { return !child.folder && !child.separator && child.url == url; });
}

std::string Bookmarks::FolderPath(int64_t id) const
{
    const auto* parent = Parent(id);
    if (!parent) return {};
    std::vector<const BookmarkNode*> path;
    if (!PathTo(fBar, parent->id, path) && !PathTo(fOther, parent->id, path)) return {};
    std::string text;
    for (const auto* folder : path) text += (text.empty() ? "" : " › ") + folder->title;
    return text;
}

std::vector<BookmarkFolderChoice> Bookmarks::Folders(int64_t except) const
{
    std::vector<BookmarkFolderChoice> list;
    ListFolders(fBar, 0, except, list);
    ListFolders(fOther, 0, except, list);
    return list;
}

static size_t Depth(const BookmarkNode& bar, const BookmarkNode& other, int64_t id)
{
    std::vector<const BookmarkNode*> path;
    if (PathTo(bar, id, path) || PathTo(other, id, path)) return path.size() - 1;
    return 0;
}

int64_t Bookmarks::Add(const PageRecord& page, int64_t folder, size_t index, int64_t added)
{
    auto* parent = FindNode(folder);
    if (!parent || !parent->folder || !IsBookmarkableURL(page.url) || fPages.size() >= kMaxBookmarks) return 0;
    BookmarkNode node{fNextID++, page.title.substr(0, 4096), page.url, false, added, {}};
    const int64_t id = node.id;
    parent->children.insert(parent->children.begin() + std::min(index, parent->children.size()), std::move(node));
    Reindex();
    return id;
}

int64_t Bookmarks::AddFolder(int64_t parent, const std::string& title, size_t index, int64_t added)
{
    auto* folder = FindNode(parent);
    if (!folder || !folder->folder || fFolders >= kMaxBookmarkFolders
        || Depth(fBar, fOther, parent) + 1 > kMaxBookmarkDepth) return 0;
    BookmarkNode node{fNextID++, title.substr(0, 4096), "", true, added, {}};
    const int64_t id = node.id;
    folder->children.insert(folder->children.begin() + std::min(index, folder->children.size()), std::move(node));
    Reindex();
    return id;
}

int64_t Bookmarks::AddSeparator(int64_t parent, size_t index, int64_t added)
{
    auto* folder = FindNode(parent);
    if (!folder || !folder->folder || fSeparators >= kMaxBookmarkSeparators) return 0;
    BookmarkNode node{fNextID++, "", "", false, added, {}, true};
    const int64_t id = node.id;
    folder->children.insert(folder->children.begin() + std::min(index, folder->children.size()), std::move(node));
    Reindex();
    return id;
}

bool Bookmarks::Edit(int64_t id, const std::string& title, const std::string& url)
{
    auto* node = FindNode(id);
    if (!node || node->separator || id == kBookmarksBarFolder || id == kOtherBookmarksFolder) return false;
    const std::string newTitle = title.substr(0, 4096);
    if (node->folder) {
        if (node->title == newTitle) return false;
        node->title = newTitle;
    } else {
        if (!IsBookmarkableURL(url) || (node->title == newTitle && node->url == url)) return false;
        node->title = newTitle;
        node->url = url;
    }
    Reindex();
    return true;
}

bool Bookmarks::Move(int64_t id, int64_t folder, size_t index)
{
    if (id == kBookmarksBarFolder || id == kOtherBookmarksFolder) return false;
    auto* node = FindNode(id);
    auto* target = FindNode(folder);
    size_t from = 0;
    auto* parent = ParentNode(id, &from);
    if (!node || !target || !target->folder || !parent) return false;
    // A folder cannot go inside itself, nor deeper than folders may nest.
    if (node->folder && (FindIn(*node, folder) || Depth(fBar, fOther, folder) + 1 + Height(*node) > kMaxBookmarkDepth))
        return false;
    index = std::min(index, target->children.size());
    if (parent == target) {
        // The index counts the node in its old place.
        if (index == from || index == from + 1) return false;
        if (index > from) --index;
    }
    BookmarkNode moved = std::move(parent->children[from]);
    parent->children.erase(parent->children.begin() + from);
    // Erasing may have moved the target (a later sibling of the node).
    target = FindNode(folder);
    target->children.insert(target->children.begin() + std::min(index, target->children.size()), std::move(moved));
    Reindex();
    return true;
}

bool Bookmarks::SetMetadata(int64_t id, BookmarkMetadata metadata)
{
    auto* node = FindNode(id);
    if (!node || node->separator || id == kBookmarksBarFolder || id == kOtherBookmarksFolder) return false;
    metadata = NormalizeBookmarkMetadata(std::move(metadata));
    if (node->metadata == metadata) return false;
    node->metadata = std::move(metadata);
    return true;
}

bool Bookmarks::Remove(int64_t id)
{
    size_t index = 0;
    auto* parent = ParentNode(id, &index);
    if (!parent) return false;
    parent->children.erase(parent->children.begin() + index);
    Reindex();
    return true;
}

bool Bookmarks::RemoveURL(const std::string& url)
{
    bool removed = false;
    while (const auto* found = FindURL(url)) {
        size_t index = 0;
        auto* parent = ParentNode(found->id, &index);
        if (!parent) break;
        parent->children.erase(parent->children.begin() + index);
        removed = true;
    }
    if (removed) Reindex();
    return removed;
}

namespace {
struct ImportState {
    std::map<std::string, int64_t> known;
    std::map<std::string, int64_t> origins;
    std::function<BookmarkNode*(int64_t)> find;
    std::function<size_t(int64_t)> depth;
    std::set<int64_t> updated;
    // Folders this import made: those left empty are removed again.
    std::set<int64_t> created;
    size_t count;
    size_t folders;
    size_t separators;
    BookmarkImportResult result;
    int64_t& nextID;
    int64_t now;
};

std::string TrimmedURL(std::string url)
{
    while (!url.empty() && std::isspace(static_cast<unsigned char>(url.back()))) url.pop_back();
    size_t start = 0;
    while (start < url.size() && std::isspace(static_cast<unsigned char>(url[start]))) ++start;
    return url.substr(start);
}

void MergeMetadata(const BookmarkNode& from, BookmarkNode& into, ImportState& state)
{
    auto metadata = into.metadata;
    if (metadata.description.empty()) metadata.description = from.metadata.description;
    metadata.tags.insert(metadata.tags.end(), from.metadata.tags.begin(), from.metadata.tags.end());
    metadata.keywords.insert(metadata.keywords.end(), from.metadata.keywords.begin(), from.metadata.keywords.end());
    metadata = NormalizeBookmarkMetadata(std::move(metadata));
    if (!(into.metadata == metadata)) {
        into.metadata = std::move(metadata);
        state.updated.insert(into.id);
    }
}

// Even without room for an import folder, existing URLs can gain metadata.
// New items still require that folder and retain the normal limit reporting.
void MergeExisting(const BookmarkNode& from, ImportState& state)
{
    for (const auto& child : from.children) {
        if (child.folder) { MergeExisting(child, state); continue; }
        if (child.separator) { ++state.result.overLimit; continue; }
        const auto url = TrimmedURL(child.url);
        if (!IsBookmarkableURL(url)) { ++state.result.unsupported; continue; }
        const auto key = BookmarkOriginKey(child.origin);
        const auto& knownItems = key.empty() ? state.known : state.origins;
        if (auto known = knownItems.find(key.empty() ? url : key); known != knownItems.end()) {
            ++state.result.duplicates;
            if (auto* existing = state.find(known->second)) MergeMetadata(child, *existing, state);
        } else ++state.result.overLimit;
    }
}

void Merge(const BookmarkNode& from, BookmarkNode& destination, size_t depth, ImportState& state)
{
    const auto destinationID = destination.id;
    std::map<std::string, size_t> separators;
    for (const auto& child : destination.children)
        if (child.separator && child.origin.Empty()) ++separators[child.title];
    for (const auto& child : from.children) {
        // Existing imported nodes may have been moved elsewhere. Resolve IDs
        // afresh: adding a sibling of this destination can move its vector.
        auto* into = state.find(destinationID);
        if (!into) return;
        const auto origin = NormalizeBookmarkOrigin(child.origin);
        const auto key = BookmarkOriginKey(origin);
        BookmarkNode* identified = nullptr;
        if (!key.empty())
            if (auto found = state.origins.find(key); found != state.origins.end()) identified = state.find(found->second);
        if (identified && (identified->folder != child.folder || identified->separator != child.separator)) {
            ++state.result.unsupported;
            continue;
        }
        if (child.folder) {
            if (depth + 1 > kMaxBookmarkDepth && !identified) {
                Merge(child, *into, depth, state);
                continue;
            }
            const std::string title = child.title.substr(0, 4096);
            auto* target = identified;
            if (!target && origin.Empty()) {
                auto found = std::find_if(into->children.begin(), into->children.end(), [&](const BookmarkNode& node) {
                    return node.folder && node.origin.Empty() && node.title == title;
                });
                if (found != into->children.end()) target = &*found;
            }
            if (!target) {
                if (state.folders >= kMaxBookmarkFolders) {
                    ++state.result.overLimit;
                    Merge(child, *into, depth, state);
                    continue;
                }
                into->children.push_back({state.nextID++, title, "", true, child.added ? child.added : state.now, {}});
                target = &into->children.back();
                target->origin = origin;
                target->metadata = NormalizeBookmarkMetadata(child.metadata);
                if (!child.children.empty()) state.created.insert(target->id);
                if (!key.empty()) state.origins.emplace(key, target->id);
                ++state.folders;
            } else MergeMetadata(child, *target, state);
            const auto targetID = target->id;
            Merge(child, *target, state.depth(targetID), state);
            continue;
        }
        if (child.separator) {
            if (identified) continue;
            const auto title = child.title.substr(0, 4096);
            auto& matching = separators[title];
            if (origin.Empty() && matching) { --matching; continue; }
            if (state.separators >= kMaxBookmarkSeparators) { ++state.result.overLimit; continue; }
            into->children.push_back({state.nextID++, title, "", false, child.added ? child.added : state.now, {}, true});
            into->children.back().origin = origin;
            if (!key.empty()) state.origins.emplace(key, into->children.back().id);
            ++state.separators;
            ++state.result.separatorsAdded;
            continue;
        }
        const std::string url = TrimmedURL(child.url);
        if (!IsBookmarkableURL(url)) { ++state.result.unsupported; continue; }
        // Firefox can bookmark an address in several folders. Its item
        // identity, rather than the URL, preserves those live memberships.
        if (!identified && origin.Empty())
            if (auto found = state.known.find(url); found != state.known.end()) identified = state.find(found->second);
        if (identified) {
            ++state.result.duplicates;
            MergeMetadata(child, *identified, state);
            continue;
        }
        if (state.count >= kMaxBookmarks) { ++state.result.overLimit; continue; }
        into->children.push_back({state.nextID++, (child.title.empty() ? url : child.title).substr(0, 4096), url, false,
            child.added ? child.added : state.now, {}});
        auto& added = into->children.back();
        added.origin = origin;
        added.metadata = NormalizeBookmarkMetadata(child.metadata);
        state.known.emplace(url, added.id);
        if (!key.empty()) state.origins.emplace(key, added.id);
        ++state.count;
        ++state.result.added;
    }
}

void RemoveEmptyCreated(BookmarkNode& folder, const std::set<int64_t>& created)
{
    for (auto& child : folder.children)
        if (child.folder) RemoveEmptyCreated(child, created);
    folder.children.erase(std::remove_if(folder.children.begin(), folder.children.end(), [&](const BookmarkNode& child) {
        return child.folder && child.children.empty() && child.metadata.Empty() && child.origin.Empty() && created.count(child.id);
    }), folder.children.end());
}
}

BookmarkImportResult Bookmarks::Import(const BookmarkNode& imported, const std::string& folderTitle, int64_t now)
{
    const size_t oldFolders = fFolders;
    ImportState state{{}, {}, [this](int64_t id) { return FindNode(id); },
        [this](int64_t id) { return Depth(fBar, fOther, id); }, {}, {}, fPages.size(), fFolders, fSeparators, {}, fNextID, now};
    std::function<void(const BookmarkNode&)> collect = [&](const BookmarkNode& folder) {
        for (const auto& child : folder.children) {
            if (auto key = BookmarkOriginKey(child.origin); !key.empty()) state.origins.emplace(key, child.id);
            if (child.folder) collect(child);
            else if (!child.separator) state.known.emplace(child.url, child.id);
        }
    };
    collect(fBar);
    collect(fOther);
    const std::string title = folderTitle.substr(0, 4096);
    const auto origin = NormalizeBookmarkOrigin(imported.origin);
    auto existing = std::find_if(fOther.children.begin(), fOther.children.end(),
        [&](const BookmarkNode& node) { return node.folder && (origin.Empty()
            ? node.origin.Empty() && node.title == title : BookmarkOriginKey(node.origin) == BookmarkOriginKey(origin)); });
    BookmarkNode* target = existing == fOther.children.end() ? nullptr : &*existing;
    if (!origin.Empty())
        if (auto found = state.origins.find(BookmarkOriginKey(origin)); found != state.origins.end()) target = FindNode(found->second);
    if (target && !target->folder) { state.result.unsupported = CountBookmarks(imported); return state.result; }
    if (!target) {
        if (fFolders >= kMaxBookmarkFolders) {
            MergeExisting(imported, state);
            state.result.updated = state.updated.size();
            if (!state.result.duplicates && !state.result.unsupported && !state.result.overLimit)
                state.result.overLimit = 1;
            return state.result;
        }
        fOther.children.push_back({fNextID++, title, "", true, now, {}});
        fOther.children.back().origin = origin;
        state.created.insert(fOther.children.back().id);
        ++state.folders;
        target = &fOther.children.back();
    }
    const int64_t folder = target->id;
    if (auto key = BookmarkOriginKey(origin); !key.empty()) state.origins.emplace(key, folder);
    Merge(imported, *target, state.depth(folder), state);
    // Folders that only held duplicates are not kept; folders that were there
    // before keep whatever they had, and so do folders empty in the file.
    RemoveEmptyCreated(fBar, state.created);
    RemoveEmptyCreated(fOther, state.created);
    auto result = state.result;
    Reindex();
    result.foldersAdded = fFolders - oldFolders;
    result.updated = state.updated.size();
    if ((result.added || result.foldersAdded || result.separatorsAdded) && Find(folder)) result.folder = folder;
    return result;
}
}
