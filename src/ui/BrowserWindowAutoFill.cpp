#if SUMMIT_MODERN_WEBKIT
// AutoFill for contact info, addresses and payment cards (#149). The page
// script (kAutoFillScript) recognises the fields; the saved entries are
// offered in a native list under the field, which a page cannot read or
// click; the chosen entry goes back to the page that asked, which fills its
// form. A form sent with a new address or card offers to save it, except in
// private windows. Addresses are in the profile, cards in the key store
// (AutoFillStore.h), which is only ever asked on threads of its own. Other
// text fields offer what was typed into fields of their name before (the
// form history, FormHistory.h), which sent forms add to. Typing filters the
// list instead of closing it.
#include "BrowserWindow.h"
#include "FeatureMessages.h"
#include "Messages.h"
#include "NoticeBar.h"
#include "SharedProfile.h"
#include <Application.h>
#include <ListView.h>
#include <Screen.h>
#include <StringView.h>
#include <TextView.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>

namespace summit {
namespace {
using nlohmann::json;
constexpr uint32 kListClicked = 'aflc';
// Scripting, for tests ('ALTB' with "index" as int32), as PasskeyAutofill.
constexpr uint32 kScriptedChoice = 'ALTB';
constexpr const char* kSettingsRow = "AutoFill Settings…";

// Cards are filled only into pages whose connection is private: https, the
// computer itself, or a file. SUMMIT_AUTOFILL_INSECURE_CARDS=1 allows any
// page, for tests over plain http.
bool SecureForCards(const std::string& origin)
{
    static const bool any = [] {
        const char* value = std::getenv("SUMMIT_AUTOFILL_INSECURE_CARDS");
        return value && *value == '1';
    }();
    if (any || origin.starts_with("https://") || origin.starts_with("file://")) return true;
    for (const char* local : {"http://localhost", "http://127.0.0.1", "http://[::1]"})
        if (origin == local || origin.starts_with(std::string(local) + ":")) return true;
    return false;
}

// The host of an origin ("https://example.com:8443": example.com), which the
// form history ranks and forgets values by.
std::string HostOf(const std::string& origin)
{
    auto start = origin.find("://");
    if (start == std::string::npos) return {};
    std::string host = origin.substr(start + 3);
    if (host.starts_with("[")) host = host.substr(0, host.find(']') + 1);
    else host = host.substr(0, host.find_first_of(":/"));
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return host;
}

// For comparing what is typed with an entry: ASCII letters in lower case.
std::string Folded(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string Text(const json& object, const char* key)
{
    auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>().substr(0, 1024) : std::string();
}

AutoFillAddress AddressFromJSON(const json& object)
{
    AutoFillAddress address;
    if (!object.is_object()) return address;
    for (const auto& [name, field] : AutoFillAddressFields()) address.*field = Text(object, name);
    return address;
}

json AddressValues(const AutoFillAddress& address)
{
    json values = json::object();
    for (const auto& [name, field] : AutoFillAddressFields()) values[name] = address.*field;
    return values;
}

// The saved field a form's key fills from.
std::string AddressValueFor(const AutoFillAddress& address, const std::string& key)
{
    if (key == "name") {
        std::string name = address.givenName;
        if (!address.familyName.empty()) name += (name.empty() ? "" : " ") + address.familyName;
        return name;
    }
    if (key == "street" || key == "line1" || key == "line2" || key == "line3") {
        const auto end = address.street.find('\n');
        return key == "line1" || key == "street" ? address.street.substr(0, end) : end == std::string::npos ? std::string() : address.street.substr(end + 1);
    }
    for (const auto& [name, field] : AutoFillAddressFields())
        if (key == name) return address.*field;
    return {};
}

// One entry of the list: what it is, and a line below it.
class AutoFillItem final : public BListItem {
public:
    AutoFillItem(const std::string& label, const std::string& detail) : fLabel(label), fDetail(detail) { }
    void DrawItem(BView* owner, BRect frame, bool) override
    {
        const rgb_color background = ui_color(IsSelected() ? B_LIST_SELECTED_BACKGROUND_COLOR : B_LIST_BACKGROUND_COLOR);
        owner->SetLowColor(background);
        owner->FillRect(frame, B_SOLID_LOW);
        font_height height;
        owner->GetFontHeight(&height);
        const float line = ceilf(height.ascent + height.descent + height.leading);
        const rgb_color text = ui_color(IsSelected() ? B_LIST_SELECTED_ITEM_TEXT_COLOR : B_LIST_ITEM_TEXT_COLOR);
        owner->SetHighColor(text);
        owner->DrawString(fLabel.c_str(), BPoint(frame.left + 8, frame.top + 4 + height.ascent));
        if (fDetail.empty()) return;
        owner->SetHighColor(IsSelected() ? text : tint_color(ui_color(B_LIST_ITEM_TEXT_COLOR), B_LIGHTEN_1_TINT));
        owner->DrawString(fDetail.c_str(), BPoint(frame.left + 8, frame.top + 4 + line + height.ascent));
    }
    void Update(BView* owner, const BFont* font) override
    {
        BListItem::Update(owner, font);
        font_height height;
        font->GetHeight(&height);
        const float line = ceilf(height.ascent + height.descent + height.leading);
        SetHeight(line * (fDetail.empty() ? 1 : 2) + 8);
        SetWidth(std::max(font->StringWidth(fLabel.c_str()), font->StringWidth(fDetail.c_str())) + 16);
    }

private:
    std::string fLabel, fDetail;
};

// Rows are chosen by a click on them, not by a selection message: the
// highlight that the arrow keys move selects rows too.
class AutoFillListView final : public BListView {
public:
    AutoFillListView(BRect frame) : BListView(frame, "entries", B_SINGLE_SELECTION_LIST, B_FOLLOW_ALL) { }
    void MouseDown(BPoint where) override
    {
        BListView::MouseDown(where);
        const int32 index = IndexOf(where);
        if (index < 0 || !Window()) return;
        BMessage clicked(kListClicked);
        clicked.AddInt32("index", index);
        Window()->PostMessage(&clicked);
    }
};

// The list under the field. Like PasskeyAutofill it takes no focus: the
// field keeps it, the window's arrow keys move the highlight here
// (kAutoFillSelect), typing replaces its rows (kAutoFillRows), and a click
// sends kAutoFillChosen.
class AutoFillList final : public BWindow {
public:
    AutoFillList(BRect field, uint64 serial, const BMessage& rows, BMessenger target)
        : BWindow(BRect(field.LeftBottom(), field.LeftBottom() + BPoint(200, 40)), "AutoFill", B_BORDERED_WINDOW_LOOK,
            B_FLOATING_SUBSET_WINDOW_FEEL, B_AVOID_FOCUS | B_NOT_MOVABLE | B_NOT_RESIZABLE | B_NOT_ZOOMABLE
            | B_NOT_MINIMIZABLE | B_NOT_CLOSABLE | B_ASYNCHRONOUS_CONTROLS),
          fField(field), fSerial(serial), fTarget(target)
    {
        fList = new AutoFillListView(Bounds());
        AddChild(fList);
        SetRows(rows);
    }
    void MessageReceived(BMessage* message) override
    {
        switch (message->what) {
        case kAutoFillRows:
            SetRows(*message);
            return;
        case kListClicked:
            // A page can open the list where the user is about to click
            // (it says where its field is): a click that lands as the list
            // appears was meant for the page, not for an entry.
            if (system_time() - fShown < 500000) return;
            Choose(message->GetInt32("index", -1));
            return;
        case kAutoFillSelect: {
            const int32 index = message->GetInt32("index", -1);
            if (index >= 0) {
                fList->Select(index);
                fList->ScrollToSelection();
            } else fList->DeselectAll();
            return;
        }
        case kScriptedChoice:
            Choose(message->GetInt32("index", message->GetInt32("account", 0)));
            return;
        default:
            BWindow::MessageReceived(message);
        }
    }

private:
    void SetRows(const BMessage& rows)
    {
        fList->DeselectAll();
        for (int32 i = fList->CountItems() - 1; i >= 0; --i) delete fList->RemoveItem(i);
        const char* label = nullptr;
        for (int32 i = 0; rows.FindString("label", i, &label) == B_OK; ++i)
            fList->AddItem(new AutoFillItem(label, rows.GetString("detail", i, "")));
        float width = std::max(260.0f, fField.Width()), height = 0;
        for (int32 i = 0; i < fList->CountItems(); ++i) {
            width = std::max(width, fList->ItemAt(i)->Width());
            height += fList->ItemAt(i)->Height() + 1;
        }
        // Below the field, or above it when the screen ends first.
        const BRect screen = BScreen(this).Frame();
        width = std::min(width, screen.Width() / 2);
        height = std::min(height, screen.Height() / 3);
        ResizeTo(width, height);
        float top = fField.bottom + 2;
        if (top + height > screen.bottom && fField.top - height - 2 >= screen.top) top = fField.top - height - 2;
        MoveTo(std::clamp(fField.left, screen.left, screen.right - width), std::clamp(top, screen.top, screen.bottom - height));
    }
    void Choose(int32 index)
    {
        if (fChosen || index < 0 || index >= fList->CountItems()) return;
        fChosen = true;
        BMessage chosen(kAutoFillChosen);
        chosen.AddUInt64("serial", fSerial);
        chosen.AddInt32("index", index);
        fTarget.SendMessage(&chosen, static_cast<BHandler*>(nullptr), 0);
    }
    BRect fField;
    BListView* fList = nullptr;
    uint64 fSerial;
    BMessenger fTarget;
    bool fChosen = false;
    const bigtime_t fShown = system_time();
};
}

void BrowserWindow::AutoFillScriptMessage(Tab& tab, const BMessage& message)
{
    const uint64 token = message.GetUInt64("reply_token", 0);
    const std::string origin = message.GetString("origin", "");
    json body = json::object();
    try {
        body = json::parse(message.GetString("body", "{}"));
    } catch (const std::exception&) { }
    if (!body.is_object()) body = json::object();
    const std::string type = Text(body, "type");
    if (type == "leave") {
        if (token) BWebKitView::ReplyToScriptMessage(token, "");
        if (fAutoFill && fAutoFill->tab == tab.id) CloseAutoFill();
        return;
    }
    if (type == "filter") {
        // Typing in the field: the list shows what still fits.
        if (token) BWebKitView::ReplyToScriptMessage(token, "");
        if (!fAutoFill || fAutoFill->tab != tab.id) return;
        fAutoFill->text = Text(body, "text").substr(0, 200);
        RefreshAutoFillList();
        return;
    }
    if (type == "field") {
        // The list of another field (or of the same one again) goes first.
        CloseAutoFill();
        auto number = [&](const char* key) {
            auto found = body.find(key);
            return found != body.end() && found->is_number() ? found->get<double>() : 0.0;
        };
        const BRect field(number("left"), number("top"), number("right"), number("bottom"));
        const double width = number("width");
        const std::string kind = Text(body, "kind"), focused = Text(body, "field"), historyName = Text(body, "history");
        std::vector<std::string> keys;
        if (auto found = body.find("keys"); found != body.end() && found->is_array())
            for (const auto& key : *found) if (key.is_string() && keys.size() < 40) keys.push_back(key.get<std::string>());
        if (!token || tab.id != fSelected || !field.IsValid() || width <= 0 || !tab.view) {
            if (token) BWebKitView::ReplyToScriptMessage(token, "");
            return;
        }
        // Each is turned off on its own (Preferences › AutoFill); the page
        // script runs while either is on.
        const auto [autoFill, formHistory] = fShared->Read([](const Profile& profile) {
            return std::pair(profile.autoFill, profile.formHistory);
        });
        fAutoFill.emplace();
        fAutoFill->serial = ++fAutoFillSerial;
        fAutoFill->token = token;
        fAutoFill->tab = tab.id;
        fAutoFill->field = field;
        fAutoFill->width = width;
        fAutoFill->text = Text(body, "text").substr(0, 200);
        fAutoFill->kind = kind;
        fAutoFill->cardNumber = kind == "card" && focused == "ccNumber";
        BMessage entries(kAutoFillCardsRead);
        entries.AddUInt64("serial", fAutoFill->serial);
        if (kind == "card") {
            if (!autoFill) {
                CloseAutoFill();
                return;
            }
            if (!SecureForCards(origin)) {
                CloseAutoFill();
                tab.notice = "Summit fills in payment cards only on secure (https) pages.";
                tab.noticeUntil = system_time() + 8 * 1000000LL;
                if (tab.id == fSelected) ShowTabStatus(tab);
                return;
            }
            // The key store may wait for the user: read it on a thread.
            std::thread([window = BMessenger(this), entries, focused]() mutable {
                for (const auto& card : CardStore::Cards()) {
                    entries.AddString("label", MaskedCard(card).c_str());
                    // Typing a name or number filters by it; other fields
                    // (the expiry) show the cards only while empty.
                    entries.AddString("match", focused == "ccNumber" ? card.number.c_str()
                        : focused == "ccName" ? card.name.c_str() : "");
                    std::string detail = card.name;
                    if (const auto expiry = CardExpiry(card); !expiry.empty())
                        detail += (detail.empty() ? "Expires " : ", expires ") + expiry;
                    entries.AddString("detail", detail.c_str());
                    char month[8];
                    std::snprintf(month, sizeof(month), "%02d", card.month);
                    entries.AddString("values", json { {"ccName", card.name}, {"ccNumber", card.number},
                        {"ccMonth", card.month ? month : ""}, {"ccYear", card.year ? std::to_string(card.year) : ""} }.dump().c_str());
                }
                window.SendMessage(&entries);
            }).detach();
            return;
        }
        // Addresses that have something for this form, most recent first.
        const auto addresses = kind == "address" && autoFill
            ? fShared->Read([](const Profile& profile) { return profile.autoFillAddresses; }) : std::vector<AutoFillAddress>();
        for (auto address = addresses.rbegin(); address != addresses.rend(); ++address) {
            if (!std::any_of(keys.begin(), keys.end(), [&](const std::string& key) { return !AddressValueFor(*address, key).empty(); }))
                continue;
            std::string label = AddressValueFor(*address, focused), detail;
            const std::string name = AddressName(*address), summary = AddressSummary(*address);
            if (label.empty()) label = name.empty() ? summary : name;
            // Under a name, the address; under anything else, whose it is.
            const bool nameField = focused == "name" || focused == "givenName" || focused == "familyName" || focused == "organization";
            detail = nameField || label == name || name.empty() ? summary : name;
            if (detail == label) detail.clear();
            entries.AddString("label", label.c_str());
            entries.AddString("detail", detail.c_str());
            entries.AddString("values", AddressValues(*address).dump().c_str());
            entries.AddString("match", AddressValueFor(*address, focused).c_str());
        }
        // The values typed into fields of this name before (FormHistory.h):
        // for any other text field, and for an address field when no saved
        // address has anything for it. Private windows offer them too, as
        // they do saved addresses; they only never add to them.
        if (!entries.HasString("label") && !historyName.empty() && formHistory) {
            for (const auto& value : fShared->Forms().Values(historyName, HostOf(origin))) {
                entries.AddString("label", value.c_str());
                entries.AddString("detail", "");
                entries.AddString("values", json { {"historyValue", value} }.dump().c_str());
                entries.AddString("match", value.c_str());
            }
            entries.AddString("history", historyName.c_str());
            fAutoFill->kind = "history";
        }
        ShowAutoFillList(entries);
        return;
    }
    if (token) BWebKitView::ReplyToScriptMessage(token, "");
    if (type != "submit" || fPrivate) return;
    // Private windows never save; nor do pages other than web pages and files.
    if (!origin.starts_with("https://") && !origin.starts_with("http://") && !origin.starts_with("file://")) return;
    const auto [autoFill, formHistory] = fShared->Read([](const Profile& profile) {
        return std::pair(profile.autoFill, profile.formHistory);
    });
    // What the user typed into the form's other text fields (the script
    // leaves out passwords, autocomplete=off and card fields; FormHistory
    // checks the values again).
    if (auto found = body.find("history"); formHistory && found != body.end() && found->is_array()) {
        std::vector<std::pair<std::string, std::string>> values;
        for (const auto& pair : *found) {
            if (!pair.is_array() || pair.size() != 2 || !pair[0].is_string() || !pair[1].is_string()) continue;
            values.emplace_back(pair[0].get<std::string>().substr(0, 256), pair[1].get<std::string>().substr(0, 1024));
            if (values.size() >= 50) break;
        }
        if (!values.empty()) {
            fShared->Forms().Remember(HostOf(origin), values, std::time(nullptr));
            std::fprintf(stderr, "Summit: form history noted %zu value%s\n", values.size(), values.size() == 1 ? "" : "s");
        }
    }
    if (!autoFill) return;
    if (auto found = body.find("address"); found != body.end() && found->is_object()) {
        const auto address = AddressFromJSON(*found);
        const bool known = fShared->Read([&](const Profile& profile) {
            return std::any_of(profile.autoFillAddresses.begin(), profile.autoFillAddresses.end(),
                [&](const AutoFillAddress& saved) { return AddressCovers(saved, address); });
        });
        if (AddressWorthSaving(address) && !known) {
            if (!tab.pendingAutoFill) tab.pendingAutoFill.emplace();
            tab.pendingAutoFill->address = address;
            RefreshNoticeBar();
        }
    }
    if (auto found = body.find("card"); found != body.end() && found->is_object() && SecureForCards(origin)) {
        AutoFillCard card { {}, Text(*found, "name"), CardNumberDigits(Text(*found, "number")), 0, 0 };
        if (auto month = found->find("month"); month != found->end() && month->is_number_integer()) card.month = std::clamp(month->get<int>(), 0, 12);
        if (auto year = found->find("year"); year != found->end() && year->is_number_integer()) card.year = std::clamp(year->get<int>(), 0, 2100);
        if (card.number.empty()) return;
        // Is it saved already? The key store answers on a thread of its own.
        std::thread([window = BMessenger(this), id = tab.id, card]() mutable {
            bool update = false;
            for (const auto& saved : CardStore::Cards()) {
                if (saved.number != card.number) continue;
                if (saved.name == card.name && (!card.month || saved.month == card.month) && (!card.year || saved.year == card.year)) return;
                // The same card with a new expiry or name: it replaces the old one.
                update = true;
                card.id = saved.id;
                if (card.name.empty()) card.name = saved.name;
                if (!card.month) card.month = saved.month;
                if (!card.year) card.year = saved.year;
            }
            BMessage checked(kAutoFillCardChecked);
            checked.AddInt64("tab", id);
            checked.AddString("id", card.id.c_str());
            checked.AddString("name", card.name.c_str());
            checked.AddString("number", card.number.c_str());
            checked.AddInt32("month", card.month);
            checked.AddInt32("year", card.year);
            checked.AddBool("update", update);
            window.SendMessage(&checked);
        }).detach();
    }
}

void BrowserWindow::ShowAutoFillList(const BMessage& entries)
{
    // An offer that was closed (or replaced) while the key store was read.
    if (!fAutoFill || fAutoFill->serial != entries.GetUInt64("serial", 0)) return;
    const char* label = nullptr;
    for (int32 i = 0; entries.FindString("label", i, &label) == B_OK; ++i)
        fAutoFill->entries.push_back({label, entries.GetString("detail", i, ""), entries.GetString("values", i, ""),
            entries.GetString("match", i, "")});
    fAutoFill->history = entries.GetString("history", "");
    // Nothing to offer, however the field is typed in.
    if (fAutoFill->entries.empty()) {
        CloseAutoFill();
        return;
    }
    RefreshAutoFillList();
}

void BrowserWindow::RefreshAutoFillList()
{
    if (!fAutoFill) return;
    auto& offer = *fAutoFill;
    std::string typed = offer.text;
    if (offer.cardNumber) std::erase_if(typed, [](char c) { return c == ' ' || c == '-'; });
    typed = Folded(typed);
    // Entries that start with what the field holds, as Chrome and Firefox
    // filter them. A remembered value the field already holds adds nothing;
    // an entry with nothing for this field shows only while it is empty.
    offer.shown.clear();
    for (size_t i = 0; i < offer.entries.size(); ++i) {
        const auto& entry = offer.entries[i];
        if (!typed.empty() && !Folded(entry.match).starts_with(typed)) continue;
        if (!offer.history.empty() && entry.match == offer.text) continue;
        offer.shown.push_back(i);
        // A short list: what is typed narrows it to the rest.
        if (!offer.history.empty() && offer.shown.size() >= 10) break;
    }
    offer.selected = -1;
    auto* tab = FindTabByID(offer.tab);
    if (offer.shown.empty() || !tab || tab->id != fSelected || !tab->view) {
        // Hidden while nothing fits; deleting what was typed brings it back.
        if (offer.list.IsValid()) offer.list.SendMessage(B_QUIT_REQUESTED);
        offer.list = BMessenger();
        return;
    }
    BMessage rows(kAutoFillRows);
    for (size_t index : offer.shown) {
        rows.AddString("label", offer.entries[index].label.c_str());
        rows.AddString("detail", offer.entries[index].detail.c_str());
    }
    if (offer.history.empty()) {
        rows.AddString("label", kSettingsRow);
        rows.AddString("detail", "");
    }
    if (offer.list.IsValid()) {
        offer.list.SendMessage(&rows);
        return;
    }
    // CSS pixels to the view's: page zoom and the display's scale both.
    BRect field = offer.field;
    const float scale = tab->view->Bounds().Width() / offer.width;
    field = BRect(field.left * scale, field.top * scale, field.right * scale, field.bottom * scale);
    const BRect bounds = tab->view->Bounds();
    field.left = std::clamp(field.left, 0.0f, bounds.right);
    field.right = std::clamp(field.right, field.left, bounds.right);
    field.top = std::clamp(field.top, 0.0f, bounds.bottom);
    field.bottom = std::clamp(field.bottom, field.top, bounds.bottom);
    field = tab->view->ConvertToScreen(field);
    auto* list = new AutoFillList(field, offer.serial, rows, BMessenger(this));
    if (list->AddToSubset(this) != B_OK) {
        list->Quit();
        CloseAutoFill();
        return;
    }
    offer.list = BMessenger(list);
    std::fprintf(stderr, "Summit: AutoFill offers %zu %s entr%s\n", offer.shown.size(), offer.kind.c_str(),
        offer.shown.size() == 1 ? "y" : "ies");
    list->Show();
}

void BrowserWindow::AutoFillChosen(int32 index)
{
    if (!fAutoFill || index < 0) return;
    if (static_cast<size_t>(index) >= fAutoFill->shown.size()) {
        // The last row: Preferences › AutoFill.
        CloseAutoFill();
        BMessage show(kShowPreferences);
        show.AddString("tab", "autofill");
        be_app->PostMessage(&show);
        return;
    }
    std::fprintf(stderr, "Summit: AutoFill %s entry %d chosen\n", fAutoFill->kind.c_str(), static_cast<int>(index));
    BWebKitView::ReplyToScriptMessage(fAutoFill->token, fAutoFill->entries[fAutoFill->shown[index]].values.c_str());
    CloseAutoFill();
}

bool BrowserWindow::AutoFillKey(const BMessage& message)
{
    if (!fAutoFill || !fAutoFill->list.IsValid()) return false;
    const int32 modifiers = message.GetInt32("modifiers", 0);
    if (modifiers & (B_COMMAND_KEY | B_CONTROL_KEY | B_OPTION_KEY)) return false;
    const int32 count = static_cast<int32>(fAutoFill->shown.size()) + (fAutoFill->history.empty() ? 1 : 0);
    auto select = [&](int32 index) {
        fAutoFill->selected = index;
        BMessage selected(kAutoFillSelect);
        selected.AddInt32("index", index);
        fAutoFill->list.SendMessage(&selected);
    };
    switch (message.GetString("bytes", "")[0]) {
    case B_DOWN_ARROW: select((fAutoFill->selected + 1) % count); return true;
    case B_UP_ARROW: select(fAutoFill->selected <= 0 ? count - 1 : fAutoFill->selected - 1); return true;
    case B_DELETE: {
        // Shift+Delete forgets the highlighted remembered value, as in
        // Firefox and Chrome; otherwise Delete is the page's.
        const int32 row = fAutoFill->selected;
        if (!(modifiers & B_SHIFT_KEY) || fAutoFill->history.empty() || row < 0 || row >= static_cast<int32>(fAutoFill->shown.size()))
            return false;
        const size_t index = fAutoFill->shown[row];
        fShared->Forms().Remove(fAutoFill->history, fAutoFill->entries[index].match);
        std::fprintf(stderr, "Summit: form history value removed\n");
        fAutoFill->entries.erase(fAutoFill->entries.begin() + index);
        RefreshAutoFillList();
        // The highlight stays on the row that took its place.
        if (fAutoFill && fAutoFill->list.IsValid() && !fAutoFill->shown.empty())
            select(std::min(row, static_cast<int32>(fAutoFill->shown.size()) - 1));
        return true;
    }
    case B_ENTER:
        if (fAutoFill->selected >= 0) {
            AutoFillChosen(fAutoFill->selected);
            return true;
        }
        // Nothing highlighted: Return is the page's (it may send the form).
        CloseAutoFill();
        return false;
    case B_ESCAPE: CloseAutoFill(); return true;
    case B_TAB: CloseAutoFill(); return false;
    default: return false;
    }
}

void BrowserWindow::CloseAutoFill()
{
    if (!fAutoFill) return;
    // Unknown and answered tokens are ignored: a chosen entry was answered.
    BWebKitView::ReplyToScriptMessage(fAutoFill->token, "");
    if (fAutoFill->list.IsValid()) fAutoFill->list.SendMessage(B_QUIT_REQUESTED);
    // The values may hold a card number: they go now, not with the next offer.
    for (auto& entry : fAutoFill->entries) {
        std::fill(entry.values.begin(), entry.values.end(), '\0');
        std::fill(entry.match.begin(), entry.match.end(), '\0');
    }
    fAutoFill.reset();
}

void BrowserWindow::AutoFillCardChecked(const BMessage& message)
{
    auto* tab = FindTabByID(message.GetInt64("tab", 0));
    if (!tab) return;
    if (!tab->pendingAutoFill) tab->pendingAutoFill.emplace();
    tab->pendingAutoFill->card = AutoFillCard { message.GetString("id", ""), message.GetString("name", ""),
        message.GetString("number", ""), static_cast<int>(message.GetInt32("month", 0)), static_cast<int>(message.GetInt32("year", 0)) };
    tab->pendingAutoFill->cardUpdate = message.GetBool("update", false);
    RefreshNoticeBar();
}

bool BrowserWindow::AutoFillNotice(const Tab& tab)
{
    if (!tab.pendingAutoFill || (!tab.pendingAutoFill->address && !tab.pendingAutoFill->card)) return false;
    const auto& pending = *tab.pendingAutoFill;
    std::string text;
    if (pending.address) {
        const std::string name = AddressName(*pending.address), summary = AddressSummary(*pending.address);
        const std::string what = name.empty() ? summary : summary.empty() ? name : name + ", " + summary;
        text = pending.card ? "Save this address (" + what + ") and the card " + MaskedCard(*pending.card) + " for AutoFill?"
            : "Save this address for AutoFill? " + what;
    } else
        text = (pending.cardUpdate ? "Update the saved card " : "Save the card ") + MaskedCard(*pending.card) + " for AutoFill?";
    if (pending.card) text += " Summit keeps cards in the Haiku key store, without the security code.";
    auto answer = [&](int32 which) {
        auto* message = new BMessage(kAutoFillBarAnswer);
        message->AddInt64("tab", tab.id);
        message->AddInt32("which", which);
        return message;
    };
    fNoticeBar->SetNotice(text, {{!pending.address && pending.cardUpdate ? "Update" : "Save", answer(0)}, {"Not Now", answer(1)}});
    return true;
}

void BrowserWindow::AutoFillBarAnswer(const BMessage& message)
{
    // Without "tab", the selected one's: scripting for tests ('afba' with
    // which=int32(N), as hey sends it).
    auto* tab = message.HasInt64("tab") ? FindTabByID(message.GetInt64("tab", 0)) : ActiveTab();
    if (!tab || !tab->pendingAutoFill) return;
    auto pending = *std::exchange(tab->pendingAutoFill, std::nullopt);
    if (message.GetInt32("which", 1) == 0 && !fPrivate) {
        if (pending.address) {
            pending.address->id = NewAutoFillID();
            pending.address->added = std::time(nullptr);
            fShared->Change([&](Profile& profile) -> uint32 {
                if (profile.autoFillAddresses.size() >= 1000) return 0;
                profile.autoFillAddresses.push_back(*pending.address);
                return SharedProfile::kSettingsChanged;
            }, BMessenger(this));
            fStatus->SetText("The address was saved for AutoFill.");
        }
        if (pending.card) {
            std::thread([window = BMessenger(this), card = *pending.card]() mutable {
                BMessage saved(kAutoFillSaved);
                saved.AddString("error", CardStore::Save(card).c_str());
                window.SendMessage(&saved);
            }).detach();
        }
    }
    RefreshNoticeBar();
}
}
#endif
