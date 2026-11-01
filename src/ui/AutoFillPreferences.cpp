#include "AutoFillPreferences.h"
#include "AutoFillStore.h"
#include "Messages.h"
#include "SharedProfile.h"
#include <Alert.h>
#include <Button.h>
#include <CheckBox.h>
#include <interface/ColumnListView.h>
#include <interface/ColumnTypes.h>
#include <LayoutBuilder.h>
#include <StringView.h>
#include <GridView.h>
#include <TextControl.h>
#include <Window.h>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <map>
#include <thread>
#include <utility>

namespace summit {
namespace {
constexpr uint32 enabledToggled = 'afen', addressSelected = 'afas', addAddress = 'afaa', editAddress = 'afea',
    removeAddress = 'afra', cardSelected = 'afcs', addCard = 'afac', editCard = 'afec', removeCard = 'afrc',
    cardsRead = 'afrd', cardsChanged = 'afcg', editorSaved = 'afes', formHistoryToggled = 'afht',
    clearFormHistory = 'afhc', clearFormHistoryConfirmed = 'afhy';

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

// A small window with a text field per value. Save sends editorSaved to the
// target with "kind", "id" and the values by key; a card number that is not
// one, or a month or year out of range, is said in the window instead.
class AutoFillEditor final : public BWindow {
public:
    AutoFillEditor(BWindow* owner, BMessenger target, const std::string& kind, const std::string& id,
        const std::vector<std::pair<std::string, std::string>>& fields, const std::map<std::string, std::string>& values,
        const std::string& note)
        : BWindow(BRect(0, 0, 420, 200), kind == "card" ? (id.empty() ? "Add Card" : "Edit Card")
              : (id.empty() ? "Add Address" : "Edit Address"), B_TITLED_WINDOW_LOOK, B_MODAL_SUBSET_WINDOW_FEEL,
              B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE),
          fTarget(target), fKind(kind), fID(id)
    {
        auto* grid = new BGridView(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING);
        int32 row = 0;
        for (const auto& [key, label] : fields) {
            auto* control = new BTextControl(key.c_str(), label.c_str(), "", nullptr);
            if (auto found = values.find(key); found != values.end()) control->SetText(found->second.c_str());
            control->TextView()->SetExplicitMinSize(BSize(260, B_SIZE_UNSET));
            BLayoutBuilder::Grid<>(grid).AddTextControl(control, 0, row++);
            fControls.emplace_back(key, control);
        }
        fProblem = new BStringView("problem", note.c_str());
        fProblem->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
        auto* save = new BButton("save", "Save", new BMessage('save'));
        BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
            .SetInsets(B_USE_WINDOW_SPACING)
            .Add(grid)
            .Add(fProblem)
            .AddGroup(B_HORIZONTAL)
                .AddGlue()
                .Add(new BButton("cancel", "Cancel", new BMessage(B_QUIT_REQUESTED)))
                .Add(save)
            .End();
        save->MakeDefault(true);
        if (!fControls.empty()) fControls.front().second->MakeFocus(true);
        if (owner) {
            AddToSubset(owner);
            CenterIn(owner->Frame());
        }
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what != 'save') {
            BWindow::MessageReceived(message);
            return;
        }
        BMessage saved(editorSaved);
        saved.AddString("kind", fKind.c_str());
        saved.AddString("id", fID.c_str());
        std::map<std::string, std::string> values;
        for (const auto& [key, control] : fControls) values[key] = control->Text();
        if (fKind == "card") {
            const std::string number = values["number"];
            if ((!number.empty() || fID.empty()) && CardNumberDigits(number).empty()) {
                fProblem->SetText("That is not a card number.");
                return;
            }
            const int month = std::atoi(values["month"].c_str());
            int year = std::atoi(values["year"].c_str());
            if (year > 0 && year < 100) year += 2000;
            if ((!values["month"].empty() && (month < 1 || month > 12)) || (!values["year"].empty() && (year < 2000 || year > 2100))) {
                fProblem->SetText("The expiry month is 1-12, and the year as on the card (28 or 2028).");
                return;
            }
            values["number"] = CardNumberDigits(number);
            values["month"] = month ? std::to_string(month) : "";
            values["year"] = year ? std::to_string(year) : "";
        }
        for (const auto& [key, value] : values) saved.AddString(key.c_str(), value.c_str());
        fTarget.SendMessage(&saved);
        Quit();
    }

private:
    BMessenger fTarget;
    std::string fKind, fID;
    std::vector<std::pair<std::string, BTextControl*>> fControls;
    BStringView* fProblem;
};

BColumnListView* List(const char* name, std::initializer_list<std::pair<const char*, float>> columns, uint32 selection)
{
    auto* list = new BColumnListView(name, B_NAVIGABLE, B_FANCY_BORDER, true);
    int32 index = 0;
    for (const auto& [title, width] : columns)
        list->AddColumn(new BStringColumn(title, width, 60, 600, B_TRUNCATE_END), index++);
    list->SetSelectionMessage(new BMessage(selection));
    list->SetSelectionMode(B_SINGLE_SELECTION_LIST);
    list->SetSortingEnabled(false);
    list->SetExplicitMinSize(BSize(470, 90));
    return list;
}
}

AutoFillPreferences::AutoFillPreferences(std::shared_ptr<SharedProfile> profile)
    : BGroupView("AutoFill", B_VERTICAL, 8), fProfile(std::move(profile))
{
    fEnabled = new BCheckBox("autofill-enabled", "Fill in forms with saved addresses and cards, and offer to save new ones",
        new BMessage(enabledToggled));
    fEnabled->SetValue(fProfile && fProfile->Read([](const Profile& p) { return p.autoFill; }) ? B_CONTROL_ON : B_CONTROL_OFF);
    fAddresses = List("autofill-addresses", {{"Name", 160}, {"Address", 220}, {"E-mail and phone", 180}}, addressSelected);
    fCards = List("autofill-cards", {{"Card", 180}, {"Name on card", 180}, {"Expires", 80}}, cardSelected);
    fEditAddress = new BButton("edit-address", "Edit…", new BMessage(editAddress));
    fRemoveAddress = new BButton("remove-address", "Remove", new BMessage(removeAddress));
    fEditCard = new BButton("edit-card", "Edit…", new BMessage(editCard));
    fRemoveCard = new BButton("remove-card", "Remove", new BMessage(removeCard));
    fCardStatus = Hint("cards-status", "");
    fFormHistory = new BCheckBox("form-history-enabled", "Remember what I type in forms", new BMessage(formHistoryToggled));
    fFormHistory->SetValue(fProfile && fProfile->Read([](const Profile& p) { return p.formHistory; }) ? B_CONTROL_ON : B_CONTROL_OFF);
    fClearFormHistory = new BButton("clear-form-history", "Clear Form History…", new BMessage(clearFormHistory));
    fFormHistoryStatus = Hint("form-history-status", "");
    BLayoutBuilder::Group<>(this)
        .SetInsets(18)
        .Add(Title("autofill-title", "AutoFill"))
        .AddGroup(B_VERTICAL, 6).SetInsets(12, 0, 0, 0)
            .Add(fEnabled)
            .Add(Hint("autofill-hint", "Tabs opened from now on follow this setting. Private windows fill in forms but never save."))
        .End()
        .AddStrut(6)
        .Add(Title("addresses-title", "Contact information and addresses"))
        .AddGroup(B_VERTICAL, 6).SetInsets(12, 0, 0, 0)
            .Add(Hint("addresses-hint", "Kept in Summit's settings."))
            .Add(fAddresses)
            .AddGroup(B_HORIZONTAL, 8)
                .Add(new BButton("add-address", "Add…", new BMessage(addAddress)))
                .AddGlue()
                .Add(fEditAddress)
                .Add(fRemoveAddress)
            .End()
        .End()
        .AddStrut(6)
        .Add(Title("cards-title", "Payment cards"))
        .AddGroup(B_VERTICAL, 6).SetInsets(12, 0, 0, 0)
            .Add(Hint("cards-hint", "Kept in the Haiku key store (keyring “Summit cards”). Security codes are never saved."))
            .Add(fCards)
            .AddGroup(B_HORIZONTAL, 8)
                .Add(new BButton("add-card", "Add…", new BMessage(addCard)))
                .Add(fCardStatus)
                .AddGlue()
                .Add(fEditCard)
                .Add(fRemoveCard)
            .End()
        .End()
        .AddStrut(6)
        .Add(Title("form-history-title", "Form history"))
        .AddGroup(B_VERTICAL, 6).SetInsets(12, 0, 0, 0)
            .Add(fFormHistory)
            .Add(Hint("form-history-hint", "Search boxes, names and other text fields of forms you send are offered again "
                "under fields of the same name."))
            .Add(Hint("form-history-hint2", "Never passwords, card numbers, fields that ask not to be, or anything in "
                "private windows. Shift+Delete removes a suggestion."))
            .AddGroup(B_HORIZONTAL, 8)
                .Add(fClearFormHistory)
                .Add(fFormHistoryStatus)
                .AddGlue()
            .End()
        .End()
        .AddGlue();
}

void AutoFillPreferences::AttachedToWindow()
{
    BGroupView::AttachedToWindow();
    // The controls talk to this tab, not to the window.
    for (const char* name : {"autofill-enabled", "edit-address", "remove-address", "add-address", "edit-card", "remove-card", "add-card",
             "form-history-enabled", "clear-form-history"})
        if (auto* control = dynamic_cast<BControl*>(FindView(name))) control->SetTarget(this);
    fAddresses->SetTarget(this);
    fCards->SetTarget(this);
    // Addresses saved from a browser window while this shows appear here.
    if (fProfile) fProfile->AddListener(BMessenger(this));
    ShowAddresses();
    ShowCards();
    ShowFormHistoryCount();
}

void AutoFillPreferences::ShowFormHistoryCount()
{
    // Read on first use: a file of at most a few hundred kilobytes.
    const size_t count = fProfile ? fProfile->Forms().Count() : 0;
    fFormHistoryStatus->SetText(count == 0 ? "Nothing remembered." : count == 1 ? "1 value remembered."
        : (std::to_string(count) + " values remembered.").c_str());
    fClearFormHistory->SetEnabled(count > 0);
}

void AutoFillPreferences::WindowActivated(bool active)
{
    BGroupView::WindowActivated(active);
    if (active) ShowFormHistoryCount();
}

void AutoFillPreferences::DetachedFromWindow()
{
    if (fProfile) fProfile->RemoveListener(BMessenger(this));
    BGroupView::DetachedFromWindow();
}

void AutoFillPreferences::ShowAddresses()
{
    fAddresses->Clear();
    fAddressIDs.clear();
    if (fProfile) fProfile->Read([&](const Profile& profile) {
        for (const auto& address : profile.autoFillAddresses) {
            std::string contact = address.email;
            if (!address.tel.empty()) contact += (contact.empty() ? "" : ", ") + address.tel;
            auto* row = new BRow();
            row->SetField(new BStringField(AddressName(address).c_str()), 0);
            row->SetField(new BStringField(AddressSummary(address).c_str()), 1);
            row->SetField(new BStringField(contact.c_str()), 2);
            fAddresses->AddRow(row);
            fAddressIDs.push_back(address.id);
        }
        return 0;
    });
    fEditAddress->SetEnabled(false);
    fRemoveAddress->SetEnabled(false);
}

void AutoFillPreferences::ShowCards()
{
    // The key store may ask the user first: read it on another thread. Only
    // what the list shows comes back, never the numbers.
    std::thread([target = BMessenger(this)] {
        BMessage cards(cardsRead);
        for (const auto& card : CardStore::Cards()) {
            cards.AddString("id", card.id.c_str());
            cards.AddString("masked", MaskedCard(card).c_str());
            cards.AddString("name", card.name.c_str());
            cards.AddInt32("month", card.month);
            cards.AddInt32("year", card.year);
        }
        target.SendMessage(&cards);
    }).detach();
}

void AutoFillPreferences::EditAddress(const std::string& id)
{
    std::map<std::string, std::string> values;
    if (!id.empty() && fProfile) fProfile->Read([&](const Profile& profile) {
        for (const auto& address : profile.autoFillAddresses) {
            if (address.id != id) continue;
            for (const auto& [name, field] : AutoFillAddressFields()) values[name] = address.*field;
            const auto end = address.street.find('\n');
            values["street"] = address.street.substr(0, end);
            if (end != std::string::npos) values["street2"] = address.street.substr(end + 1);
            std::replace(values["street2"].begin(), values["street2"].end(), '\n', ',');
        }
        return 0;
    });
    (new AutoFillEditor(Window(), BMessenger(this), "address", id,
        {{"givenName", "First name:"}, {"familyName", "Last name:"}, {"organization", "Organization:"},
            {"street", "Street:"}, {"street2", "Street (line 2):"}, {"city", "City:"}, {"region", "State or region:"},
            {"postalCode", "Postal code:"}, {"country", "Country:"}, {"email", "E-mail:"}, {"tel", "Phone:"}},
        values, ""))->Show();
}

void AutoFillPreferences::EditCard(const std::string& id)
{
    std::map<std::string, std::string> values;
    std::string note = "The security code (CVC) is never saved.";
    for (const auto& card : fCardRows) {
        if (card.id != id) continue;
        values["name"] = card.name;
        if (card.month) values["month"] = std::to_string(card.month);
        if (card.year) values["year"] = std::to_string(card.year);
        note = "Leave the number empty to keep " + card.masked + ".";
    }
    (new AutoFillEditor(Window(), BMessenger(this), "card", id,
        {{"name", "Name on card:"}, {"number", "Card number:"}, {"month", "Expiry month:"}, {"year", "Expiry year:"}},
        values, note))->Show();
}

void AutoFillPreferences::MessageReceived(BMessage* message)
{
    auto selectedAddress = [&]() -> std::string {
        auto* row = fAddresses->CurrentSelection();
        const int32 index = row ? fAddresses->IndexOf(row) : -1;
        return index >= 0 && index < static_cast<int32>(fAddressIDs.size()) ? fAddressIDs[index] : std::string();
    };
    auto selectedCard = [&]() -> std::string {
        auto* row = fCards->CurrentSelection();
        const int32 index = row ? fCards->IndexOf(row) : -1;
        return index >= 0 && index < static_cast<int32>(fCardRows.size()) ? fCardRows[index].id : std::string();
    };
    switch (message->what) {
    case enabledToggled: {
        const bool enabled = fEnabled->Value() == B_CONTROL_ON;
        if (fProfile) fProfile->Change([&](Profile& profile) -> uint32 {
            if (profile.autoFill == enabled) return 0;
            profile.autoFill = enabled;
            return SharedProfile::kSettingsChanged;
        });
        return;
    }
    case formHistoryToggled: {
        const bool enabled = fFormHistory->Value() == B_CONTROL_ON;
        if (fProfile) fProfile->Change([&](Profile& profile) -> uint32 {
            if (profile.formHistory == enabled) return 0;
            profile.formHistory = enabled;
            return SharedProfile::kSettingsChanged;
        });
        return;
    }
    case clearFormHistory: {
        auto* alert = new BAlert("Clear Form History", "Clear your form history?\n\nWhat you typed in search boxes and "
            "other fields of forms is forgotten. Saved addresses and cards are kept.", "Cancel", "Clear", nullptr,
            B_WIDTH_AS_USUAL, B_WARNING_ALERT);
        alert->SetShortcut(0, B_ESCAPE);
        alert->Go(new BInvoker(new BMessage(clearFormHistoryConfirmed), this));
        return;
    }
    case clearFormHistoryConfirmed:
        if (message->GetInt32("which", 0) != 1 || !fProfile) return;
        fProfile->Forms().Clear(0);
        ShowFormHistoryCount();
        fFormHistoryStatus->SetText("Form history cleared.");
        return;
    case kProfileChanged:
        if (message->GetUInt32("changes", 0) & SharedProfile::kSettingsChanged) ShowAddresses();
        // History cleared elsewhere (Clear History… clears the form history).
        if (message->GetUInt32("changes", 0) & SharedProfile::kHistoryCleared) ShowFormHistoryCount();
        return;
    case addressSelected:
        fEditAddress->SetEnabled(!selectedAddress().empty());
        fRemoveAddress->SetEnabled(!selectedAddress().empty());
        return;
    case addAddress: EditAddress({}); return;
    case editAddress:
        if (const auto id = selectedAddress(); !id.empty()) EditAddress(id);
        return;
    case removeAddress: {
        const auto id = selectedAddress();
        if (id.empty() || !fProfile) return;
        fProfile->Change([&](Profile& profile) -> uint32 {
            return std::erase_if(profile.autoFillAddresses, [&](const AutoFillAddress& address) { return address.id == id; })
                ? SharedProfile::kSettingsChanged : 0;
        });
        ShowAddresses();
        return;
    }
    case cardSelected:
        fEditCard->SetEnabled(!selectedCard().empty());
        fRemoveCard->SetEnabled(!selectedCard().empty());
        return;
    case addCard: EditCard({}); return;
    case editCard:
        if (const auto id = selectedCard(); !id.empty()) EditCard(id);
        return;
    case removeCard: {
        const auto id = selectedCard();
        if (id.empty()) return;
        std::thread([target = BMessenger(this), id] {
            BMessage changed(cardsChanged);
            if (!CardStore::Remove(id)) changed.AddString("error", "The card could not be removed.");
            target.SendMessage(&changed);
        }).detach();
        return;
    }
    case cardsRead: {
        fCards->Clear();
        fCardRows.clear();
        const char* id = nullptr;
        for (int32 i = 0; message->FindString("id", i, &id) == B_OK; ++i) {
            CardRow card { id, message->GetString("masked", i, ""), message->GetString("name", i, ""),
                static_cast<int>(message->GetInt32("month", i, 0)), static_cast<int>(message->GetInt32("year", i, 0)) };
            AutoFillCard shown { card.id, card.name, {}, card.month, card.year };
            auto* row = new BRow();
            row->SetField(new BStringField(card.masked.c_str()), 0);
            row->SetField(new BStringField(card.name.c_str()), 1);
            row->SetField(new BStringField(CardExpiry(shown).c_str()), 2);
            fCards->AddRow(row);
            fCardRows.push_back(std::move(card));
        }
        fEditCard->SetEnabled(false);
        fRemoveCard->SetEnabled(false);
        return;
    }
    case cardsChanged:
        fCardStatus->SetText(message->GetString("error", ""));
        ShowCards();
        return;
    case editorSaved: {
        const std::string kind = message->GetString("kind", ""), id = message->GetString("id", "");
        if (kind == "address" && fProfile) {
            AutoFillAddress address;
            for (const auto& [name, field] : AutoFillAddressFields()) address.*field = message->GetString(name, "");
            if (const std::string second = message->GetString("street2", ""); !second.empty())
                address.street += (address.street.empty() ? "" : "\n") + second;
            address.id = id.empty() ? NewAutoFillID() : id;
            address.added = std::time(nullptr);
            fProfile->Change([&](Profile& profile) -> uint32 {
                auto found = std::find_if(profile.autoFillAddresses.begin(), profile.autoFillAddresses.end(),
                    [&](const AutoFillAddress& saved) { return saved.id == address.id; });
                if (found != profile.autoFillAddresses.end()) {
                    address.added = found->added;
                    *found = address;
                } else if (profile.autoFillAddresses.size() < 1000) profile.autoFillAddresses.push_back(address);
                else return 0;
                return SharedProfile::kSettingsChanged;
            });
            ShowAddresses();
        } else if (kind == "card") {
            AutoFillCard card { id, message->GetString("name", ""), message->GetString("number", ""),
                std::atoi(message->GetString("month", "0")), std::atoi(message->GetString("year", "0")) };
            std::thread([target = BMessenger(this), card]() mutable {
                // An unchanged number is the saved one.
                if (card.number.empty())
                    for (const auto& saved : CardStore::Cards()) if (saved.id == card.id) card.number = saved.number;
                BMessage changed(cardsChanged);
                if (const auto error = CardStore::Save(card); !error.empty()) changed.AddString("error", error.c_str());
                target.SendMessage(&changed);
            }).detach();
        }
        return;
    }
    }
    BGroupView::MessageReceived(message);
}
}
