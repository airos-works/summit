#pragma once
#include "Bookmarks.h"
#include "PageRecord.h"
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace summit {
// Reading other browsers' bookmarks (and history) and writing Summit's as the
// Netscape bookmarks HTML that every browser imports.

// What an import read: a folder of bookmarks and folders to copy into the
// profile (Bookmarks::Import), and pages visited, most recent first.
struct ImportedBookmarks {
    // "Firefox", "Chrome", "WebPositive"..., or the file's name: the folder
    // the bookmarks go into is "Imported from <source>".
    std::string source;
    BookmarkNode bookmarks { 0, "", "", true, 0, {} };
    std::vector<PageRecord> history;
};
// Reads a bookmark file of a folder of them (WebPositive's): the address
// and the title (empty for the file's name). False for files that are not
// bookmarks.
using BookmarkFileReader = std::function<bool(const std::filesystem::path& file, std::string& url, std::string& title)>;
struct BookmarkImportOptions {
    // SQLite is not linked: places.sqlite and Chrome's History are read
    // with this library, loaded when needed.
    std::string sqliteLibrary = "/boot/system/lib/libsqlite3.so.0";
    bool history = true;
    BookmarkFileReader readBookmarkFile;
};
// Recognises what path holds: a folder of bookmark files (WebPositive's
// Bookmarks folder), Netscape bookmarks HTML (Firefox's, Chrome's or any
// browser's export), Chrome's or Chromium's Bookmarks JSON (and the History
// beside it), Firefox's places.sqlite (bookmarks and history) or Chrome's
// History database.
bool ReadImport(const std::filesystem::path& path, const BookmarkImportOptions& options, ImportedBookmarks& imported,
    std::string& error);

// The formats, for the tests and the readers above.
bool ParseNetscapeBookmarks(std::string_view html, BookmarkNode& folder, std::string& error);
// "Firefox", "Chrome" or "Summit" when an HTML export says which browser wrote it.
std::string NetscapeBookmarksSource(std::string_view html);
bool ParseChromeBookmarks(std::string_view json, BookmarkNode& folder, std::string& error);
bool ReadBookmarkDirectory(const std::filesystem::path& directory, const BookmarkFileReader& read, BookmarkNode& folder,
    std::string& error);
bool ReadFirefoxPlaces(const std::filesystem::path& places, const std::string& sqliteLibrary, BookmarkNode* folder,
    std::vector<PageRecord>* history, std::string& error);
bool ReadChromeHistory(const std::filesystem::path& historyFile, const std::string& sqliteLibrary,
    std::vector<PageRecord>& history, std::string& error);

// The bookmarks bar as a folder marked as the toolbar, then Other Bookmarks'
// contents, as Chrome exports them.
std::string WriteNetscapeBookmarks(const Bookmarks& bookmarks);
}
