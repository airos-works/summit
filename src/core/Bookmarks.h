#pragma once
#include "PageRecord.h"
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace summit {
struct BookmarkKeyword {
    std::string name;
    // Firefox's stored POST_DATA template (possibly escaped), empty for GET.
    std::string postData;
    std::string charset;
    bool operator==(const BookmarkKeyword& other) const
        { return name == other.name && postData == other.postData && charset == other.charset; }
};
struct BookmarkNavigation {
    std::string url, postData, title, error;
    bool post = false;
    int64_t bookmarkID = 0;
};
// Identity in an imported bookmark database. The scope distinguishes databases
// whose numeric IDs and built-in root GUIDs overlap. It contains no source path.
struct BookmarkOrigin {
    std::string scope, guid;
    int64_t id = 0;
    bool Empty() const { return scope.empty(); }
    bool operator==(const BookmarkOrigin& other) const
        { return scope == other.scope && guid == other.guid && id == other.id; }
};
BookmarkOrigin NormalizeBookmarkOrigin(BookmarkOrigin origin);
std::string BookmarkOriginKey(const BookmarkOrigin& origin);
struct BookmarkMetadata {
    std::string description;
    std::vector<std::string> tags;
    std::vector<BookmarkKeyword> keywords = {};
    bool Empty() const { return description.empty() && tags.empty() && keywords.empty(); }
    bool operator==(const BookmarkMetadata& other) const
        { return description == other.description && tags == other.tags && keywords == other.keywords; }
};
constexpr size_t kMaxBookmarkDescription = 16384;
constexpr size_t kMaxBookmarkTags = 100;
constexpr size_t kMaxBookmarkTagLength = 256;
constexpr size_t kMaxBookmarkKeywords = 16;
constexpr size_t kMaxBookmarkKeywordLength = 256;
constexpr size_t kMaxBookmarkPostData = 65536;
BookmarkMetadata NormalizeBookmarkMetadata(BookmarkMetadata metadata);
std::string BookmarkKeywordsText(const std::vector<BookmarkKeyword>& keywords);
// Existing names keep their request data; new/renamed aliases use the first
// shortcut's request, so renaming a POST shortcut does not turn it into GET.
std::vector<BookmarkKeyword> EditBookmarkKeywords(const std::vector<BookmarkKeyword>& existing, std::string_view text);
// Comma-separated tags for the editors. Quotes and doubled quotes preserve
// commas and quotes inside a tag; these are not the Netscape TAGS syntax.
std::vector<std::string> ParseBookmarkTags(std::string_view text);
std::string BookmarkTagsText(const std::vector<std::string>& tags);
// A bookmark, a separator, or a folder of nodes.
struct BookmarkNode {
    // Unique among the profile's bookmarks while Summit runs. Not saved: the
    // nodes are numbered again when the profile is read.
    int64_t id = 0;
    std::string title;
    // Empty for a folder.
    std::string url;
    bool folder = false;
    // When it was added, in seconds since the epoch (0 when not known).
    int64_t added = 0;
    std::vector<BookmarkNode> children;
    // Separators have no address or children; their position is significant.
    bool separator = false;
    BookmarkMetadata metadata = {};
    BookmarkOrigin origin = {};
};
// The two folders every profile has, which cannot be moved, renamed or deleted.
constexpr int64_t kBookmarksBarFolder = 1;
constexpr int64_t kOtherBookmarksFolder = 2;
constexpr size_t kMaxBookmarks = 10000;
constexpr size_t kMaxBookmarkFolders = 5000;
constexpr size_t kMaxBookmarkSeparators = 5000;
// Folders inside folders: the bar and Other Bookmarks are depth 0.
constexpr size_t kMaxBookmarkDepth = 32;
constexpr size_t kBookmarkEnd = std::numeric_limits<size_t>::max();

// Addresses Summit keeps as bookmarks: http, https, file, javascript and
// saved place: searches (opened by the bookmark UI, not by the web engine).
bool IsBookmarkableURL(const std::string& url);
bool IsBookmarkletURL(const std::string& url);
// Editors add https to a schemeless address, preserving bookmarklet source.
std::string NormalizeBookmarkAddress(std::string_view text);
// Bookmarks (not folders or separators) inside a folder, at any depth.
size_t CountBookmarks(const BookmarkNode& folder);

// A folder, for choosers: its depth (0 for the bar and Other Bookmarks).
struct BookmarkFolderChoice {
    int64_t id;
    size_t depth;
    std::string title;
};
struct BookmarkImportResult {
    size_t added = 0;
    size_t foldersAdded = 0;
    size_t separatorsAdded = 0;
    size_t updated = 0;     // existing nodes enriched with imported metadata
    size_t duplicates = 0;   // already bookmarked (or twice in the import)
    size_t unsupported = 0;  // addresses Summit does not keep (data:...)
    size_t overLimit = 0;    // past a bookmark, folder or separator limit
    int64_t folder = 0;      // the "Imported from …" folder, 0 when nothing was added
    bool Changed() const { return added || foldersAdded || separatorsAdded || updated; }
};

// A profile's bookmarks: the bookmarks bar and Other Bookmarks, each a
// folder of bookmarks and folders. Every change goes through these methods,
// which keep the flat list (Pages()) in step.
class Bookmarks {
public:
    Bookmarks();
    const BookmarkNode& Bar() const { return fBar; }
    const BookmarkNode& Other() const { return fOther; }
    // Every bookmark, the bar's first, in folder order. Those directly on the
    // bar have `bar` set. Older builds read only this list (profile.json's
    // "bookmarks").
    const std::vector<PageRecord>& Pages() const { return fPages; }
    size_t Count() const;
    size_t FolderCount() const { return fFolders; }
    size_t SeparatorCount() const { return fSeparators; }
    bool Empty() const;

    // Replaces every bookmark; the nodes' ids are assigned here.
    void Assign(BookmarkNode bar, BookmarkNode other);
    // From the flat list of older profiles: the bar's, then the others.
    void AssignPages(const std::vector<PageRecord>& pages);

    const BookmarkNode* Find(int64_t id) const;
    // The folder holding a node, and the node's index in it.
    const BookmarkNode* Parent(int64_t id, size_t* index = nullptr) const;
    // The first bookmark of that address.
    const BookmarkNode* FindURL(const std::string& url) const;
    // A known shortcut, including any binding error, or nullopt for normal
    // address/search handling. Explicit addresses are never interpreted here.
    std::optional<BookmarkNavigation> ResolveKeyword(std::string_view input) const;
    bool Contains(const std::string& url) const { return FindURL(url) != nullptr; }
    // Whether a bookmark of that address is on the bar itself (not in a folder there).
    bool OnBar(const std::string& url) const;
    // "Bookmarks Bar › Work › Rust" for the folder holding a node.
    std::string FolderPath(int64_t id) const;
    // Every folder in order, the bar and Other Bookmarks included; except
    // leaves out a folder and what is inside it (where it cannot be moved).
    std::vector<BookmarkFolderChoice> Folders(int64_t except = 0) const;

    // Each returns the new node's id, or 0 when the folder does not exist,
    // the address is not bookmarkable or a limit is reached. The index is a
    // position among the folder's children (kBookmarkEnd: at the end).
    int64_t Add(const PageRecord& page, int64_t folder, size_t index = kBookmarkEnd, int64_t added = 0);
    int64_t AddFolder(int64_t parent, const std::string& title, size_t index = kBookmarkEnd, int64_t added = 0);
    int64_t AddSeparator(int64_t parent, size_t index = kBookmarkEnd, int64_t added = 0);
    // A folder's address is ignored. False when nothing changed.
    bool Edit(int64_t id, const std::string& title, const std::string& url);
    bool SetMetadata(int64_t id, BookmarkMetadata metadata);
    // Puts a node before the child now at index of folder (kBookmarkEnd: at
    // the end). False for the two fixed folders, a folder moved into itself,
    // or no change.
    bool Move(int64_t id, int64_t folder, size_t index);
    // A bookmark, or a folder with everything in it.
    bool Remove(int64_t id);
    // Every bookmark of that address.
    bool RemoveURL(const std::string& url);
    // Copies an imported folder's contents into the folder titled folderTitle
    // in Other Bookmarks (made when missing; folders of the same title
    // inside are merged). Addresses already bookmarked are skipped, and so are
    // folders left empty.
    BookmarkImportResult Import(const BookmarkNode& imported, const std::string& folderTitle, int64_t now = 0);

private:
    BookmarkNode* FindNode(int64_t id);
    BookmarkNode* ParentNode(int64_t id, size_t* index);
    void Number(BookmarkNode& node);
    void Reindex();
    BookmarkNode fBar, fOther;
    std::vector<PageRecord> fPages;
    size_t fFolders = 0;
    size_t fSeparators = 0;
    int64_t fNextID = kOtherBookmarksFolder + 1;
};
}
