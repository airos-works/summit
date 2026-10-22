#include "core/Bookmarks.h"
#include "core/Profile.h"
#include "ui/BookmarkImport.h"
#include "ui/BookmarkEditor.h"
#include "ui/Chrome.h"
#include "ui/Messages.h"
#include "ui/SharedProfile.h"
#include <Application.h>
#include <Bitmap.h>
#include <Button.h>
#include <Menu.h>
#include <MenuItem.h>
#include <SeparatorItem.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>

using namespace summit;
namespace {
constexpr uint32 kExercise = 'bkex', kFence = 'bkfn', kDone = 'bkdn';
int checks = 0, failures = 0;
void Check(bool pass, const char* name)
{
    ++checks;
    if (!pass) ++failures;
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", name);
    std::fflush(stdout);
}
std::shared_ptr<BookmarkButton> Snapshot()
{
    auto root = std::make_shared<BookmarkButton>();
    root->folder = true;
    BookmarkButton separator;
    separator.id = 3;
    separator.separator = true;
    BookmarkButton page;
    page.id = 4;
    page.title = "Example";
    page.url = "https://example.com/";
    root->children = {separator, page, separator};
    return root;
}
class TestWindow final : public BWindow {
public:
    TestWindow() : BWindow(BRect(20, 20, 420, 70), "Bookmark tests", B_TITLED_WINDOW, B_NOT_RESIZABLE)
    {
        fBar = new BookmarksBar;
        AddChild(fBar);
        fBar->ResizeTo(400, 27);
        fBar->SetBookmarks(Snapshot());
        Run(); // Keep the window hidden; it only receives this test's messages.
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == kExercise) {
            fBar->MouseDown(BPoint(10, 10)); // Separator: never opens an address.
            fBar->MouseUp(BPoint(10, 10));
            fBar->MouseDown(BPoint(30, 10)); // The adjacent bookmark still opens.
            fBar->MouseUp(BPoint(30, 10));
            PostMessage(kFence);
        } else if (message->what == kOpenBookmark) {
            ++fOpened;
            fURL = message->GetString("url", "");
        } else if (message->what == kFence) {
            BMessage done(kDone);
            done.AddInt32("opened", fOpened);
            done.AddString("url", fURL.c_str());
            be_app_messenger.SendMessage(&done);
            Quit();
        } else BWindow::MessageReceived(message);
    }
private:
    BookmarksBar* fBar;
    int32 fOpened = 0;
    std::string fURL;
};
class Tests final : public BApplication {
public:
    explicit Tests(std::filesystem::path directory)
        : BApplication("application/x-vnd.Summit-BookmarkTests"), fDirectory(std::move(directory)) { }
    void ReadyToRun() override
    {
        std::filesystem::create_directories(fDirectory);
        const auto profilePath = fDirectory / "profile.json";
        Check(!std::filesystem::exists(profilePath), "isolated profile starts empty");
        if (std::filesystem::exists(profilePath)) { PostMessage(B_QUIT_REQUESTED); return; }
        fShared = std::make_unique<SharedProfile>(profilePath);
        fShared->AddListener(BMessenger(this));
        std::string error;
        Check(fShared->Save(error), "initial profile saved before import");
        const auto emptyFile = fDirectory / "empty.html";
        { std::ofstream file(emptyFile); file << "<DL><DT><H3>Empty folder</H3><DL></DL></DL>"; }
        auto result = ImportBookmarksFrom(*fShared, emptyFile, false);
        Check(result.ok && result.bookmarks == 0 && result.folder, "empty-folder import reports a change");
        Check(result.message.find("folder") != std::string::npos && result.message.find("no new") == std::string::npos,
            "empty-folder result describes the imported folders");
        Check(fShared->Save(error), "folder-only import can be saved");
        auto loaded = Profile::Load(profilePath, error);
        Check(error.empty() && loaded.bookmarks.FolderCount() == 2 && loaded.bookmarks.Count() == 0,
            "folder-only import survives reopening the saved profile");
        result = ImportBookmarksFrom(*fShared, emptyFile, false);
        Check(result.ok && !result.folder, "repeated empty-folder import has no change");
        const auto separatorsFile = fDirectory / "separators.html";
        { std::ofstream file(separatorsFile); file << "<DL><HR><HR></DL>"; }
        result = ImportBookmarksFrom(*fShared, separatorsFile, false);
        Check(result.ok && result.bookmarks == 0 && result.folder && result.message.find("2 separators") != std::string::npos,
            "separator-only import is reported");
        Check(fShared->Save(error), "separator-only import can be saved");
        loaded = Profile::Load(profilePath, error);
        Check(error.empty() && loaded.bookmarks.SeparatorCount() == 2 && loaded.bookmarks.Count() == 0,
            "separator-only import survives reopening the saved profile");
        result = ImportBookmarksFrom(*fShared, separatorsFile, false);
        Check(result.ok && !result.folder, "repeated separator import has no change");

        const auto metadataFile = fDirectory / "metadata.html";
        { std::ofstream file(metadataFile); file << "<DL><DT><A HREF='https://metadata.example/' TAGS='first'>Metadata</A></DL>"; }
        result = ImportBookmarksFrom(*fShared, metadataFile, false);
        Check(result.ok && result.bookmarks == 1, "bookmark with metadata imported");
        Check(fShared->Save(error), "initial metadata import saved");
        { std::ofstream file(metadataFile); file << "<DL><DT><A HREF='https://metadata.example/' TAGS='first,second'>Other title</A>"
            "<DD>Imported description</DL>"; }
        result = ImportBookmarksFrom(*fShared, metadataFile, false);
        Check(result.ok && result.bookmarks == 0 && result.message.find("Updated tags and descriptions") != std::string::npos,
            "metadata-only import reports an update");
        Check(fShared->Save(error), "metadata-only import saved");
        loaded = Profile::Load(profilePath, error);
        const auto* updated = loaded.bookmarks.FindURL("https://metadata.example/");
        Check(updated && updated->title == "Metadata" && updated->metadata.description == "Imported description"
            && updated->metadata.tags == std::vector<std::string>{"first", "second"},
            "metadata-only import survives reopening without replacing the title");
        result = ImportBookmarksFrom(*fShared, metadataFile, false);
        Check(result.ok && result.message.find("Updated") == std::string::npos, "identical metadata reimport changes nothing");

        BookmarkEditor::Fields fields;
        fields.title = "Editor test";
        fields.url = "https://metadata.example/";
        fields.metadata = {"Original\nnotes", {"one", "comma, tag"}};
        fields.folders = {{kOtherBookmarksFolder, 0, "Other Bookmarks"}};
        auto* editor = new BookmarkEditor(fields, BMessenger(this), BRect(0, 0, 800, 600));
        fEditor = BMessenger(editor);
        auto* tags = dynamic_cast<BTextControl*>(editor->FindView("tags"));
        auto* description = dynamic_cast<BTextView*>(editor->FindView("description"));
        auto* save = dynamic_cast<BButton*>(editor->FindView("save"));
        Check(tags && description && save, "native editor exposes tags and a multiline description");
        if (tags && description && save) {
            Check(ParseBookmarkTags(tags->Text()) == fields.metadata.tags && description->Text() == fields.metadata.description,
                "native editor shows imported metadata without losing commas or newlines");
            tags->SetText("one, \"comma, tag\", \"quote\"\"tag\"");
            description->SetText("Edited\nsecond line");
            save->Invoke();
        } else editor->PostMessage(B_QUIT_REQUESTED);
        editor->Run();

        auto snapshot = Snapshot();
        BMenu menu("Bookmarks");
        AddBookmarkItems(&menu, snapshot, *snapshot, BMessenger(this));
        Check(menu.CountItems() == 5, "menu includes ordered separators, bookmark and Open All");
        Check(dynamic_cast<BSeparatorItem*>(menu.ItemAt(0)) && dynamic_cast<BSeparatorItem*>(menu.ItemAt(2)),
            "imported separators use native separator menu items");
        Check(!menu.ItemAt(0)->Message() && !menu.ItemAt(2)->Message(), "menu separators have no navigation message");
        Check(menu.ItemAt(1)->Message() && std::string(menu.ItemAt(1)->Message()->GetString("url", "")) == "https://example.com/",
            "bookmark between separators retains its address");
        auto onlySeparators = Snapshot();
        onlySeparators->children.erase(onlySeparators->children.begin() + 1);
        BMenu emptyMenu("Separators");
        AddBookmarkItems(&emptyMenu, onlySeparators, *onlySeparators, BMessenger(this));
        Check(emptyMenu.CountItems() == 2, "separator-only menu offers no Open All command");

        BBitmap bitmap(BRect(0, 0, 399, 27), B_RGB32, true);
        const bool locked = bitmap.InitCheck() == B_OK && bitmap.Lock();
        Check(locked, "native toolbar bitmap is available");
        if (!locked) { PostMessage(B_QUIT_REQUESTED); return; }
        auto* bar = new BookmarksBar;
        bitmap.AddChild(bar);
        bar->ResizeTo(399, 27);
        bar->SetBookmarks(snapshot);
        bar->Draw(bar->Bounds());
        bar->Sync();
        auto* pixels = static_cast<const uint32*>(bitmap.Bits());
        const int32 stride = bitmap.BytesPerRow() / 4;
        Check(pixels[10 * stride + 11] != pixels[10 * stride + 8], "toolbar separator draws a vertical line");
        Check(pixels[4 * stride + 11] == pixels[4 * stride + 8], "toolbar separator leaves space above the line");
        bitmap.Unlock();
        fWindow = BMessenger(new TestWindow);
        fStarted = system_time();
        fWindow.SendMessage(kExercise);
        SetPulseRate(100000);
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == kProfileChanged) {
            if (message->GetUInt32("changes", 0) & SharedProfile::kBookmarksChanged) ++fNotifications;
        } else if (message->what == kBookmarkEdited) {
            Check(std::string(message->GetString("description", "")) == "Edited\nsecond line",
                "native Save retains the edited multiline description");
            std::vector<std::string> tags;
            const char* tag;
            for (int32 i = 0; message->FindString("tag", i, &tag) == B_OK; ++i) tags.emplace_back(tag);
            Check(tags == std::vector<std::string>{"one", "comma, tag", "quote\"tag"}, "native Save retains quoted tag names");
            fEditorDone = true;
        } else if (message->what == kDone) {
            Check(message->GetInt32("opened", -1) == 1 && std::string(message->GetString("url", "")) == "https://example.com/",
                "toolbar click ignores separator and opens the adjacent bookmark once");
            fDone = true;
        } else BApplication::MessageReceived(message);
    }
    void Pulse() override
    {
        if (fDone && !fWindow.IsValid() && fEditorDone && !fEditor.IsValid()) {
            Check(fNotifications == 4, "only changed imports notify the browser windows");
            fShared.reset();
            std::printf("%d native checks, %d failures\n", checks, failures);
            PostMessage(B_QUIT_REQUESTED);
        } else if (system_time() - fStarted > 5000000) {
            Check(false, "native test finishes within five seconds");
            if (fWindow.IsValid()) fWindow.SendMessage(B_QUIT_REQUESTED);
            if (fEditor.IsValid()) fEditor.SendMessage(B_QUIT_REQUESTED);
            PostMessage(B_QUIT_REQUESTED);
        }
    }
private:
    std::filesystem::path fDirectory;
    std::unique_ptr<SharedProfile> fShared;
    BMessenger fWindow;
    BMessenger fEditor;
    int fNotifications = 0;
    bool fDone = false;
    bool fEditorDone = false;
    bigtime_t fStarted = 0;
};
}
int main(int argc, char** argv)
{
    if (argc != 2) return 64;
    Tests app(argv[1]);
    app.Run();
    return failures ? 1 : 0;
}
