#if SUMMIT_MODERN_WEBKIT
#include "DevToolsPanels.h"
#include <interface/ColumnListView.h>
#include <interface/ColumnTypes.h>
#include <Button.h>
#include <CardLayout.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <OutlineListView.h>
#include <ScrollView.h>
#include <SplitView.h>
#include <StringView.h>
#include <TextControl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <vector>

namespace summit {
using namespace devtools;
namespace {
constexpr uint32 kAreaSelected = 'dsar', kItemSelected = 'dsit', kRefresh = 'dsrf', kDelete = 'dsdl', kClearArea = 'dscl',
    kFilterItems = 'dsfl', kSaveItem = 'dssv';
enum { kItemKey, kItemValue };
enum { kCookieName, kCookieValue, kCookieDomain, kCookiePath, kCookieExpires, kCookieSize, kCookieHttpOnly, kCookieSecure,
    kCookieSameSite, kCookiePartition };
enum { kTableItems, kTableCookies };

std::string LowerCase(std::string text)
{
    for (auto& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return text;
}

// One line of a value for a cell: its line breaks would not show.
std::string OneLine(const std::string& text)
{
    std::string line = text.substr(0, 1024);
    std::replace(line.begin(), line.end(), '\n', ' ');
    std::replace(line.begin(), line.end(), '\r', ' ');
    std::replace(line.begin(), line.end(), '\t', ' ');
    return line;
}

// What names a cookie among the page's: several can share a name.
std::string CookieID(const Cookie& cookie) { return cookie.ID(); }

const char* KindName(StorageKind kind)
{
    return kind == StorageKind::Local ? "local" : kind == StorageKind::Session ? "session"
        : kind == StorageKind::IndexedDB ? "indexeddb" : "cookies";
}

BView* Placeholder(const char* text)
{
    auto* note = new BStringView("placeholder", text);
    note->SetAlignment(B_ALIGN_CENTER);
    note->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    note->SetExplicitMinSize(BSize(40, 40));
    note->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    auto* group = new BGroupView(B_VERTICAL, 0);
    BLayoutBuilder::Group<>(group).AddGlue().Add(note).AddGlue();
    group->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    return group;
}

class SizeColumn : public BStringColumn {
public:
    SizeColumn(const char* title, float width) : BStringColumn(title, width, 40, 400, B_TRUNCATE_END, B_ALIGN_RIGHT) { }
    int CompareFields(BField* first, BField* second) override
    {
        const auto a = std::stoull("0" + std::string(static_cast<BStringField*>(first)->String()));
        const auto b = std::stoull("0" + std::string(static_cast<BStringField*>(second)->String()));
        return a < b ? -1 : a > b ? 1 : 0;
    }
};
}

// "Local Storage", "Session Storage" and "Cookies", and the page's origins
// under the first two.
class StoragePanel::AreaItem : public BStringItem {
public:
    AreaItem(const char* label, StorageKind kind, std::string origin, bool selectable, uint32 level = 0)
        : BStringItem(label, level), kind(kind), origin(std::move(origin)), selectable(selectable) { }
    StorageKind kind;
    std::string origin;
    bool selectable;
};

class StoragePanel::Row : public BRow {
public:
    std::string id;
};

StoragePanel::StoragePanel(Session& session, DevToolsHost& host)
    : BView("Storage", 0), fSession(session), fHost(host)
{
    fRefresh = new BButton("refresh", "Refresh", new BMessage(kRefresh));
    fRefresh->SetToolTip("Read the selected storage from the page again");
    fDelete = new BButton("delete", "Delete", new BMessage(kDelete));
    fDelete->SetToolTip("Delete the selected item or cookie");
    fClear = new BButton("clear", "Clear All", new BMessage(kClearArea));
    fClear->SetToolTip("Delete every item of this storage, or every cookie listed");
    fFilter = new BTextControl("filter", "Filter:", "", nullptr);
    fFilter->SetModificationMessage(new BMessage(kFilterItems));
    fFilter->TextView()->SetExplicitMinSize(BSize(140, B_SIZE_UNSET));
    fFilter->SetToolTip("Filter keys, values, cookie domains or partition keys");

    fAreas = new BOutlineListView("areas");
    fAreas->SetSelectionMessage(new BMessage(kAreaSelected));
    auto* areas = new BScrollView("areas-scroll", fAreas, 0, false, true, B_NO_BORDER);
    areas->SetExplicitMinSize(BSize(170, 80));
    areas->SetExplicitPreferredSize(BSize(240, B_SIZE_UNSET));

    fItems = new BColumnListView("items", 0, B_NO_BORDER);
    fItems->AddColumn(new BStringColumn("Key", 220, 40, 2000, B_TRUNCATE_MIDDLE), kItemKey);
    fItems->AddColumn(new BStringColumn("Value", 420, 40, 4000, B_TRUNCATE_END), kItemValue);
    fCookies = new BColumnListView("cookies", 0, B_NO_BORDER);
    fCookies->AddColumn(new BStringColumn("Name", 150, 40, 2000, B_TRUNCATE_MIDDLE), kCookieName);
    fCookies->AddColumn(new BStringColumn("Value", 220, 40, 4000, B_TRUNCATE_END), kCookieValue);
    fCookies->AddColumn(new BStringColumn("Domain", 150, 40, 1000, B_TRUNCATE_MIDDLE), kCookieDomain);
    fCookies->AddColumn(new BStringColumn("Path", 70, 30, 1000, B_TRUNCATE_MIDDLE), kCookiePath);
    fCookies->AddColumn(new BStringColumn("Expires", 150, 40, 400, B_TRUNCATE_END), kCookieExpires);
    fCookies->AddColumn(new SizeColumn("Size", 50), kCookieSize);
    fCookies->AddColumn(new BStringColumn("HttpOnly", 70, 30, 200, B_TRUNCATE_END, B_ALIGN_CENTER), kCookieHttpOnly);
    fCookies->AddColumn(new BStringColumn("Secure", 60, 30, 200, B_TRUNCATE_END, B_ALIGN_CENTER), kCookieSecure);
    fCookies->AddColumn(new BStringColumn("SameSite", 70, 30, 200, B_TRUNCATE_END), kCookieSameSite);
    fCookies->AddColumn(new BStringColumn("Partition Key", 180, 60, 2000, B_TRUNCATE_MIDDLE), kCookiePartition);
    for (auto* list : {fItems, fCookies}) {
        list->SetSelectionMode(B_SINGLE_SELECTION_LIST);
        list->SetSelectionMessage(new BMessage(kItemSelected));
        list->SetSortingEnabled(true);
        list->SetExplicitMinSize(BSize(200, 60));
    }
    auto* tables = new BView("tables", 0);
    fTables = new BCardLayout();
    tables->SetLayout(fTables);
    fTables->AddView(fItems);
    fTables->AddView(fCookies);
    fTables->AddView(Placeholder("Choose the storage of an origin, or the cookies, on the left."));
    fTables->SetVisibleItem(static_cast<int32>(2));
    fSummary = new BStringView("summary", "");
    fSummary->SetTruncation(B_TRUNCATE_END);
    fSummary->SetExplicitMinSize(BSize(40, B_SIZE_UNSET));
    fSummary->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    fSummary->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);

    fValue = new BodyView("value");
    fKey = new BTextControl("key", "Key:", "", nullptr);
    fKey->TextView()->SetExplicitMinSize(BSize(120, B_SIZE_UNSET));
    fNewValue = new BTextControl("new-value", "Value:", "", new BMessage(kSaveItem));
    fNewValue->TextView()->SetExplicitMinSize(BSize(200, B_SIZE_UNSET));
    fSave = new BButton("save", "Save Item", new BMessage(kSaveItem));
    fSave->SetToolTip("Set the key to the value in the page's storage (a new key adds an item)");
    fEditor = new BGroupView(B_HORIZONTAL, 8);
    BLayoutBuilder::Group<>(static_cast<BGroupView*>(fEditor))
        .SetInsets(8, 4, 8, 6)
        .Add(fKey, 2)
        .Add(fNewValue, 3)
        .Add(fSave);

    auto* outer = new BSplitView(B_HORIZONTAL, 2);
    outer->SetCollapsible(false);
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .AddGroup(B_HORIZONTAL, 8).SetInsets(8, 6, 8, 6)
            .Add(fRefresh)
            .Add(fDelete)
            .Add(fClear)
            .AddStrut(8)
            .Add(fFilter, 1)
        .End()
        .AddSplit(outer, 1)
            .Add(areas, 0)
            .AddSplit(B_VERTICAL, 2, 3)
                .SetCollapsible(false)
                .AddGroup(B_VERTICAL, 0, 3)
                    .Add(tables, 1)
                    .AddGroup(B_HORIZONTAL, 0).SetInsets(8, 3, 8, 3)
                        .Add(fSummary)
                    .End()
                .End()
                .AddGroup(B_VERTICAL, 0, 2)
                    .Add(fValue, 1)
                    .Add(fEditor)
                .End()
            .End()
        .End();
    outer->SetItemWeight(0, 1, false);
    outer->SetItemWeight(1, 4, false);
    RebuildAreas();
    Fill();
}

void StoragePanel::AttachedToWindow()
{
    BView::AttachedToWindow();
    AdoptParentColors();
    for (BControl* control : std::initializer_list<BControl*> {fRefresh, fDelete, fClear, fFilter, fNewValue, fSave})
        control->SetTarget(this);
    fAreas->SetTarget(this);
    fItems->SetTarget(this);
    fCookies->SetTarget(this);
}

const StorageArea* StoragePanel::Area() const
{
    return fHasArea ? fSession.FindArea(fKind, fOrigin) : nullptr;
}

void StoragePanel::SetShown(bool shown)
{
    fShown = shown;
    if (shown) {
        Load(false);
        fHost.Sync();
    }
}

// An IndexedDB origin (not one of its object stores): its databases.
static bool IsDatabasesArea(StorageKind kind, const std::string& origin)
{
    return kind == StorageKind::IndexedDB && origin.find('\n') == std::string::npos;
}

void StoragePanel::Load(bool again)
{
    if (!fShown || !fHasArea) return;
    if (IsDatabasesArea(fKind, fOrigin)) {
        const auto* databases = fSession.Databases(fOrigin);
        if (again || !databases || (!databases->loaded && !databases->waiting)) fSession.LoadDatabases(fOrigin);
        return;
    }
    const auto* area = Area();
    if (again || !area || (!area->loaded && !area->waiting)) fSession.LoadStorage(fKind, fOrigin);
}

void StoragePanel::RebuildAreas()
{
    const auto& origins = fSession.StorageOrigins();
    std::vector<BListItem*> old;
    for (int32 i = 0; i < fAreas->FullListCountItems(); ++i) old.push_back(fAreas->FullListItemAt(i));
    fAreas->MakeEmpty();
    for (auto* item : old) delete item;
    AreaItem* selected = nullptr;
    for (const auto kind : {StorageKind::Local, StorageKind::Session}) {
        auto* title = new AreaItem(kind == StorageKind::Local ? "Local Storage" : "Session Storage", kind, { }, false);
        title->SetExpanded(true);
        fAreas->AddItem(title);
        // Appended at the next level they are the title's, in the page's order.
        for (const auto& origin : origins) {
            auto* item = new AreaItem(origin.c_str(), kind, origin, true, 1);
            fAreas->AddItem(item);
            if (fHasArea && fKind == kind && fOrigin == origin) selected = item;
        }
    }
    // IndexedDB: an origin's databases are asked for when it is chosen; then
    // their object stores are listed under it.
    auto* indexed = new AreaItem("IndexedDB", StorageKind::IndexedDB, { }, false);
    indexed->SetExpanded(true);
    fAreas->AddItem(indexed);
    for (const auto& origin : origins) {
        auto* item = new AreaItem(origin.c_str(), StorageKind::IndexedDB, origin, true, 1);
        fAreas->AddItem(item);
        if (fHasArea && fKind == StorageKind::IndexedDB && fOrigin == origin) selected = item;
        const auto* databases = fSession.Databases(origin);
        if (!databases || !databases->loaded) continue;
        for (const auto& database : databases->databases) {
            for (const auto& store : database.stores) {
                const auto key = IndexedDBStore(origin, database.name, store);
                auto* storeItem = new AreaItem((database.name + " › " + store).c_str(), StorageKind::IndexedDB, key, true, 2);
                fAreas->AddItem(storeItem);
                if (fHasArea && fKind == StorageKind::IndexedDB && fOrigin == key) selected = storeItem;
            }
        }
    }
    auto* cookies = new AreaItem("Cookies", StorageKind::Cookies, { }, true);
    fAreas->AddItem(cookies);
    if (fHasArea && fKind == StorageKind::Cookies) selected = cookies;
    // An object store chosen before its origin's databases were read stays
    // chosen while they are.
    if (fHasArea && !selected && fKind == StorageKind::IndexedDB && fOrigin.find('\n') != std::string::npos) {
        const auto origin = fOrigin.substr(0, fOrigin.find('\n'));
        const auto* databases = fSession.Databases(origin);
        if (std::find(origins.begin(), origins.end(), origin) != origins.end() && (!databases || !databases->loaded)) {
            if (!databases) fSession.LoadDatabases(origin);
            return;
        }
    }
    if (fHasArea && !selected) {
        // The page no longer has the origin that was shown.
        fHasArea = false;
        fSelected.clear();
        Fill();
    }
    if (selected) fAreas->Select(fAreas->IndexOf(selected));
}

void StoragePanel::SelectArea(StorageKind kind, const std::string& origin)
{
    const auto shown = kind == StorageKind::Cookies ? std::string() : origin;
    if (fHasArea && fKind == kind && fOrigin == shown) return;
    fHasArea = true;
    fKind = kind;
    fOrigin = shown;
    fSelected.clear();
    Load(false);
    Fill();
    fHost.Sync();
}

void StoragePanel::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case kAreaSelected: {
            auto* item = dynamic_cast<AreaItem*>(fAreas->ItemAt(fAreas->CurrentSelection()));
            if (item && item->selectable) SelectArea(item->kind, item->origin);
            break;
        }
        case kItemSelected: {
            auto* list = fKind == StorageKind::Cookies ? fCookies : fItems;
            auto* row = static_cast<Row*>(list->CurrentSelection());
            fSelected = row ? row->id : std::string();
            ShowItem();
            break;
        }
        case kRefresh:
            Load(true);
            fHost.Sync();
            break;
        case kDelete: Delete(); break;
        case kClearArea:
            if (!fHasArea) break;
            fSession.ClearStorage(fKind, fOrigin);
            fHost.Sync();
            break;
        case kFilterItems: {
            const auto text = LowerCase(fFilter->Text());
            if (text == fFilterText) break;
            fFilterText = text;
            Fill();
            break;
        }
        case kSaveItem: {
            if (fHasArea && fKind == StorageKind::Cookies && *fKey->Text()) {
                SaveCookie();
                break;
            }
            if (!fHasArea || fKind == StorageKind::Cookies || !*fKey->Text()) break;
            fSelected = fKey->Text();
            fSession.SetStorageItem(fKind, fOrigin, fKey->Text(), fNewValue->Text());
            fHost.Sync();
            break;
        }
        default: BView::MessageReceived(message);
    }
}

// The selected cookie with the value typed; another name adds a cookie like
// it (as Save Item adds an item); with none selected, a new session cookie
// for the page's host.
void StoragePanel::SaveCookie()
{
    const auto* area = Area();
    if (!area) return;
    Cookie cookie;
    const Cookie* selected = nullptr;
    for (const auto& known : area->cookies)
        if (CookieID(known) == fSelected) selected = &known;
    if (selected) cookie = *selected;
    else {
        Request page;
        page.url = fSession.PageURL();
        cookie.domain = page.Host();
        if (const auto colon = cookie.domain.find(':'); colon != std::string::npos) cookie.domain.erase(colon);
        cookie.path = "/";
        cookie.session = true;
        cookie.secure = page.url.rfind("https:", 0) == 0;
        cookie.sameSite = "Lax";
        if (cookie.domain.empty()) return;
    }
    cookie.name = fKey->Text();
    cookie.value = fNewValue->Text();
    fSession.SetCookie(cookie);
    fSelected = CookieID(cookie);
    fHost.Sync();
}

void StoragePanel::Delete()
{
    const auto* area = Area();
    if (!area || fSelected.empty()) return;
    if (fKind == StorageKind::Cookies) {
        for (const auto& cookie : area->cookies) {
            if (CookieID(cookie) != fSelected) continue;
            fSession.DeleteCookie(cookie);
            break;
        }
    } else fSession.RemoveStorageItem(fKind, fOrigin, fSelected);
    fSelected.clear();
    fHost.Sync();
}

void StoragePanel::Apply(const Changes& changes)
{
    if (changes.origins) RebuildAreas();
    if (!changes.storage && !changes.origins) return;
    // An area forgotten by a navigation is read again while it is looked at.
    Load(false);
    Fill();
}

void StoragePanel::Fill()
{
    const auto* area = Area();
    const bool cookies = fHasArea && fKind == StorageKind::Cookies;
    const bool indexed = fHasArea && fKind == StorageKind::IndexedDB;
    if (indexed && IsDatabasesArea(fKind, fOrigin)) {
        // An origin: its databases are listed under it in the areas.
        fTables->SetVisibleItem(static_cast<int32>(2));
        const auto* databases = fSession.Databases(fOrigin);
        size_t stores = 0;
        if (databases)
            for (const auto& database : databases->databases) stores += database.stores.size();
        fSummary->SetText(!databases || !databases->loaded ? "Reading…" : !databases->error.empty() ? databases->error.c_str()
            : databases->databases.empty() ? "This origin has no IndexedDB databases."
            : (std::to_string(databases->databases.size()) + (databases->databases.size() == 1 ? " database · " : " databases · ")
                + std::to_string(stores) + (stores == 1 ? " object store: choose it on the left." : " object stores: choose one on the left.")).c_str());
        for (auto* control : std::initializer_list<BControl*> {fDelete, fClear, fSave}) control->SetEnabled(false);
        fValue->ShowNote("");
        return;
    }
    fTables->SetVisibleItem(static_cast<int32>(!fHasArea ? 2 : cookies ? kTableCookies : kTableItems));
    // Cookies are edited by name and value too.
    fKey->SetLabel(cookies ? "Name:" : "Key:");
    fSave->SetLabel(cookies ? "Save Cookie" : "Save Item");
    // An object store's entries are read, and cleared as a whole.
    fDelete->SetEnabled(area && area->loaded && !indexed);
    fClear->SetEnabled(area && area->loaded);
    fSave->SetEnabled(fHasArea && !indexed);
    auto* list = cookies ? fCookies : fItems;
    // Rows are made again: an area holds at most a few thousand items.
    fItems->Clear();
    fCookies->Clear();
    size_t shown = 0, total = 0;
    Row* selected = nullptr;
    const auto matches = [&](const std::string& a, const std::string& b) {
        return fFilterText.empty() || LowerCase(a).find(fFilterText) != std::string::npos
            || LowerCase(b).find(fFilterText) != std::string::npos;
    };
    if (area && cookies) {
        total = area->cookies.size();
        for (const auto& cookie : area->cookies) {
            if (!matches(cookie.name, cookie.value + " " + cookie.domain + " " + cookie.partitionKey)) continue;
            auto* row = new Row();
            row->id = CookieID(cookie);
            row->SetField(new BStringField(OneLine(cookie.name).c_str()), kCookieName);
            row->SetField(new BStringField(OneLine(cookie.value).c_str()), kCookieValue);
            row->SetField(new BStringField(cookie.domain.c_str()), kCookieDomain);
            row->SetField(new BStringField(cookie.path.c_str()), kCookiePath);
            row->SetField(new BStringField(cookie.ExpiresLabel().c_str()), kCookieExpires);
            row->SetField(new BStringField(std::to_string(cookie.Size()).c_str()), kCookieSize);
            row->SetField(new BStringField(cookie.httpOnly ? "✓" : ""), kCookieHttpOnly);
            row->SetField(new BStringField(cookie.secure ? "✓" : ""), kCookieSecure);
            row->SetField(new BStringField(cookie.sameSite.c_str()), kCookieSameSite);
            row->SetField(new BStringField(cookie.partitionKey.c_str()), kCookiePartition);
            fCookies->AddRow(row);
            if (row->id == fSelected) selected = row;
            ++shown;
        }
    } else if (area) {
        total = area->items.size();
        for (const auto& item : area->items) {
            if (!matches(item.key, item.value)) continue;
            auto* row = new Row();
            row->id = item.key;
            row->SetField(new BStringField(OneLine(item.key).c_str()), kItemKey);
            row->SetField(new BStringField(OneLine(item.value).c_str()), kItemValue);
            fItems->AddRow(row);
            if (row->id == fSelected) selected = row;
            ++shown;
        }
    }
    if (selected) {
        list->AddToSelection(selected);
        list->ScrollTo(selected);
    } else fSelected.clear();

    std::string summary;
    if (!fHasArea) summary = fSession.StorageOrigins().empty() ? "This page keeps no storage of its own." : "";
    else if (!area || (area->waiting && !area->loaded)) summary = "Reading…";
    else {
        const char* noun = cookies ? (total == 1 ? " cookie" : " cookies") : indexed ? (total == 1 ? " entry" : " entries")
            : (total == 1 ? " item" : " items");
        summary = std::to_string(total) + noun + " · " + FormatBytes(area->Bytes());
        if (shown != total) summary += " · " + std::to_string(shown) + " shown";
        if (!area->error.empty()) summary += " · " + area->error;
    }
    fSummary->SetText(summary.c_str());
    ShowItem();
}

void StoragePanel::ShowItem()
{
    const auto* area = Area();
    if (!area || fSelected.empty()) {
        fValue->ShowNote(fHasArea ? (fKind == StorageKind::Cookies ? "Select a cookie to see or change its value, or type a name and a value below to add one."
            : "Select an item to see its value, or type a key and a value below to add one.") : "");
        return;
    }
    if (fKind == StorageKind::Cookies) {
        for (const auto& cookie : area->cookies) {
            if (CookieID(cookie) != fSelected) continue;
            fValue->ShowBody(cookie.value, "text/plain");
            // Save Item changes it.
            if (fKey->Text() != cookie.name) fKey->SetText(cookie.name.c_str());
            if (fNewValue->Text() != cookie.value) fNewValue->SetText(cookie.value.c_str());
            return;
        }
    } else {
        for (const auto& item : area->items) {
            if (item.key != fSelected) continue;
            fValue->ShowBody(item.value, "text/plain");
            if (fKey->Text() != item.key) fKey->SetText(item.key.c_str());
            if (fNewValue->Text() != item.value) fNewValue->SetText(item.value.c_str());
            return;
        }
    }
    fValue->ShowNote("");
}

bool StoragePanel::Command(const std::string& action, const std::string& argument, std::string& error)
{
    if (action == "storage-select") {
        // "local ORIGIN", "session ORIGIN" or "cookies".
        const auto space = argument.find(' ');
        const auto kind = argument.substr(0, space);
        const auto origin = space == std::string::npos ? std::string() : argument.substr(space + 1);
        if (kind == "cookies") SelectArea(StorageKind::Cookies, { });
        else if (kind == "indexeddb") {
            // "indexeddb ORIGIN" or "indexeddb ORIGIN|DATABASE|STORE".
            auto area = origin;
            std::replace(area.begin(), area.end(), '|', '\n');
            SelectArea(StorageKind::IndexedDB, area);
        }
        else if (kind == "local" || kind == "session") {
            const auto& origins = fSession.StorageOrigins();
            const auto chosen = origin.empty() && !origins.empty() ? origins.front() : origin;
            if (std::find(origins.begin(), origins.end(), chosen) == origins.end()) error = "The page has no such origin";
            else SelectArea(kind == "local" ? StorageKind::Local : StorageKind::Session, chosen);
        } else error = "Choose local, session, indexeddb or cookies";
        RebuildAreas();
    } else if (action == "storage-refresh") {
        Load(true);
        fHost.Sync();
    } else if (action == "storage-item") {
        // Selects the item with this key, or the cookie with this name.
        fSelected.clear();
        if (const auto* area = Area()) {
            for (const auto& item : area->items) if (item.key == argument) fSelected = item.key;
            for (const auto& cookie : area->cookies) if (cookie.name == argument || cookie.ID() == argument) fSelected = CookieID(cookie);
        }
        if (fSelected.empty()) error = "No such item";
        Fill();
    } else if (action == "storage-set") {
        const auto equals = argument.find('=');
        if (equals == std::string::npos || !equals) error = "Give KEY=VALUE";
        else {
            fKey->SetText(argument.substr(0, equals).c_str());
            fNewValue->SetText(argument.substr(equals + 1).c_str());
            BMessage save(kSaveItem);
            MessageReceived(&save);
        }
    } else if (action == "storage-delete") Delete();
    else if (action == "storage-clear") {
        fSession.ClearStorage(fKind, fOrigin);
        fHost.Sync();
    } else if (action == "filter-storage") {
        fFilter->SetText(argument.c_str());
        fFilterText = LowerCase(argument);
        Fill();
    } else return false;
    return true;
}

std::string StoragePanel::StateJSON()
{
    using nlohmann::json;
    json areas = json::array(), rows = json::array();
    for (int32 i = 0; i < fAreas->FullListCountItems(); ++i) {
        auto* item = static_cast<AreaItem*>(fAreas->FullListItemAt(i));
        areas.push_back(std::string(item->OutlineLevel() ? "  " : "") + item->Text());
    }
    auto* list = fHasArea && fKind == StorageKind::Cookies ? fCookies : fItems;
    for (int32 i = 0; i < list->CountRows(); ++i) {
        auto* row = list->RowAt(i);
        json cells = json::array();
        for (int32 field = 0; field < row->CountFields(); ++field)
            cells.push_back(static_cast<BStringField*>(row->GetField(field))->String());
        rows.push_back(cells);
    }
    const auto* area = Area();
    json state = {{"shown", fShown}, {"areas", areas}, {"kind", fHasArea ? KindName(fKind) : ""}, {"origin", fOrigin},
        {"loaded", area && area->loaded}, {"error", area ? area->error : std::string()}, {"rows", rows},
        {"selected", fSelected}, {"summary", fSummary->Text()}, {"value", json::parse(fValue->StateJSON())},
        {"key", fKey->Text()}, {"newValue", fNewValue->Text()}};
    return state.dump(-1, ' ', false, json::error_handler_t::replace);
}
}
#endif
