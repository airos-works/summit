#pragma once
#include "core/Bookmarks.h"
#include <Messenger.h>
#include <Window.h>
#include <string>
#include <vector>

class BButton;
class BMenuField;
class BTextControl;
class BTextView;

namespace summit {
// Edits a bookmark or a folder, or makes one: its name, its address and the
// folder it is in. The answer goes to the browser window as kBookmarkEdited
// (or kDeleteBookmarkNode for Delete); the window changes the profile.
class BookmarkEditor final : public BWindow {
public:
    enum Mode : int32 { kEditBookmarkMode, kNewBookmarkMode, kEditFolderMode, kNewFolderMode };
    struct Fields {
        Mode mode = kEditBookmarkMode;
        int64 id = 0;
        std::string title, url;
        int64 folder = kOtherBookmarksFolder;
        // Where it can go (Bookmarks::Folders).
        std::vector<BookmarkFolderChoice> folders;
        BookmarkMetadata metadata;
    };
    BookmarkEditor(const Fields& fields, BMessenger target, BRect owner);
    void MessageReceived(BMessage* message) override;

private:
    void Save();
    Fields fFields;
    BMessenger fTarget;
    BTextControl* fName = nullptr;
    BTextControl* fAddress = nullptr;
    BTextControl* fTags = nullptr;
    BTextView* fDescription = nullptr;
    BMenuField* fFolder = nullptr;
};
}
