#if SUMMIT_MODERN_WEBKIT
#include "DevToolsPanels.h"
#include "core/McpInspector.h"
#include <interface/ColumnListView.h>
#include <interface/ColumnTypes.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <SplitView.h>
#include <StringView.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace summit {
using namespace devtools;
using nlohmann::json;
namespace {
constexpr uint32 kRecord = 'dprc', kRecordSelected = 'dprs';
enum { kTypeName, kTypeCount, kTypeTotal, kTypeLongest };
enum { kRecordAt, kRecordType, kRecordDuration, kRecordWhat, kRecordInside };

// Milliseconds as a number column sorts them.
class MillisecondsColumn : public BStringColumn {
public:
    MillisecondsColumn(const char* title, float width) : BStringColumn(title, width, 40, 400, B_TRUNCATE_END, B_ALIGN_RIGHT) { }
    int CompareFields(BField* first, BField* second) override
    {
        const double a = std::atof(static_cast<BStringField*>(first)->String());
        const double b = std::atof(static_cast<BStringField*>(second)->String());
        return a < b ? -1 : a > b ? 1 : 0;
    }
};

// A number of a summary (one that is missing or not a number is 0: a
// recording's records come from the page and must not throw).
double Number(const json& object, const char* key)
{
    if (!object.is_object()) return 0;
    const auto found = object.find(key);
    return found != object.end() && found->is_number() ? found->get<double>() : 0;
}
std::string Text(const json& object, const char* key)
{
    if (!object.is_object()) return { };
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

std::string Milliseconds(double value)
{
    char text[32];
    std::snprintf(text, sizeof text, value < 10 ? "%.1f" : "%.0f", value);
    return text;
}

// What a record was about, from its data and stack: the function and
// where it is, the script, the timer, the event.
std::string What(const json& entry)
{
    const auto data = entry.is_object() && entry.contains("data") && entry["data"].is_object() ? entry["data"] : json::object();
    std::string what;
    const auto text = [&](const char* key) { return data.contains(key) && data[key].is_string() ? data[key].get<std::string>() : std::string(); };
    if (const auto type = text("type"); !type.empty()) what = type;
    if (const auto name = text("functionName"); !name.empty()) what += (what.empty() ? "" : " ") + name;
    if (const auto url = text("url"); !url.empty()) {
        what += (what.empty() ? "" : " ") + url.substr(url.rfind('/') == std::string::npos ? 0 : url.rfind('/') + 1);
        if (data.contains("lineNumber") && data["lineNumber"].is_number()) what += ":" + std::to_string(static_cast<int>(Number(data, "lineNumber")));
    }
    if (data.contains("timerId") && data["timerId"].is_number()) what += (what.empty() ? "timer " : " timer ") + std::to_string(static_cast<int>(Number(data, "timerId")));
    if (what.empty() && entry.contains("stack") && entry["stack"].is_array() && !entry["stack"].empty() && entry["stack"][0].is_string())
        what = entry["stack"][0].get<std::string>();
    return what;
}
}

class PerformancePanel::Row : public BRow {
public:
    int index = -1;
};

PerformancePanel::PerformancePanel(Session& session, DevToolsHost& host)
    : BView("Performance", 0), fSession(session), fHost(host)
{
    fRecord = new BButton("record", "Record", new BMessage(kRecord));
    fRecord->SetToolTip("Record what the page's main thread does (scripts, style, layout, painting) until Stop");
    fStatus = new BStringView("status", "");
    fStatus->SetTruncation(B_TRUNCATE_END);
    fStatus->SetExplicitMinSize(BSize(40, B_SIZE_UNSET));
    fStatus->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    fTypes = new BColumnListView("types", 0, B_NO_BORDER);
    fTypes->AddColumn(new BStringColumn("Kind of work", 200, 60, 600, B_TRUNCATE_END), kTypeName);
    fTypes->AddColumn(new MillisecondsColumn("Count", 60), kTypeCount);
    fTypes->AddColumn(new MillisecondsColumn("Total ms", 80), kTypeTotal);
    fTypes->AddColumn(new MillisecondsColumn("Longest ms", 90), kTypeLongest);
    fRecords = new BColumnListView("records", 0, B_NO_BORDER);
    fRecords->AddColumn(new MillisecondsColumn("At ms", 70), kRecordAt);
    fRecords->AddColumn(new BStringColumn("Kind", 150, 60, 600, B_TRUNCATE_END), kRecordType);
    fRecords->AddColumn(new MillisecondsColumn("ms", 60), kRecordDuration);
    fRecords->AddColumn(new BStringColumn("What", 260, 60, 2000, B_TRUNCATE_MIDDLE), kRecordWhat);
    fRecords->AddColumn(new BStringColumn("Inside it", 260, 60, 2000, B_TRUNCATE_END), kRecordInside);
    fRecords->SetSelectionMessage(new BMessage(kRecordSelected));
    for (auto* list : {fTypes, fRecords}) {
        list->SetSelectionMode(B_SINGLE_SELECTION_LIST);
        list->SetSortingEnabled(true);
        list->SetExplicitMinSize(BSize(200, 60));
    }
    fDetail = new BodyView("detail");
    auto* split = new BSplitView(B_VERTICAL, 2);
    BLayoutBuilder::Split<>(split).Add(fTypes, 2).Add(fRecords, 3).Add(fDetail, 2);
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .AddGroup(B_HORIZONTAL, 8).SetInsets(8, 6, 8, 6)
            .Add(fRecord)
            .Add(fStatus, 1)
        .End()
        .Add(split, 1);
    Show();
}

void PerformancePanel::AttachedToWindow()
{
    BView::AttachedToWindow();
    AdoptParentColors();
    fRecord->SetTarget(this);
    fRecords->SetTarget(this);
}

void PerformancePanel::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case kRecord:
            fSession.RecordTimeline(!fSession.TimelineRecording());
            fHost.Sync();
            Show();
            break;
        case kRecordSelected: ShowRecord(); break;
        default: BView::MessageReceived(message);
    }
}

void PerformancePanel::Apply(const Changes& changes)
{
    if (!changes.timeline) return;
    // While recording only the count changes; the rest when it stops.
    if (fSession.TimelineRecording() == fWasRecording && fSession.TimelineRecording()) {
        ShowStatus();
        return;
    }
    Show();
}

void PerformancePanel::ShowStatus()
{
    const auto& records = fSession.TimelineRecords();
    if (fSession.TimelineRecording())
        fStatus->SetText(("Recording… " + std::to_string(records.size()) + (records.size() == 1 ? " record" : " records")).c_str());
}

void PerformancePanel::Show()
{
    const bool recording = fSession.TimelineRecording();
    fWasRecording = recording;
    fRecord->SetLabel(recording ? "Stop" : "Record");
    if (recording) {
        ShowStatus();
        return;
    }
    fTypes->Clear();
    fRecords->Clear();
    fLongest.clear();
    const auto& texts = fSession.TimelineRecords();
    if (texts.empty()) {
        fStatus->SetText("Record, use the page, then Stop: where its main thread's time went.");
        fDetail->ShowNote("");
        return;
    }
    std::vector<json> records;
    records.reserve(texts.size());
    for (const auto& text : texts)
        if (auto record = json::parse(text, nullptr, false); record.is_object()) records.push_back(std::move(record));
    json summary;
    try {
        summary = mcp::SummarizeTimeline(records, 200);
    } catch (const std::exception& error) {
        fStatus->SetText((std::string("The recording could not be read: ") + error.what()).c_str());
        return;
    }
    for (const auto& type : summary.value("byType", json::array())) {
        if (!type.is_object()) continue;
        auto* row = new BRow();
        row->SetField(new BStringField(Text(type, "type").c_str()), kTypeName);
        row->SetField(new BStringField(std::to_string(static_cast<long long>(Number(type, "count"))).c_str()), kTypeCount);
        row->SetField(new BStringField(Milliseconds(Number(type, "totalMs")).c_str()), kTypeTotal);
        row->SetField(new BStringField(Milliseconds(Number(type, "longestMs")).c_str()), kTypeLongest);
        fTypes->AddRow(row);
    }
    const auto longest = summary.value("longest", json::array());
    for (size_t i = 0; i < longest.size(); ++i) {
        const auto& entry = longest[i];
        fLongest.push_back(entry.dump(2));
        auto* row = new Row();
        row->index = static_cast<int>(i);
        row->SetField(new BStringField(Milliseconds(Number(entry, "atMs")).c_str()), kRecordAt);
        row->SetField(new BStringField(Text(entry, "type").c_str()), kRecordType);
        row->SetField(new BStringField(Milliseconds(Number(entry, "durationMs")).c_str()), kRecordDuration);
        row->SetField(new BStringField(What(entry).c_str()), kRecordWhat);
        std::string inside;
        if (const auto children = entry.find("childrenMs"); children != entry.end() && children->is_object())
            for (const auto& [type, ms] : children->items())
                inside += (inside.empty() ? "" : ", ") + type + " " + Milliseconds(ms.is_number() ? ms.get<double>() : 0);
        row->SetField(new BStringField(inside.c_str()), kRecordInside);
        fRecords->AddRow(row);
    }
    // The span, how busy the main thread was, and the frames drawn.
    std::string status = std::to_string(records.size()) + " records over " + Milliseconds(Number(summary, "spanMs")) + " ms";
    if (summary.contains("mainThreadBusyPercent"))
        status += " · main thread busy " + Milliseconds(Number(summary, "mainThreadBusyPercent")) + "%";
    if (const auto frames = summary.find("renderingFrames"); frames != summary.end() && frames->is_object())
        status += " · " + std::to_string(static_cast<long long>(Number(*frames, "count"))) + " frames, median " + Milliseconds(Number(*frames, "medianMs"))
            + " ms, 95th percentile " + Milliseconds(Number(*frames, "p95Ms")) + " ms";
    if (texts.size() >= Session::kTimelineRecords) status += " · the first " + std::to_string(Session::kTimelineRecords) + " records";
    fStatus->SetText(status.c_str());
    fDetail->ShowNote("Select a record to see its data and where it came from.");
}

void PerformancePanel::ShowRecord()
{
    auto* row = static_cast<Row*>(fRecords->CurrentSelection());
    if (!row || row->index < 0 || row->index >= static_cast<int>(fLongest.size())) {
        fDetail->ShowNote("");
        return;
    }
    fDetail->ShowBody(fLongest[row->index], "application/json");
}

bool PerformancePanel::Command(const std::string& action, const std::string&, std::string& error)
{
    if (action == "performance-record" || action == "performance-stop") {
        const bool on = action == "performance-record";
        if (on == fSession.TimelineRecording()) error = on ? "Already recording" : "Not recording";
        fSession.RecordTimeline(on);
        fHost.Sync();
        Show();
        return true;
    }
    return false;
}

std::string PerformancePanel::StateJSON()
{
    json types = json::array(), rows = json::array();
    for (int32 i = 0; i < fTypes->CountRows(); ++i) {
        auto* row = fTypes->RowAt(i);
        types.push_back({static_cast<BStringField*>(row->GetField(kTypeName))->String(), static_cast<BStringField*>(row->GetField(kTypeTotal))->String()});
    }
    for (int32 i = 0; i < fRecords->CountRows() && i < 20; ++i) {
        auto* row = fRecords->RowAt(i);
        json cells = json::array();
        for (int32 field = 0; field < row->CountFields(); ++field)
            cells.push_back(static_cast<BStringField*>(row->GetField(field))->String());
        rows.push_back(cells);
    }
    json state = {{"recording", fSession.TimelineRecording()}, {"records", fSession.TimelineRecords().size()}, {"status", fStatus->Text()},
        {"types", types}, {"longest", rows}};
    return state.dump(-1, ' ', false, json::error_handler_t::replace);
}
}
#endif
