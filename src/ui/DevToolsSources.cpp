#if SUMMIT_MODERN_WEBKIT
#include "DevToolsPanels.h"
#include <Button.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <ScrollView.h>
#include <SplitView.h>
#include <StringView.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <functional>

namespace summit {
using namespace devtools;
namespace {
constexpr uint32 kScriptSelected = 'dsss', kFrameSelected = 'dsfs', kPauseResume = 'dspr', kStepOver = 'dsso', kStepInto = 'dssi',
    kStepOut = 'dssu', kContentScriptsChanged = 'dscs', kPrettyChanged = 'dspp';
// The margin of breakpoints and the markers in it; the paused line's background.
constexpr int kBreakpointMargin = 2;
enum { kBreakpointMark = 1, kPausedMark = 2, kPausedLine = 3 };

// A place in a script's document as a place in its own text, and back.
SourcePosition Relative(const Script& script, int line, int column)
{
    const int relative = line - script.startLine;
    return {relative, relative == 0 ? std::max(column - script.startColumn, 0) : column};
}
SourcePosition Absolute(const Script& script, SourcePosition place)
{
    return {script.startLine + place.line, place.line == 0 ? script.startColumn + place.column : place.column};
}

// The last part of an original source's address, for labels.
std::string ShortName(const std::string& address)
{
    auto path = address.substr(0, address.find_first_of("?#"));
    const auto scheme = path.find("://");
    if (scheme != std::string::npos) {
        const auto slash = path.find('/', scheme + 3);
        path = slash == std::string::npos ? std::string() : path.substr(slash + 1);
    }
    while (path.rfind("./", 0) == 0) path.erase(0, 2);
    return path.empty() ? address : path;
}

// An original source of a mapped script is listed as "SCRIPT#INDEX".
bool OriginalEntry(const std::string& entry, std::string& scriptId, int& index)
{
    const auto hash = entry.find('#');
    if (hash == std::string::npos) return false;
    scriptId = entry.substr(0, hash);
    index = std::atoi(entry.c_str() + hash + 1);
    return true;
}

std::string FrameLabel(const Session& session, const DebugFrame& frame)
{
    const auto* script = session.FindScript(frame.scriptId);
    auto where = script ? script->Place(frame.line + 1) : std::string("?");
    // Where it is in the original source, when the script has a map.
    if (script && script->map)
        if (const auto* original = script->map->Original(Relative(*script, frame.line, frame.column)))
            where = ShortName(script->map->sources[original->source]) + ":" + std::to_string(original->line + 1);
    return (frame.function.empty() ? std::string("(anonymous)") : frame.function) + "  " + where;
}
}

// The selected script's source, read-only with line numbers. A click in the
// margin beside a line sets or clears its breakpoint.
class SourcesPanel::Code : public SourceView {
public:
    explicit Code(std::function<void(int)> toggled)
        : SourceView("code", true, true), fToggled(std::move(toggled))
    {
        SendMessage(SCI_SETMARGINTYPEN, kBreakpointMargin, SC_MARGIN_SYMBOL);
        SendMessage(SCI_SETMARGINMASKN, kBreakpointMargin, (1 << kBreakpointMark) | (1 << kPausedMark));
        SendMessage(SCI_SETMARGINWIDTHN, kBreakpointMargin, 16);
        SendMessage(SCI_SETMARGINSENSITIVEN, kBreakpointMargin, 1);
        SendMessage(SCI_SETMARGINCURSORN, kBreakpointMargin, SC_CURSORARROW);
        ApplyPalette();
    }
    void NotificationReceived(SCNotification* notification) override
    {
        SourceView::NotificationReceived(notification);
        if (notification->nmhdr.code == SCN_MARGINCLICK && notification->margin == kBreakpointMargin && fToggled)
            fToggled(static_cast<int>(SendMessage(SCI_LINEFROMPOSITION, notification->position)));
    }
    void Mark(const std::vector<int>& breakpoints, int pausedLine)
    {
        SendMessage(SCI_MARKERDELETEALL, kBreakpointMark);
        SendMessage(SCI_MARKERDELETEALL, kPausedMark);
        SendMessage(SCI_MARKERDELETEALL, kPausedLine);
        for (const int line : breakpoints)
            if (line >= 0) SendMessage(SCI_MARKERADD, line, kBreakpointMark);
        if (pausedLine >= 0) {
            SendMessage(SCI_MARKERADD, pausedLine, kPausedMark);
            SendMessage(SCI_MARKERADD, pausedLine, kPausedLine);
            SendMessage(SCI_ENSUREVISIBLEENFORCEPOLICY, pausedLine);
            SendMessage(SCI_GOTOLINE, pausedLine);
        }
    }
    int Lines() { return static_cast<int>(SendMessage(SCI_GETLINECOUNT)); }
    // For the state: shown lines from 1 with a marker.
    int PausedLine() { const auto line = SendMessage(SCI_MARKERNEXT, 0, 1 << kPausedMark); return line < 0 ? 0 : static_cast<int>(line) + 1; }
    std::vector<int> BreakpointLines()
    {
        std::vector<int> lines;
        for (auto line = SendMessage(SCI_MARKERNEXT, 0, 1 << kBreakpointMark); line >= 0; line = SendMessage(SCI_MARKERNEXT, line + 1, 1 << kBreakpointMark))
            lines.push_back(static_cast<int>(line) + 1);
        return lines;
    }

protected:
    void ApplyPalette() override
    {
        SourceView::ApplyPalette();
        const auto& palette = CurrentSourcePalette();
        SendMessage(SCI_MARKERDEFINE, kBreakpointMark, SC_MARK_CIRCLE);
        SendMessage(SCI_MARKERSETFORE, kBreakpointMark, SciColor(palette.removed));
        SendMessage(SCI_MARKERSETBACK, kBreakpointMark, SciColor(palette.removed));
        SendMessage(SCI_MARKERDEFINE, kPausedMark, SC_MARK_SHORTARROW);
        SendMessage(SCI_MARKERSETFORE, kPausedMark, SciColor(palette.accent));
        SendMessage(SCI_MARKERSETBACK, kPausedMark, SciColor(palette.accent));
        SendMessage(SCI_MARKERDEFINE, kPausedLine, SC_MARK_BACKGROUND);
        SendMessage(SCI_MARKERSETBACK, kPausedLine, SciColor(palette.Tint(palette.accent, palette.dark ? .25f : .15f)));
        SendMessage(SCI_SETMARGINBACKN, kBreakpointMargin, SciColor(palette.background));
    }

private:
    std::function<void(int)> fToggled;
};

SourcesPanel::SourcesPanel(Session& session, DevToolsHost& host)
    : BView("Sources", 0), fSession(session), fHost(host)
{
    fPauseResume = new BButton("pause", "Pause", new BMessage(kPauseResume));
    fPauseResume->SetToolTip("Pause the page's script where it is, or let it run on");
    fStepOver = new BButton("step-over", "Step Over", new BMessage(kStepOver));
    fStepInto = new BButton("step-into", "Step Into", new BMessage(kStepInto));
    fStepOut = new BButton("step-out", "Step Out", new BMessage(kStepOut));
    fContentScripts = new BCheckBox("content-scripts", "Extension scripts", new BMessage(kContentScriptsChanged));
    fContentScripts->SetToolTip("List the scripts extensions and the browser add to pages");
    fPrettyPrint = new BCheckBox("pretty-print", "Pretty print", new BMessage(kPrettyChanged));
    fPrettyPrint->SetToolTip("Lay the script out a statement per line (breakpoints and the paused line follow)");
    fStatus = new BStringView("status", "");
    fStatus->SetTruncation(B_TRUNCATE_END);
    fStatus->SetExplicitMinSize(BSize(40, B_SIZE_UNSET));
    fStatus->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    fScripts = new BListView("scripts");
    fScripts->SetSelectionMessage(new BMessage(kScriptSelected));
    fCode = new Code([this](int line) { ToggleBreakpoint(line); });
    fFrames = new BListView("frames");
    fFrames->SetSelectionMessage(new BMessage(kFrameSelected));
    fScopes = new ObjectTree(session, host);
    auto* scripts = new BScrollView("scripts-scroll", fScripts, 0, true, true, B_NO_BORDER);
    auto* frames = new BScrollView("frames-scroll", fFrames, 0, false, true, B_NO_BORDER);
    auto* scopes = new BScrollView("scopes-scroll", fScopes, 0, true, true, B_NO_BORDER);
    auto* right = new BSplitView(B_VERTICAL, 2);
    BLayoutBuilder::Split<>(right).Add(frames, 1).Add(scopes, 2);
    auto* split = new BSplitView(B_HORIZONTAL, 2);
    BLayoutBuilder::Split<>(split).Add(scripts, 1).Add(fCode, 3).Add(right, 2);
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .AddGroup(B_HORIZONTAL, 8).SetInsets(8, 6, 8, 6)
            .Add(fPauseResume)
            .Add(fStepOver)
            .Add(fStepInto)
            .Add(fStepOut)
            .Add(fStatus, 1)
            .Add(fPrettyPrint)
            .Add(fContentScripts)
        .End()
        .Add(split, 1);
    ShowPause();
}

void SourcesPanel::AttachedToWindow()
{
    BView::AttachedToWindow();
    AdoptParentColors();
    for (BControl* control : std::initializer_list<BControl*> {fPauseResume, fStepOver, fStepInto, fStepOut, fContentScripts, fPrettyPrint})
        control->SetTarget(this);
    fScripts->SetTarget(this);
    fFrames->SetTarget(this);
}

void SourcesPanel::SetShown(bool shown)
{
    // The debugger costs the page something: it is turned on when looked at.
    if (!shown || fShownOnce) return;
    fShownOnce = true;
    fSession.EnableDebugger();
    fHost.Sync();
}

const Script* SourcesPanel::Selected() const
{
    std::string scriptId;
    int index = 0;
    return fSession.FindScript(OriginalEntry(fSelectedScript, scriptId, index) ? scriptId : fSelectedScript);
}

void SourcesPanel::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case kScriptSelected: {
            const int32 index = fScripts->CurrentSelection();
            if (index >= 0 && index < static_cast<int32>(fScriptIDs.size()) && fScriptIDs[index] != fSelectedScript)
                SelectScript(fScriptIDs[index]);
            break;
        }
        case kFrameSelected:
            if (const int32 index = fFrames->CurrentSelection(); index >= 0 && index != fFrame) SelectFrame(index);
            break;
        case kPauseResume:
            fSession.DebuggerCommand(fSession.Debugger().paused ? "resume" : "pause");
            fHost.Sync();
            break;
        case kStepOver:
        case kStepInto:
        case kStepOut:
            fSession.DebuggerCommand(message->what == kStepOver ? "stepOver" : message->what == kStepInto ? "stepInto" : "stepOut");
            fHost.Sync();
            break;
        case kContentScriptsChanged: RebuildScripts(); break;
        case kPrettyChanged:
            fPrettyChosen = true;
            ShowSource();
            break;
        default: BView::MessageReceived(message);
    }
}

void SourcesPanel::Apply(const Changes& changes)
{
    // A source map read adds its sources to the list.
    if (changes.scripts || !changes.sources.empty()) RebuildScripts();
    if (const auto* script = Selected();
        script && std::find(changes.sources.begin(), changes.sources.end(), script->id) != changes.sources.end())
        ShowSource();
    if (changes.debugger) ShowPause();
    if (!changes.properties.empty()) fScopes->Answered(changes.properties);
}

void SourcesPanel::RebuildScripts()
{
    const bool all = fContentScripts->Value() == B_CONTROL_ON;
    std::vector<const Script*> shown;
    for (const auto& [id, script] : fSession.Scripts()) {
        // The page's own: not what extensions and the browser add
        // (user-script:N), nor eval()'s.
        if (!all && (script.contentScript || script.url.empty() || script.url.rfind("user-script:", 0) == 0)) continue;
        shown.push_back(&script);
    }
    std::stable_sort(shown.begin(), shown.end(), [](const Script* a, const Script* b) { return a->Label() < b->Label(); });
    // Each script, then the original sources its map names.
    std::vector<std::string> ids, labels;
    for (const auto* script : shown) {
        ids.push_back(script->id);
        labels.push_back(script->Label());
        if (fShownOnce && !script->sourceMapURL.empty()) fSession.LoadSourceMap(script->id);
        if (!script->map) continue;
        for (size_t i = 0; i < script->map->sources.size(); ++i) {
            ids.push_back(script->id + "#" + std::to_string(i));
            labels.push_back("    ↳ " + ShortName(script->map->sources[i]));
        }
    }
    fHost.Sync();
    if (ids == fScriptIDs) return;
    fScriptIDs = std::move(ids);
    for (int32 i = fScripts->CountItems() - 1; i >= 0; --i) delete fScripts->RemoveItem(i);
    int32 selected = -1;
    for (size_t i = 0; i < fScriptIDs.size(); ++i) {
        fScripts->AddItem(new BStringItem(labels[i].c_str()));
        if (fScriptIDs[i] == fSelectedScript) selected = static_cast<int32>(i);
    }
    if (selected >= 0) fScripts->Select(selected);
    else if (!Selected()) {
        // The page moved on: its scripts are new ones.
        fSelectedScript.clear();
        fCode->SetSource("", "null");
    }
}

void SourcesPanel::SelectScript(const std::string& id)
{
    fSelectedScript = id;
    std::string scriptId;
    int index = 0;
    fSession.LoadScriptSource(OriginalEntry(id, scriptId, index) ? scriptId : id);
    fHost.Sync();
    ShowSource();
}

void SourcesPanel::ShowSource()
{
    const auto* script = Selected();
    if (!script) return;
    // An original source: the map's text of it.
    if (const int original = OriginalIndex(); original >= 0) {
        fPrettyPrint->SetEnabled(false);
        if (!script->map || original >= static_cast<int>(script->map->sources.size())) return;
        fCode->SetSource(script->map->hasContent[original] ? script->map->contents[original]
            : "The source map has no text of " + script->map->sources[original] + ".", script->map->hasContent[original] ? "cpp" : "null");
        ShowMarkers();
        return;
    }
    fPrettyPrint->SetEnabled(true);
    if (!script->sourceLoaded) {
        fCode->SetSource("Loading…", "null");
        return;
    }
    if (!script->sourceError.empty()) {
        fCode->SetSource("The source is not available: " + script->sourceError, "null");
        return;
    }
    // A minified script (a line longer than a screen) is laid out, unless
    // pretty printing was chosen by hand.
    if (!fPrettyChosen) {
        bool minified = false;
        for (size_t start = 0; start < script->source.size() && !minified;) {
            const auto end = script->source.find('\n', start);
            minified = (end == std::string::npos ? script->source.size() : end) - start > 500;
            if (end == std::string::npos) break;
            start = end + 1;
        }
        fPrettyPrint->SetValue(minified ? B_CONTROL_ON : B_CONTROL_OFF);
    }
    fCode->SetSource(Pretty() ? Formatted(*script).text : script->source, "cpp");
    ShowMarkers();
}

bool SourcesPanel::Pretty() const { return fPrettyPrint->Value() == B_CONTROL_ON && OriginalIndex() < 0; }

int SourcesPanel::OriginalIndex() const
{
    std::string scriptId;
    int index = 0;
    return OriginalEntry(fSelectedScript, scriptId, index) ? index : -1;
}

const PrettyScript& SourcesPanel::Formatted(const Script& script)
{
    auto found = fFormatted.find(script.id);
    if (found == fFormatted.end()) {
        // Scripts of pages long gone are not kept.
        if (fFormatted.size() > 32) fFormatted.clear();
        found = fFormatted.emplace(script.id, FormatJavaScript(script.source)).first;
    }
    return found->second;
}

// A place in the document (a frame's or a breakpoint's) as a line shown;
// -1 when it is not in the original source shown.
int SourcesPanel::ShownLine(const Script& script, int line, int column)
{
    const auto relative = Relative(script, line, column);
    if (const int original = OriginalIndex(); original >= 0) {
        const auto* mapping = script.map ? script.map->Original(relative) : nullptr;
        return mapping && mapping->source == original ? mapping->line : -1;
    }
    if (!Pretty()) return relative.line;
    return Formatted(script).LineOf(relative);
}

SourcePosition SourcesPanel::DocumentPlace(const Script& script, int shownLine)
{
    if (const int original = OriginalIndex(); original >= 0) {
        if (!script.map) return {-1, 0};
        const auto generated = script.map->Generated(original, shownLine);
        return generated.line < 0 ? generated : Absolute(script, generated);
    }
    if (!Pretty()) return {script.startLine + shownLine, 0};
    const auto& starts = Formatted(script).starts;
    if (shownLine < 0 || shownLine >= static_cast<int>(starts.size())) return {-1, 0};
    return Absolute(script, starts[shownLine]);
}

void SourcesPanel::ShowMarkers()
{
    const auto* script = Selected();
    if (!script || (!script->sourceLoaded && OriginalIndex() < 0)) return;
    // Lines of an inline script count from its document's start.
    std::vector<int> lines;
    for (const auto& breakpoint : fSession.Breakpoints())
        if (breakpoint.url == script->url) lines.push_back(ShownLine(*script, breakpoint.line, breakpoint.column));
    int paused = -1;
    const auto& state = fSession.Debugger();
    if (state.paused && fFrame >= 0 && fFrame < static_cast<int>(state.frames.size()) && state.frames[fFrame].scriptId == script->id)
        paused = ShownLine(*script, state.frames[fFrame].line, state.frames[fFrame].column);
    lines.erase(std::remove(lines.begin(), lines.end(), -1), lines.end());
    fCode->Mark(lines, paused);
}

void SourcesPanel::ToggleBreakpoint(int line)
{
    const auto* script = Selected();
    if (!script || script->url.empty() || (!script->sourceLoaded && OriginalIndex() < 0)) return;
    // One shown on this line is cleared; else one is set where it starts.
    for (const auto& breakpoint : fSession.Breakpoints()) {
        if (breakpoint.url != script->url || ShownLine(*script, breakpoint.line, breakpoint.column) != line) continue;
        fSession.RemoveBreakpoint(breakpoint.url, breakpoint.line, breakpoint.column);
        fHost.Sync();
        ShowMarkers();
        return;
    }
    const auto place = DocumentPlace(*script, line);
    if (place.line < 0) return;
    fSession.SetBreakpoint(script->url, place.line, place.column);
    fHost.Sync();
    ShowMarkers();
}

void SourcesPanel::ShowPause()
{
    const auto& state = fSession.Debugger();
    fPauseResume->SetLabel(state.paused ? "Resume" : "Pause");
    for (BControl* control : std::initializer_list<BControl*> {fStepOver, fStepInto, fStepOut}) control->SetEnabled(state.paused);
    fPauseResume->SetEnabled(state.enabled);
    for (int32 i = fFrames->CountItems() - 1; i >= 0; --i) delete fFrames->RemoveItem(i);
    if (!state.paused) {
        fFrame = -1;
        fScopes->Clear();
        fStatus->SetText(state.enabled ? "Running" : "");
        ShowMarkers();
        return;
    }
    for (const auto& frame : state.frames) fFrames->AddItem(new BStringItem(FrameLabel(fSession, frame).c_str()));
    const auto top = state.frames.empty() ? std::string() : FrameLabel(fSession, state.frames.front());
    fStatus->SetText(("Paused (" + (state.reason.empty() ? std::string("other") : state.reason) + ")" + (top.empty() ? "" : " in " + top)).c_str());
    fFrame = -1;
    SelectFrame(state.frames.empty() ? -1 : 0);
}

void SourcesPanel::SelectFrame(int index)
{
    const auto& state = fSession.Debugger();
    if (index < 0 || index >= static_cast<int>(state.frames.size())) return;
    fFrame = index;
    fSession.SelectDebugFrame(index);
    if (fFrames->CurrentSelection() != index) fFrames->Select(index);
    const auto& frame = state.frames[index];
    // Its script is shown, at the paused line: its original source when its
    // map has the place and the text.
    std::string entry = frame.scriptId;
    if (const auto* script = fSession.FindScript(frame.scriptId); script && script->map)
        if (const auto* original = script->map->Original(Relative(*script, frame.line, frame.column));
            original && script->map->hasContent[original->source])
            entry = frame.scriptId + "#" + std::to_string(original->source);
    if (entry != fSelectedScript && fSession.FindScript(frame.scriptId)) {
        fSelectedScript = entry;
        if (const auto found = std::find(fScriptIDs.begin(), fScriptIDs.end(), entry); found != fScriptIDs.end())
            fScripts->Select(static_cast<int32>(found - fScriptIDs.begin()));
        fSession.LoadScriptSource(frame.scriptId);
        fHost.Sync();
        ShowSource();
    } else ShowMarkers();
    // Its scopes, the innermost open.
    std::vector<ConsoleObject> scopes;
    for (const auto& scope : frame.scopes) {
        if (scope.objectId.empty()) continue;
        auto label = scope.type.empty() ? std::string("Scope") : scope.type;
        label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
        if (!scope.name.empty()) label += " (" + scope.name + ")";
        scopes.push_back({label, scope.objectId});
    }
    if (!frame.thisObjectId.empty()) scopes.push_back({"this", frame.thisObjectId});
    fScopes->ShowObjects(scopes);
    if (scopes.size() > 1) fScopes->OpenRow(scopes.front().label);
}

bool SourcesPanel::Command(const std::string& action, const std::string& argument, std::string& error)
{
    if (action.rfind("sources-", 0) != 0) return false;
    const auto verb = action.substr(8);
    if (verb == "open") {
        // The first script whose name or address contains the argument.
        for (const auto& id : fScriptIDs) {
            std::string scriptId;
            int index = 0;
            if (OriginalEntry(id, scriptId, index)) {
                // An original source, by its address.
                const auto* script = fSession.FindScript(scriptId);
                if (script && script->map && index < static_cast<int>(script->map->sources.size())
                    && script->map->sources[index].find(argument) != std::string::npos) {
                    fScripts->Select(static_cast<int32>(std::find(fScriptIDs.begin(), fScriptIDs.end(), id) - fScriptIDs.begin()));
                    SelectScript(id);
                    return true;
                }
                continue;
            }
            const auto* script = fSession.FindScript(id);
            if (script && (script->Label().find(argument) != std::string::npos || script->url.find(argument) != std::string::npos)) {
                fScripts->Select(static_cast<int32>(std::find(fScriptIDs.begin(), fScriptIDs.end(), id) - fScriptIDs.begin()));
                SelectScript(id);
                return true;
            }
        }
        error = "No script shown has that in its name";
    } else if (verb == "break") ToggleBreakpoint(std::atoi(argument.c_str()) - 1);
    else if (verb == "pause" || verb == "resume") {
        fSession.DebuggerCommand(verb);
        fHost.Sync();
    } else if (verb == "step") {
        fSession.DebuggerCommand(argument == "into" ? "stepInto" : argument == "out" ? "stepOut" : "stepOver");
        fHost.Sync();
    } else if (verb == "frame") SelectFrame(std::atoi(argument.c_str()));
    else if (verb == "expand") {
        if (!fScopes->OpenRow(argument)) error = "No such row";
    } else if (verb == "pretty") {
        fPrettyPrint->SetValue(argument == "on" ? B_CONTROL_ON : B_CONTROL_OFF);
        fPrettyChosen = true;
        ShowSource();
    } else if (verb == "content-scripts") {
        fContentScripts->SetValue(argument == "on" ? B_CONTROL_ON : B_CONTROL_OFF);
        RebuildScripts();
    } else error = "Unknown action";
    return true;
}

std::string SourcesPanel::StateJSON()
{
    using nlohmann::json;
    const auto& state = fSession.Debugger();
    json scripts = json::array(), breakpoints = json::array(), frames = json::array();
    for (int32 i = 0; i < fScripts->CountItems(); ++i) scripts.push_back(static_cast<BStringItem*>(fScripts->ItemAt(i))->Text());
    for (const auto& breakpoint : fSession.Breakpoints())
        breakpoints.push_back(breakpoint.url + ":" + std::to_string(breakpoint.line + 1)
            + (breakpoint.column ? ":" + std::to_string(breakpoint.column + 1) : std::string()) + (breakpoint.resolved ? "" : " (unresolved)"));
    for (const auto& frame : state.frames) frames.push_back(FrameLabel(fSession, frame));
    const auto* script = Selected();
    const int original = OriginalIndex();
    json result = {{"enabled", state.enabled}, {"scripts", scripts}, {"selected", script ? script->Label() : std::string()},
        {"original", original >= 0 && script && script->map && original < static_cast<int>(script->map->sources.size())
            ? script->map->sources[original] : std::string()}, {"mapError", script ? script->mapError : std::string()},
        {"sourceLoaded", script && script->sourceLoaded}, {"lines", script && script->sourceLoaded ? fCode->Lines() : 0},
        {"breakpoints", breakpoints}, {"paused", state.paused}, {"reason", state.reason}, {"frames", frames}, {"frame", fFrame},
        {"scopes", fScopes->Rows()}, {"status", fStatus->Text()}, {"pretty", Pretty()}, {"pausedLine", fCode->PausedLine()},
        {"breakpointLines", fCode->BreakpointLines()}};
    return result.dump(-1, ' ', false, json::error_handler_t::replace);
}
}
#endif
