#include "BookmarkEditor.h"
#include "Messages.h"
#include "core/Address.h"
#include <Alert.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <SpaceLayoutItem.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>

namespace summit {
namespace {
constexpr uint32 kSave = 'bsav';
constexpr uint32 kDelete = 'bdel';

// A folder in the chooser, indented by its depth.
class FolderItem final : public BMenuItem {
public:
    FolderItem(const char* label, BMessage* message, size_t depth) : BMenuItem(label, message), fIndent(depth * 16.0f) { }
    void GetContentSize(float* width, float* height) override
    {
        BMenuItem::GetContentSize(width, height);
        *width += fIndent;
    }
    void DrawContent() override
    {
        Menu()->MovePenBy(fIndent, 0);
        BMenuItem::DrawContent();
    }

private:
    float fIndent;
};

const char* WindowTitle(BookmarkEditor::Mode mode)
{
    switch (mode) {
        case BookmarkEditor::kNewBookmarkMode: return "Bookmark This Page";
        case BookmarkEditor::kEditFolderMode: return "Edit Folder";
        case BookmarkEditor::kNewFolderMode: return "New Folder";
        default: return "Edit Bookmark";
    }
}
}

BookmarkEditor::BookmarkEditor(const Fields& fields, BMessenger target, BRect owner)
    : BWindow(BRect(0, 0, 460, 160), WindowTitle(fields.mode), B_TITLED_WINDOW_LOOK, B_MODAL_APP_WINDOW_FEEL,
        B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_NOT_V_RESIZABLE | B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS
        | B_CLOSE_ON_ESCAPE),
      fFields(fields), fTarget(target)
{
    const bool folder = fields.mode == kEditFolderMode || fields.mode == kNewFolderMode;
    const bool creating = fields.mode == kNewBookmarkMode || fields.mode == kNewFolderMode;
    fName = new BTextControl("name", "Name:", fields.title.c_str(), nullptr);
    fName->SetExplicitMinSize(BSize(360, B_SIZE_UNSET));
    if (!folder) fAddress = new BTextControl("address", "Address:", fields.url.c_str(), nullptr);
    fTags = new BTextControl("tags", "Tags:", BookmarkTagsText(fields.metadata.tags).c_str(), nullptr);
    fTags->SetToolTip("Separate tags with commas. Quote a tag that contains a comma.");
    if (!folder) {
        fKeywords = new BTextControl("keywords", "Keywords:", BookmarkKeywordsText(fields.metadata.keywords).c_str(), nullptr);
        fKeywords->SetToolTip("Shortcuts to type in the address bar, separated by commas. New names use the first shortcut's search request.");
    }
    fDescription = new BTextView("description");
    fDescription->SetText(fields.metadata.description.c_str());
    fDescription->SetExplicitMinSize(BSize(300, 64));
    fDescription->SetFontAndColor(be_plain_font);
    auto* description = new BScrollView("description-scroll", fDescription, 0, false, true);
    auto* menu = new BPopUpMenu("folders");
    for (const auto& choice : fields.folders) {
        auto* message = new BMessage('fold');
        message->AddInt64("folder", choice.id);
        auto* item = new FolderItem(choice.title.c_str(), message, choice.depth);
        item->SetMarked(choice.id == fields.folder);
        menu->AddItem(item);
    }
    if (!menu->FindMarked() && menu->CountItems()) menu->ItemAt(0)->SetMarked(true);
    fFolder = new BMenuField("folder", "Folder:", menu);
    auto* save = new BButton("save", creating ? (folder ? "Create" : "Add") : "Save", new BMessage(kSave));
    auto* cancel = new BButton("cancel", "Cancel", new BMessage(B_QUIT_REQUESTED));
    BGridLayout* grid;
    BGroupLayout* buttons;
    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_SPACING)
        .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING).GetLayout(&grid).End()
        .AddGroup(B_HORIZONTAL).GetLayout(&buttons).End();
    int32 row = 0;
    grid->AddItem(fName->CreateLabelLayoutItem(), 0, row);
    grid->AddItem(fName->CreateTextViewLayoutItem(), 1, row++);
    if (fAddress) {
        grid->AddItem(fAddress->CreateLabelLayoutItem(), 0, row);
        grid->AddItem(fAddress->CreateTextViewLayoutItem(), 1, row++);
    }
    grid->AddItem(fFolder->CreateLabelLayoutItem(), 0, row);
    grid->AddItem(fFolder->CreateMenuBarLayoutItem(), 1, row++);
    grid->AddItem(fTags->CreateLabelLayoutItem(), 0, row);
    grid->AddItem(fTags->CreateTextViewLayoutItem(), 1, row++);
    if (fKeywords) {
        grid->AddItem(fKeywords->CreateLabelLayoutItem(), 0, row);
        grid->AddItem(fKeywords->CreateTextViewLayoutItem(), 1, row++);
    }
    grid->AddView(new BStringView("description-label", "Description:"), 0, row);
    grid->AddView(description, 1, row++);
    // Delete sits at the left, away from Save.
    if (!creating) buttons->AddView(new BButton("delete", folder ? "Delete Folder" : "Delete", new BMessage(kDelete)));
    buttons->AddItem(BSpaceLayoutItem::CreateGlue());
    buttons->AddView(cancel);
    buttons->AddView(save);
    SetDefaultButton(save);
    fName->MakeFocus(true);
    fName->TextView()->SelectAll();
    CenterIn(owner);
}

void BookmarkEditor::Save()
{
    const bool folder = fFields.mode == kEditFolderMode || fFields.mode == kNewFolderMode;
    std::string title = Trim(fName->Text());
    std::string url = fAddress ? Trim(fAddress->Text()) : std::string();
    if (!folder) {
        url = NormalizeBookmarkAddress(url);
        if (!IsBookmarkableURL(url)) {
            auto* alert = new BAlert("Edit Bookmark", "Bookmarks keep web addresses (https://…), files (file://…), bookmarklets (javascript:…) and smart folders (place:…).",
                "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
            alert->SetShortcut(0, B_ESCAPE);
            alert->Go(nullptr);
            fAddress->MakeFocus(true);
            return;
        }
        if (title.empty()) title = url;
    } else if (title.empty()) title = "Untitled Folder";
    int64 parent = fFields.folder;
    if (auto* marked = fFolder->Menu()->FindMarked(); marked && marked->Message())
        marked->Message()->FindInt64("folder", &parent);
    BMessage edited(kBookmarkEdited);
    edited.AddInt32("mode", fFields.mode);
    edited.AddInt64("id", fFields.id);
    edited.AddString("title", title.c_str());
    edited.AddString("url", url.c_str());
    edited.AddInt64("folder", parent);
    edited.AddString("description", fDescription->Text());
    for (const auto& tag : ParseBookmarkTags(fTags->Text())) edited.AddString("tag", tag.c_str());
    if (fKeywords) edited.AddString("keywords", fKeywords->Text());
    fTarget.SendMessage(&edited);
    Quit();
}

void BookmarkEditor::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case kSave: Save(); break;
        case kDelete: {
            BMessage remove(kDeleteBookmarkNode);
            remove.AddInt64("id", fFields.id);
            fTarget.SendMessage(&remove);
            Quit();
            break;
        }
        default: BWindow::MessageReceived(message);
    }
}
}
