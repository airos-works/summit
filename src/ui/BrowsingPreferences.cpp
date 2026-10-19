#include "BrowsingPreferences.h"
#include "Messages.h"
#include "FeatureMessages.h"
#include "PasswordStore.h"
#include <WebKit/WebKitView.h>
#include <Alert.h>
#include <Button.h>
#include <CheckBox.h>
#include <interface/ColumnListView.h>
#include <interface/ColumnTypes.h>
#include <File.h>
#include <FilePanel.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <NodeInfo.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <StringView.h>
#include <Window.h>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>

namespace summit {
namespace {
constexpr uint32 autoplayChosen = 'bpau', spellingToggled = 'bpsp', passwordsToggled = 'bppw',
    accountSelected = 'bpas', showPassword = 'bpsh', removeAccount = 'bprm', importPasswords = 'bpim',
    exportPasswords = 'bpex', importChosen = 'bpic', exportChosen = 'bpec', accountsRead = 'bpar',
    appSelected = 'bpap', removeApp = 'bpra';
BStringView* Title(const char* name, const char* text)
{
    auto* title = new BStringView(name, text);
    title->SetFont(be_bold_font);
    return title;
}
BStringView* Hint(const char* name, const char* text)
{
    auto* hint = new BStringView(name, text);
    hint->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    return hint;
}
}

BrowsingPreferences::BrowsingPreferences(BMessenger owner, const std::string& autoplay, bool spellChecking, bool savePasswords,
    const std::vector<std::tuple<std::string, std::string, std::string>>& webApps)
    : BGroupView("Browsing", B_VERTICAL, 8), fOwner(owner)
{
    auto* choices = new BPopUpMenu("autoplay");
    for (const auto& [value, label] : std::initializer_list<std::pair<const char*, const char*>> {
            {"allow", "Allow audio and video"}, {"block-audio", "Block audio (video without sound plays)"},
            {"block", "Block audio and video"}}) {
        auto* message = new BMessage(autoplayChosen);
        message->AddString("autoplay", value);
        auto* item = new BMenuItem(label, message);
        item->SetMarked(autoplay == value);
        choices->AddItem(item);
    }
    fAutoplay = new BMenuField("autoplay", "Media that start by themselves:", choices);
    fSpelling = new BCheckBox("spell-checking", "Check spelling while typing", new BMessage(spellingToggled));
    fSpelling->SetValue(spellChecking ? B_CONTROL_ON : B_CONTROL_OFF);
    fSpellingInfo = Hint("spelling-info", "");
    fSavePasswords = new BCheckBox("save-passwords", "Offer to save passwords when you sign in", new BMessage(passwordsToggled));
    fSavePasswords->SetValue(savePasswords ? B_CONTROL_ON : B_CONTROL_OFF);
    fAccounts = new BColumnListView("saved-passwords", B_NAVIGABLE, B_FANCY_BORDER, true);
    fAccounts->AddColumn(new BStringColumn("Site", 240, 80, 600, B_TRUNCATE_MIDDLE), 0);
    fAccounts->AddColumn(new BStringColumn("Account", 200, 60, 500, B_TRUNCATE_END), 1);
    fAccounts->SetSelectionMessage(new BMessage(accountSelected));
    fAccounts->SetSelectionMode(B_SINGLE_SELECTION_LIST);
    fAccounts->SetExplicitMinSize(BSize(470, 100));
    fApps = new BColumnListView("web-apps", B_NAVIGABLE, B_FANCY_BORDER, true);
    fApps->AddColumn(new BStringColumn("App", 200, 80, 400, B_TRUNCATE_END), 0);
    fApps->AddColumn(new BStringColumn("Opens", 260, 80, 600, B_TRUNCATE_MIDDLE), 1);
    fApps->SetSelectionMessage(new BMessage(appSelected));
    fApps->SetSelectionMode(B_SINGLE_SELECTION_LIST);
    fApps->SetSortingEnabled(false);
    fApps->SetExplicitMinSize(BSize(470, 60));
    for (const auto& [id, name, start] : webApps) {
        auto* row = new BRow();
        row->SetField(new BStringField(name.c_str()), 0);
        row->SetField(new BStringField(start.c_str()), 1);
        fApps->AddRow(row);
        fAppIDs.push_back(id);
    }
    fRemoveApp = new BButton("remove-web-app", "Uninstall", new BMessage(removeApp));
    fRemoveApp->SetEnabled(false);
    fShow = new BButton("show-password", "Show Password…", new BMessage(showPassword));
    fRemove = new BButton("remove-password", "Remove", new BMessage(removeAccount));
    BLayoutBuilder::Group<>(this)
        .SetInsets(18)
        .Add(Title("autoplay-title", "Autoplay"))
        .AddGroup(B_VERTICAL, 6).SetInsets(12, 0, 0, 0)
            .AddGroup(B_HORIZONTAL, 0).Add(fAutoplay).AddGlue().End()
            .Add(Hint("autoplay-hint", "Sites can be given their own choice in Site Permissions."))
        .End()
        .AddStrut(6)
        .Add(Title("spelling-title", "Spelling"))
        .AddGroup(B_VERTICAL, 6).SetInsets(12, 0, 0, 0)
            .Add(fSpelling)
            .Add(fSpellingInfo)
        .End()
        .AddStrut(6)
        .Add(Title("passwords-title", "Passwords"))
        .AddGroup(B_VERTICAL, 6).SetInsets(12, 0, 0, 0)
            .Add(fSavePasswords)
            .Add(Hint("passwords-hint", "Saved passwords are kept in the Haiku key store, not in Summit's settings."))
            .Add(fAccounts)
            .AddGroup(B_HORIZONTAL, 8)
                .Add(new BButton("import-passwords", "Import…", new BMessage(importPasswords)))
                .Add(new BButton("export-passwords", "Export…", new BMessage(exportPasswords)))
                .AddGlue()
                .Add(fShow)
                .Add(fRemove)
            .End()
        .End()
        .AddStrut(6)
        .Add(Title("apps-title", "Web Apps"))
        .AddGroup(B_VERTICAL, 6).SetInsets(12, 0, 0, 0)
            .Add(Hint("apps-hint", "Sites installed with File › Install as App…. They are in the Deskbar's Applications menu."))
            .Add(fApps)
            .AddGroup(B_HORIZONTAL, 8).AddGlue().Add(fRemoveApp).End()
        .End();
}

BrowsingPreferences::~BrowsingPreferences() = default;

void BrowsingPreferences::AttachedToWindow()
{
    BGroupView::AttachedToWindow();
    // The controls talk to this tab, not to the window.
    for (BView* view : { static_cast<BView*>(fSpelling), static_cast<BView*>(fSavePasswords),
            static_cast<BView*>(fShow), static_cast<BView*>(fRemove) })
        if (auto* control = dynamic_cast<BControl*>(view)) control->SetTarget(this);
    for (const char* name : { "import-passwords", "export-passwords" })
        if (auto* button = dynamic_cast<BButton*>(FindView(name))) button->SetTarget(this);
    fAutoplay->Menu()->SetTargetForItems(this);
    fAccounts->SetTarget(this);
    fApps->SetTarget(this);
    fRemoveApp->SetTarget(this);
    const std::string language = BWebKitSpellCheckingLanguage();
    fSpellingInfo->SetText(language.empty() ? "No dictionary is in use."
        : ("Dictionary: " + language + ". Words you add are kept in your settings folder.").c_str());
    ShowAccounts();
}

void BrowsingPreferences::ShowAccounts()
{
    // The key store may ask the user first: read it on another thread.
    std::thread([target = BMessenger(this)] {
        BMessage accounts(accountsRead);
        for (const auto& account : PasswordStore::Accounts()) {
            accounts.AddString("origin", account.origin.c_str());
            accounts.AddString("username", account.username.c_str());
        }
        target.SendMessage(&accounts);
    }).detach();
}

void BrowsingPreferences::Send(const char* name, bool value)
{
    BMessage changed(kPreferencesChanged);
    changed.AddBool(name, value);
    fOwner.SendMessage(&changed);
}

void BrowsingPreferences::MessageReceived(BMessage* message)
{
    auto selected = [&]() -> std::optional<SavedAccount> {
        auto* row = fAccounts->CurrentSelection();
        if (!row) return std::nullopt;
        const std::string site = static_cast<BStringField*>(row->GetField(0))->String();
        const std::string username = static_cast<BStringField*>(row->GetField(1))->String();
        for (const auto& account : PasswordStore::Accounts())
            if (account.username == username && (account.origin == "https://" + site || account.origin == site)) return account;
        return std::nullopt;
    };
    switch (message->what) {
    case autoplayChosen: {
        BMessage changed(kPreferencesChanged);
        changed.AddString("autoplay", message->GetString("autoplay", "block-audio"));
        fOwner.SendMessage(&changed);
        return;
    }
    case accountsRead: {
        fAccounts->Clear();
        const char* origin = nullptr;
        for (int32 i = 0; message->FindString("origin", i, &origin) == B_OK; ++i) {
            const std::string text = origin;
            auto* row = new BRow();
            row->SetField(new BStringField((text.rfind("https://", 0) == 0 ? text.substr(8) : text).c_str()), 0);
            row->SetField(new BStringField(message->GetString("username", i, "")), 1);
            fAccounts->AddRow(row);
        }
        fShow->SetEnabled(false);
        fRemove->SetEnabled(false);
        return;
    }
    case appSelected: fRemoveApp->SetEnabled(fApps->CurrentSelection() != nullptr); return;
    case removeApp: {
        auto* row = fApps->CurrentSelection();
        const int32 index = row ? fApps->IndexOf(row) : -1;
        if (index < 0 || index >= int32(fAppIDs.size())) return;
        BMessage remove(kRemoveWebApp);
        remove.AddString("id", fAppIDs[index].c_str());
        fOwner.SendMessage(&remove);
        fApps->RemoveRow(row);
        delete row;
        fAppIDs.erase(fAppIDs.begin() + index);
        fRemoveApp->SetEnabled(false);
        return;
    }
    case spellingToggled: Send("spell_checking", fSpelling->Value() == B_CONTROL_ON); return;
    case passwordsToggled: Send("save_passwords", fSavePasswords->Value() == B_CONTROL_ON); return;
    case accountSelected: {
        const bool any = fAccounts->CurrentSelection() != nullptr;
        fShow->SetEnabled(any);
        fRemove->SetEnabled(any);
        return;
    }
    case showPassword: {
        if (auto account = selected())
            (new BAlert("Saved password — Summit", ("Site: " + account->origin + "\nAccount: " + account->username
                + "\nPassword: " + account->password).c_str(), "OK"))->Go(nullptr);
        return;
    }
    case removeAccount: {
        if (auto account = selected()) PasswordStore::Remove(account->origin, account->username);
        ShowAccounts();
        return;
    }
    case importPasswords: case exportPasswords: {
        BMessenger target(this);
        const bool import = message->what == importPasswords;
        BMessage chosen(import ? importChosen : exportChosen);
        fPanel = std::make_unique<BFilePanel>(import ? B_OPEN_PANEL : B_SAVE_PANEL, &target, nullptr, 0, false, &chosen);
        if (!import) fPanel->SetSaveText("Summit passwords.csv");
        fPanel->Window()->SetTitle(import ? "Import passwords (CSV) — Summit" : "Export passwords — Summit");
        fPanel->Show();
        return;
    }
    case importChosen: {
        entry_ref ref;
        if (message->FindRef("refs", &ref) != B_OK) return;
        BPath path(&ref);
        std::ifstream file(path.Path());
        std::stringstream text;
        text << file.rdbuf();
        std::string error;
        const size_t imported = PasswordStore::ImportCSV(text.str(), error);
        (new BAlert("Import passwords — Summit", imported ? ("Imported " + std::to_string(imported)
            + (imported == 1 ? " account." : " accounts.")).c_str() : error.c_str(), "OK"))->Go(nullptr);
        ShowAccounts();
        return;
    }
    case exportChosen: {
        entry_ref directory;
        const char* name = nullptr;
        if (message->FindRef("directory", &directory) != B_OK || message->FindString("name", &name) != B_OK) return;
        BPath path(&directory);
        path.Append(name);
        const auto csv = PasswordStore::ExportCSV();
        BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
        if (file.InitCheck() == B_OK && file.Write(csv.data(), csv.size()) == ssize_t(csv.size())) {
            BNodeInfo(&file).SetType("text/csv");
            (new BAlert("Export passwords — Summit", "The file holds your passwords unencrypted. Keep it safe, "
                "and delete it when you no longer need it.", "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT))->Go(nullptr);
        }
        return;
    }
    }
    BGroupView::MessageReceived(message);
}
}
