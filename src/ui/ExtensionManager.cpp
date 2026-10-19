#include "ExtensionManager.h"
#if SUMMIT_MODERN_WEBKIT
#include "ExtensionInstaller.h"
#include "Messages.h"
#include "core/ExtensionPackage.h"
#include <WebKit/WebKitView.h>
#include <Bitmap.h>
#include <Button.h>
#include <DataIO.h>
#include <TranslationUtils.h>
#include <CheckBox.h>
#include <FilePanel.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <ListItem.h>
#include <ListView.h>
#include <Message.h>
#include <ScrollView.h>
#include <String.h>
#include <StringView.h>
#include <TextView.h>
#include <TextControl.h>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace summit {
namespace {
constexpr uint32 choosePackage = 'exfp', selectEntry = 'exse', pinnedToggled = 'expt', privateToggled = 'exvt';
std::string field(const BMessage& message, const char* name)
{
    const char* value = nullptr;
    return message.FindString(name, &value) == B_OK && value ? value : "";
}
float Scale() { return std::max(1.0f, be_plain_font->Size() / 12.0f); }
rgb_color Mix(rgb_color a, rgb_color b, float amount)
{
    auto channel = [&](uint8 x, uint8 y) { return static_cast<uint8>(x + (y - x) * amount); };
    return make_color(channel(a.red, b.red), channel(a.green, b.green), channel(a.blue, b.blue));
}
// An extension in the list: its icon and its name, dimmed while it is off.
class Item final : public BListItem {
public:
    explicit Item(const BMessage& entry)
        : identifier(field(entry, "identifier")), name(field(entry, "name")), version(field(entry, "version"))
        , error(field(entry, "error")), notice(field(entry, "notice")), package(field(entry, "package"))
    {
        entry.FindBool("enabled", &enabled);
        entry.FindBool("loaded", &loaded);
        entry.FindBool("installed", &installed);
        entry.FindBool("pinned", &pinned);
        entry.FindBool("private", &privateBrowsing);
    }
    void Update(BView* owner, const BFont* font) override
    {
        BListItem::Update(owner, font);
        font_height height;
        font->GetHeight(&height);
        SetHeight(std::ceil(std::max(IconSize(), height.ascent + height.descent) + 10 * Scale()));
    }
    void DrawItem(BView* owner, BRect frame, bool) override
    {
        const rgb_color background = ui_color(IsSelected() ? B_LIST_SELECTED_BACKGROUND_COLOR : B_LIST_BACKGROUND_COLOR);
        const rgb_color text = ui_color(IsSelected() ? B_LIST_SELECTED_ITEM_TEXT_COLOR : B_LIST_ITEM_TEXT_COLOR);
        owner->SetHighColor(background);
        owner->FillRect(frame);
        const float s = Scale(), size = IconSize();
        const BRect iconFrame(frame.left + std::round(6 * s), frame.top + std::floor((frame.Height() + 1 - size) / 2),
            frame.left + std::round(6 * s) + size - 1, frame.top + std::floor((frame.Height() + 1 - size) / 2) + size - 1);
        const bool on = loaded || enabled;
        owner->SetDrawingMode(B_OP_ALPHA);
        if (icon) {
            owner->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
            if (!on) {
                owner->SetHighColor(0, 0, 0, 110);
                owner->SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
            }
            owner->DrawBitmap(icon.get(), icon->Bounds(), iconFrame, B_FILTER_BITMAP_BILINEAR);
        } else {
            // No icon Summit can read: the name's first letter on a tile.
            owner->SetHighColor(Mix(background, text, on ? 0.22f : 0.12f));
            owner->FillRoundRect(iconFrame, 4 * s, 4 * s);
            BFont font(be_bold_font);
            font.SetSize(size * 0.55f);
            owner->SetFont(&font);
            owner->SetHighColor(Mix(background, text, on ? 0.85f : 0.5f));
            const std::string letter = name.empty() ? "?" : std::string(1, char(std::toupper(static_cast<unsigned char>(name[0]))));
            font_height height;
            font.GetHeight(&height);
            owner->DrawString(letter.c_str(), BPoint(iconFrame.left + std::floor((size - font.StringWidth(letter.c_str())) / 2),
                iconFrame.top + std::floor((size + height.ascent - height.descent) / 2)));
            owner->SetFont(be_plain_font);
        }
        owner->SetDrawingMode(B_OP_COPY);
        font_height height;
        be_plain_font->GetHeight(&height);
        owner->SetHighColor(on ? text : Mix(background, text, 0.55f));
        BString label(ExtensionDisplayText(name).c_str());
        const float left = iconFrame.right + std::round(8 * s);
        owner->TruncateString(&label, B_TRUNCATE_END, std::max(0.0f, frame.right - left - 4));
        owner->DrawString(label.String(), BPoint(left, frame.top + std::floor((frame.Height() + 1 + height.ascent - height.descent) / 2)));
    }
    static float IconSize() { return std::round(24 * Scale()); }
    std::string identifier, name, version, error, notice, package;
    bool enabled = false, loaded = false, installed = true, pinned = true, privateBrowsing = false;
    std::shared_ptr<BBitmap> icon;
};
// The extension's own icon from its package, at the screen's density.
std::shared_ptr<BBitmap> LoadIcon(const std::string& package)
{
    if (package.empty()) return nullptr;
    const int pixels = int(std::ceil(Item::IconSize() * std::max(1.0f, BWebKitDisplayScale())));
    const auto data = ExtensionIconData(package, pixels);
    if (data.empty()) return nullptr;
    BMemoryIO input(data.data(), data.size());
    std::shared_ptr<BBitmap> icon(BTranslationUtils::GetBitmap(&input));
    if (!icon || icon->InitCheck() != B_OK) return nullptr;
    return icon;
}
}
ExtensionManager::ExtensionManager(BMessenger owner, std::shared_ptr<std::atomic<unsigned>> windows)
    : BWindow(BRect(130, 110, 1040, 680), "Extensions — Summit", B_TITLED_WINDOW,
        B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE)
    , fOwner(owner), fWindows(std::move(windows))
{
    ++*fWindows;
    auto* title = new BStringView("extensions-title", "Extensions");
    title->SetFont(be_bold_font);
    fAdd = new BButton("extension-add", "Add extension…", new BMessage(choosePackage));
    auto storeButton = [](const char* label, const char* url) {
        auto* message = new BMessage(kBrowseExtensionStore);
        message->AddString("url", url);
        return new BButton(label, label, message);
    };
    fStoreURL = new BTextControl("extension-store-url", "Store link:", "", new BMessage(kExtensionStoreInstall));
    fStoreURL->SetToolTip("Paste an extension's detail-page link from either store.");
    fStoreInstall = new BButton("extension-store-install", "Review extension…", new BMessage(kExtensionStoreInstall));
    fList = new BListView("extension-list");
    fList->SetSelectionMessage(new BMessage(selectEntry));
    auto* list = new BScrollView("extension-list-scroll", fList, 0, false, true);
    list->SetExplicitMinSize(BSize(220, 250));
    list->SetExplicitMaxSize(BSize(280, B_SIZE_UNLIMITED));
    fHeading = new BStringView("extension-heading", "No extension selected");
    fHeading->SetFont(be_bold_font);
    fDetails = new BTextView("extension-details");
    fDetails->MakeEditable(false);
    fDetails->MakeSelectable(true);
    fDetails->SetWordWrap(true);
    fDetails->SetInsets(12, 12, 12, 12);
    fDetails->SetViewColor(ui_color(B_DOCUMENT_BACKGROUND_COLOR));
    fDetails->SetLowColor(fDetails->ViewColor());
    auto color = ui_color(B_DOCUMENT_TEXT_COLOR);
    fDetails->SetFontAndColor(be_plain_font, B_FONT_ALL, &color);
    auto* details = new BScrollView("extension-details-scroll", fDetails, 0, false, true);
    details->SetExplicitMinSize(BSize(420, 260));
    fToggle = new BButton("extension-toggle", "Enable", new BMessage(kExtensionEnable));
    fRemove = new BButton("extension-remove", "Remove", new BMessage(kExtensionRemove));
    fPinned = new BCheckBox("extension-pinned", "Show on the toolbar", new BMessage(pinnedToggled));
    fPinned->SetToolTip("Without a toolbar button, the extension is in the menu of the toolbar's extensions button.");
    fPrivate = new BCheckBox("extension-private", "Allow in private windows", new BMessage(privateToggled));
    fPrivate->SetToolTip("The extension can then see and change the pages of private windows, "
        "and knows what you do there. Its own data is the same as in other windows.");
    fStatus = new BStringView("extension-status", "");
    fStatus->SetTruncation(B_TRUNCATE_END);
    fFiles = new BCheckBox("extension-files", "Allow access to local files", new BMessage());
    fInstall = new BButton("extension-install", "Install", new BMessage(kExtensionApprove));
    fCancel = new BButton("extension-cancel", "Cancel installation", new BMessage(kExtensionCancel));
    fApproval = new BGroupView(B_VERTICAL, 8);
    BLayoutBuilder::Group<>(fApproval)
        .Add(fFiles)
        .AddGroup(B_HORIZONTAL, 8).AddGlue().Add(fCancel).Add(fInstall).End();
    BLayoutBuilder::Group<>(this, B_VERTICAL, 12)
        .SetInsets(16)
        .AddGroup(B_HORIZONTAL).Add(title).AddGlue().Add(fAdd).End()
        .AddGroup(B_HORIZONTAL, 8)
            .Add(storeButton("Chrome Web Store", kChromeExtensionStore))
            .Add(storeButton("Firefox Add-ons", kFirefoxExtensionStore)).AddGlue().End()
        .AddGroup(B_HORIZONTAL, 8).Add(fStoreURL).Add(fStoreInstall).End()
        .AddGroup(B_HORIZONTAL, 16)
            .Add(list)
            .AddGroup(B_VERTICAL, 8)
                .Add(fHeading).Add(details)
                .AddGroup(B_HORIZONTAL, 8).Add(fToggle).Add(fRemove).AddGlue().End()
                .Add(fPinned)
                .Add(fPrivate)
                .Add(fApproval)
            .End()
        .End()
        .Add(fStatus);
    fApproval->Hide();
    Render();
}
ExtensionManager::~ExtensionManager()
{
    fPanel.reset();
    while (auto* item = fList->RemoveItem(int32(0))) delete item;
    --*fWindows;
}
void ExtensionManager::Send(BMessage& message)
{
    message.AddMessenger("window", BMessenger(this));
    if (fOwner.SendMessage(&message, static_cast<BHandler*>(nullptr), 0) != B_OK)
        fStatus->SetText("Could not send the extension action. Please try again.");
}
bool ExtensionManager::QuitRequested()
{
    BMessage closed(kExtensionManagerClosed);
    closed.AddUInt64("generation", fGeneration);
    Send(closed);
    return true;
}
void ExtensionManager::Apply(const BMessage& state)
{
    auto* selected = dynamic_cast<Item*>(fList->ItemAt(fList->CurrentSelection()));
    const auto identifier = selected ? selected->identifier : std::string();
    while (auto* item = fList->RemoveItem(int32(0))) delete item;
    BMessage entry;
    int32 selection = -1;
    std::map<std::string, std::shared_ptr<BBitmap>> icons;
    for (int32 i = 0; state.FindMessage("entry", i, &entry) == B_OK; ++i) {
        auto* item = new Item(entry);
        // Icons are read from the package once (until it is replaced).
        const auto key = item->identifier + '\n' + item->package;
        auto known = fIcons.find(key);
        item->icon = known != fIcons.end() ? known->second : LoadIcon(item->package);
        icons[key] = item->icon;
        fList->AddItem(item);
        if (item->identifier == identifier) selection = i;
    }
    fIcons = std::move(icons);
    // Manage Extension on a store page asked for this one.
    bool chosen = false;
    if (!fSelect.empty())
        for (int32 i = 0; i < fList->CountItems(); ++i)
            if (static_cast<Item*>(fList->ItemAt(i))->identifier == fSelect) {
                selection = i;
                chosen = true;
                fSelect.clear();
            }
    if (selection < 0 && fList->CountItems()) selection = 0;
    if (selection >= 0) fList->Select(selection);
    if (chosen) {
        fList->ScrollToSelection();
        fList->MakeFocus();
    }
    state.FindBool("ready", &fReady);
    state.FindBool("import_busy", &fBusy);
    state.FindBool("approval_ready", &fApprovalReady);
    uint64 generation = 0;
    state.FindUInt64("generation", &generation);
    if (generation != fGeneration) fFiles->SetValue(B_CONTROL_OFF);
    fGeneration = generation;
    fDraftName = field(state, "draft_name");
    fDraftBody = field(state, "draft_body");
    fStatusText = field(state, "import_status");
    if (!field(state, "catalog_error").empty()) fStatusText = field(state, "catalog_error");
    Render();
}
void ExtensionManager::Render()
{
    auto* item = dynamic_cast<Item*>(fList->ItemAt(fList->CurrentSelection()));
    fAdd->SetEnabled(fReady && !fBusy);
    fStoreURL->SetEnabled(fReady && !fBusy);
    fStoreInstall->SetEnabled(fReady && !fBusy);
    fToggle->SetEnabled(fReady && !fBusy && item && item->installed);
    fRemove->SetEnabled(fReady && !fBusy && item);
    fToggle->SetLabel(item && (item->loaded || item->enabled) ? "Disable" : "Enable");
    fPinned->SetEnabled(fReady && !fBusy && item);
    fPinned->SetValue(item && item->pinned ? B_CONTROL_ON : B_CONTROL_OFF);
    fPrivate->SetEnabled(fReady && !fBusy && item && item->installed);
    fPrivate->SetValue(item && item->privateBrowsing ? B_CONTROL_ON : B_CONTROL_OFF);
    fInstall->SetEnabled(fApprovalReady);
    fFiles->SetEnabled(fApprovalReady);
    fCancel->SetEnabled(fBusy);
    if (fBusy && fApproval->IsHidden()) fApproval->Show();
    else if (!fBusy && !fApproval->IsHidden()) fApproval->Hide();
    if (fBusy) {
        fHeading->SetText(fDraftName.empty() ? "Add extension" : ("Install " + fDraftName + "?").c_str());
        fDetails->SetText(fDraftBody.empty() ? fStatusText.c_str() : fDraftBody.c_str());
    } else if (item) {
        fHeading->SetText(ExtensionDisplayText(item->name).c_str());
        std::string body = "Version " + ExtensionDisplayText(item->version) + "\n"
            + (item->loaded ? "Running" : item->enabled ? "Enabled, but not running" : "Disabled")
            + (item->privateBrowsing ? ", also in private windows" : "")
            + "\nIdentifier: " + ExtensionDisplayText(item->identifier);
        if (!item->installed) body += "\nThis temporary runtime could not be saved as an installation.";
        if (!item->error.empty()) body += "\n\n" + ExtensionDisplayText(item->error);
        if (!item->notice.empty()) body += "\n\n" + ExtensionDisplayText(item->notice);
        body += "\n\nRemoving stops this extension and removes it from startup. Its package and saved data stay in this profile.";
        fDetails->SetText(body.c_str());
    } else {
        fHeading->SetText("No extensions installed");
        fDetails->SetText("Add a CRX3, ZIP or XPI package, or an unpacked extension folder. You can review its requested access before installing it.");
    }
    fStatus->SetText(ExtensionDisplayText(fStatusText).c_str());
    fStatus->SetToolTip(fStatusText.c_str());
}
void ExtensionManager::MessageReceived(BMessage* message)
{
    if (message->what == kExtensionsState) { Apply(*message); return; }
    if (message->what == kShowExtensions) {
        Activate();
        if (const char* identifier = message->GetString("extension_identifier", nullptr); identifier && *identifier) {
            fSelect = identifier;
            for (int32 i = 0; i < fList->CountItems(); ++i)
                if (static_cast<Item*>(fList->ItemAt(i))->identifier == fSelect) {
                    fList->Select(i);
                    fList->ScrollToSelection();
                    fList->MakeFocus();
                    fSelect.clear();
                    Render();
                    break;
                }
        }
        return;
    }
    if (message->what == kCloseExtensionManager) { Quit(); return; }
    if (message->what == selectEntry) { Render(); return; }
    if (message->what == kBrowseExtensionStore) { BMessage request(*message); Send(request); return; }
    if (message->what == kExtensionStoreInstall) {
        if (fReady && !fBusy) {
            BMessage request(kExtensionStoreInstall);
            request.AddString("url", fStoreURL->Text());
            Send(request);
        }
        return;
    }
    if (message->what == choosePackage) {
        if (!fReady || fBusy) return;
        if (!fPanel) {
            BMessenger target(this);
            BMessage selected(kExtensionSelected);
            fPanel = std::make_unique<BFilePanel>(B_OPEN_PANEL, &target, nullptr,
                B_FILE_NODE | B_DIRECTORY_NODE, false, &selected);
            fPanel->SetButtonLabel(B_DEFAULT_BUTTON, "Review extension");
            fPanel->Window()->SetTitle("Choose extension package — Summit");
        }
        fPanel->Show();
        return;
    }
    if (message->what == kExtensionSelected) {
        if (fReady && !fBusy) {
            BMessage request(*message);
            Send(request);
        }
        return;
    }
    if (message->what == kExtensionApprove || message->what == kExtensionCancel) {
        BMessage request(message->what);
        request.AddUInt64("generation", fGeneration);
        request.AddBool("allow_files", fFiles->Value() == B_CONTROL_ON);
        Send(request);
        return;
    }
    if (message->what == pinnedToggled || message->what == privateToggled) {
        auto* item = dynamic_cast<Item*>(fList->ItemAt(fList->CurrentSelection()));
        if (!item || !fReady || fBusy) { Render(); return; }
        const bool pinned = message->what == pinnedToggled;
        BMessage request(pinned ? kExtensionSetPinned : kExtensionSetPrivate);
        request.AddString("extension_identifier", item->identifier.c_str());
        request.AddBool(pinned ? "pinned" : "allowed", (pinned ? fPinned : fPrivate)->Value() == B_CONTROL_ON);
        Send(request);
        return;
    }
    if (message->what == kExtensionEnable || message->what == kExtensionRemove) {
        auto* item = dynamic_cast<Item*>(fList->ItemAt(fList->CurrentSelection()));
        if (!item || !fReady || fBusy) return;
        BMessage request(message->what);
        request.AddString("extension_identifier", item->identifier.c_str());
        request.AddBool("enabled", !(item->loaded || item->enabled));
        Send(request);
        return;
    }
    BWindow::MessageReceived(message);
}
}
#endif
