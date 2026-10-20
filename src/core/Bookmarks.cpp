#include "Bookmarks.h"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <set>

namespace summit {
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
    for (const auto& child : folder.children) count += child.folder ? CountBookmarks(child) : 1;
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
        if (!child.folder && child.url == url) return &child;
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

void Flatten(const BookmarkNode& folder, bool bar, std::vector<PageRecord>& pages, size_t& folders)
{
    for (const auto& child : folder.children) {
        if (child.folder) {
            ++folders;
            Flatten(child, false, pages, folders);
            continue;
        }
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
    Flatten(fBar, true, fPages, fFolders);
    Flatten(fOther, false, fPages, fFolders);
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
        [&](const BookmarkNode& child) { return !child.folder && child.url == url; });
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

bool Bookmarks::Edit(int64_t id, const std::string& title, const std::string& url)
{
    auto* node = FindNode(id);
    if (!node || id == kBookmarksBarFolder || id == kOtherBookmarksFolder) return false;
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
    std::set<std::string> known;
    // Folders this import made: those left empty are removed again.
    std::set<int64_t> created;
    size_t count;
    size_t folders;
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

void Merge(const BookmarkNode& from, BookmarkNode& into, size_t depth, ImportState& state)
{
    for (const auto& child : from.children) {
        if (child.folder) {
            // Folders past the limits give their bookmarks to the folder above.
            if (depth + 1 > kMaxBookmarkDepth || state.folders >= kMaxBookmarkFolders) {
                Merge(child, into, depth, state);
                continue;
            }
            const std::string title = child.title.substr(0, 4096);
            auto existing = std::find_if(into.children.begin(), into.children.end(),
                [&](const BookmarkNode& node) { return node.folder && node.title == title; });
            if (existing == into.children.end()) {
                into.children.push_back({state.nextID++, title, "", true, child.added ? child.added : state.now, {}});
                // A folder empty in the file stays (one exported empty comes
                // back); one left empty by duplicates goes (RemoveEmptyCreated).
                if (!child.children.empty()) state.created.insert(into.children.back().id);
                ++state.folders;
                existing = into.children.end() - 1;
            }
            // Only the folder's own children change below, so the reference stays valid.
            Merge(child, *existing, depth + 1, state);
            continue;
        }
        const std::string url = TrimmedURL(child.url);
        if (!IsBookmarkableURL(url)) { ++state.result.unsupported; continue; }
        if (!state.known.insert(url).second) { ++state.result.duplicates; continue; }
        if (state.count >= kMaxBookmarks) { ++state.result.overLimit; continue; }
        into.children.push_back({state.nextID++, (child.title.empty() ? url : child.title).substr(0, 4096), url, false,
            child.added ? child.added : state.now, {}});
        ++state.count;
        ++state.result.added;
    }
}

void RemoveEmptyCreated(BookmarkNode& folder, const std::set<int64_t>& created)
{
    for (auto& child : folder.children)
        if (child.folder) RemoveEmptyCreated(child, created);
    folder.children.erase(std::remove_if(folder.children.begin(), folder.children.end(), [&](const BookmarkNode& child) {
        return child.folder && child.children.empty() && created.count(child.id);
    }), folder.children.end());
}
}

BookmarkImportResult Bookmarks::Import(const BookmarkNode& imported, const std::string& folderTitle, int64_t now)
{
    ImportState state{{}, {}, fPages.size(), fFolders, {}, fNextID, now};
    for (const auto& page : fPages) state.known.insert(page.url);
    const std::string title = folderTitle.substr(0, 4096);
    auto existing = std::find_if(fOther.children.begin(), fOther.children.end(),
        [&](const BookmarkNode& node) { return node.folder && node.title == title; });
    if (existing == fOther.children.end()) {
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
    if (result.added) result.folder = folder;
    Reindex();
    return result;
}
}
