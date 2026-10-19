#include "BookmarkImport.h"
#include "Messages.h"
#include "SharedProfile.h"
#include "core/Address.h"
#include <Button.h>
#include <CheckBox.h>
#include <DateTime.h>
#include <Directory.h>
#include <File.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <LayoutBuilder.h>
#include <Node.h>
#include <NodeInfo.h>
#include <Path.h>
#include <StringView.h>
#include <TextView.h>
#include <Window.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <thread>

namespace summit {
std::string BookmarkFileURL(const char* path)
{
    BNode node(path);
    BString url;
    if (node.InitCheck() != B_OK || node.ReadAttrString("META:url", &url) != B_OK || url.IsEmpty()) return {};
    return url.String();
}

std::string RefURL(const entry_ref& ref)
{
    BPath path(&ref);
    if (path.InitCheck() != B_OK) return {};
    const auto url = BookmarkFileURL(path.Path());
    return url.empty() ? FileURL(path.Path()) : url;
}

void AddBookmarkFileDragData(BMessage& drag, const std::string& url, const std::string& title)
{
    // A file name: no slashes, and short enough for any volume.
    std::string name = title.empty() ? url : title;
    for (auto& c : name) if (c == '/') c = '-';
    if (name.size() > 200) {
        size_t cut = 200;
        while (cut && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) --cut;
        name.resize(cut);
    }
    drag.AddInt32("be:actions", B_COPY_TARGET);
    drag.AddString("be:types", B_FILE_MIME_TYPE);
    drag.AddString("be:filetypes", kBookmarkFileType);
    drag.AddString("be:type_descriptions", "Bookmark");
    drag.AddString("be:clip_name", name.c_str());
    // Text views take the address.
    drag.AddData("text/plain", B_MIME_TYPE, url.data(), url.size());
}

bool WriteDroppedBookmarkFile(const BMessage& reply, const std::string& url, const std::string& title, std::string& error)
{
    if (reply.what != B_COPY_TARGET) return false;
    entry_ref directoryRef;
    const char* name = nullptr;
    if (reply.FindRef("directory", &directoryRef) != B_OK || reply.FindString("name", &name) != B_OK || !name || !*name) {
        error = "The bookmark file was not asked for.";
        return true;
    }
    BDirectory directory(&directoryRef);
    BFile file(&directory, name, B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
    status_t status = file.InitCheck();
    BString urlText(url.c_str()), titleText(title.c_str());
    if (status == B_OK) status = file.WriteAttrString("META:url", &urlText) >= 0 ? B_OK : B_ERROR;
    if (status == B_OK && !title.empty()) file.WriteAttrString("META:title", &titleText);
    if (status == B_OK) status = BNodeInfo(&file).SetType(kBookmarkFileType);
    if (status != B_OK) error = std::string("Could not make the bookmark file: ") + strerror(status);
    return true;
}

size_t ExportBookmarksFile(SharedProfile& shared, const char* path, std::string& error)
{
    error.clear();
    const auto [html, count] = shared.Read([](const Profile& profile) {
        return std::make_pair(WriteNetscapeBookmarks(profile.bookmarks), profile.bookmarks.Count());
    });
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << html;
        if (!file.flush()) {
            error = std::strerror(errno);
            return 0;
        }
    }
    BNode node(path);
    BNodeInfo(&node).SetType("text/html");
    return count;
}

std::filesystem::path WebPositiveBookmarksFolder()
{
    BPath path;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK) return {};
    return std::filesystem::path(path.Path()) / "WebPositive" / "Bookmarks";
}

// WebPositive's BrowsingHistory: a flattened message of "history item"s with
// "url", "date time" (a BDateTime archive) and "invokations".
static void ReadWebPositiveHistory(std::vector<PageRecord>& history)
{
    BPath path;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK || path.Append("WebPositive/BrowsingHistory") != B_OK) return;
    BFile file(path.Path(), B_READ_ONLY);
    BMessage archive;
    if (file.InitCheck() != B_OK || archive.Unflatten(&file) != B_OK) return;
    BMessage item;
    for (int32 i = 0; history.size() < 4000 && archive.FindMessage("history item", i, &item) == B_OK; ++i) {
        const char* url = nullptr;
        if (item.FindString("url", &url) != B_OK || !url) continue;
        BMessage when;
        int64 visited = 0;
        if (item.FindMessage("date time", &when) == B_OK) visited = BDateTime(&when).Time_t();
        uint32 count = 1;
        item.FindUInt32("invokations", &count);
        history.push_back({url, "", visited > 0 ? visited : 0, false, static_cast<int>(std::min<uint32>(count, 1000000))});
    }
}

static std::string Count(size_t count, const char* one, const char* many)
{
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

ImportOutcome ImportBookmarksFrom(SharedProfile& shared, const std::filesystem::path& path, bool history)
{
    ImportOutcome outcome;
    BookmarkImportOptions options;
    options.history = history;
    // A bookmark file's title is its name, as Tracker and WebPositive show it.
    options.readBookmarkFile = [](const std::filesystem::path& file, std::string& url, std::string& title) {
        url = BookmarkFileURL(file.c_str());
        title.clear();
        return !url.empty();
    };
    ImportedBookmarks imported;
    std::string error;
    if (!ReadImport(path, options, imported, error)) {
        outcome.message = error;
        return outcome;
    }
    if (history && imported.source == "WebPositive") ReadWebPositiveHistory(imported.history);
    const std::string folderTitle = "Imported from " + imported.source;
    BookmarkImportResult result;
    const size_t found = CountBookmarks(imported.bookmarks);
    shared.Change([&](Profile& profile) -> uint32 {
        uint32 changes = 0;
        result = profile.bookmarks.Import(imported.bookmarks, folderTitle, std::time(nullptr));
        if (result.added) changes |= SharedProfile::kBookmarksChanged;
        if (history) {
            outcome.history = profile.ImportHistory(imported.history);
            if (outcome.history) changes |= SharedProfile::kHistoryChanged;
        }
        return changes;
    });
    outcome.ok = true;
    outcome.bookmarks = result.added;
    outcome.folder = result.folder;
    std::string text;
    if (result.added)
        text = "Imported " + Count(result.added, "bookmark", "bookmarks") + " from " + imported.source
            + " into the folder “" + folderTitle + "” in Other Bookmarks.";
    else if (found && result.duplicates == found)
        text = "Nothing new to import: every bookmark from " + imported.source + " is bookmarked already.";
    else if (found || imported.history.empty()) text = "There were no new bookmarks to import from " + imported.source + ".";
    if (result.added && result.duplicates)
        text += " " + std::to_string(result.duplicates) + (result.duplicates == 1 ? " was already bookmarked." : " were already bookmarked.");
    if (result.unsupported)
        text += " " + Count(result.unsupported, "bookmark", "bookmarks") + " could not be kept: Summit bookmarks web "
            "addresses and files (not javascript: bookmarklets or searches).";
    if (result.overLimit)
        text += " " + std::to_string(result.overLimit) + " did not fit: Summit keeps at most "
            + std::to_string(kMaxBookmarks) + " bookmarks.";
    if (history && outcome.history) text += (text.empty() ? "" : " ") + std::string("Added ") + Count(outcome.history, "page", "pages") + " to History.";
    else if (history && !imported.history.empty()) text += (text.empty() ? "" : " ") + std::string("History already had every page visited there.");
    outcome.message = text;
    return outcome;
}

namespace {
constexpr uint32 kImportWebPositive = 'imwp';
constexpr uint32 kChooseImportFile = 'imch';
constexpr uint32 kImportFileChosen = 'imfc';

BTextView* Paragraph(const char* name, const std::string& text, float width = 420)
{
    auto* view = new BTextView(name);
    view->MakeEditable(false);
    view->MakeSelectable(false);
    view->SetWordWrap(true);
    view->SetStylable(false);
    view->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
    view->SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
    const rgb_color color = ui_color(B_PANEL_TEXT_COLOR);
    view->SetFontAndColor(be_plain_font, B_FONT_ALL, &color);
    view->SetText(text.c_str());
    view->SetExplicitMinSize(BSize(width, B_SIZE_UNSET));
    return view;
}

size_t CountBookmarkFiles(const std::filesystem::path& folder)
{
    size_t count = 0;
    std::error_code code;
    for (auto entry = std::filesystem::recursive_directory_iterator(folder, code);
        !code && entry != std::filesystem::recursive_directory_iterator() && count < 100000; entry.increment(code)) {
        std::error_code ignored;
        if (entry->is_regular_file(ignored) && !BookmarkFileURL(entry->path().c_str()).empty()) ++count;
    }
    return count;
}

class BookmarkImportWindow final : public BWindow {
public:
    BookmarkImportWindow(std::shared_ptr<SharedProfile> shared, BRect owner)
        : BWindow(BRect(0, 0, 500, 400), "Import Bookmarks and History", B_TITLED_WINDOW,
            B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE),
          fShared(std::move(shared))
    {
        auto* heading = new BStringView("import-heading", "Bring your bookmarks and history from another browser");
        heading->SetFont(be_bold_font);
        auto* intro = Paragraph("import-intro", "Bookmarks go into a folder named “Imported from …” in Other Bookmarks, "
            "with their folders. Pages that are already bookmarked are left out, so importing again only adds what is new.");
        const auto webPositive = WebPositiveBookmarksFolder();
        const size_t count = CountBookmarkFiles(webPositive);
        auto* webPositiveTitle = new BStringView("webpositive-title", "WebPositive");
        webPositiveTitle->SetFont(be_bold_font);
        auto* webPositiveText = Paragraph("webpositive-text", count
            ? "Its " + Count(count, "bookmark", "bookmarks") + " in " + webPositive.string() + ", and the pages it visited."
            : "No WebPositive bookmarks were found in " + webPositive.string() + ".", 300);
        fWebPositive = new BButton("import-webpositive", "Import", new BMessage(kImportWebPositive));
        fWebPositive->SetEnabled(count > 0);
        auto* fileTitle = new BStringView("file-title", "Firefox, Chrome or another browser");
        fileTitle->SetFont(be_bold_font);
        auto* fileText = Paragraph("file-text", "Firefox: places.sqlite from a profile folder (bookmarks and history), "
            "or an exported bookmarks HTML file. Chrome and Chromium: the Bookmarks file in a profile folder (the History "
            "file beside it is read too). Any other browser: its exported bookmarks HTML file.", 300);
        fChoose = new BButton("import-choose", "Choose File…", new BMessage(kChooseImportFile));
        fHistory = new BCheckBox("import-history", "Also import the history of visited pages", nullptr);
        fHistory->SetValue(B_CONTROL_ON);
        fStatus = Paragraph("import-status", "");
        auto* close = new BButton("import-close", "Close", new BMessage(B_QUIT_REQUESTED));
        BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
            .SetInsets(B_USE_WINDOW_SPACING)
            .Add(heading)
            .Add(intro)
            .AddStrut(4)
            .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
                .Add(webPositiveTitle, 0, 0)
                .Add(webPositiveText, 0, 1)
                .Add(fWebPositive, 1, 1)
                .Add(fileTitle, 0, 2)
                .Add(fileText, 0, 3)
                .Add(fChoose, 1, 3)
            .End()
            .Add(fHistory)
            .Add(fStatus)
            .AddGroup(B_HORIZONTAL)
                .AddGlue()
                .Add(close)
            .End();
        SetDefaultButton(close);
        CenterIn(owner);
    }
    ~BookmarkImportWindow() override
    {
        if (fWorker.joinable()) fWorker.join();
    }
    void MessageReceived(BMessage* message) override
    {
        switch (message->what) {
            case kImportWebPositive: Start(WebPositiveBookmarksFolder()); break;
            case kChooseImportFile: {
                if (!fPanel) {
                    BMessenger target(this);
                    fPanel = std::make_unique<BFilePanel>(B_OPEN_PANEL, &target, nullptr, B_FILE_NODE | B_DIRECTORY_NODE,
                        false, new BMessage(kImportFileChosen));
                    fPanel->Window()->SetTitle("Summit: Import Bookmarks");
                }
                fPanel->Show();
                break;
            }
            case kImportFileChosen: {
                entry_ref ref;
                BPath path;
                if (message->FindRef("refs", &ref) == B_OK && path.SetTo(&ref) == B_OK) Start(path.Path());
                break;
            }
            case kBookmarkImportDone: {
                if (fWorker.joinable()) fWorker.join();
                SetBusy(false);
                fStatus->SetText(message->GetString("message", ""));
                // The window grows to show the whole message.
                InvalidateLayout();
                ResizeToPreferred();
                break;
            }
            default: BWindow::MessageReceived(message);
        }
    }

private:
    void SetBusy(bool busy)
    {
        fBusy = busy;
        fWebPositive->SetEnabled(!busy && CountBookmarkFiles(WebPositiveBookmarksFolder()) > 0);
        fChoose->SetEnabled(!busy);
        fHistory->SetEnabled(!busy);
    }
    void Start(std::filesystem::path path)
    {
        if (fBusy || path.empty()) return;
        if (fWorker.joinable()) fWorker.join();
        SetBusy(true);
        fStatus->SetText(("Importing from " + path.filename().string() + "…").c_str());
        // Reading a large places.sqlite takes a moment; the window stays responsive.
        fWorker = std::thread([shared = fShared, path, history = fHistory->Value() == B_CONTROL_ON, target = BMessenger(this)] {
            const auto outcome = ImportBookmarksFrom(*shared, path, history);
            std::fprintf(stderr, "Summit: import from %s: %s\n", path.c_str(), outcome.message.c_str());
            BMessage done(kBookmarkImportDone);
            done.AddString("message", outcome.message.c_str());
            done.AddBool("error", !outcome.ok);
            target.SendMessage(&done);
        });
    }
    std::shared_ptr<SharedProfile> fShared;
    BButton* fWebPositive;
    BButton* fChoose;
    BCheckBox* fHistory;
    BTextView* fStatus;
    std::unique_ptr<BFilePanel> fPanel;
    std::thread fWorker;
    bool fBusy = false;
};

std::mutex sImportWindowLock;
BMessenger sImportWindow;
}

void ShowBookmarkImportWindow(std::shared_ptr<SharedProfile> shared, BRect owner)
{
    std::lock_guard lock(sImportWindowLock);
    if (sImportWindow.IsValid()) {
        BMessage activate(B_SET_PROPERTY);
        activate.AddBool("data", true);
        activate.AddSpecifier("Active");
        if (sImportWindow.SendMessage(&activate) == B_OK) return;
    }
    auto* window = new BookmarkImportWindow(std::move(shared), owner);
    sImportWindow = BMessenger(window);
    window->Show();
}
}
