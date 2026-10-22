#include "Bookmarks.h"
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
    return metadata;
}

std::vector<std::string> ParseBookmarkTags(std::string_view text)
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
    return NormalizeBookmarkMetadata(std::move(metadata)).tags;
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

static bool StartsWithLower(const std::string& text, const char* prefix)
{
    for (size_t i = 0; prefix[i]; ++i)
        if (i >= text.size() || std::tolower(static_cast<unsigned char>(text[i])) != prefix[i]) return false;
    return true;
}

bool IsBookmarkableURL(const std::string& url)
{
    if (url.empty() || url.size() > 65536 || url.find('\0') != std::string::npos) return false;
    return StartsWithLower(url, "https://") || StartsWithLower(url, "http://") || StartsWithLower(url, "file://");
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
    std::function<BookmarkNode*(int64_t)> find;
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
        if (auto known = state.known.find(url); known != state.known.end()) {
            ++state.result.duplicates;
            if (auto* existing = state.find(known->second)) MergeMetadata(child, *existing, state);
        } else ++state.result.overLimit;
    }
}

void Merge(const BookmarkNode& from, BookmarkNode& into, size_t depth, ImportState& state)
{
    // Reimporting the same folder must not multiply its separators. Match
    // their occurrences within this folder; repeated separators in the file
    // are distinct. Existing nodes keep their order, as with bookmarks.
    std::map<std::string, size_t> separators;
    for (const auto& child : into.children)
        if (child.separator) ++separators[child.title];
    for (const auto& child : from.children) {
        if (child.folder) {
            // Folders past the limits give their bookmarks to the folder above.
            if (depth + 1 > kMaxBookmarkDepth) {
                Merge(child, into, depth, state);
                continue;
            }
            const std::string title = child.title.substr(0, 4096);
            auto existing = std::find_if(into.children.begin(), into.children.end(),
                [&](const BookmarkNode& node) { return node.folder && node.title == title; });
            const bool created = existing == into.children.end();
            if (created) {
                if (state.folders >= kMaxBookmarkFolders) {
                    ++state.result.overLimit;
                    Merge(child, into, depth, state);
                    continue;
                }
                into.children.push_back({state.nextID++, title, "", true, child.added ? child.added : state.now, {}});
                // A folder empty in the file stays (one exported empty comes
                // back); one left empty by duplicates goes (RemoveEmptyCreated).
                if (!child.children.empty()) state.created.insert(into.children.back().id);
                ++state.folders;
                existing = into.children.end() - 1;
            }
            if (created) existing->metadata = NormalizeBookmarkMetadata(child.metadata);
            else MergeMetadata(child, *existing, state);
            // Only the folder's own children change below, so the reference stays valid.
            Merge(child, *existing, depth + 1, state);
            continue;
        }
        if (child.separator) {
            const auto title = child.title.substr(0, 4096);
            auto& matching = separators[title];
            if (matching) { --matching; continue; }
            if (state.separators >= kMaxBookmarkSeparators) { ++state.result.overLimit; continue; }
            into.children.push_back({state.nextID++, title, "", false, child.added ? child.added : state.now, {}, true});
            ++state.separators;
            ++state.result.separatorsAdded;
            continue;
        }
        const std::string url = TrimmedURL(child.url);
        if (!IsBookmarkableURL(url)) { ++state.result.unsupported; continue; }
        if (auto found = state.known.find(url); found != state.known.end()) {
            ++state.result.duplicates;
            if (auto* existing = state.find(found->second)) MergeMetadata(child, *existing, state);
            continue;
        }
        if (state.count >= kMaxBookmarks) { ++state.result.overLimit; continue; }
        into.children.push_back({state.nextID++, (child.title.empty() ? url : child.title).substr(0, 4096), url, false,
            child.added ? child.added : state.now, {}});
        into.children.back().metadata = NormalizeBookmarkMetadata(child.metadata);
        state.known.emplace(url, into.children.back().id);
        ++state.count;
        ++state.result.added;
    }
}

void RemoveEmptyCreated(BookmarkNode& folder, const std::set<int64_t>& created)
{
    for (auto& child : folder.children)
        if (child.folder) RemoveEmptyCreated(child, created);
    folder.children.erase(std::remove_if(folder.children.begin(), folder.children.end(), [&](const BookmarkNode& child) {
        return child.folder && child.children.empty() && child.metadata.Empty() && created.count(child.id);
    }), folder.children.end());
}
}

BookmarkImportResult Bookmarks::Import(const BookmarkNode& imported, const std::string& folderTitle, int64_t now)
{
    const size_t oldFolders = fFolders;
    ImportState state{{}, [this](int64_t id) { return FindNode(id); }, {}, {}, fPages.size(), fFolders, fSeparators, {}, fNextID, now};
    std::function<void(const BookmarkNode&)> collect = [&](const BookmarkNode& folder) {
        for (const auto& child : folder.children) {
            if (child.folder) collect(child);
            else if (!child.separator) state.known.emplace(child.url, child.id);
        }
    };
    collect(fBar);
    collect(fOther);
    const std::string title = folderTitle.substr(0, 4096);
    auto existing = std::find_if(fOther.children.begin(), fOther.children.end(),
        [&](const BookmarkNode& node) { return node.folder && node.title == title; });
    if (existing == fOther.children.end()) {
        if (fFolders >= kMaxBookmarkFolders) {
            MergeExisting(imported, state);
            state.result.updated = state.updated.size();
            if (!state.result.duplicates && !state.result.unsupported && !state.result.overLimit)
                state.result.overLimit = 1;
            return state.result;
        }
        fOther.children.push_back({fNextID++, title, "", true, now, {}});
        state.created.insert(fOther.children.back().id);
        ++state.folders;
        existing = fOther.children.end() - 1;
    }
    const int64_t folder = existing->id;
    Merge(imported, *existing, 1, state);
    // Folders that only held duplicates are not kept; folders that were there
    // before keep whatever they had, and so do folders empty in the file.
    BookmarkNode root;
    root.children.push_back(std::move(*existing));
    RemoveEmptyCreated(root, state.created);
    if (root.children.empty()) fOther.children.erase(existing);
    else *existing = std::move(root.children.front());
    auto result = state.result;
    Reindex();
    result.foldersAdded = fFolders - oldFolders;
    result.updated = state.updated.size();
    if ((result.added || result.foldersAdded || result.separatorsAdded) && Find(folder)) result.folder = folder;
    return result;
}
}
