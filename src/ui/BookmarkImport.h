#pragma once
#include "core/BookmarkIO.h"
#include <Entry.h>
#include <Message.h>
#include <Rect.h>
#include <filesystem>
#include <memory>
#include <string>

namespace summit {
class SharedProfile;

// Haiku's bookmark files, which WebPositive keeps and Tracker shows: the
// address is the file's META:url attribute, its name the title.
constexpr const char* kBookmarkFileType = "application/x-vnd.Be-bookmark";
// The address of a bookmark file, or empty when it is not one. Its title is
// its name (WebPositive also writes META:title, which Tracker does not show).
std::string BookmarkFileURL(const char* path);
// What opening a file means: a bookmark file's page, otherwise the file
// itself (file://); empty when the entry has no path.
std::string RefURL(const entry_ref& ref);
// What a drag needs for Tracker to make a bookmark file where it is dropped
// (the negotiated drop: Tracker answers with B_COPY_TARGET).
void AddBookmarkFileDragData(BMessage& drag, const std::string& url, const std::string& title);
// Tracker's B_COPY_TARGET answer to such a drag: writes the bookmark file it
// asked for. False when the message is not that answer.
bool WriteDroppedBookmarkFile(const BMessage& reply, const std::string& url, const std::string& title, std::string& error);

// ~/config/settings/WebPositive/Bookmarks.
std::filesystem::path WebPositiveBookmarksFolder();
struct ImportOutcome {
    bool ok = false;
    // What to tell the user.
    std::string message;
    size_t bookmarks = 0, history = 0;
    int64 folder = 0;
};
// Reads what path holds (ReadImport; WebPositive's folder with its history),
// copies it into the profile and announces the change. Any thread.
ImportOutcome ImportBookmarksFrom(SharedProfile& shared, const std::filesystem::path& path, bool history);
// Writes every bookmark to path as Netscape bookmarks HTML (text/html);
// returns how many, with error set when it could not.
size_t ExportBookmarksFile(SharedProfile& shared, const char* path, std::string& error);
// File › Import Bookmarks and History…: one window for the application,
// brought forward when it is already open.
void ShowBookmarkImportWindow(std::shared_ptr<SharedProfile> shared, BRect owner);
}
