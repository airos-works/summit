#if SUMMIT_MODERN_WEBKIT
#include "DevToolsPanels.h"
#include "PageSourceWindow.h"
#include <interface/ColumnListView.h>
#include <interface/ColumnTypes.h>
#include <Button.h>
#include <CheckBox.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <PopUpMenu.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <SplitView.h>
#include <String.h>
#include <StringView.h>
#include <TabView.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace summit {
using namespace devtools;
namespace {
constexpr uint32 kPickToggled = 'deps', kSearchRun = 'desr', kSearchPrevious = 'desp', kSearchNext = 'desn',
    kStyleFilterChanged = 'desf', kComputedFilterChanged = 'decf', kComputedAllChanged = 'deca', kEditDone = 'deed',
    kTreeCommand = 'detc', kStyleCommand = 'dest', kApplyHTML = 'deah', kSideTabChanged = 'deat', kForceMenu = 'defm',
    kForcePseudo = 'defp', kSearchTyped = 'deft', kSearchSettled = 'dess', kNewRuleButton = 'denr';
// What :hov can force on an element, as other browsers offer.
constexpr const char* kPseudoClasses[] = {"active", "focus", "focus-visible", "focus-within", "hover", "visited"};
// The side's tabs: Styles, Computed, Accessibility, Event Listeners.
enum { kStylesTab, kComputedTab, kAccessibilityTab, kListenersTab };
enum { kListenerEvent, kListenerNode, kListenerHandler, kListenerOptions };

// Tells the panel when another of the side's tabs is shown.
class SideTabs final : public BTabView {
public:
    explicit SideTabs(BHandler* panel) : BTabView("side", B_WIDTH_FROM_LABEL), fPanel(panel) { }
    void Select(int32 index) override
    {
        BTabView::Select(index);
        if (Window()) Window()->PostMessage(kSideTabChanged, fPanel);
    }
private:
    BHandler* fPanel;
};
// What the tree's menu does ("command").
enum TreeAction {
    kEditAttributes, kAddAttribute, kEditTag, kEditText, kEditHTML, kCopyHTML, kCopySelector, kExpandAll, kCollapseAll,
    kScrollIntoView, kDeleteNode, kUndo, kRedo
};
// What the styles' menu does.
enum StyleAction { kEditDeclaration, kToggleDeclaration, kDeleteDeclaration, kAddDeclaration, kCopyDeclaration, kCopyRule, kShowSource,
    kEditSelector, kNewRule };
// What an editor in place edits ("kind").
enum EditKind { kEditingAttributes, kAddingAttribute, kEditingTag, kEditingText, kEditingDeclaration, kAddingDeclaration,
    kEditingSelector };
// What a node's outer HTML was asked for.
enum { kForCopy = 1, kForEdit = 2 };
enum { kComputedName, kComputedValue };
// The tree: space before the triangles, and per level.
constexpr float kMargin = 6, kIndent = 14, kTriangle = 14;

std::string LowerCase(std::string text)
{
    for (auto& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return text;
}

std::string Trimmed(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return { };
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

// "8", "12.5", "–" for nothing: the box model's numbers.
std::string Amount(double value)
{
    if (std::fabs(value) < 0.005) return "–";
    char text[32];
    std::snprintf(text, sizeof text, "%.2f", value);
    std::string result = text;
    while (result.back() == '0') result.pop_back();
    if (result.back() == '.') result.pop_back();
    return result;
}

// A declaration as an editor shows it: without its comment or semicolon.
std::string DeclarationText(const Declaration& declaration)
{
    auto text = Trimmed(declaration.text.empty() ? declaration.name + ": " + declaration.value
        + (declaration.important ? " !important" : "") : declaration.text);
    if (text.size() >= 4 && text.compare(0, 2, "/*") == 0 && text.compare(text.size() - 2, 2, "*/") == 0)
        text = Trimmed(text.substr(2, text.size() - 4));
    while (!text.empty() && (text.back() == ';' || text.back() == ' ')) text.pop_back();
    return text;
}

std::string RuleText(const StyleBlock& block)
{
    std::string text = block.selector + " {\n";
    for (const auto& declaration : block.declarations) text += "    " + DeclarationText(declaration) + ";\n";
    return text + "}";
}

// The length of a text in characters, for the width of fixed-font lines.
size_t Characters(const std::string& text)
{
    return static_cast<size_t>(std::count_if(text.begin(), text.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

rgb_color Mix(rgb_color a, rgb_color b, float weight)
{
    const auto mix = [weight](uint8 from, uint8 to) { return static_cast<uint8>(std::clamp(from + (to - from) * weight, 0.f, 255.f)); };
    return {mix(a.red, b.red), mix(a.green, b.green), mix(a.blue, b.blue), 255};
}

// A line of text edited in place. Enter (or leaving it) keeps what was typed,
// Escape drops it; either way the owner is told with a copy of its message
// ("commit", "text", and "keys": whether a key ended it, so that the owner
// takes the focus back).
class LineEditor : public BTextView {
public:
    LineEditor(BRect frame, const BMessage& done, BHandler* owner)
        : BTextView(frame, "editor", BRect(3, 2, frame.Width() - 3, frame.Height() - 2), B_FOLLOW_NONE, B_WILL_DRAW | B_NAVIGABLE)
        , fDone(done)
        , fOwner(owner)
    {
        const auto& palette = CurrentSourcePalette();
        SetWordWrap(false);
        SetStylable(false);
        SetViewColor(palette.background);
        SetLowColor(palette.background);
        SetFontAndColor(be_fixed_font, B_FONT_ALL, &palette.text);
    }
    void Draw(BRect update) override
    {
        BTextView::Draw(update);
        SetHighColor(CurrentSourcePalette().accent);
        StrokeRect(Bounds());
    }
    void KeyDown(const char* bytes, int32 count) override
    {
        if (bytes[0] == B_ENTER || bytes[0] == B_TAB) Finish(true, true);
        else if (bytes[0] == B_ESCAPE) Finish(false, true);
        else BTextView::KeyDown(bytes, count);
    }
    void MakeFocus(bool focus) override
    {
        BTextView::MakeFocus(focus);
        // Clicking elsewhere keeps what was typed, as in other browsers.
        if (!focus && Window()) Finish(true, false);
    }
    void Finish(bool commit, bool keys)
    {
        if (fFinished) return;
        fFinished = true;
        BMessage done(fDone);
        done.AddBool("commit", commit);
        done.AddBool("keys", keys);
        done.AddString("text", Text());
        BMessenger(fOwner).SendMessage(&done);
    }
    // Taken away by its owner: it tells nothing more.
    void Abandon() { fFinished = true; }

private:
    BMessage fDone;
    BHandler* fOwner;
    bool fFinished = false;
};

// A view of lines of one height, scrolled by a BScrollView: the DOM tree and
// the styles.
class LineView : public BView {
public:
    explicit LineView(const char* name)
        : BView(name, B_WILL_DRAW | B_FRAME_EVENTS | B_NAVIGABLE)
    {
        SetFont(be_fixed_font);
        font_height height;
        be_fixed_font->GetHeight(&height);
        fAscent = std::ceil(height.ascent);
        fLineHeight = std::ceil(height.ascent + height.descent + height.leading) + 4;
        fCharWidth = be_fixed_font->StringWidth("m");
        SetViewColor(B_TRANSPARENT_COLOR);
        SetExplicitMinSize(BSize(120, 60));
    }
    void FrameResized(float, float) override { UpdateScrollBars(); }
    void AttachedToWindow() override
    {
        BView::AttachedToWindow();
        UpdateScrollBars();
    }
    bool Editing() const { return fEditor != nullptr; }
    // For tests: ends the open editor as Enter (or Escape) does, with this text.
    bool FinishEditing(const std::string& text, bool commit)
    {
        if (!fEditor) return false;
        fEditor->SetText(text.c_str());
        fEditor->Finish(commit, true);
        return true;
    }

protected:
    virtual size_t CountLines() const = 0;
    // The widest line, in characters and indentation.
    virtual float Width() const = 0;
    int LineAt(BPoint where) const
    {
        const int index = static_cast<int>(std::floor(where.y / fLineHeight));
        return where.y >= 0 && index < static_cast<int>(CountLines()) ? index : -1;
    }
    BRect LineFrame(int index) const
    {
        const BRect bounds = Bounds();
        return BRect(bounds.left, index * fLineHeight, bounds.right, (index + 1) * fLineHeight - 1);
    }
    float Baseline(int index) const { return index * fLineHeight + 2 + fAscent; }
    void UpdateScrollBars()
    {
        const BRect bounds = Bounds();
        if (auto* vertical = ScrollBar(B_VERTICAL)) {
            const float height = CountLines() * fLineHeight;
            vertical->SetRange(0, std::max(0.f, height - bounds.Height()));
            vertical->SetProportion(height > 0 ? std::min(1.f, bounds.Height() / height) : 1);
            vertical->SetSteps(fLineHeight, std::max(fLineHeight, bounds.Height() - fLineHeight));
        }
        if (auto* horizontal = ScrollBar(B_HORIZONTAL)) {
            const float width = Width();
            horizontal->SetRange(0, std::max(0.f, width - bounds.Width()));
            horizontal->SetProportion(width > 0 ? std::min(1.f, bounds.Width() / width) : 1);
            horizontal->SetSteps(fCharWidth * 4, std::max(fCharWidth, bounds.Width() - fCharWidth * 4));
        }
    }
    void ScrollToLine(int index)
    {
        if (index < 0) return;
        const BRect bounds = Bounds();
        const float top = index * fLineHeight, bottom = top + fLineHeight;
        if (top < bounds.top) ScrollTo(bounds.left, top);
        else if (bottom > bounds.bottom) ScrollTo(bounds.left, bottom - bounds.Height() - 1);
    }
    void DrawNote(const char* text)
    {
        SetHighColor(CurrentSourcePalette().muted);
        SetFont(be_plain_font);
        DrawString(text, BPoint(Bounds().left + 10, Baseline(0) + 4));
        SetFont(be_fixed_font);
    }
    // An editor over a line, from x to the right edge.
    LineEditor* Edit(int index, float x, const BMessage& done, const std::string& text)
    {
        StopEditing();
        ScrollToLine(index);
        BRect frame = LineFrame(index);
        frame.left = std::max(frame.left, x - 3);
        frame.right = std::max(frame.left + 200, Bounds().right - 2);
        frame.InsetBy(0, -1);
        // Each editor's message is told from an earlier one's.
        BMessage message(done);
        message.AddInt32("editor", ++fEditorSerial);
        fEditor = new LineEditor(frame, message, this);
        AddChild(fEditor);
        fEditor->SetText(text.c_str());
        fEditor->SelectAll();
        fEditor->MakeFocus(true);
        return fEditor;
    }
    // Takes the editor away; true if what it had is to be kept, false too
    // for a message of an editor that is gone already.
    bool EditorDone(const BMessage& message)
    {
        if (!fEditor || message.GetInt32("editor", -1) != fEditorSerial) return false;
        StopEditing();
        if (message.GetBool("keys", false)) MakeFocus(true);
        return message.GetBool("commit", false);
    }
    void StopEditing()
    {
        if (!fEditor) return;
        auto* editor = fEditor;
        fEditor = nullptr;
        // Removing it takes its focus away, which must not count as leaving it.
        editor->Abandon();
        editor->RemoveSelf();
        delete editor;
    }
    BMessage* CurrentMessage() const { return Window() ? Window()->CurrentMessage() : nullptr; }
    float fAscent = 10, fLineHeight = 16, fCharWidth = 7;
    LineEditor* fEditor = nullptr;
    int32 fEditorSerial = 0;
};
}

// The DOM as lines: an element's opening tag, its children indented, and its
// closing tag. A triangle opens and closes a node; hovering a line has the
// page highlight its node.
class ElementsPanel::Tree : public LineView {
public:
    Tree(ElementsPanel& panel, DOMModel& dom, DevToolsHost& host)
        : LineView("tree"), fPanel(panel), fDOM(dom), fHost(host)
    {
    }

    void Rebuild()
    {
        fRows = fDOM.Rows();
        fWidest = 0;
        for (const auto& row : fRows) {
            size_t characters = 0;
            for (const auto& part : fDOM.DescribeRow(row)) characters += Characters(part.text);
            // Very long lines are drawn cut; this is where scrolling stops.
            fWidest = std::max(fWidest, kMargin + row.depth * kIndent + kTriangle + std::min<size_t>(characters, 400) * fCharWidth);
        }
        if (fHover >= static_cast<int>(fRows.size())) fHover = -1;
        SelectionChanged();
        UpdateScrollBars();
        Invalidate();
    }

    void SelectionChanged()
    {
        // Keys move the cursor themselves; otherwise it goes to the selection.
        if (!fKeepCursor) fCursor = OpenRow(fDOM.Selected());
        fKeepCursor = false;
        ScrollToLine(fCursor);
        Invalidate();
    }

    void Draw(BRect update) override
    {
        const auto& palette = CurrentSourcePalette();
        SetHighColor(palette.background);
        FillRect(update);
        if (fRows.empty()) {
            DrawNote(fDOM.Loaded() ? "The page has no document." : fDOM.Enabled() ? "Reading the page…" : "");
            return;
        }
        const int first = std::max(0, static_cast<int>(std::floor(update.top / fLineHeight)));
        const int last = std::min(static_cast<int>(fRows.size()) - 1, static_cast<int>(std::floor(update.bottom / fLineHeight)));
        for (int index = first; index <= last; ++index) DrawRow(index, update);
    }

    void MouseDown(BPoint where) override
    {
        MakeFocus(true);
        int32 buttons = B_PRIMARY_MOUSE_BUTTON, clicks = 1;
        if (const auto* message = CurrentMessage()) {
            message->FindInt32("buttons", &buttons);
            message->FindInt32("clicks", &clicks);
        }
        const int index = LineAt(where);
        if (index < 0) return;
        const auto row = fRows[index];
        fCursor = index;
        fKeepCursor = true;
        if (buttons & B_SECONDARY_MOUSE_BUTTON) {
            fDOM.Select(row.node);
            fHost.Sync();
            ShowMenu(row.node, ConvertToScreen(where));
            return;
        }
        const float x = kMargin + row.depth * kIndent;
        if (!row.closing && fDOM.Expandable(row.node) && where.x >= x - 3 && where.x < x + kTriangle - 2) {
            const bool expand = !fDOM.IsExpanded(row.node);
            // With Shift, everything under the node too.
            if (modifiers() & (B_SHIFT_KEY | B_OPTION_KEY)) {
                if (expand) fDOM.ExpandAll(row.node);
                else fDOM.CollapseAll(row.node);
            } else fDOM.SetExpanded(row.node, expand);
            fKeepCursor = false;
            fHost.Sync();
            return;
        }
        fDOM.Select(row.node);
        fHost.Sync();
        if (clicks == 2 && !row.closing) EditAt(index, where.x);
    }

    void MouseMoved(BPoint where, uint32 transit, const BMessage* dragged) override
    {
        LineView::MouseMoved(where, transit, dragged);
        const int index = transit == B_EXITED_VIEW || transit == B_OUTSIDE_VIEW ? -1 : LineAt(where);
        if (index == fHover) return;
        if (fHover >= 0) Invalidate(LineFrame(fHover));
        fHover = index;
        if (fHover >= 0) Invalidate(LineFrame(fHover));
        // The page shows where the node is.
        if (index >= 0) fDOM.Highlight(fRows[index].node);
        else if (!fDOM.Picking()) fDOM.HideHighlight();
        fHost.Sync();
    }

    void KeyDown(const char* bytes, int32 count) override
    {
        const auto* node = fDOM.Find(fDOM.Selected());
        // Alt+Z and Shift+Alt+Z, where the window has no shortcut for them.
        if ((modifiers() & B_COMMAND_KEY) && (bytes[0] == 'z' || bytes[0] == 'Z')) {
            Command(modifiers() & B_SHIFT_KEY ? kRedo : kUndo, 0);
            return;
        }
        switch (bytes[0]) {
            case B_UP_ARROW: Move(-1); break;
            case B_DOWN_ARROW: Move(1); break;
            case B_PAGE_UP: Move(-std::max(1, static_cast<int>(Bounds().Height() / fLineHeight) - 1)); break;
            case B_PAGE_DOWN: Move(std::max(1, static_cast<int>(Bounds().Height() / fLineHeight) - 1)); break;
            case B_HOME: Move(-static_cast<int>(fRows.size())); break;
            case B_END: Move(static_cast<int>(fRows.size())); break;
            case B_LEFT_ARROW:
                if (!node) break;
                if (fDOM.IsExpanded(node->id) && fDOM.Expandable(node->id)) fDOM.SetExpanded(node->id, false);
                else if (const int parent = OpenRow(node->parent); parent >= 0) {
                    fCursor = parent;
                    fKeepCursor = true;
                    fDOM.Select(node->parent);
                }
                fHost.Sync();
                break;
            case B_RIGHT_ARROW:
                if (!node) break;
                if (fDOM.Expandable(node->id) && !fDOM.IsExpanded(node->id)) {
                    fDOM.SetExpanded(node->id, true);
                    fHost.Sync();
                } else if (fDOM.IsExpanded(node->id))
                    Move(1);
                break;
            case B_ENTER:
                if (node) Command(node->type == kElementNode ? kEditAttributes : kEditText, node->id);
                break;
            case B_DELETE:
            case B_BACKSPACE:
                if (node) Command(kDeleteNode, node->id);
                break;
            case B_ESCAPE:
                if (fDOM.Picking()) fPanel.SetPicking(false);
                break;
            default: LineView::KeyDown(bytes, count);
        }
    }

    void MessageReceived(BMessage* message) override
    {
        switch (message->what) {
            case kTreeCommand: Command(message->GetInt32("command", -1), message->GetInt32("node", 0)); break;
            case kEditDone: {
                if (!EditorDone(*message)) break;
                const int node = message->GetInt32("node", 0);
                const std::string text = message->GetString("text", "");
                switch (message->GetInt32("kind", -1)) {
                    case kEditingAttributes:
                        if (text != fDOM.AttributesText(node)) fDOM.SetAttributesText(node, text);
                        break;
                    case kAddingAttribute: fDOM.AddAttributes(node, text); break;
                    case kEditingTag: fDOM.SetTagName(node, text); break;
                    case kEditingText:
                        if (const auto* edited = fDOM.Find(node); edited && edited->value != text) fDOM.SetNodeText(node, text);
                        break;
                }
                fHost.Sync();
                break;
            }
            case B_COPY: Command(kCopyHTML, fDOM.Selected()); break;
            case B_UNDO: Command(kUndo, 0); break;
            case B_REDO: Command(kRedo, 0); break;
            default: LineView::MessageReceived(message);
        }
    }

    void Command(int32 action, int node)
    {
        const auto* target = fDOM.Find(node);
        switch (action) {
            case kEditAttributes:
            case kAddAttribute:
            case kEditTag:
            case kEditText: {
                if (!target) break;
                int line = OpenRow(node);
                float x = 0;
                std::string text;
                int kind = kEditingText;
                if (action == kEditText && target->type == kElementNode) {
                    // The text shown in the element's line is its child's.
                    if (!fDOM.InlineText(node)) break;
                    node = target->children[0];
                    text = fDOM.Find(node)->value;
                    x = PieceX(line, Piece::Text);
                } else if (action == kEditText) {
                    text = target->value;
                    x = PieceX(line, Piece::Text);
                    if (x < 0) x = PieceX(line, Piece::Comment);
                } else if (action == kEditTag) {
                    kind = kEditingTag;
                    text = target->Tag();
                    x = PieceX(line, Piece::Tag);
                } else {
                    kind = action == kAddAttribute ? kAddingAttribute : kEditingAttributes;
                    text = action == kAddAttribute ? std::string() : fDOM.AttributesText(node);
                    x = PieceX(line, Piece::AttributeName);
                    if (x < 0 || action == kAddAttribute) x = PieceX(line, Piece::Tag) + 2 * fCharWidth;
                }
                if (line < 0) break;
                if (x < 0) x = kMargin + fRows[line].depth * kIndent + kTriangle;
                BMessage done(kEditDone);
                done.AddInt32("node", node);
                done.AddInt32("kind", kind);
                Edit(line, x, done, text);
                break;
            }
            case kEditHTML:
            case kCopyHTML:
                if (target) fDOM.RequestOuterHTML(node, action == kEditHTML ? kForEdit : kForCopy);
                break;
            case kCopySelector:
                if (target) CopyToClipboard(fDOM.Selector(node));
                break;
            case kExpandAll: fDOM.ExpandAll(node); break;
            case kCollapseAll: fDOM.CollapseAll(node); break;
            case kScrollIntoView: fDOM.ScrollIntoView(node); break;
            case kDeleteNode:
                if (target && target->type != kDocumentNode) fDOM.Remove(node);
                break;
            case kUndo: fDOM.Undo(); break;
            case kRedo: fDOM.Redo(); break;
        }
        fHost.Sync();
    }

    // For tests: the lines as text.
    nlohmann::json Lines(size_t limit) const
    {
        auto lines = nlohmann::json::array();
        for (size_t i = 0; i < fRows.size() && i < limit; ++i) {
            std::string line(static_cast<size_t>(fRows[i].depth) * 2, ' ');
            if (fRows[i].node == fDOM.Selected() && !fRows[i].closing) line += "> ";
            for (const auto& part : fDOM.DescribeRow(fRows[i])) line += part.text;
            lines.push_back(line);
        }
        return lines;
    }
    size_t Count() const { return fRows.size(); }
    int RowOf(size_t index) const { return index < fRows.size() ? fRows[index].node : 0; }

protected:
    size_t CountLines() const override { return fRows.size(); }
    float Width() const override { return fWidest; }

private:
    int OpenRow(int node) const
    {
        for (size_t i = 0; i < fRows.size(); ++i)
            if (fRows[i].node == node && !fRows[i].closing) return static_cast<int>(i);
        return -1;
    }

    void Move(int delta)
    {
        if (fRows.empty()) return;
        const int index = std::clamp(fCursor + delta, 0, static_cast<int>(fRows.size()) - 1);
        fCursor = index;
        fKeepCursor = true;
        ScrollToLine(index);
        fDOM.Select(fRows[index].node);
        fHost.Sync();
        // The same node (its closing tag) changes no selection.
        fKeepCursor = false;
        Invalidate();
    }

    rgb_color ColorOf(Piece kind) const
    {
        const auto& palette = CurrentSourcePalette();
        switch (kind) {
            case Piece::Tag: return palette.type;
            case Piece::Punctuation: return palette.Tint(palette.type, .65f);
            case Piece::AttributeName: return palette.keyword;
            case Piece::AttributeValue: return palette.string;
            case Piece::Comment: return palette.comment;
            case Piece::Muted: return palette.muted;
            case Piece::Pseudo: return palette.number;
            default: return palette.text;
        }
    }

    // Where a line's first piece of a kind begins, or -1.
    float PieceX(int index, Piece kind) const
    {
        if (index < 0 || index >= static_cast<int>(fRows.size())) return -1;
        float x = kMargin + fRows[index].depth * kIndent + kTriangle;
        for (const auto& part : fDOM.DescribeRow(fRows[index])) {
            if (part.kind == kind) return x;
            x += StringWidth(part.text.c_str());
        }
        return -1;
    }

    // What a double click edits: the tag name, the text, or the attributes.
    void EditAt(int index, float where)
    {
        const auto& row = fRows[index];
        const auto* node = fDOM.Find(row.node);
        if (!node) return;
        if (node->type == kTextNode || node->type == kCommentNode) {
            Command(kEditText, row.node);
            return;
        }
        if (node->type != kElementNode || !node->pseudoType.empty()) return;
        float x = kMargin + row.depth * kIndent + kTriangle;
        Piece hit = Piece::Punctuation;
        for (const auto& part : fDOM.DescribeRow(row)) {
            const float width = StringWidth(part.text.c_str());
            if (where >= x && where < x + width) {
                hit = part.kind;
                break;
            }
            x += width;
        }
        if (hit == Piece::Tag && where < PieceX(index, Piece::Tag) + StringWidth(node->Tag().c_str()) + 1) Command(kEditTag, row.node);
        else if (hit == Piece::Text) Command(kEditText, row.node);
        else Command(kEditAttributes, row.node);
    }

    void DrawRow(int index, BRect update)
    {
        const auto& palette = CurrentSourcePalette();
        const auto& row = fRows[index];
        const BRect frame = LineFrame(index);
        if (row.node == fDOM.Selected()) {
            SetHighColor(row.closing ? palette.Tint(palette.selection, .55f)
                : IsFocus() ? palette.selection : palette.Tint(palette.selection, .75f));
            FillRect(frame);
        } else if (index == fHover) {
            SetHighColor(palette.line);
            FillRect(frame);
        }
        float x = kMargin + row.depth * kIndent;
        if (!row.closing && fDOM.Expandable(row.node)) {
            // ▸ closed, ▾ open.
            const float middle = frame.top + fLineHeight / 2, left = x + 1;
            SetHighColor(palette.muted);
            if (fDOM.IsExpanded(row.node) && !fDOM.ShownChildren(row.node).empty())
                FillTriangle(BPoint(left, middle - 2), BPoint(left + 8, middle - 2), BPoint(left + 4, middle + 3));
            else
                FillTriangle(BPoint(left + 2, middle - 4), BPoint(left + 2, middle + 4), BPoint(left + 7, middle));
        }
        x += kTriangle;
        const float baseline = Baseline(index);
        for (const auto& part : fDOM.DescribeRow(row)) {
            if (x > update.right) break;
            SetHighColor(ColorOf(part.kind));
            // A long attribute is cut where it can no longer be seen.
            const auto& text = part.text.size() > 2000 ? part.text.substr(0, 2000) : part.text;
            DrawString(text.c_str(), BPoint(x, baseline));
            x += StringWidth(text.c_str());
        }
    }

    void ShowMenu(int node, BPoint screen)
    {
        const auto* target = fDOM.Find(node);
        if (!target) return;
        auto* menu = new BPopUpMenu("tree", false, false);
        const auto add = [&](const char* label, int32 action, bool enabled = true) {
            auto* message = new BMessage(kTreeCommand);
            message->AddInt32("command", action);
            message->AddInt32("node", node);
            auto* item = new BMenuItem(label, message);
            item->SetEnabled(enabled);
            menu->AddItem(item);
        };
        const bool element = target->type == kElementNode && target->pseudoType.empty();
        if (element) {
            add("Edit Attributes", kEditAttributes);
            add("Add Attribute", kAddAttribute);
            add("Edit Tag Name", kEditTag);
            if (fDOM.InlineText(node)) add("Edit Text", kEditText);
        } else if (target->type == kTextNode || target->type == kCommentNode)
            add("Edit Text", kEditText);
        add("Edit as HTML", kEditHTML, element);
        menu->AddSeparatorItem();
        add("Copy Outer HTML", kCopyHTML, target->type != kDocumentNode);
        add("Copy Selector", kCopySelector, element);
        menu->AddSeparatorItem();
        add("Expand All", kExpandAll, fDOM.Expandable(node));
        add("Collapse All", kCollapseAll, fDOM.Expandable(node));
        add("Scroll into View", kScrollIntoView, element);
        menu->AddSeparatorItem();
        add("Delete Node", kDeleteNode, target->type != kDocumentNode);
        menu->AddSeparatorItem();
        add("Undo", kUndo);
        add("Redo", kRedo);
        menu->SetTargetForItems(this);
        menu->SetAsyncAutoDestruct(true);
        menu->Go(screen, true, false, true);
    }

    ElementsPanel& fPanel;
    DOMModel& fDOM;
    DevToolsHost& fHost;
    std::vector<TreeRow> fRows;
    float fWidest = 0;
    int fHover = -1, fCursor = -1;
    bool fKeepCursor = false;
};

// The selected element's styles: its own rules from the strongest, the
// browser's, then what it inherits. Declarations switch on and off with their
// boxes and are edited with a double click.
class ElementsPanel::Styles : public LineView {
public:
    Styles(ElementsPanel& panel, DOMModel& dom, DevToolsHost& host)
        : LineView("styles"), fPanel(panel), fDOM(dom), fHost(host)
    {
    }

    void SetFilter(const std::string& filter)
    {
        fFilter = LowerCase(filter);
        Rebuild();
    }

    void Rebuild()
    {
        // Not under a declaration being edited: once it is done.
        if (Editing()) {
            fRebuildWaiting = true;
            return;
        }
        fRebuildWaiting = false;
        const auto& styles = fDOM.Styles();
        if (fDOM.Selected() && styles.node != fDOM.Selected() && fDOM.StylesLoading() && !fLines.empty()) return;
        if (styles.node != fShownNode) {
            fShownNode = styles.node;
            ScrollTo(0, 0);
        }
        fLines.clear();
        fWidest = 0;
        const auto* node = fDOM.Find(fDOM.Selected());
        const auto note = [&](const std::string& text) { fLines.push_back({Line::Note, -1, -1, text}); };
        if (!node || !node->IsElement() || !node->pseudoType.empty()) note("Select an element to see its styles.");
        else if (styles.node != node->id) note("Reading…");
        else {
            if (!styles.error.empty()) note(styles.error);
            int inheritedFrom = 0;
            std::string pseudo;
            for (size_t b = 0; b < styles.blocks.size(); ++b) {
                const auto& block = styles.blocks[b];
                std::vector<int> shown;
                for (size_t d = 0; d < block.declarations.size(); ++d) {
                    const auto& declaration = block.declarations[d];
                    if (fFilter.empty() || LowerCase(declaration.name + ": " + declaration.value).find(fFilter) != std::string::npos)
                        shown.push_back(static_cast<int>(d));
                }
                if (!fFilter.empty() && shown.empty() && LowerCase(block.selector).find(fFilter) == std::string::npos) continue;
                if (block.inheritedFrom != inheritedFrom) {
                    inheritedFrom = block.inheritedFrom;
                    const auto* from = fDOM.Find(inheritedFrom);
                    fLines.push_back({Line::Section, static_cast<int>(b), -1, "Inherited from " + (from ? from->Label() : std::string("an ancestor"))});
                } else if (block.pseudo != pseudo) {
                    pseudo = block.pseudo;
                    if (!pseudo.empty()) fLines.push_back({Line::Section, static_cast<int>(b), -1, "Pseudo element " + pseudo});
                }
                for (auto grouping = block.groupings.rbegin(); grouping != block.groupings.rend(); ++grouping)
                    fLines.push_back({Line::Grouping, static_cast<int>(b), -1, *grouping});
                fLines.push_back({Line::Header, static_cast<int>(b), -1, block.selector + " {"});
                for (const int d : shown) fLines.push_back({Line::Declaration, static_cast<int>(b), d, { }});
                fLines.push_back({Line::Close, static_cast<int>(b), -1, "}"});
                fLines.push_back({Line::Space, static_cast<int>(b), -1, { }});
            }
            if (fLines.empty()) note(fFilter.empty() ? "No styles." : "No styles match the filter.");
        }
        for (const auto& line : fLines) {
            size_t characters = Characters(line.text) + 4;
            if (line.kind == Line::Declaration) {
                const auto& declaration = styles.blocks[line.block].declarations[line.declaration];
                characters = Characters(declaration.name) + Characters(declaration.value) + 20;
            } else if (line.kind == Line::Header)
                characters += Characters(styles.blocks[line.block].Location()) + 4;
            fWidest = std::max(fWidest, 20 + std::min<size_t>(characters, 400) * fCharWidth);
        }
        if (fHover >= static_cast<int>(fLines.size())) fHover = -1;
        UpdateScrollBars();
        Invalidate();
    }

    void Draw(BRect update) override
    {
        const auto& palette = CurrentSourcePalette();
        SetHighColor(palette.background);
        FillRect(update);
        const int first = std::max(0, static_cast<int>(std::floor(update.top / fLineHeight)));
        const int last = std::min(static_cast<int>(fLines.size()) - 1, static_cast<int>(std::floor(update.bottom / fLineHeight)));
        for (int index = first; index <= last; ++index) DrawLine(index);
    }

    void MouseDown(BPoint where) override
    {
        MakeFocus(true);
        int32 buttons = B_PRIMARY_MOUSE_BUTTON, clicks = 1;
        if (const auto* message = CurrentMessage()) {
            message->FindInt32("buttons", &buttons);
            message->FindInt32("clicks", &clicks);
        }
        const int index = LineAt(where);
        if (index < 0) return;
        const auto line = fLines[index];
        if (line.block < 0 || line.kind == Line::Note || line.kind == Line::Space) return;
        const auto& block = fDOM.Styles().blocks[line.block];
        if (buttons & B_SECONDARY_MOUSE_BUTTON) {
            ShowMenu(line, ConvertToScreen(where));
            return;
        }
        switch (line.kind) {
            case Line::Declaration:
                if (block.Editable() && where.x < kCheckRight) Command(kToggleDeclaration, line.block, line.declaration);
                else if (clicks == 2 && block.Editable()) Command(kEditDeclaration, line.block, line.declaration);
                break;
            case Line::Header:
                if (where.x >= fLocationLeft && Openable(block)) Command(kShowSource, line.block, -1);
                // On the selector it is edited; after it, a declaration is added.
                else if (clicks == 2 && block.SelectorEditable() && where.x < 10 + StringWidth(block.selector.c_str()))
                    Command(kEditSelector, line.block, -1);
                else if (clicks == 2 && block.Editable()) Command(kAddDeclaration, line.block, -1);
                break;
            case Line::Close:
                if (clicks == 2 && block.Editable()) Command(kAddDeclaration, line.block, -1);
                break;
            default: break;
        }
    }

    void MouseMoved(BPoint where, uint32 transit, const BMessage* dragged) override
    {
        LineView::MouseMoved(where, transit, dragged);
        const int index = transit == B_EXITED_VIEW || transit == B_OUTSIDE_VIEW ? -1 : LineAt(where);
        if (index == fHover) return;
        if (fHover >= 0) Invalidate(LineFrame(fHover));
        fHover = index;
        if (fHover >= 0) Invalidate(LineFrame(fHover));
    }

    void MessageReceived(BMessage* message) override
    {
        switch (message->what) {
            case kStyleCommand:
                Command(message->GetInt32("command", -1), message->GetInt32("block", -1), message->GetInt32("declaration", -1));
                break;
            case kEditDone: {
                const bool commit = EditorDone(*message);
                if (fRebuildWaiting && !Editing()) Rebuild();
                // The styles may have been read again meanwhile: the edit is
                // made only where it was begun.
                const auto& blocks = fDOM.Styles().blocks;
                const auto block = static_cast<size_t>(message->GetInt32("block", -1));
                if (!commit || message->GetInt32("node", 0) != fDOM.Styles().node || block >= blocks.size()
                    || blocks[block].selector != message->GetString("selector", ""))
                    break;
                const std::string text = message->GetString("text", "");
                if (message->GetInt32("kind", -1) == kEditingSelector) {
                    if (Trimmed(text) != message->GetString("was", "")) fDOM.SetRuleSelector(block, text);
                } else if (message->GetInt32("kind", -1) == kAddingDeclaration) fDOM.AddDeclaration(block, text);
                else {
                    const auto index = static_cast<size_t>(message->GetInt32("declaration", -1));
                    if (index < blocks[block].declarations.size()
                        && DeclarationText(blocks[block].declarations[index]) == message->GetString("was", "")
                        && Trimmed(text) != message->GetString("was", ""))
                        fDOM.EditDeclaration(block, index, text);
                }
                fHost.Sync();
                break;
            }
            default: LineView::MessageReceived(message);
        }
    }

    void Command(int32 action, int block, int declaration)
    {
        if (action == kNewRule) {
            // An empty rule for the selected element, to be filled in.
            if (fDOM.Selected()) fDOM.AddRule(fDOM.Selector(fDOM.Selected()));
            fHost.Sync();
            return;
        }
        const auto& blocks = fDOM.Styles().blocks;
        if (block < 0 || block >= static_cast<int>(blocks.size())) return;
        const auto& style = blocks[block];
        const bool valid = declaration >= 0 && declaration < static_cast<int>(style.declarations.size());
        switch (action) {
            case kToggleDeclaration:
                if (valid) fDOM.ToggleDeclaration(block, declaration);
                break;
            case kDeleteDeclaration:
                if (valid) fDOM.EditDeclaration(block, declaration, "");
                break;
            case kEditDeclaration:
            case kAddDeclaration: {
                int line = -1;
                for (size_t i = 0; i < fLines.size(); ++i) {
                    const auto& candidate = fLines[i];
                    if (candidate.block != block) continue;
                    if (action == kEditDeclaration ? candidate.kind == Line::Declaration && candidate.declaration == declaration
                        : candidate.kind == Line::Close)
                        line = static_cast<int>(i);
                }
                if (line < 0 || (action == kEditDeclaration && !valid)) break;
                BMessage done(kEditDone);
                done.AddInt32("node", fDOM.Styles().node);
                done.AddInt32("kind", action == kAddDeclaration ? kAddingDeclaration : kEditingDeclaration);
                done.AddInt32("block", block);
                done.AddInt32("declaration", declaration);
                done.AddString("selector", style.selector.c_str());
                const auto was = action == kAddDeclaration ? std::string() : DeclarationText(style.declarations[declaration]);
                done.AddString("was", was.c_str());
                Edit(line, kCheckRight + 2, done, was);
                return;
            }
            case kEditSelector: {
                if (!style.SelectorEditable()) break;
                int line = -1;
                for (size_t i = 0; i < fLines.size() && line < 0; ++i)
                    if (fLines[i].block == block && fLines[i].kind == Line::Header) line = static_cast<int>(i);
                if (line < 0) break;
                BMessage done(kEditDone);
                done.AddInt32("node", fDOM.Styles().node);
                done.AddInt32("kind", kEditingSelector);
                done.AddInt32("block", block);
                done.AddString("selector", style.selector.c_str());
                done.AddString("was", style.selector.c_str());
                Edit(line, 8, done, style.selector);
                return;
            }
            case kCopyDeclaration:
                if (valid) CopyToClipboard(DeclarationText(style.declarations[declaration]) + ";");
                break;
            case kCopyRule: CopyToClipboard(RuleText(style)); break;
            case kShowSource: fDOM.RequestSheetSource(block); break;
        }
        fHost.Sync();
    }

protected:
    size_t CountLines() const override { return fLines.size(); }
    float Width() const override { return fWidest; }

private:
    struct Line {
        enum Kind { Note, Section, Grouping, Header, Declaration, Close, Space };
        Kind kind;
        int block, declaration;
        std::string text;
    };
    // The declarations' boxes end here; their text begins after.
    static constexpr float kCheckRight = 30;

    static bool Openable(const StyleBlock& block) { return block.kind == StyleBlock::Rule && !block.styleSheetId.empty(); }

    void DrawLine(int index)
    {
        const auto& palette = CurrentSourcePalette();
        const auto& line = fLines[index];
        const BRect frame = LineFrame(index);
        const float baseline = Baseline(index);
        if (line.kind == Line::Note) {
            SetHighColor(palette.muted);
            SetFont(be_plain_font);
            DrawString(line.text.c_str(), BPoint(10, baseline));
            SetFont(be_fixed_font);
            return;
        }
        if (line.kind == Line::Space) return;
        const auto& block = fDOM.Styles().blocks[line.block];
        if (index == fHover && line.kind == Line::Declaration) {
            SetHighColor(palette.line);
            FillRect(frame);
        }
        switch (line.kind) {
            case Line::Section: {
                SetHighColor(palette.border);
                StrokeLine(BPoint(frame.left, frame.top + 1), BPoint(frame.right, frame.top + 1));
                SetHighColor(palette.muted);
                BFont bold(be_bold_font);
                bold.SetSize(be_plain_font->Size());
                SetFont(&bold);
                DrawString(line.text.c_str(), BPoint(10, baseline + 1));
                SetFont(be_fixed_font);
                break;
            }
            case Line::Grouping:
                SetHighColor(palette.muted);
                DrawString(line.text.c_str(), BPoint(10, baseline));
                break;
            case Line::Header: {
                const auto location = block.Location();
                const float locationWidth = location.empty() ? 0 : StringWidth(location.c_str());
                // A long selector is cut so that where the rule is stays in view.
                BString selector(block.selector.c_str());
                const float room = std::max(12 * fCharWidth, frame.right - 8 - locationWidth - 3 * fCharWidth - 10
                    - StringWidth(" {"));
                if (StringWidth(selector) > room) TruncateString(&selector, B_TRUNCATE_END, room);
                SetHighColor(block.kind == StyleBlock::Rule ? palette.text : palette.accent);
                DrawString(selector.String(), BPoint(10, baseline));
                SetHighColor(palette.muted);
                DrawString(" {");
                if (!location.empty()) {
                    const float width = locationWidth;
                    const float x = std::max(PenLocation().x + 3 * fCharWidth, frame.right - width - 8);
                    fLocationLeft = x;
                    SetHighColor(Openable(block) ? palette.accent : palette.muted);
                    DrawString(location.c_str(), BPoint(x, baseline));
                    if (Openable(block)) StrokeLine(BPoint(x, baseline + 2), BPoint(x + width, baseline + 2));
                }
                break;
            }
            case Line::Declaration: {
                const auto& declaration = block.declarations[line.declaration];
                const bool off = !declaration.enabled, struck = off || declaration.overridden || !declaration.parsedOk;
                if (block.Editable()) {
                    // The box: ticked while the declaration counts.
                    const BRect box(14, frame.top + (fLineHeight - 11) / 2, 24, frame.top + (fLineHeight - 11) / 2 + 10);
                    SetHighColor(palette.background);
                    FillRect(box);
                    SetHighColor(palette.muted);
                    StrokeRect(box);
                    if (!off) {
                        SetHighColor(palette.accent);
                        SetPenSize(2);
                        StrokeLine(BPoint(box.left + 2, box.top + 5), BPoint(box.left + 4, box.bottom - 2));
                        StrokeLine(BPoint(box.left + 4, box.bottom - 2), BPoint(box.right - 2, box.top + 2));
                        SetPenSize(1);
                    }
                }
                const float start = kCheckRight + 2;
                const rgb_color name = !declaration.parsedOk ? palette.removed : struck ? palette.Tint(palette.type, .5f) : palette.type;
                const rgb_color value = struck ? palette.muted : palette.text;
                SetHighColor(name);
                DrawString(declaration.name.c_str(), BPoint(start, baseline));
                SetHighColor(value);
                DrawString(": ");
                DrawString(declaration.value.c_str());
                if (declaration.important) {
                    SetHighColor(struck ? palette.muted : palette.keyword);
                    DrawString(" !important");
                }
                SetHighColor(value);
                DrawString(";");
                // What var() came to on this element, as other browsers show it.
                if (!struck && !block.inheritedFrom && declaration.value.find("var(") != std::string::npos) {
                    if (const auto resolved = fDOM.ComputedValue(declaration.name); !resolved.empty() && resolved != declaration.value) {
                        SetHighColor(palette.muted);
                        DrawString(("  = " + resolved).c_str());
                    }
                }
                if (struck) {
                    const float end = PenLocation().x;
                    SetHighColor(Mix(palette.muted, palette.background, .2f));
                    const float middle = baseline - fAscent / 3;
                    StrokeLine(BPoint(start, middle), BPoint(end, middle));
                }
                break;
            }
            case Line::Close:
                SetHighColor(palette.muted);
                DrawString("}", BPoint(10, baseline));
                break;
            default: break;
        }
    }

    void ShowMenu(const Line& line, BPoint screen)
    {
        const auto& block = fDOM.Styles().blocks[line.block];
        auto* menu = new BPopUpMenu("styles", false, false);
        const auto add = [&](const char* label, int32 action, bool enabled = true) {
            auto* message = new BMessage(kStyleCommand);
            message->AddInt32("command", action);
            message->AddInt32("block", line.block);
            message->AddInt32("declaration", line.declaration);
            auto* item = new BMenuItem(label, message);
            item->SetEnabled(enabled);
            menu->AddItem(item);
        };
        if (line.kind == Line::Declaration) {
            const bool on = block.declarations[line.declaration].enabled;
            add("Edit Declaration", kEditDeclaration, block.Editable());
            add(on ? "Disable Declaration" : "Enable Declaration", kToggleDeclaration, block.Editable());
            add("Delete Declaration", kDeleteDeclaration, block.Editable());
            menu->AddSeparatorItem();
            add("Copy Declaration", kCopyDeclaration);
        }
        add("Add Declaration", kAddDeclaration, block.Editable());
        add("Edit Selector", kEditSelector, block.SelectorEditable());
        add("New Style Rule", kNewRule, fDOM.Selected() != 0);
        add("Copy Rule", kCopyRule);
        add("Show in Style Sheet", kShowSource, Openable(block));
        menu->SetTargetForItems(this);
        menu->SetAsyncAutoDestruct(true);
        menu->Go(screen, true, false, true);
    }

    ElementsPanel& fPanel;
    DOMModel& fDOM;
    DevToolsHost& fHost;
    std::vector<Line> fLines;
    std::string fFilter;
    float fWidest = 0, fLocationLeft = 1e9;
    int fHover = -1, fShownNode = 0;
    bool fRebuildWaiting = false;
};

// The selected element's boxes, one in another as in other browsers' tools:
// margin, border, padding and content, with their sizes in CSS pixels.
class ElementsPanel::BoxModel : public BView {
public:
    explicit BoxModel(DOMModel& dom)
        : BView("box-model", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE), fDOM(dom)
    {
        SetViewColor(B_TRANSPARENT_COLOR);
        SetExplicitMinSize(BSize(300, 196));
        SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 196));
    }

    void Draw(BRect) override
    {
        const auto& palette = CurrentSourcePalette();
        const BRect bounds = Bounds();
        SetHighColor(palette.background);
        FillRect(bounds);
        BFont font(be_plain_font);
        font.SetSize(std::max(9.f, be_plain_font->Size() * .85f));
        SetFont(&font);
        font_height height;
        font.GetHeight(&height);
        const auto& styles = fDOM.Styles();
        const auto* node = fDOM.Find(fDOM.Selected());
        if (!node || styles.node != node->id || !styles.box.known) {
            SetHighColor(palette.muted);
            const char* note = !node || !node->IsElement() ? "Select an element to see its boxes."
                : styles.node != node->id ? "Reading…" : "The element has no box: it is not shown.";
            DrawString(note, BPoint(10, 20));
            return;
        }
        const auto& box = styles.box;
        const bool positioned = !box.position.empty() && box.position != "static";
        struct Ring {
            const char* label;
            rgb_color color;
            std::string sides[4];
        };
        std::vector<Ring> rings;
        const auto sides = [](const double values[4], Ring& ring) {
            for (int side = 0; side < 4; ++side) ring.sides[side] = Amount(values[side]);
        };
        if (positioned) {
            Ring ring {"position", palette.background, { }};
            for (int side = 0; side < 4; ++side) {
                const auto& offset = box.offsets[side];
                ring.sides[side] = offset == "auto" || offset.empty() ? "–" : Amount(std::atof(offset.c_str()));
            }
            rings.push_back(ring);
        }
        Ring margin {"margin", {246, 178, 107, 255}, { }}, border {"border", {255, 229, 153, 255}, { }},
            padding {"padding", {147, 196, 125, 255}, { }};
        sides(box.margin, margin);
        sides(box.border, border);
        sides(box.padding, padding);
        rings.push_back(margin);
        rings.push_back(border);
        rings.push_back(padding);
        const float lineHeight = std::ceil(height.ascent + height.descent);
        const float dx = std::max(30.f, StringWidth("0000.0") + 8), dy = lineHeight + 6;
        BRect rect = bounds.InsetByCopy(6, 6);
        const float contentWidth = std::max(StringWidth("0000.00 × 0000.00") + 16, 90.f);
        // Narrow panes get a narrower diagram.
        const float rectWidth = std::min(rect.Width(), contentWidth + 2 * dx * rings.size() + 20);
        rect.left += (rect.Width() - rectWidth) / 2;
        rect.right = rect.left + rectWidth;
        for (const auto& ring : rings) {
            const rgb_color fill = palette.Tint(ring.color, palette.dark ? .28f : .45f);
            SetHighColor(ring.label == std::string("position") ? palette.background : fill);
            FillRect(rect);
            SetHighColor(palette.muted);
            if (ring.label == std::string("position")) SetHighColor(palette.border);
            StrokeRect(rect);
            SetHighColor(palette.text);
            DrawString(ring.label, BPoint(rect.left + 4, rect.top + height.ascent + 2));
            const auto centred = [&](const std::string& text, float x, float baseline) {
                DrawString(text.c_str(), BPoint(x - StringWidth(text.c_str()) / 2, baseline));
            };
            const float middleX = (rect.left + rect.right) / 2, middleY = (rect.top + rect.bottom) / 2 + height.ascent / 2 - 1;
            centred(ring.sides[0], middleX, rect.top + height.ascent + 3);
            centred(ring.sides[2], middleX, rect.bottom - height.descent - 2);
            centred(ring.sides[3], rect.left + dx / 2, middleY);
            centred(ring.sides[1], rect.right - dx / 2, middleY);
            rect.InsetBy(dx, dy);
        }
        // The content box.
        SetHighColor(palette.Tint(rgb_color {111, 168, 220, 255}, palette.dark ? .35f : .5f));
        FillRect(rect);
        SetHighColor(palette.muted);
        StrokeRect(rect);
        SetHighColor(palette.text);
        const auto size = Amount(box.width) + " × " + Amount(box.height);
        DrawString(size.c_str(), BPoint((rect.left + rect.right - StringWidth(size.c_str())) / 2,
            (rect.top + rect.bottom) / 2 + height.ascent / 2 - 1));
    }

private:
    DOMModel& fDOM;
};

// Edit as HTML: the node's outer HTML in an editor; Apply puts it in the page.
class ElementsPanel::HTMLEditor : public BWindow {
public:
    HTMLEditor(BMessenger panel, int node, const std::string& html, const std::string& label, BRect near)
        : BWindow(BRect(near.left + 60, near.top + 60, near.left + 760, near.top + 520), ("Edit as HTML — " + label).c_str(),
            B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE)
        , fPanel(panel)
        , fNode(node)
    {
        fSource = new SourceView("html", true, true);
        fSource->SetEditable(true);
        fSource->SetWrap(true);
        fSource->SetSource(html, "hypertext");
        auto* apply = new BButton("apply", "Apply", new BMessage(kApplyHTML));
        apply->SetToolTip("Replace the node in the page with this HTML (Alt+Enter)");
        auto* cancel = new BButton("cancel", "Cancel", new BMessage(B_QUIT_REQUESTED));
        BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
            .Add(fSource, 1)
            .AddGroup(B_HORIZONTAL, 8).SetInsets(8, 6, 8, 6)
                .AddGlue()
                .Add(cancel)
                .Add(apply)
            .End();
        AddShortcut(B_ENTER, 0, new BMessage(kApplyHTML));
        SetSizeLimits(320, 10000, 200, 10000);
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what != kApplyHTML) {
            BWindow::MessageReceived(message);
            return;
        }
        BMessage apply(kApplyHTML);
        apply.AddInt32("node", fNode);
        apply.AddString("html", fSource->Text().c_str());
        fPanel.SendMessage(&apply);
        PostMessage(B_QUIT_REQUESTED);
    }
    void Show() override
    {
        BWindow::Show();
        if (LockLooper()) {
            fSource->TextView()->MakeFocus(true);
            UnlockLooper();
        }
    }

private:
    BMessenger fPanel;
    int fNode;
    SourceView* fSource;
};

ElementsPanel::ElementsPanel(Session& session, DevToolsHost& host)
    : BView("Elements", 0), fSession(session), fHost(host)
{
    fPick = new BButton("pick", "Select Element", new BMessage(kPickToggled));
    fPick->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);
    fPick->SetToolTip("Choose an element by pointing at it in the page (Shift+Alt+C)");
    fSearch = new BTextControl("search", "Find:", "", new BMessage(kSearchRun));
    fSearch->TextView()->SetExplicitMinSize(BSize(160, B_SIZE_UNSET));
    fSearch->SetModificationMessage(new BMessage(kSearchTyped));
    fSearch->SetToolTip("Text, a CSS selector or an XPath; Enter finds the next match (Alt+F)");
    fSearchCount = new BStringView("search-count", "");
    fSearchCount->SetExplicitMinSize(BSize(be_plain_font->StringWidth("0000 of 0000"), B_SIZE_UNSET));
    fSearchPrevious = new BButton("search-previous", "Previous", new BMessage(kSearchPrevious));
    fSearchNext = new BButton("search-next", "Next", new BMessage(kSearchNext));
    fSearchPrevious->SetEnabled(false);
    fSearchNext->SetEnabled(false);

    fTree = new Tree(*this, session.DOM(), host);
    auto* treeScroll = new BScrollView("tree-scroll", fTree, 0, true, true, B_NO_BORDER);
    treeScroll->SetExplicitMinSize(BSize(200, 80));
    fCrumbs = new BStringView("crumbs", "");
    fCrumbs->SetTruncation(B_TRUNCATE_BEGINNING);
    fCrumbs->SetExplicitMinSize(BSize(40, B_SIZE_UNSET));
    fCrumbs->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    fCrumbs->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    fNotice = new BStringView("notice", "");
    fNotice->SetTruncation(B_TRUNCATE_END);
    fNotice->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    fNotice->SetHighColor(CurrentSourcePalette().removed);

    fStyleFilter = new BTextControl("style-filter", "Filter:", "", nullptr);
    fStyleFilter->SetModificationMessage(new BMessage(kStyleFilterChanged));
    fStyleFilter->SetToolTip("Show declarations whose property or value contains this");
    fForce = new BButton("force", ":hov", new BMessage(kForceMenu));
    fForce->SetToolTip("Force :hover, :focus and other states on the selected element");
    fNewRule = new BButton("new-rule", "+", new BMessage(kNewRuleButton));
    fNewRule->SetToolTip("New style rule for the selected element");
    fStyles = new Styles(*this, session.DOM(), host);
    auto* stylesScroll = new BScrollView("styles-scroll", fStyles, 0, true, true, B_NO_BORDER);
    auto* stylesTab = new BGroupView(B_VERTICAL, 0);
    BLayoutBuilder::Group<>(stylesTab)
        .AddGroup(B_HORIZONTAL, 6).SetInsets(6, 4, 6, 4)
            .Add(fStyleFilter)
            .Add(fForce)
            .Add(fNewRule)
        .End()
        .Add(stylesScroll, 1);

    fBox = new BoxModel(session.DOM());
    fComputedSearch = new BTextControl("computed-filter", "Filter:", "", nullptr);
    fComputedSearch->SetModificationMessage(new BMessage(kComputedFilterChanged));
    fComputedAll = new BCheckBox("computed-all", "Show all", new BMessage(kComputedAllChanged));
    fComputedAll->SetToolTip("Every property, not only those the page's rules set or the element inherits");
    fComputed = new BColumnListView("computed", 0, B_NO_BORDER);
    fComputed->AddColumn(new BStringColumn("Property", 190, 60, 1000, B_TRUNCATE_END), kComputedName);
    fComputed->AddColumn(new BStringColumn("Value", 260, 60, 4000, B_TRUNCATE_END), kComputedValue);
    fComputed->SetSortingEnabled(true);
    fComputed->SetSortColumn(fComputed->ColumnAt(kComputedName), false, true);
    auto* computedTab = new BGroupView(B_VERTICAL, 0);
    BLayoutBuilder::Group<>(computedTab)
        .Add(fBox)
        .AddGroup(B_HORIZONTAL, 8).SetInsets(6, 4, 6, 4)
            .Add(fComputedSearch, 1)
            .Add(fComputedAll)
        .End()
        .Add(fComputed, 1);

    fSide = new SideTabs(this);
    fSide->SetBorder(B_NO_BORDER);
    const auto add = [this](BView* view, const char* label) {
        auto* tab = new BTab();
        fSide->AddTab(view, tab);
        tab->SetLabel(label);
    };
    // The role and name assistive technology would be given, and the states.
    fAccessibility = new BColumnListView("accessibility", 0, B_NO_BORDER);
    fAccessibility->AddColumn(new BStringColumn("Property", 190, 60, 1000, B_TRUNCATE_END), kComputedName);
    fAccessibility->AddColumn(new BStringColumn("Value", 260, 60, 4000, B_TRUNCATE_END), kComputedValue);
    fAccessibility->SetSortingEnabled(false);
    // The listeners its events reach: its own, then those of the nodes above it.
    fListeners = new BColumnListView("listeners", 0, B_NO_BORDER);
    fListeners->AddColumn(new BStringColumn("Event", 110, 40, 400, B_TRUNCATE_END), kListenerEvent);
    fListeners->AddColumn(new BStringColumn("On", 150, 40, 1000, B_TRUNCATE_END), kListenerNode);
    fListeners->AddColumn(new BStringColumn("Handler", 150, 40, 1000, B_TRUNCATE_END), kListenerHandler);
    fListeners->AddColumn(new BStringColumn("Options", 160, 40, 1000, B_TRUNCATE_END), kListenerOptions);
    fListeners->SetSortingEnabled(false);
    stylesTab->SetName("Styles");
    computedTab->SetName("Computed");
    fAccessibility->SetName("Accessibility");
    fListeners->SetName("Event Listeners");
    add(stylesTab, "Styles");
    add(computedTab, "Computed");
    add(fAccessibility, "Accessibility");
    add(fListeners, "Event Listeners");

    auto* split = new BSplitView(B_HORIZONTAL, 2);
    split->SetCollapsible(false);
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .AddGroup(B_HORIZONTAL, 8).SetInsets(8, 6, 8, 6)
            .Add(fPick)
            .AddStrut(8)
            .Add(fSearch, 1)
            .Add(fSearchCount)
            .Add(fSearchPrevious)
            .Add(fSearchNext)
        .End()
        .AddSplit(split, 1)
            .AddGroup(B_VERTICAL, 0, 3)
                .Add(treeScroll, 1)
                .AddGroup(B_HORIZONTAL, 8).SetInsets(8, 3, 8, 3)
                    .Add(fCrumbs, 3)
                    .Add(fNotice, 2)
                .End()
            .End()
            .Add(fSide, 2)
        .End();
    split->SetItemWeight(0, 3, false);
    split->SetItemWeight(1, 2, false);
}

void ElementsPanel::AttachedToWindow()
{
    BView::AttachedToWindow();
    AdoptParentColors();
    for (BControl* control : std::initializer_list<BControl*> {fPick, fSearch, fSearchPrevious, fSearchNext, fStyleFilter,
        fForce, fNewRule, fComputedSearch, fComputedAll})
        control->SetTarget(this);
}

void ElementsPanel::SetShown(bool shown)
{
    fShown = shown;
    auto& dom = fSession.DOM();
    if (shown) dom.Enable();
    else if (!dom.Picking()) dom.HideHighlight();
    fHost.Sync();
}

void ElementsPanel::SetPicking(bool picking)
{
    fSession.DOM().SetPicking(picking);
    fHost.Sync();
    ShowPicking();
}

bool ElementsPanel::Picking() const { return fSession.DOM().Picking(); }

void ElementsPanel::InspectAt(float x, float y)
{
    fSession.DOM().InspectAt(x, y);
    fHost.Sync();
}

void ElementsPanel::FocusSearch()
{
    fSearch->MakeFocus(true);
    fSearch->TextView()->SelectAll();
}

bool ElementsPanel::TakePicked() { return std::exchange(fPicked, false); }

void ElementsPanel::MessageReceived(BMessage* message)
{
    auto& dom = fSession.DOM();
    switch (message->what) {
        case kPickToggled: SetPicking(fPick->Value() == B_CONTROL_ON); break;
        case kSearchRun: {
            const std::string query = fSearch->Text();
            // Enter again goes to the next match; Shift+Enter to the one before.
            if (query == dom.SearchQuery() && dom.SearchCount()) dom.SearchStep(modifiers() & B_SHIFT_KEY ? -1 : 1);
            else dom.Search(query);
            fHost.Sync();
            break;
        }
        case kSearchTyped:
            // Find as the text is typed, once the typing pauses.
            fSearchTimer = std::make_unique<BMessageRunner>(BMessenger(this), BMessage(kSearchSettled), 300000, 1);
            break;
        case kSearchSettled:
            fSearchTimer.reset();
            if (fSearch->Text() != dom.SearchQuery()) {
                dom.Search(fSearch->Text());
                fHost.Sync();
            }
            break;
        case kSearchPrevious:
        case kSearchNext:
            if (fSearch->Text() != dom.SearchQuery()) dom.Search(fSearch->Text());
            else dom.SearchStep(message->what == kSearchNext ? 1 : -1);
            fHost.Sync();
            break;
        case kStyleFilterChanged: fStyles->SetFilter(fStyleFilter->Text()); break;
        case kNewRuleButton: fStyles->Command(kNewRule, -1, -1); break;
        case kForceMenu: {
            const auto forced = dom.ForcedPseudoClasses(dom.Selected());
            auto* menu = new BPopUpMenu("force", false, false);
            for (const char* name : kPseudoClasses) {
                auto* choose = new BMessage(kForcePseudo);
                choose->AddString("class", name);
                auto* item = new BMenuItem((std::string(":") + name).c_str(), choose);
                item->SetMarked(std::find(forced.begin(), forced.end(), name) != forced.end());
                item->SetEnabled(dom.Selected() != 0);
                menu->AddItem(item);
            }
            menu->SetTargetForItems(this);
            menu->SetAsyncAutoDestruct(true);
            const auto frame = fForce->ConvertToScreen(fForce->Bounds());
            menu->Go(frame.LeftBottom() + BPoint(0, 1), true, false, frame, true);
            break;
        }
        case kForcePseudo: {
            const std::string name = message->GetString("class", "");
            auto forced = dom.ForcedPseudoClasses(dom.Selected());
            if (const auto found = std::find(forced.begin(), forced.end(), name); found != forced.end()) forced.erase(found);
            else forced.push_back(name);
            dom.SetForcedPseudoClasses(dom.Selected(), std::move(forced));
            ShowForced();
            fHost.Sync();
            break;
        }
        case kComputedFilterChanged:
        case kComputedAllChanged: ShowComputed(); break;
        case kSideTabChanged:
            AskAccessibility();
            fHost.Sync();
            break;
        case kApplyHTML:
            dom.SetOuterHTML(message->GetInt32("node", 0), message->GetString("html", ""));
            fHost.Sync();
            break;
        default: BView::MessageReceived(message);
    }
}

void ElementsPanel::Apply(const Changes& changes)
{
    if (!changes.elements) return;
    auto& dom = fSession.DOM();
    const auto what = dom.TakeChanges();
    if (what.tree) fTree->Rebuild();
    else if (what.nodes) fTree->Invalidate();
    if (what.selection) {
        if (!what.tree) fTree->SelectionChanged();
        ShowSelection();
    }
    if (what.styles || what.selection) {
        ShowStyles();
        ShowComputed();
        AskAccessibility();
    }
    if (what.accessibility) ShowAccessibility();
    if (what.listeners) ShowListeners();
    if (what.search) ShowSearch();
    if (what.picking) ShowPicking();
    if (what.picked) fPicked = true;
    if (what.outerHTML) {
        const auto& answer = dom.OuterHTML();
        if (!answer.error.empty()) Notice(answer.error);
        else if (answer.purpose == kForCopy) CopyToClipboard(answer.html);
        else if (answer.purpose == kForEdit) {
            const auto* node = dom.Find(answer.node);
            auto* editor = new HTMLEditor(BMessenger(this), answer.node, answer.html, node ? node->Label() : std::string("node"),
                Window()->Frame());
            editor->Show();
        }
    }
    if (what.source) OpenSource();
    if (!what.notice.empty()) Notice(what.notice);
}

void ElementsPanel::ShowSelection()
{
    const auto& dom = fSession.DOM();
    fCrumbs->SetText(dom.Breadcrumbs(dom.Selected()).c_str());
    fNotice->SetText("");
    ShowForced();
}

void ElementsPanel::ShowForced()
{
    // The button says what is forced on the selected element.
    std::string label;
    for (const auto& name : fSession.DOM().ForcedPseudoClasses(fSession.DOM().Selected())) label += ":" + name + " ";
    if (!label.empty()) label.pop_back();
    fForce->SetLabel(label.empty() ? ":hov" : label.c_str());
}

void ElementsPanel::ShowStyles()
{
    fStyles->Rebuild();
    fBox->Invalidate();
}

void ElementsPanel::ShowComputed()
{
    const auto& dom = fSession.DOM();
    const auto filter = LowerCase(fComputedSearch->Text());
    const bool all = fComputedAll->Value() == B_CONTROL_ON;
    fComputed->Clear();
    const auto& styles = dom.Styles();
    if (styles.node != dom.Selected()) return;
    for (const auto& [name, value] : styles.computed) {
        if (!all && !dom.Declared(name)) continue;
        if (!filter.empty() && LowerCase(name + ": " + value).find(filter) == std::string::npos) continue;
        auto* row = new BRow();
        row->SetField(new BStringField(name.c_str()), kComputedName);
        row->SetField(new BStringField(value.c_str()), kComputedValue);
        fComputed->AddRow(row);
    }
}

void ElementsPanel::AskAccessibility()
{
    // Each side tab that asks the page is asked for when it shows.
    fAccessibility->Clear();
    fListeners->Clear();
    if (fSide->Selection() == kAccessibilityTab) fSession.DOM().RequestAccessibility();
    if (fSide->Selection() == kListenersTab) fSession.DOM().RequestListeners();
}

void ElementsPanel::ShowListeners()
{
    const auto& dom = fSession.DOM();
    const auto& listeners = dom.Listeners();
    fListeners->Clear();
    if (listeners.node != dom.Selected()) return;
    const auto add = [&](const std::string& event, const std::string& node, const std::string& handler, const std::string& options) {
        auto* row = new BRow();
        row->SetField(new BStringField(event.c_str()), kListenerEvent);
        row->SetField(new BStringField(node.c_str()), kListenerNode);
        row->SetField(new BStringField(handler.c_str()), kListenerHandler);
        row->SetField(new BStringField(options.c_str()), kListenerOptions);
        fListeners->AddRow(row);
    };
    if (!listeners.error.empty()) add("error", listeners.error, "", "");
    else if (listeners.listeners.empty()) add("No listeners", "", "", "");
    for (const auto& listener : listeners.listeners) {
        std::string options = listener.capture ? "capture" : "bubble";
        if (listener.once) options += ", once";
        if (listener.passive) options += ", passive";
        if (listener.attribute) options += ", attribute";
        if (listener.disabled) options += ", disabled";
        auto handler = listener.handler;
        if (!listener.location.empty()) handler += " (" + listener.location + ")";
        add(listener.type, listener.node, handler, options);
    }
}

void ElementsPanel::ShowAccessibility()
{
    const auto& dom = fSession.DOM();
    const auto& accessibility = dom.Accessibility();
    fAccessibility->Clear();
    if (accessibility.node != dom.Selected()) return;
    auto add = [&](const std::string& name, const std::string& value) {
        auto* row = new BRow();
        row->SetField(new BStringField(name.c_str()), kComputedName);
        row->SetField(new BStringField(value.c_str()), kComputedValue);
        fAccessibility->AddRow(row);
    };
    if (!accessibility.error.empty()) add("error", accessibility.error);
    for (const auto& [name, value] : accessibility.properties) add(name, value);
}

void ElementsPanel::ShowSearch()
{
    const auto& dom = fSession.DOM();
    std::string text;
    if (dom.Searching()) text = "Finding…";
    else if (!dom.SearchQuery().empty() && dom.Loaded())
        text = dom.SearchCount() ? std::to_string(dom.SearchIndex() + 1) + " of " + std::to_string(dom.SearchCount()) : "No matches";
    fSearchCount->SetText(text.c_str());
    fSearchPrevious->SetEnabled(dom.SearchCount() > 1);
    fSearchNext->SetEnabled(dom.SearchCount() > 1);
}

void ElementsPanel::ShowPicking()
{
    const bool picking = fSession.DOM().Picking();
    if ((fPick->Value() == B_CONTROL_ON) != picking) fPick->SetValue(picking ? B_CONTROL_ON : B_CONTROL_OFF);
}

void ElementsPanel::Notice(const std::string& text) { fNotice->SetText(text.c_str()); }

void ElementsPanel::OpenSource()
{
    const auto& source = fSession.DOM().Source();
    auto* window = new SourceWindow(source.title, Window()->Frame());
    if (!source.error.empty()) window->ShowNote(source.error);
    else window->ShowText(source.text, "css", source.line, source.title);
    window->Show();
}

bool ElementsPanel::Command(const std::string& action, const std::string& argument, std::string& error)
{
    if (action.rfind("elements-", 0) != 0) return false;
    auto& dom = fSession.DOM();
    const auto verb = action.substr(9);
    const auto number = [&](size_t index) {
        // The argument's numbers, separated by commas.
        size_t start = 0;
        for (size_t i = 0; i < index; ++i) {
            start = argument.find(',', start);
            if (start == std::string::npos) return -1L;
            ++start;
        }
        return std::strtol(argument.c_str() + start, nullptr, 10);
    };
    const auto rest = [&](size_t commas) {
        size_t start = 0;
        for (size_t i = 0; i < commas && start != std::string::npos; ++i) {
            start = argument.find(',', start);
            if (start != std::string::npos) ++start;
        }
        return start == std::string::npos ? std::string() : argument.substr(start);
    };
    if (verb == "row") {
        // Selects the node of a line of the tree, from 0.
        const int node = fTree->RowOf(static_cast<size_t>(std::max(0L, number(0))));
        if (!node) error = "No such line";
        else dom.Select(node);
    } else if (verb == "expand" || verb == "collapse") {
        const int node = argument.empty() ? dom.Selected() : static_cast<int>(number(0));
        if (!dom.Find(node)) error = "No such node";
        else dom.SetExpanded(node, verb == "expand");
    } else if (verb == "expand-all") dom.ExpandAll(argument.empty() ? dom.Selected() : static_cast<int>(number(0)));
    else if (verb == "search") {
        fSearch->SetText(argument.c_str());
        dom.Search(argument);
    } else if (verb == "search-next") dom.SearchStep(1);
    else if (verb == "search-previous") dom.SearchStep(-1);
    else if (verb == "pick") SetPicking(argument != "off");
    else if (verb == "inspect") InspectAt(static_cast<float>(std::atof(argument.c_str())),
        static_cast<float>(std::atof(rest(1).c_str())));
    else if (verb == "highlight") {
        if (argument.empty()) dom.HideHighlight();
        else dom.Highlight(static_cast<int>(number(0)));
    } else if (verb == "attributes") dom.SetAttributesText(dom.Selected(), argument);
    else if (verb == "add-attribute") dom.AddAttributes(dom.Selected(), argument);
    else if (verb == "text") {
        int node = dom.Selected();
        if (const auto* element = dom.Find(node); element && element->IsElement() && dom.InlineText(node)) node = element->children[0];
        dom.SetNodeText(node, argument);
    } else if (verb == "tag") dom.SetTagName(dom.Selected(), argument);
    else if (verb == "html") dom.SetOuterHTML(dom.Selected(), argument);
    else if (verb == "copy-html") dom.RequestOuterHTML(dom.Selected(), kForCopy);
    else if (verb == "edit-html") dom.RequestOuterHTML(dom.Selected(), kForEdit);
    else if (verb == "delete") dom.Remove(dom.Selected());
    else if (verb == "undo") dom.Undo();
    else if (verb == "redo") dom.Redo();
    else if (verb == "scroll") dom.ScrollIntoView(dom.Selected());
    else if (verb == "toggle") dom.ToggleDeclaration(static_cast<size_t>(number(0)), static_cast<size_t>(number(1)));
    else if (verb == "edit") dom.EditDeclaration(static_cast<size_t>(number(0)), static_cast<size_t>(number(1)), rest(2));
    else if (verb == "add") dom.AddDeclaration(static_cast<size_t>(number(0)), rest(1));
    else if (verb == "source") dom.RequestSheetSource(static_cast<size_t>(number(0)));
    else if (verb == "refresh") dom.RefreshStyles();
    else if (verb == "selector") dom.SetRuleSelector(static_cast<size_t>(number(0)), rest(1));
    else if (verb == "new-rule") dom.AddRule(argument.empty() ? dom.Selector(dom.Selected()) : argument);
    else if (verb == "force") {
        // "hover,focus", or "" for none, on the selected element.
        std::vector<std::string> classes;
        for (size_t from = 0; from < argument.size();) {
            const auto end = std::min(argument.find(',', from), argument.size());
            if (end > from) classes.push_back(argument.substr(from, end - from));
            from = end + 1;
        }
        dom.SetForcedPseudoClasses(dom.Selected(), std::move(classes));
        ShowForced();
    }
    else if (verb == "filter") {
        fStyleFilter->SetText(argument.c_str());
        fStyles->SetFilter(argument);
    } else if (verb == "computed-all") {
        fComputedAll->SetValue(argument == "on" ? B_CONTROL_ON : B_CONTROL_OFF);
        ShowComputed();
    } else if (verb == "tab") fSide->Select(argument == "computed" ? kComputedTab : argument == "accessibility" ? kAccessibilityTab
        : argument == "listeners" ? kListenersTab : kStylesTab);
    else if (verb == "edit-line") {
        // Opens the tree's editor as a double click does.
        fTree->Command(argument == "tag" ? kEditTag : argument == "text" ? kEditText : argument == "add-attribute" ? kAddAttribute
            : kEditAttributes, dom.Selected());
    } else if (verb == "edit-style") fStyles->Command(kEditDeclaration, static_cast<int>(number(0)), static_cast<int>(number(1)));
    else if (verb == "add-style") fStyles->Command(kAddDeclaration, static_cast<int>(number(0)), -1);
    else if (verb == "editor" || verb == "editor-cancel") {
        // Ends the open editor as Enter (or Escape) does.
        if (!fTree->FinishEditing(argument, verb == "editor") && !fStyles->FinishEditing(argument, verb == "editor"))
            error = "No editor is open";
    }
    else error = "Unknown action";
    fHost.Sync();
    return true;
}

std::string ElementsPanel::StateJSON()
{
    using nlohmann::json;
    const auto& dom = fSession.DOM();
    const auto& styles = dom.Styles();
    json blocks = json::array();
    for (const auto& block : styles.blocks) {
        json declarations = json::array();
        for (const auto& declaration : block.declarations)
            declarations.push_back(std::string(declaration.enabled ? "[x] " : "[ ] ") + declaration.name + ": " + declaration.value
                + (declaration.important ? " !important" : "") + (declaration.overridden ? " (overridden)" : "")
                + (declaration.parsedOk ? "" : " (invalid)"));
        const auto* from = dom.Find(block.inheritedFrom);
        blocks.push_back({{"selector", block.selector}, {"location", block.Location()}, {"editable", block.Editable()},
            {"groupings", block.groupings}, {"pseudo", block.pseudo}, {"inheritedFrom", from ? from->Label() : std::string()},
            {"declarations", declarations}});
    }
    json computed = json::object();
    for (const auto& [name, value] : styles.computed) computed[name] = value;
    const auto& box = styles.box;
    json boxes = {{"known", box.known}, {"width", box.width}, {"height", box.height}, {"position", box.position},
        {"margin", {box.margin[0], box.margin[1], box.margin[2], box.margin[3]}},
        {"border", {box.border[0], box.border[1], box.border[2], box.border[3]}},
        {"padding", {box.padding[0], box.padding[1], box.padding[2], box.padding[3]}}};
    const auto* selected = dom.Find(dom.Selected());
    json selection = selected ? json {{"id", selected->id}, {"label", selected->Label()}, {"breadcrumbs", dom.Breadcrumbs(selected->id)},
        {"attributes", dom.AttributesText(selected->id)}, {"selector", dom.Selector(selected->id)},
        {"forced", dom.ForcedPseudoClasses(selected->id)}} : json(nullptr);
    json state = {{"shown", fShown}, {"enabled", dom.Enabled()}, {"loaded", dom.Loaded()}, {"nodes", dom.NodeCount()},
        {"lines", fTree->Count()}, {"tree", fTree->Lines(300)}, {"selected", selection}, {"picking", dom.Picking()},
        {"editing", fTree->Editing() ? "tree" : fStyles->Editing() ? "styles" : ""},
        {"search", {{"query", dom.SearchQuery()}, {"count", dom.SearchCount()}, {"index", dom.SearchIndex()}, {"finding", dom.Searching()},
            {"shown", fSearchCount->Text()}}},
        {"styles", {{"node", styles.node}, {"loading", dom.StylesLoading()}, {"error", styles.error}, {"blocks", blocks}}},
        {"computed", computed}, {"computedShown", fComputed->CountRows()}, {"box", boxes}, {"crumbs", fCrumbs->Text()},
        {"sideTab", fSide->Selection()}, {"listeners", [&] {
            json rows = json::array();
            for (const auto& listener : dom.Listeners().listeners)
                rows.push_back(listener.type + " on " + listener.node + ": " + listener.handler + (listener.capture ? " (capture)" : ""));
            return json {{"node", dom.Listeners().node}, {"error", dom.Listeners().error}, {"rows", rows}, {"shown", fListeners->CountRows()}};
        }()}, {"accessibility", [&] {
            json properties = json::object();
            for (const auto& [name, value] : dom.Accessibility().properties) properties[name] = value;
            return json {{"node", dom.Accessibility().node}, {"error", dom.Accessibility().error}, {"properties", properties},
                {"shown", fAccessibility->CountRows()}};
        }()},
        {"notice", fNotice->Text()}, {"outerHTML", dom.OuterHTML().html.substr(0, 4000)}, {"source", {{"title", dom.Source().title},
            {"line", dom.Source().line}, {"size", dom.Source().text.size()}, {"error", dom.Source().error}}}};
    return state.dump(-1, ' ', false, json::error_handler_t::replace);
}
}
#endif
