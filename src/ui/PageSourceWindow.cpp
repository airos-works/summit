#if SUMMIT_MODERN_WEBKIT
#include "PageSourceWindow.h"
#include "SourceView.h"
#include "core/DevToolsFormat.h"
#include <WebKit/WebKitInspector.h>
#include <Button.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <MessageRunner.h>
#include <Screen.h>
#include <StringView.h>
#include <TextControl.h>
#include <algorithm>

namespace summit {
namespace {
constexpr uint32 kFindChanged = 'swfc', kFindNext = 'swfn', kFindPrevious = 'swfp', kWrapChanged = 'swwr', kFocusFind = 'swff',
    kPulse = 'swpu';
// Scintilla's indicators 8 and up are the container's.
constexpr int kFindIndicator = 8;
// How long a page may take to give its source.
constexpr bigtime_t kPatience = 20000000;
// Matches marked; more are counted but not marked.
constexpr size_t kMarked = 20000;

BRect FrameNear(BRect near)
{
    const BRect screen = BScreen().Frame();
    BRect frame(0, 0, 979, 719);
    frame.OffsetTo(near.left + 40, near.top + 50);
    frame.right = std::min(frame.right, frame.left + screen.Width() - 20);
    frame.bottom = std::min(frame.bottom, frame.top + screen.Height() - 50);
    if (frame.right > screen.right - 5) frame.OffsetBy(screen.right - 5 - frame.right, 0);
    if (frame.bottom > screen.bottom - 5) frame.OffsetBy(0, screen.bottom - 5 - frame.bottom);
    if (frame.left < 5) frame.OffsetTo(5, frame.top);
    if (frame.top < 30) frame.OffsetTo(frame.left, 30);
    return frame;
}

sptr_t Pointer(const char* text) { return reinterpret_cast<sptr_t>(text); }

std::string CountText(size_t count, const char* one, const char* many)
{
    return std::to_string(count) + " " + (count == 1 ? one : many);
}
}

SourceWindow::SourceWindow(const std::string& title, BRect near)
    : BWindow(FrameNear(near), title.c_str(), B_DOCUMENT_WINDOW, B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS)
{
    fSource = new SourceView("source", true, true);
    fSource->SetWrap(true);
    const auto& palette = CurrentSourcePalette();
    fSource->SendMessage(SCI_INDICSETSTYLE, kFindIndicator, INDIC_ROUNDBOX);
    fSource->SendMessage(SCI_INDICSETFORE, kFindIndicator, SciColor(rgb_color {240, 180, 40, 255}));
    fSource->SendMessage(SCI_INDICSETALPHA, kFindIndicator, palette.dark ? 110 : 90);
    fSource->SendMessage(SCI_INDICSETUNDER, kFindIndicator, 1);
    fFind = new BTextControl("find", "Find:", "", new BMessage(kFindNext));
    fFind->SetModificationMessage(new BMessage(kFindChanged));
    fFind->TextView()->SetExplicitMinSize(BSize(220, B_SIZE_UNSET));
    fFind->SetToolTip("Enter finds the next match, Shift+Enter the one before (Alt+G, Shift+Alt+G)");
    fPrevious = new BButton("previous", "Previous", new BMessage(kFindPrevious));
    fNext = new BButton("next", "Next", new BMessage(kFindNext));
    fMatches = new BStringView("matches", "");
    fMatches->SetExplicitMinSize(BSize(be_plain_font->StringWidth("00000 matches"), B_SIZE_UNSET));
    fWrap = new BCheckBox("wrap", "Wrap lines", new BMessage(kWrapChanged));
    fWrap->SetValue(B_CONTROL_ON);
    fInfo = new BStringView("info", "");
    fInfo->SetTruncation(B_TRUNCATE_MIDDLE);
    fInfo->SetExplicitMinSize(BSize(40, B_SIZE_UNSET));
    fInfo->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    fInfo->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .AddGroup(B_HORIZONTAL, 8).SetInsets(8, 6, 8, 6)
            .Add(fFind, 1)
            .Add(fPrevious)
            .Add(fNext)
            .Add(fMatches)
            .AddGlue()
            .Add(fWrap)
        .End()
        .Add(fSource, 1)
        .AddGroup(B_HORIZONTAL, 0).SetInsets(8, 3, 8, 3)
            .Add(fInfo)
        .End();
    AddShortcut('F', 0, new BMessage(kFocusFind));
    AddShortcut('G', 0, new BMessage(kFindNext));
    AddShortcut('G', B_SHIFT_KEY, new BMessage(kFindPrevious));
    SetSizeLimits(420, 10000, 240, 10000);
    fPrevious->SetEnabled(false);
    fNext->SetEnabled(false);
}

SourceWindow::~SourceWindow()
{
    fPulse.reset();
    fInspector.reset();
}

void SourceWindow::ShowText(const std::string& text, const char* lexer, int line, const std::string& info)
{
    fSource->SetSource(text, lexer);
    fInfo->SetText(info.c_str());
    if (line > 0) {
        const auto target = std::min<sptr_t>(line - 1, fSource->SendMessage(SCI_GETLINECOUNT) - 1);
        fSource->SendMessage(SCI_SETFIRSTVISIBLELINE, std::max<sptr_t>(0, fSource->SendMessage(SCI_VISIBLEFROMDOCLINE, target) - 3));
        fSource->SendMessage(SCI_SETSEL, fSource->SendMessage(SCI_POSITIONFROMLINE, target),
            fSource->SendMessage(SCI_GETLINEENDPOSITION, target));
    }
    MarkMatches();
}

void SourceWindow::ShowNote(const std::string& note)
{
    fSource->SetSource(note, "null");
    fInfo->SetText("");
}

void SourceWindow::FetchPageSource(std::shared_ptr<BWebKitInspectorSession> inspector, const std::string& pageTarget)
{
    fInspector = std::move(inspector);
    fRequest.AssumeTarget(pageTarget);
    for (const auto& message : fRequest.TakeOutgoing()) fInspector->Send(message.c_str());
    fAsked = system_time();
    fInfo->SetText("Asking the page for its source…");
    // An announcement that found the window's port full is made up for here,
    // and a page that never answers is given up.
    BMessage pulse(kPulse);
    fPulse = std::make_unique<BMessageRunner>(BMessenger(this), &pulse, 500000);
}

void SourceWindow::Drain()
{
    if (!fInspector) return;
    std::vector<std::string> messages;
    fInspector->Take(messages);
    for (const auto& message : messages) fRequest.Receive(message);
    for (const auto& message : fRequest.TakeOutgoing()) fInspector->Send(message.c_str());
    if (fRequest.Status() == devtools::PageSourceRequest::State::Waiting && system_time() - fAsked > kPatience)
        fRequest.Fail("The page did not give its source.");
    if (fRequest.Status() != devtools::PageSourceRequest::State::Waiting) Finish();
}

void SourceWindow::Finish()
{
    // The page's tools end with the session: it has nothing more to say.
    fPulse.reset();
    fInspector.reset();
    if (fRequest.Status() != devtools::PageSourceRequest::State::Done) {
        ShowNote(fRequest.Error());
        return;
    }
    const auto& text = fRequest.Text();
    const auto kind = devtools::DetectBodyKind(fRequest.MimeType(), text);
    const auto lines = static_cast<size_t>(std::count(text.begin(), text.end(), '\n')) + (text.empty() || text.back() == '\n' ? 0 : 1);
    std::string info = fRequest.URL();
    if (!fRequest.MimeType().empty()) info += " · " + fRequest.MimeType();
    info += " · " + devtools::FormatBytes(text.size()) + " · " + CountText(lines, "line", "lines");
    SetTitle(("Source of " + fRequest.URL()).c_str());
    ShowText(text, devtools::BodyKindLexer(kind), 0, info);
}

void SourceWindow::MarkMatches()
{
    const auto length = fSource->SendMessage(SCI_GETLENGTH);
    fSource->SendMessage(SCI_SETINDICATORCURRENT, kFindIndicator);
    fSource->SendMessage(SCI_INDICATORCLEARRANGE, 0, length);
    fMatchCount = 0;
    if (!fQuery.empty()) {
        fSource->SendMessage(SCI_SETSEARCHFLAGS, 0);
        for (sptr_t from = 0; from < length;) {
            fSource->SendMessage(SCI_SETTARGETRANGE, from, length);
            const auto found = fSource->SendMessage(SCI_SEARCHINTARGET, fQuery.size(), Pointer(fQuery.c_str()));
            if (found < 0) break;
            const auto end = fSource->SendMessage(SCI_GETTARGETEND);
            if (++fMatchCount <= kMarked) fSource->SendMessage(SCI_INDICATORFILLRANGE, found, end - found);
            from = std::max(end, found + 1);
        }
    }
    fMatches->SetText(fQuery.empty() ? "" : fMatchCount ? CountText(fMatchCount, "match", "matches").c_str() : "No matches");
    fPrevious->SetEnabled(fMatchCount > 0);
    fNext->SetEnabled(fMatchCount > 0);
}

void SourceWindow::Find(int direction, bool fromSelection)
{
    if (fQuery.empty() || !fMatchCount) return;
    const auto length = fSource->SendMessage(SCI_GETLENGTH);
    const auto start = fSource->SendMessage(SCI_GETSELECTIONSTART), end = fSource->SendMessage(SCI_GETSELECTIONEND);
    const auto search = [&](sptr_t from, sptr_t to) {
        // A range that ends before it starts is searched backwards.
        fSource->SendMessage(SCI_SETTARGETRANGE, from, to);
        return fSource->SendMessage(SCI_SEARCHINTARGET, fQuery.size(), Pointer(fQuery.c_str()));
    };
    fSource->SendMessage(SCI_SETSEARCHFLAGS, 0);
    sptr_t found;
    if (direction > 0) {
        const auto from = fromSelection ? end : start;
        found = search(from, length);
        if (found < 0) found = search(0, from);
    } else {
        found = search(start, 0);
        if (found < 0) found = search(length, end);
    }
    if (found < 0) return;
    const auto foundEnd = fSource->SendMessage(SCI_GETTARGETEND);
    fSource->SendMessage(SCI_SETSEL, found, foundEnd);
    fSource->SendMessage(SCI_SCROLLRANGE, foundEnd, found);
}

void SourceWindow::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case B_WEBKIT_INSPECTOR_MESSAGES:
        case kPulse: Drain(); break;
        case kFindChanged: {
            // Scripts (and tests) can give the text to find with the message.
            if (const char* text = nullptr; message->FindString("text", &text) == B_OK) fFind->SetText(text);
            const std::string query = fFind->Text();
            if (query == fQuery) break;
            fQuery = query;
            MarkMatches();
            // As it is typed, the match stays where it begins.
            Find(1, false);
            break;
        }
        case kFindNext:
            // Shift+Enter in the field goes back.
            Find(modifiers() & B_SHIFT_KEY ? -1 : 1);
            break;
        case kFindPrevious: Find(-1); break;
        case kWrapChanged: fSource->SetWrap(fWrap->Value() == B_CONTROL_ON); break;
        case kFocusFind:
            fFind->MakeFocus(true);
            fFind->TextView()->SelectAll();
            break;
        default: BWindow::MessageReceived(message);
    }
}
}
#endif
