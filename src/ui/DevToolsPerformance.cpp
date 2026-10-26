#if SUMMIT_MODERN_WEBKIT
#include "DevToolsPanels.h"
#include "SourceView.h"
#include "core/McpInspector.h"
#include <interface/ColumnListView.h>
#include <interface/ColumnTypes.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <SplitView.h>
#include <StringView.h>
#include <Window.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace summit {
using namespace devtools;
using nlohmann::json;
namespace {
constexpr uint32 kRecord = 'dprc', kRecordSelected = 'dprs', kFlameSelected = 'dpfs', kFit = 'dpft';
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

// The recording as a flame chart: time from left to right, each record a
// box under the one it happened in, coloured by kind (script, style and
// layout, painting). The wheel zooms where the pointer is, dragging moves
// along, and a click selects a record for the detail below.
class PerformancePanel::Flame : public BView {
public:
    Flame()
        : BView("flame", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_FRAME_EVENTS)
    {
        SetViewColor(B_TRANSPARENT_COLOR);
        SetExplicitMinSize(BSize(200, 90));
    }

    // The records, and the process's CPU use and memory meanwhile (time on
    // the records' clock, value), drawn as two strips over them.
    void SetRecords(std::vector<json>&& records, std::vector<std::pair<double, double>> cpu = { },
        std::vector<std::pair<double, double>> memory = { })
    {
        fRecords = std::move(records);
        fCPU = std::move(cpu);
        fMemory = std::move(memory);
        fBoxes.clear();
        fSelected = fHover = -1;
        fOrigin = INFINITY;
        double last = -INFINITY;
        for (const auto& record : fRecords) {
            if (!record.is_object()) continue;
            fOrigin = std::min(fOrigin, Number(record, "startTime"));
            last = std::max(last, std::max(Number(record, "endTime"), Number(record, "startTime")));
        }
        const auto walk = [&](auto&& self, const json& record, int depth) -> void {
            if (!record.is_object()) return;
            const double start = Number(record, "startTime");
            const double end = std::max(start, Number(record, "endTime"));
            fBoxes.push_back({start - fOrigin, end - fOrigin, depth, &record});
            fDepth = std::max(fDepth, depth + 1);
            if (const auto children = record.find("children"); children != record.end() && children->is_array())
                for (const auto& child : *children) self(self, child, depth + 1);
        };
        fDepth = 0;
        for (const auto& record : fRecords) walk(walk, record, 0);
        fSpan = std::isfinite(fOrigin) && last > fOrigin ? last - fOrigin : 0;
        Fit();
    }

    // The whole recording in view.
    void Fit()
    {
        fViewStart = 0;
        fViewEnd = fSpan > 0 ? fSpan : 1;
        Invalidate();
    }

    // Shows factor times as much time (less than 1 zooms in), keeping the
    // time under x where it is.
    void ZoomAt(double factor, float x)
    {
        const double width = std::max(1.f, Bounds().Width());
        const double at = fViewStart + (fViewEnd - fViewStart) * std::clamp(x / width, 0.0, 1.0);
        double length = std::clamp((fViewEnd - fViewStart) * factor, 0.00002, std::max(fSpan, 0.00002));
        fViewStart = at - (at - fViewStart) * length / (fViewEnd - fViewStart);
        fViewEnd = fViewStart + length;
        Clamp();
        Invalidate();
    }

    void Select(int index)
    {
        fSelected = index >= 0 && index < static_cast<int>(fBoxes.size()) ? index : -1;
        Invalidate();
        if (fTarget.IsValid()) fTarget.SendMessage(kFlameSelected);
    }

    // The longest record that has no record around it.
    int Longest() const
    {
        int longest = -1;
        for (size_t i = 0; i < fBoxes.size(); ++i)
            if (!fBoxes[i].depth && (longest < 0 || Duration(fBoxes[i]) > Duration(fBoxes[longest]))) longest = static_cast<int>(i);
        return longest;
    }

    // The selected record without its children (counted instead), and
    // when it started and how long it took, as JSON.
    std::string SelectedJSON() const
    {
        if (fSelected < 0) return { };
        const auto& box = fBoxes[fSelected];
        json record = *box.record;
        if (const auto children = record.find("children"); children != record.end() && children->is_array()) {
            const auto count = children->size();
            record.erase("children");
            record["childCount"] = count;
        }
        record["atMs"] = std::round(box.start * 10000) / 10;
        record["durationMs"] = std::round(Duration(box) * 10000) / 10;
        return record.dump(2, ' ', false, json::error_handler_t::replace);
    }

    json State() const
    {
        double cpuPeak = 0, memoryPeak = 0;
        for (const auto& sample : fCPU) cpuPeak = std::max(cpuPeak, sample.second);
        for (const auto& sample : fMemory) memoryPeak = std::max(memoryPeak, sample.second);
        json state = {{"boxes", fBoxes.size()}, {"depth", fDepth}, {"spanMs", std::round(fSpan * 10000) / 10},
            {"cpuSamples", fCPU.size()}, {"cpuPeakPercent", std::round(cpuPeak * 10) / 10},
            {"memorySamples", fMemory.size()}, {"memoryPeakMiB", std::round(memoryPeak / 1048576 * 10) / 10},
            {"viewMs", {std::round(fViewStart * 10000) / 10, std::round(fViewEnd * 10000) / 10}}, {"selected", nullptr}};
        if (fSelected >= 0) {
            const auto& box = fBoxes[fSelected];
            state["selected"] = {{"type", Text(*box.record, "type")}, {"depth", box.depth},
                {"atMs", std::round(box.start * 10000) / 10}, {"durationMs", std::round(Duration(box) * 10000) / 10}};
        }
        return state;
    }

    void SetTarget(BMessenger target) { fTarget = target; }

    void Draw(BRect update) override
    {
        const auto& palette = CurrentSourcePalette();
        const BRect bounds = Bounds();
        SetHighColor(palette.background);
        FillRect(update);
        BFont font(be_plain_font);
        font.SetSize(std::max(9.f, be_plain_font->Size() * .85f));
        SetFont(&font);
        font_height height;
        font.GetHeight(&height);
        const float lineHeight = std::ceil(height.ascent + height.descent);
        const float axis = lineHeight + 6, row = lineHeight + 4;
        const float boxesTop = BoxesTop(axis);
        if (fBoxes.empty()) {
            SetHighColor(palette.muted);
            DrawString("Record, use the page, then Stop: its main thread's records appear here.", BPoint(8, axis));
            return;
        }
        // The time axis: a step of 1, 2 or 5 times a power of ten, at
        // least 80 pixels apart.
        const double length = fViewEnd - fViewStart, scale = bounds.Width() / length;
        double step = 0.000001;
        for (int i = 0; step * scale < 80 && i < 30; ++i) step *= (i % 3 == 1) ? 2.5 : 2;
        SetHighColor(palette.border);
        StrokeLine(BPoint(bounds.left, axis - 1), BPoint(bounds.right, axis - 1));
        for (double t = std::ceil(fViewStart / step) * step; t <= fViewEnd; t += step) {
            const float x = static_cast<float>((t - fViewStart) * scale);
            SetHighColor(palette.border);
            StrokeLine(BPoint(x, axis - 4), BPoint(x, bounds.bottom));
            char label[32];
            const double ms = t * 1000;
            std::snprintf(label, sizeof label, step >= 0.001 ? "%.0f ms" : step >= 0.0001 ? "%.1f ms" : "%.2f ms", ms);
            SetHighColor(palette.muted);
            DrawString(label, BPoint(x + 3, height.ascent + 2));
        }
        // CPU (percent of a core, at least 100) and memory, as areas.
        float stripTop = axis + 1;
        const auto strip = [&](const std::vector<std::pair<double, double>>& samples, rgb_color color, const std::string& label, double ceiling) {
            if (samples.empty()) return;
            const BRect band(bounds.left, stripTop, bounds.right, stripTop + kStripHeight - 1);
            SetHighColor(palette.Tint(color, .12f));
            FillRect(band);
            std::vector<BPoint> points;
            points.emplace_back(BPoint(bounds.left, band.bottom));
            for (const auto& [time, value] : samples) {
                const float x = static_cast<float>((time - fOrigin - fViewStart) * scale);
                const float y = band.bottom - static_cast<float>(std::clamp(value / ceiling, 0.0, 1.0)) * (kStripHeight - 3);
                if (points.size() == 1) points.emplace_back(BPoint(x, band.bottom));
                points.emplace_back(BPoint(x, y));
            }
            points.emplace_back(BPoint(points.back().x, band.bottom));
            SetHighColor(palette.Tint(color, palette.dark ? .6f : .5f));
            FillPolygon(points.data(), static_cast<int32>(points.size()));
            SetHighColor(palette.muted);
            DrawString(label.c_str(), BPoint(bounds.left + 3, band.top + height.ascent + 1));
            stripTop += kStripHeight + 1;
        };
        if (!fCPU.empty()) {
            double peak = 0;
            for (const auto& sample : fCPU) peak = std::max(peak, sample.second);
            strip(fCPU, {230, 120, 60, 255}, "CPU (peak " + Milliseconds(peak) + " %)", std::max(100.0, peak));
        }
        if (!fMemory.empty()) {
            double peak = 0;
            for (const auto& sample : fMemory) peak = std::max(peak, sample.second);
            strip(fMemory, {70, 140, 210, 255}, "Memory (peak " + Milliseconds(peak / 1048576) + " MiB)", peak);
        }
        for (size_t i = 0; i < fBoxes.size(); ++i) {
            const auto& box = fBoxes[i];
            if (box.end < fViewStart || box.start > fViewEnd) continue;
            BRect frame((box.start - fViewStart) * scale, boxesTop + box.depth * row, (box.end - fViewStart) * scale, boxesTop + box.depth * row + row - 2);
            if (frame.top > update.bottom || frame.bottom < update.top) continue;
            frame.left = std::max(frame.left, bounds.left - 1);
            frame.right = std::min(std::max(frame.right, frame.left + 1), bounds.right + 1);
            SetHighColor(palette.Tint(KindColor(Text(*box.record, "type")), palette.dark ? .55f : .7f));
            FillRect(frame);
            if (frame.Width() > 6) {
                SetHighColor(palette.background);
                StrokeLine(frame.RightTop(), frame.RightBottom());
            }
            const std::string label = Label(box);
            if (frame.Width() > 24) {
                SetHighColor(palette.text);
                BString text(label.c_str());
                font.TruncateString(&text, B_TRUNCATE_END, frame.Width() - 6);
                DrawString(text.String(), BPoint(frame.left + 3, frame.top + height.ascent + 1));
            }
            if (static_cast<int>(i) == fSelected) {
                SetHighColor(palette.accent);
                SetPenSize(2);
                StrokeRect(frame.InsetByCopy(-1, -1));
                SetPenSize(1);
            }
        }
    }

    void MouseDown(BPoint where) override
    {
        if (!IsFocus()) MakeFocus(true);
        fPressed = true;
        fMoved = false;
        fPressedAt = where;
        fPressedStart = fViewStart;
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    }

    void MouseMoved(BPoint where, uint32 transit, const BMessage* dragged) override
    {
        BView::MouseMoved(where, transit, dragged);
        if (fPressed) {
            if (std::abs(where.x - fPressedAt.x) > 3) fMoved = true;
            if (fMoved) {
                const double length = fViewEnd - fViewStart;
                fViewStart = fPressedStart - (where.x - fPressedAt.x) * length / std::max(1.f, Bounds().Width());
                fViewEnd = fViewStart + length;
                Clamp();
                Invalidate();
            }
            return;
        }
        const int hover = transit == B_EXITED_VIEW ? -1 : BoxAt(where);
        if (hover == fHover) return;
        fHover = hover;
        if (hover < 0) {
            SetToolTip(static_cast<const char*>(nullptr));
            return;
        }
        const auto& box = fBoxes[hover];
        SetToolTip((Label(box) + " · " + Milliseconds(Duration(box) * 1000) + " ms at " + Milliseconds(box.start * 1000) + " ms").c_str());
        ShowToolTip(ToolTip());
    }

    void MouseUp(BPoint where) override
    {
        if (fPressed && !fMoved) Select(BoxAt(where));
        fPressed = false;
    }

    void MessageReceived(BMessage* message) override
    {
        if (message->what == B_MOUSE_WHEEL_CHANGED && !fBoxes.empty()) {
            float delta = 0;
            if (message->FindFloat("be:wheel_delta_y", &delta) == B_OK && delta) {
                BPoint where;
                uint32 buttons;
                GetMouse(&where, &buttons, false);
                ZoomAt(std::pow(1.25, delta), where.x);
            }
            return;
        }
        BView::MessageReceived(message);
    }

private:
    struct Box {
        double start, end;
        int depth;
        const json* record;
    };

    static double Duration(const Box& box) { return box.end - box.start; }

    static rgb_color KindColor(const std::string& type)
    {
        static const char* const script[] = {"EvaluateScript", "FunctionCall", "TimerFire", "TimerInstall", "TimerRemove",
            "EventDispatch", "ObserverCallback", "FireAnimationFrame", "RequestAnimationFrame", "CancelAnimationFrame",
            "ConsoleProfile", "TimeStamp", "Microtask"};
        static const char* const style[] = {"RecalculateStyles", "Layout", "InvalidateLayout", "ScheduleLayout",
            "ScheduleStyleRecalculation", "InvalidateStyles"};
        static const char* const paint[] = {"Paint", "Composite", "RenderingFrame", "Screenshot"};
        for (const char* name : script) if (type == name) return {232, 184, 46, 255};
        for (const char* name : style) if (type == name) return {155, 112, 220, 255};
        for (const char* name : paint) if (type == name) return {98, 176, 98, 255};
        return {150, 150, 150, 255};
    }

    static std::string Label(const Box& box)
    {
        std::string label = Text(*box.record, "type");
        if (const auto what = What(*box.record); !what.empty() && what != label) label += " " + what;
        return label;
    }

    int BoxAt(BPoint where) const
    {
        font_height height;
        BFont font(be_plain_font);
        font.SetSize(std::max(9.f, be_plain_font->Size() * .85f));
        font.GetHeight(&height);
        const float lineHeight = std::ceil(height.ascent + height.descent), axis = lineHeight + 6, row = lineHeight + 4;
        const float top = BoxesTop(axis);
        if (where.y < top) return -1;
        const int depth = static_cast<int>((where.y - top) / row);
        const double length = fViewEnd - fViewStart, perPixel = length / std::max(1.f, Bounds().Width());
        const double at = fViewStart + where.x * perPixel;
        // A box at least a pixel wide, so that a thin one can be hit.
        int found = -1;
        for (size_t i = 0; i < fBoxes.size(); ++i) {
            const auto& box = fBoxes[i];
            if (box.depth == depth && at >= box.start - perPixel && at <= std::max(box.end, box.start + perPixel)) {
                found = static_cast<int>(i);
                if (at >= box.start && at <= box.end) break;
            }
        }
        return found;
    }

    // Where the records start: under the time axis and the CPU and memory
    // strips there are.
    static constexpr float kStripHeight = 28;
    float BoxesTop(float axis) const
    {
        return axis + 1 + (fCPU.empty() ? 0 : kStripHeight + 1) + (fMemory.empty() ? 0 : kStripHeight + 1);
    }

    void Clamp()
    {
        const double length = fViewEnd - fViewStart;
        if (fViewStart < 0) fViewStart = 0;
        if (fSpan > 0 && fViewStart + length > fSpan) fViewStart = std::max(0.0, fSpan - length);
        fViewEnd = fViewStart + length;
    }

    std::vector<json> fRecords;
    std::vector<std::pair<double, double>> fCPU, fMemory;
    std::vector<Box> fBoxes;
    double fOrigin = 0, fSpan = 0, fViewStart = 0, fViewEnd = 1, fPressedStart = 0;
    int fDepth = 0, fSelected = -1, fHover = -1;
    bool fPressed = false, fMoved = false;
    BPoint fPressedAt;
    BMessenger fTarget;
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
    fFlame = new Flame();
    fFit = new BButton("fit", "Zoom to Fit", new BMessage(kFit));
    fFit->SetToolTip("Show the whole recording in the chart (the wheel zooms where the pointer is; drag to move along)");
    auto* lists = new BSplitView(B_HORIZONTAL, 2);
    BLayoutBuilder::Split<>(lists).Add(fTypes, 2).Add(fRecords, 3);
    auto* split = new BSplitView(B_VERTICAL, 2);
    BLayoutBuilder::Split<>(split).Add(fFlame, 3).Add(lists, 3).Add(fDetail, 2);
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .AddGroup(B_HORIZONTAL, 8).SetInsets(8, 6, 8, 6)
            .Add(fRecord)
            .Add(fFit)
            .Add(fStatus, 1)
        .End()
        .Add(split, 1);
    ShowRecording();
}

void PerformancePanel::AttachedToWindow()
{
    BView::AttachedToWindow();
    AdoptParentColors();
    fRecord->SetTarget(this);
    fRecords->SetTarget(this);
    fFit->SetTarget(this);
    fFlame->SetTarget(BMessenger(this));
}

void PerformancePanel::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case kRecord:
            fSession.RecordTimeline(!fSession.TimelineRecording());
            fHost.Sync();
            ShowRecording();
            break;
        case kRecordSelected: ShowRecord(); break;
        case kFlameSelected: ShowFlameRecord(); break;
        case kFit: fFlame->Fit(); break;
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
    ShowRecording();
}

void PerformancePanel::ShowStatus()
{
    const auto& records = fSession.TimelineRecords();
    if (fSession.TimelineRecording())
        fStatus->SetText(("Recording… " + std::to_string(records.size()) + (records.size() == 1 ? " record" : " records")).c_str());
}

void PerformancePanel::ShowRecording()
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
    fFit->SetEnabled(!texts.empty());
    if (texts.empty()) {
        fFlame->SetRecords({ });
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
        fFlame->SetRecords({ });
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
    if (const auto& cpu = fSession.TimelineCPU(); !cpu.empty()) {
        double total = 0, peak = 0;
        for (const auto& sample : cpu) {
            total += sample.second;
            peak = std::max(peak, sample.second);
        }
        status += " · CPU " + Milliseconds(total / cpu.size()) + " % on average, " + Milliseconds(peak) + " % at most";
    }
    if (const auto& memory = fSession.TimelineMemory(); !memory.empty()) {
        double least = memory.front().second, most = least;
        for (const auto& sample : memory) {
            least = std::min(least, sample.second);
            most = std::max(most, sample.second);
        }
        status += " · memory " + Milliseconds(least / 1048576) + "–" + Milliseconds(most / 1048576) + " MiB";
    }
    if (texts.size() >= Session::kTimelineRecords) status += " · the first " + std::to_string(Session::kTimelineRecords) + " records";
    fStatus->SetText(status.c_str());
    fDetail->ShowNote("Select a record to see its data and where it came from.");
    fFlame->SetRecords(std::move(records), fSession.TimelineCPU(), fSession.TimelineMemory());
}

void PerformancePanel::ShowFlameRecord()
{
    const auto text = fFlame->SelectedJSON();
    if (text.empty()) fDetail->ShowNote("");
    else fDetail->ShowBody(text, "application/json");
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

bool PerformancePanel::Command(const std::string& action, const std::string& argument, std::string& error)
{
    if (action == "performance-record" || action == "performance-stop") {
        const bool on = action == "performance-record";
        if (on == fSession.TimelineRecording()) error = on ? "Already recording" : "Not recording";
        fSession.RecordTimeline(on);
        fHost.Sync();
        ShowRecording();
        return true;
    }
    // Test actions: the flame chart's longest record (or the Nth box)
    // selected, a zoom by a factor around a point (x in pixels), and Zoom to
    // Fit.
    if (action == "performance-flame-select") {
        fFlame->Select(argument.empty() || argument == "longest" ? fFlame->Longest() : std::atoi(argument.c_str()));
        ShowFlameRecord();
        return true;
    }
    if (action == "performance-flame-zoom") {
        double factor = 0.5;
        float x = fFlame->Bounds().Width() / 2;
        std::sscanf(argument.c_str(), "%lf %f", &factor, &x);
        if (factor > 0) fFlame->ZoomAt(factor, x);
        return true;
    }
    if (action == "performance-flame-fit") {
        fFlame->Fit();
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
        {"types", types}, {"longest", rows}, {"flame", fFlame->State()}};
    return state.dump(-1, ' ', false, json::error_handler_t::replace);
}
}
#endif
