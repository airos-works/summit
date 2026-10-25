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
    kStepOut = 'dssu', kContentScriptsChanged = 'dscs';
// The margin of breakpoints and the markers in it; the paused line's background.
constexpr int kBreakpointMargin = 2;
enum { kBreakpointMark = 1, kPausedMark = 2, kPausedLine = 3 };

std::string FrameLabel(const Session& session, const DebugFrame& frame)
{
    const auto* script = session.FindScript(frame.scriptId);
    const auto where = script ? script->Place(frame.line + 1) : std::string("?");
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
            .Add(fContentScripts)
        .End()
        .Add(split, 1);
    ShowPause();
}

void SourcesPanel::AttachedToWindow()
{
    BView::AttachedToWindow();
    AdoptParentColors();
    for (BControl* control : std::initializer_list<BControl*> {fPauseResume, fStepOver, fStepInto, fStepOut, fContentScripts})
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

const Script* SourcesPanel::Selected() const { return fSession.FindScript(fSelectedScript); }

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
        default: BView::MessageReceived(message);
    }
}

void SourcesPanel::Apply(const Changes& changes)
{
    if (changes.scripts) RebuildScripts();
    if (std::find(changes.sources.begin(), changes.sources.end(), fSelectedScript) != changes.sources.end()) ShowSource();
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
    std::vector<std::string> ids;
    for (const auto* script : shown) ids.push_back(script->id);
    if (ids == fScriptIDs) return;
    fScriptIDs = std::move(ids);
    for (int32 i = fScripts->CountItems() - 1; i >= 0; --i) delete fScripts->RemoveItem(i);
    int32 selected = -1;
    for (size_t i = 0; i < shown.size(); ++i) {
        fScripts->AddItem(new BStringItem(shown[i]->Label().c_str()));
        if (shown[i]->id == fSelectedScript) selected = static_cast<int32>(i);
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
    fSession.LoadScriptSource(id);
    fHost.Sync();
    ShowSource();
}

void SourcesPanel::ShowSource()
{
    const auto* script = Selected();
    if (!script) return;
    if (!script->sourceLoaded) {
        fCode->SetSource("Loading…", "null");
        return;
    }
    fCode->SetSource(script->sourceError.empty() ? script->source : "The source is not available: " + script->sourceError, "cpp");
    ShowMarkers();
}

void SourcesPanel::ShowMarkers()
{
    const auto* script = Selected();
    if (!script || !script->sourceLoaded) return;
    // Lines of an inline script count from its document's start.
    std::vector<int> lines;
    for (const auto& breakpoint : fSession.Breakpoints())
        if (breakpoint.url == script->url) lines.push_back(breakpoint.line - script->startLine);
    int paused = -1;
    const auto& state = fSession.Debugger();
    if (state.paused && fFrame >= 0 && fFrame < static_cast<int>(state.frames.size()) && state.frames[fFrame].scriptId == script->id)
        paused = state.frames[fFrame].line - script->startLine;
    fCode->Mark(lines, paused);
}

void SourcesPanel::ToggleBreakpoint(int line)
{
    const auto* script = Selected();
    if (!script || script->url.empty() || !script->sourceLoaded) return;
    const int documentLine = line + script->startLine;
    const auto& breakpoints = fSession.Breakpoints();
    const bool set = std::any_of(breakpoints.begin(), breakpoints.end(),
        [&](const Breakpoint& breakpoint) { return breakpoint.url == script->url && breakpoint.line == documentLine; });
    if (set) fSession.RemoveBreakpoint(script->url, documentLine);
    else fSession.SetBreakpoint(script->url, documentLine);
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
    if (fFrames->CurrentSelection() != index) fFrames->Select(index);
    const auto& frame = state.frames[index];
    // Its script is shown, at the paused line.
    if (frame.scriptId != fSelectedScript && fSession.FindScript(frame.scriptId)) {
        fSelectedScript = frame.scriptId;
        if (const auto found = std::find(fScriptIDs.begin(), fScriptIDs.end(), frame.scriptId); found != fScriptIDs.end())
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
    for (const auto& id : fScriptIDs)
        if (const auto* script = fSession.FindScript(id)) scripts.push_back(script->Label());
    for (const auto& breakpoint : fSession.Breakpoints())
        breakpoints.push_back(breakpoint.url + ":" + std::to_string(breakpoint.line + 1) + (breakpoint.resolved ? "" : " (unresolved)"));
    for (const auto& frame : state.frames) frames.push_back(FrameLabel(fSession, frame));
    const auto* script = Selected();
    json result = {{"enabled", state.enabled}, {"scripts", scripts}, {"selected", script ? script->Label() : std::string()},
        {"sourceLoaded", script && script->sourceLoaded}, {"lines", script && script->sourceLoaded ? fCode->Lines() : 0},
        {"breakpoints", breakpoints}, {"paused", state.paused}, {"reason", state.reason}, {"frames", frames}, {"frame", fFrame},
        {"scopes", fScopes->Rows()}, {"status", fStatus->Text()}};
    return result.dump(-1, ' ', false, json::error_handler_t::replace);
}
}
#endif
