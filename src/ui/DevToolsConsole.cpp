#if SUMMIT_MODERN_WEBKIT
#include "DevToolsPanels.h"
#include <Button.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <MessageFilter.h>
#include <OutlineListView.h>
#include <ScrollView.h>
#include <SplitView.h>
#include <StringView.h>
#include <TextControl.h>
#include <Window.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>

namespace summit {
using namespace devtools;
namespace {
constexpr uint32 kClearConsole = 'dccl', kPreserveConsole = 'dcpr', kLevelsChanged = 'dclv', kFilterConsole = 'dcfl',
    kEvaluate = 'dcev';

// Styles of the log. The lexers' are below 40 and the StyledText runs' from 64.
enum {
    kLogStyle = 90, kInfoStyle, kDebugStyle, kWarningStyle, kErrorStyle, kStackStyle, kWarningStackStyle,
    kErrorStackStyle, kInputStyle, kResultStyle, kFailedResultStyle, kNavigationStyle, kTimeStyle, kPlaceStyle,
    kWarningPlaceStyle, kErrorPlaceStyle
};
// Marks in the margin, one per severity: shape and colour both tell them apart.
enum { kErrorMark = 1, kWarningMark, kInfoMark, kDebugMark, kInputMark, kResultMark, kNavigationMark };

std::string LowerCase(std::string text)
{
    for (auto& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return text;
}

std::string ClockTime(double secondsSinceEpoch)
{
    if (secondsSinceEpoch <= 0) return { };
    const auto whole = static_cast<std::time_t>(secondsSinceEpoch);
    std::tm local { };
    localtime_r(&whole, &local);
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec,
        static_cast<int>((secondsSinceEpoch - static_cast<double>(whole)) * 1000) % 1000);
    return buffer;
}

sptr_t Pointer(const char* text) { return reinterpret_cast<sptr_t>(text); }

// Up and Down in the input bring back what was typed before.
class HistoryKeys : public BMessageFilter {
public:
    explicit HistoryKeys(ConsolePanel* panel)
        : BMessageFilter(B_ANY_DELIVERY, B_ANY_SOURCE, B_KEY_DOWN), fPanel(panel)
    {
    }
    filter_result Filter(BMessage* message, BHandler**) override
    {
        const char* bytes = nullptr;
        if (message->FindString("bytes", &bytes) != B_OK || !bytes) return B_DISPATCH_MESSAGE;
        if (bytes[0] != B_UP_ARROW && bytes[0] != B_DOWN_ARROW) return B_DISPATCH_MESSAGE;
        fPanel->StepHistory(bytes[0] == B_UP_ARROW ? -1 : 1);
        return B_SKIP_MESSAGE;
    }
private:
    ConsolePanel* fPanel;
};
}

// The console's text: one entry after another, each in the colours of its
// severity, with its time and a mark in the margin and its place in the
// page's sources after its first line.
class ConsolePanel::Log : public SourceView {
public:
    Log()
        : SourceView("log", false, false)
    {
        SendMessage(SCI_SETILEXER, 0, 0);
        SendMessage(SCI_SETMARGINTYPEN, 0, SC_MARGIN_SYMBOL);
        SendMessage(SCI_SETMARGINMASKN, 0, 0xFE);
        SendMessage(SCI_SETMARGINWIDTHN, 0, 20);
        SendMessage(SCI_SETMARGINTYPEN, 1, SC_MARGIN_RTEXT);
        SendMessage(SCI_SETMARGINMASKN, 1, 0);
        SendMessage(SCI_SETMARGINSENSITIVEN, 1, 0);
        SendMessage(SCI_EOLANNOTATIONSETVISIBLE, EOLANNOTATION_STANDARD);
        SendMessage(SCI_SETENDATLASTLINE, 1);
        // A severity's background reaches the edge of the view.
        SendMessage(SCI_SETMARGINRIGHT, 0, 0);
        ApplyPalette();
    }
    void AllAttached() override
    {
        SourceView::AllAttached();
        ApplyPalette();
    }
    size_t Count() const { return fStarts.size(); }
    bool AtEnd()
    {
        const auto last = SendMessage(SCI_GETLINECOUNT) - 1;
        const auto end = SendMessage(SCI_VISIBLEFROMDOCLINE, last) + SendMessage(SCI_WRAPCOUNT, last);
        return SendMessage(SCI_GETFIRSTVISIBLELINE) + SendMessage(SCI_LINESONSCREEN) >= end;
    }
    void ScrollToEnd() { SendMessage(SCI_SCROLLTOEND); }
    void Clear()
    {
        SendMessage(SCI_SETREADONLY, 0);
        SendMessage(SCI_CLEARALL);
        SendMessage(SCI_SETREADONLY, 1);
        SendMessage(SCI_MARKERDELETEALL, static_cast<uptr_t>(-1));
        SendMessage(SCI_MARGINTEXTCLEARALL);
        SendMessage(SCI_EOLANNOTATIONCLEARALL);
        SendMessage(SCI_SETSCROLLWIDTH, 1);
        fStarts.clear();
        fSerials.clear();
        fLastStyle = kLogStyle;
    }
    // Clicking (or moving the caret) into an entry tells which one.
    void SetSelected(std::function<void(uint64)> selected) { fSelected = std::move(selected); }
    void NotificationReceived(SCNotification* notification) override
    {
        SourceView::NotificationReceived(notification);
        if (notification->nmhdr.code != SCN_UPDATEUI || !(notification->updated & SC_UPDATE_SELECTION) || !fSelected
            || fStarts.empty()) return;
        const auto caret = SendMessage(SCI_GETCURRENTPOS);
        const auto found = std::upper_bound(fStarts.begin(), fStarts.end(), caret);
        if (found == fStarts.begin()) return;
        const auto serial = fSerials[static_cast<size_t>(found - fStarts.begin() - 1)];
        if (serial == fSelectedSerial) return;
        fSelectedSerial = serial;
        fSelected(serial);
    }
    void Append(const ConsoleEntry& entry)
    {
        int style = kLogStyle, stackStyle = kStackStyle, placeStyle = kPlaceStyle, mark = 0;
        std::string text = entry.text;
        switch (entry.kind) {
            case EntryKind::Input: style = kInputStyle; mark = kInputMark; break;
            case EntryKind::Result:
                style = entry.level == Level::Error ? kFailedResultStyle : kResultStyle;
                mark = entry.level == Level::Error ? kErrorMark : kResultMark;
                break;
            case EntryKind::Navigation: style = kNavigationStyle; mark = kNavigationMark; break;
            case EntryKind::Message:
                switch (entry.level) {
                    case Level::Error: style = kErrorStyle; stackStyle = kErrorStackStyle; placeStyle = kErrorPlaceStyle; mark = kErrorMark; break;
                    case Level::Warning: style = kWarningStyle; stackStyle = kWarningStackStyle; placeStyle = kWarningPlaceStyle; mark = kWarningMark; break;
                    case Level::Info: style = kInfoStyle; mark = kInfoMark; break;
                    case Level::Debug: style = kDebugStyle; mark = kDebugMark; break;
                    default: break;
                }
                break;
        }
        if (text.empty()) text = " ";
        if (entry.repeat > 1) text += "  (×" + std::to_string(entry.repeat) + ")";
        SendMessage(SCI_SETREADONLY, 0);
        // The line end belongs to the entry before: it carries that entry's
        // background to the edge of the view.
        if (SendMessage(SCI_GETLENGTH) > 0) Add("\n", fLastStyle);
        const auto start = SendMessage(SCI_GETLENGTH);
        fStarts.push_back(start);
        fSerials.push_back(entry.serial);
        Add(text, style);
        if (!entry.stack.empty()) {
            std::string stack;
            for (size_t from = 0; from <= entry.stack.size();) {
                const auto end = std::min(entry.stack.find('\n', from), entry.stack.size());
                stack += "\n    " + entry.stack.substr(from, end - from);
                from = end + 1;
            }
            Add(stack, stackStyle);
            fLastStyle = stackStyle;
        } else fLastStyle = style;
        SendMessage(SCI_SETREADONLY, 1);
        const auto line = SendMessage(SCI_LINEFROMPOSITION, start);
        if (mark) SendMessage(SCI_MARKERADD, line, mark);
        if (const auto time = ClockTime(entry.time); !time.empty()) {
            SendMessage(SCI_MARGINSETTEXT, line, Pointer(time.c_str()));
            SendMessage(SCI_MARGINSETSTYLE, line, kTimeStyle);
        }
        if (const auto place = entry.Location(); !place.empty()) {
            SendMessage(SCI_EOLANNOTATIONSETTEXT, line, Pointer(place.c_str()));
            SendMessage(SCI_EOLANNOTATIONSETSTYLE, line, placeStyle);
        }
    }
    void RemoveLast()
    {
        if (fStarts.empty()) return;
        const auto start = fStarts.back();
        fStarts.pop_back();
        fSerials.pop_back();
        const auto first = SendMessage(SCI_LINEFROMPOSITION, start), last = SendMessage(SCI_GETLINECOUNT) - 1;
        // What is attached to a line would move to the line before it.
        for (auto line = first; line <= last; ++line) {
            SendMessage(SCI_MARKERDELETE, line, static_cast<sptr_t>(-1));
            SendMessage(SCI_MARGINSETTEXT, line, 0);
            SendMessage(SCI_EOLANNOTATIONSETTEXT, line, 0);
        }
        const auto from = std::max<sptr_t>(0, start - 1);
        SendMessage(SCI_SETREADONLY, 0);
        SendMessage(SCI_DELETERANGE, from, SendMessage(SCI_GETLENGTH) - from);
        SendMessage(SCI_SETREADONLY, 1);
        const auto length = SendMessage(SCI_GETLENGTH);
        fLastStyle = length > 0 ? static_cast<int>(SendMessage(SCI_GETSTYLEAT, length - 1)) : kLogStyle;
    }

protected:
    void ApplyPalette() override
    {
        SourceView::ApplyPalette();
        const auto& palette = CurrentSourcePalette();
        const auto style = [&](int number, rgb_color text, rgb_color back, bool bold = false, bool italic = false) {
            SendMessage(SCI_STYLESETFORE, number, SciColor(text));
            SendMessage(SCI_STYLESETBACK, number, SciColor(back));
            SendMessage(SCI_STYLESETBOLD, number, bold);
            SendMessage(SCI_STYLESETITALIC, number, italic);
            SendMessage(SCI_STYLESETEOLFILLED, number, 1);
        };
        const rgb_color amber = palette.dark ? rgb_color {245, 200, 110, 255} : rgb_color {138, 92, 0, 255};
        const auto errorBack = palette.Tint(palette.removed, palette.dark ? .20f : .11f);
        const auto warningBack = palette.Tint(rgb_color {240, 180, 40, 255}, palette.dark ? .18f : .16f);
        style(kLogStyle, palette.text, palette.background);
        style(kInfoStyle, palette.accent, palette.background);
        style(kDebugStyle, palette.muted, palette.background);
        style(kWarningStyle, amber, warningBack);
        style(kErrorStyle, palette.removed, errorBack, true);
        style(kStackStyle, palette.muted, palette.background);
        style(kWarningStackStyle, palette.Tint(amber, .75f), warningBack);
        style(kErrorStackStyle, palette.Tint(palette.removed, .8f), errorBack);
        style(kInputStyle, palette.keyword, palette.background, true);
        style(kResultStyle, palette.string, palette.background);
        style(kFailedResultStyle, palette.removed, errorBack);
        style(kNavigationStyle, palette.accent, palette.Tint(palette.accent, .10f), false, true);
        style(kTimeStyle, palette.muted, palette.background);
        style(kPlaceStyle, palette.muted, palette.background, false, true);
        style(kWarningPlaceStyle, palette.Tint(amber, .7f), warningBack, false, true);
        style(kErrorPlaceStyle, palette.Tint(palette.removed, .7f), errorBack, false, true);
        const auto mark = [&](int number, int symbol, rgb_color color) {
            SendMessage(SCI_MARKERDEFINE, number, symbol);
            SendMessage(SCI_MARKERSETFORE, number, SciColor(color));
            SendMessage(SCI_MARKERSETBACK, number, SciColor(color));
        };
        mark(kErrorMark, SC_MARK_CIRCLE, palette.removed);
        mark(kWarningMark, SC_MARK_ARROWDOWN, rgb_color {224, 160, 20, 255});
        mark(kInfoMark, SC_MARK_ROUNDRECT, palette.accent);
        mark(kDebugMark, SC_MARK_SMALLRECT, palette.muted);
        mark(kInputMark, SC_MARK_SHORTARROW, palette.keyword);
        mark(kResultMark, SC_MARK_MINUS, palette.string);
        mark(kNavigationMark, SC_MARK_ARROWS, palette.accent);
        SendMessage(SCI_SETMARGINBACKN, 0, SciColor(palette.background));
        SendMessage(SCI_SETMARGINWIDTHN, 1, SendMessage(SCI_TEXTWIDTH, kTimeStyle, Pointer("00:00:00.000")) + 14);
    }

private:
    void Add(std::string_view text, int style)
    {
        const auto start = SendMessage(SCI_GETLENGTH);
        SendMessage(SCI_APPENDTEXT, text.size(), Pointer(text.data()));
        SendMessage(SCI_STARTSTYLING, start);
        SendMessage(SCI_SETSTYLING, text.size(), style);
    }
    std::vector<sptr_t> fStarts;
    std::vector<uint64> fSerials;
    std::function<void(uint64)> fSelected;
    uint64 fSelectedSerial = 0;
    int fLastStyle = kLogStyle;
};

ObjectTree::ObjectTree(Session& session, DevToolsHost& host)
    : BOutlineListView("objects", B_SINGLE_SELECTION_LIST), fSession(session), fHost(host)
{
}

struct ObjectTree::Item : BStringItem {
    Item(const std::string& text, uint32 level, std::string object)
        : BStringItem(text.c_str(), level, false), objectId(std::move(object))
    {
    }
    std::string objectId;
    bool requested = false, filled = false;
};

void ObjectTree::ShowObjects(const std::vector<ConsoleObject>& objects)
{
    Clear();
    for (const auto& object : objects) AddObject(object.label, object.objectId, nullptr);
    // One object opens at once, as in other browsers' consoles.
    if (objects.size() == 1) Open(static_cast<Item*>(FullListItemAt(0)));
}

void ObjectTree::Clear()
{
    for (int32 i = FullListCountItems() - 1; i >= 0; --i) delete FullListItemAt(i);
    MakeEmpty();
}

void ObjectTree::Answered(const std::vector<std::string>& objects)
{
    for (const auto& objectId : objects) {
        for (int32 i = 0; i < FullListCountItems(); ++i) {
            auto* item = dynamic_cast<Item*>(FullListItemAt(i));
            if (item && item->objectId == objectId && item->requested && !item->filled) Fill(item);
        }
    }
}

bool ObjectTree::OpenRow(const std::string& prefix)
{
    for (int32 i = 0; i < CountItems(); ++i) {
        auto* item = dynamic_cast<Item*>(ItemAt(i));
        if (item && std::string(item->Text()).rfind(prefix, 0) == 0 && !item->objectId.empty()) {
            Open(item);
            return true;
        }
    }
    return false;
}

std::vector<std::string> ObjectTree::Rows()
{
    std::vector<std::string> rows;
    for (int32 i = 0; i < CountItems(); ++i) {
        auto* item = static_cast<BStringItem*>(ItemAt(i));
        rows.push_back(std::string(item->OutlineLevel() * 2, ' ') + item->Text());
    }
    return rows;
}

void ObjectTree::MouseDown(BPoint where)
{
    BOutlineListView::MouseDown(where);
    LoadExpanded();
}

void ObjectTree::KeyDown(const char* bytes, int32 count)
{
    BOutlineListView::KeyDown(bytes, count);
    LoadExpanded();
}

void ObjectTree::AddObject(const std::string& text, const std::string& objectId, Item* parent)
{
    auto* item = new Item(text, parent ? parent->OutlineLevel() + 1 : 0, objectId);
    if (parent) AddUnder(item, parent);
    else AddItem(item);
    // A row to open: its properties come when it is.
    if (!objectId.empty()) AddUnder(new BStringItem("Loading…"), item);
}

void ObjectTree::Open(Item* item)
{
    if (!item) return;
    Expand(item);
    LoadExpanded();
}

void ObjectTree::LoadExpanded()
{
    for (int32 i = 0; i < FullListCountItems(); ++i) {
        auto* item = dynamic_cast<Item*>(FullListItemAt(i));
        if (!item || !item->IsExpanded() || item->requested || item->objectId.empty()) continue;
        item->requested = true;
        if (const auto* known = fSession.Properties(item->objectId); known && known->loaded) Fill(item);
        else {
            fSession.LoadProperties(item->objectId);
            fHost.Sync();
        }
    }
}

void ObjectTree::Fill(Item* item)
{
    const auto* properties = fSession.Properties(item->objectId);
    if (!properties || !properties->loaded) return;
    item->filled = true;
    // In place of "Loading…".
    while (CountItemsUnder(item, true) > 0) {
        auto* child = EachItemUnder(item, true, [](BListItem* row, void*) { return row; }, nullptr);
        if (!child) break;
        RemoveItem(child);
        delete child;
    }
    if (!properties->error.empty()) AddUnder(new BStringItem(("Not available: " + properties->error).c_str()), item);
    else if (properties->items.empty()) AddUnder(new BStringItem("No properties"), item);
    // AddUnder puts each first under its row: last property first.
    for (auto property = properties->items.rbegin(); property != properties->items.rend(); ++property)
        AddObject(property->name + ": " + property->value, property->objectId, item);
    Invalidate();
}

ConsolePanel::ConsolePanel(Session& session, DevToolsHost& host)
    : BView("Console", 0), fSession(session), fHost(host)
{
    fClear = new BButton("clear", "Clear", new BMessage(kClearConsole));
    fClear->SetToolTip("Remove the messages shown");
    fPreserve = new BCheckBox("preserve", "Preserve log", new BMessage(kPreserveConsole));
    fPreserve->SetToolTip("Keep the messages of earlier pages when the page changes");
    const auto level = [](const char* name, const char* label) {
        auto* box = new BCheckBox(name, label, new BMessage(kLevelsChanged));
        box->SetValue(B_CONTROL_ON);
        return box;
    };
    fErrors = level("errors", "Errors");
    fWarnings = level("warnings", "Warnings");
    fInfo = level("info", "Info");
    fInfo->SetToolTip("console.info() and console.log()");
    fDebug = level("debug", "Debug");
    fFilter = new BTextControl("filter", "Filter:", "", nullptr);
    fFilter->SetModificationMessage(new BMessage(kFilterConsole));
    fFilter->TextView()->SetExplicitMinSize(BSize(120, B_SIZE_UNSET));
    fFilter->SetToolTip("Show messages that contain this");
    fCounts = new BStringView("counts", "");
    fCounts->SetTruncation(B_TRUNCATE_END);
    fCounts->SetExplicitMinSize(BSize(40, B_SIZE_UNSET));
    fCounts->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    fCounts->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    fLog = new Log();
    fLog->SetSelected([this](uint64 serial) { Inspect(serial); });
    fObjects = new ObjectTree(session, host);
    auto* objects = new BScrollView("objects scroll", fObjects, 0, true, true, B_NO_BORDER);
    fSplit = new BSplitView(B_HORIZONTAL, 0);
    BLayoutBuilder::Split<>(fSplit)
        .Add(fLog, 3)
        .Add(objects, 1);
    fSplit->SetCollapsible(true);
    fSplit->SetItemCollapsed(1, true);
    fInput = new BTextControl("input", "›", "", new BMessage(kEvaluate));
    fInput->SetToolTip("Run JavaScript in the page");
    fInput->TextView()->SetFontAndColor(be_fixed_font);
    fInput->TextView()->AddFilter(new HistoryKeys(this));
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .AddGroup(B_HORIZONTAL, 8).SetInsets(8, 6, 8, 6)
            .Add(fClear)
            .Add(fPreserve)
            .AddStrut(8)
            .Add(fErrors)
            .Add(fWarnings)
            .Add(fInfo)
            .Add(fDebug)
            .Add(fFilter, 1)
        .End()
        .Add(fSplit, 1)
        .AddGroup(B_HORIZONTAL, 0).SetInsets(8, 3, 8, 3)
            .Add(fCounts)
        .End()
        .AddGroup(B_HORIZONTAL, 0).SetInsets(8, 0, 8, 6)
            .Add(fInput)
        .End();
    ShowCounts();
}

void ConsolePanel::AttachedToWindow()
{
    BView::AttachedToWindow();
    AdoptParentColors();
    for (BControl* control : std::initializer_list<BControl*> {fClear, fPreserve, fErrors, fWarnings, fInfo, fDebug, fFilter, fInput})
        control->SetTarget(this);
}

void ConsolePanel::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case kClearConsole:
            fSession.ClearConsole();
            fHost.Sync();
            break;
        case kPreserveConsole: fSession.SetPreserveConsole(fPreserve->Value() == B_CONTROL_ON); break;
        case kLevelsChanged:
            fLevels.errors = fErrors->Value() == B_CONTROL_ON;
            fLevels.warnings = fWarnings->Value() == B_CONTROL_ON;
            fLevels.info = fInfo->Value() == B_CONTROL_ON;
            fLevels.debug = fDebug->Value() == B_CONTROL_ON;
            Rebuild();
            break;
        case kFilterConsole: {
            const auto text = LowerCase(fFilter->Text());
            if (text == fFilterText) break;
            fFilterText = text;
            Rebuild();
            break;
        }
        case kEvaluate: {
            const std::string expression = fInput->Text();
            if (expression.find_first_not_of(" \t\r\n") == std::string::npos) break;
            if (fHistory.empty() || fHistory.back() != expression) fHistory.push_back(expression);
            if (fHistory.size() > 200) fHistory.erase(fHistory.begin());
            fHistoryIndex = fHistory.size();
            fInput->SetText("");
            fSession.Evaluate(expression);
            fHost.Sync();
            fLog->ScrollToEnd();
            break;
        }
        default: BView::MessageReceived(message);
    }
}

bool ConsolePanel::Command(const std::string& action, const std::string& argument, std::string&)
{
    const bool on = argument == "on";
    if (action == "clear-console") {
        fSession.ClearConsole();
        fHost.Sync();
    } else if (action == "preserve-console") {
        fPreserve->SetValue(on ? B_CONTROL_ON : B_CONTROL_OFF);
        fSession.SetPreserveConsole(on);
    } else if (action == "levels") {
        // The levels to show, by name: "errors,warnings".
        const auto has = [&](const char* name) { return argument.find(name) != std::string::npos; };
        fLevels = {has("errors"), has("warnings"), has("info"), has("debug")};
        fErrors->SetValue(fLevels.errors);
        fWarnings->SetValue(fLevels.warnings);
        fInfo->SetValue(fLevels.info);
        fDebug->SetValue(fLevels.debug);
        Rebuild();
    } else if (action == "filter-console") {
        fFilter->SetText(argument.c_str());
        fFilterText = LowerCase(argument);
        Rebuild();
    } else if (action == "evaluate") {
        fInput->SetText(argument.c_str());
        BMessage evaluate(kEvaluate);
        MessageReceived(&evaluate);
    } else if (action == "inspect") {
        // The entry's position in the log, from 0, or the last for "last".
        const auto& entries = fSession.Entries();
        if (entries.empty()) return false;
        const size_t index = argument == "last" ? entries.size() - 1 : std::strtoul(argument.c_str(), nullptr, 10);
        if (index >= entries.size()) return false;
        Inspect(entries[index].serial);
    } else if (action == "expand") {
        if (!fObjects->OpenRow(argument)) return false;
    } else return false;
    return true;
}

std::string ConsolePanel::StateJSON()
{
    using nlohmann::json;
    auto text = fLog->Text();
    if (text.size() > 256 * 1024) text.erase(0, text.size() - 256 * 1024);
    json state = {{"preserve", fPreserve->Value() == B_CONTROL_ON}, {"filter", fFilter->Text()},
        {"levels", {{"errors", fLevels.errors}, {"warnings", fLevels.warnings}, {"info", fLevels.info}, {"debug", fLevels.debug}}},
        {"shown", fLog->Count()}, {"counts", fCounts->Text()}, {"text", text}, {"objects", fObjects->Rows()}};
    return state.dump(-1, ' ', false, json::error_handler_t::replace);
}

void ConsolePanel::StepHistory(int direction)
{
    if (fHistory.empty()) return;
    if (direction < 0 && fHistoryIndex > 0) --fHistoryIndex;
    else if (direction > 0 && fHistoryIndex < fHistory.size()) ++fHistoryIndex;
    else return;
    const auto text = fHistoryIndex < fHistory.size() ? fHistory[fHistoryIndex] : std::string();
    fInput->SetText(text.c_str());
    fInput->TextView()->Select(static_cast<int32>(text.size()), static_cast<int32>(text.size()));
}

bool ConsolePanel::Shows(const ConsoleEntry& entry) const
{
    if (!fLevels.Shows(entry)) return false;
    return fFilterText.empty() || LowerCase(entry.text).find(fFilterText) != std::string::npos;
}

void ConsolePanel::Rebuild()
{
    fLog->Clear();
    fLastShown = 0;
    for (const auto& entry : fSession.Entries()) {
        if (!Shows(entry)) continue;
        fLog->Append(entry);
        fLastShown = entry.serial;
    }
    fLog->ScrollToEnd();
    ShowCounts();
}

void ConsolePanel::Inspect(uint64 serial)
{
    for (const auto& entry : fSession.Entries()) {
        if (entry.serial != serial) continue;
        if (entry.objects.empty()) return;
        fObjects->ShowObjects(entry.objects);
        if (fSplit->IsItemCollapsed(1)) fSplit->SetItemCollapsed(1, false);
        return;
    }
}

void ConsolePanel::Apply(const Changes& changes)
{
    if (!changes.properties.empty()) fObjects->Answered(changes.properties);
    if (changes.consoleReset) {
        fObjects->Clear();
        Rebuild();
        return;
    }
    if (!changes.consoleAdded && !changes.consoleLastChanged) return;
    const auto& entries = fSession.Entries();
    const bool follow = fLog->AtEnd();
    if (changes.consoleLastChanged && !entries.empty()) {
        const auto& last = entries[entries.size() - 1 - std::min(changes.consoleAdded, entries.size() - 1)];
        if (last.serial == fLastShown) {
            fLog->RemoveLast();
            fLog->Append(last);
        }
    }
    for (size_t i = entries.size() - std::min(changes.consoleAdded, entries.size()); i < entries.size(); ++i) {
        if (!Shows(entries[i])) continue;
        fLog->Append(entries[i]);
        fLastShown = entries[i].serial;
    }
    if (follow) fLog->ScrollToEnd();
    ShowCounts();
}

void ConsolePanel::ShowCounts()
{
    size_t errors = 0, warnings = 0, messages = 0;
    for (const auto& entry : fSession.Entries()) {
        if (entry.kind != EntryKind::Message) continue;
        ++messages;
        if (entry.level == Level::Error) ++errors;
        else if (entry.level == Level::Warning) ++warnings;
    }
    if (fSession.Entries().empty()) {
        fCounts->SetText("No messages.");
        return;
    }
    const auto count = [](size_t number, const char* one, const char* many) {
        return std::to_string(number) + " " + (number == 1 ? one : many);
    };
    auto text = count(messages, "message", "messages");
    if (errors) text += " · " + count(errors, "error", "errors");
    if (warnings) text += " · " + count(warnings, "warning", "warnings");
    if (const auto hidden = fSession.Entries().size() - fLog->Count()) text += " · " + std::to_string(hidden) + " hidden";
    fCounts->SetText(text.c_str());
}
}
#endif
