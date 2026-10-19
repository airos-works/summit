#if SUMMIT_MODERN_WEBKIT
#include "SourceView.h"
#include <Clipboard.h>
#include <Font.h>
#include <ILexer.h>
#include <Lexilla.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <SciLexer.h>
#include <Window.h>
#include <algorithm>
#include <cstring>

namespace summit {
namespace {
constexpr uint32 kCopySelection = 'svcp', kCopyAll = 'svca', kSelectAll = 'svsa', kToggleWrap = 'svwr';

constexpr rgb_color Color(unsigned value)
{
    return {static_cast<uint8>(value >> 16), static_cast<uint8>(value >> 8), static_cast<uint8>(value), 255};
}

// Kiri's built-in palettes (its src/core/ColorTheme.cpp), in its order of roles.
SourcePalette MakePalette(bool dark, const unsigned colors[15])
{
    return {Color(colors[0]), Color(colors[1]), Color(colors[2]), Color(colors[3]), Color(colors[4]), Color(colors[5]),
        Color(colors[6]), Color(colors[7]), Color(colors[8]), Color(colors[9]), Color(colors[10]), Color(colors[11]),
        Color(colors[12]), Color(colors[13]), Color(colors[14]), dark};
}
// background, panel, text, muted, border, accent, selection, line, comment,
// keyword, string, number, type, added, removed.
constexpr unsigned kDaylight[15] = {0xfafbfd, 0xf0f2f6, 0x273448, 0x68788d, 0xcfd6e0, 0x2567c6, 0xc8ddf8, 0xf0f4fa,
    0x718096, 0x8047a8, 0x3d7a40, 0xa55028, 0x187f86, 0x24723b, 0xc1384c};
constexpr unsigned kObsidian[15] = {0x171b22, 0x202630, 0xdce3ed, 0x8c9bad, 0x343e4b, 0x78b7ff, 0x334e70, 0x202936,
    0x8195a6, 0xc6a0f6, 0xa6da95, 0xf5a97f, 0x8bd5ca, 0xa6da95, 0xed8796};

sptr_t Pointer(const char* text) { return reinterpret_cast<sptr_t>(text); }
}

rgb_color SourcePalette::Tint(rgb_color color, float weight) const
{
    const auto mix = [weight](uint8 from, uint8 to) {
        return static_cast<uint8>(std::clamp(from + (to - from) * weight, 0.f, 255.f));
    };
    return {mix(background.red, color.red), mix(background.green, color.green), mix(background.blue, color.blue), 255};
}

const SourcePalette& CurrentSourcePalette()
{
    static const SourcePalette light = MakePalette(false, kDaylight), dark = MakePalette(true, kObsidian);
    const auto document = ui_color(B_DOCUMENT_BACKGROUND_COLOR);
    return document.red + document.green + document.blue < 384 ? dark : light;
}

int SciColor(rgb_color color) { return color.red | (color.green << 8) | (color.blue << 16); }

void CopyToClipboard(const std::string& text)
{
    if (!be_clipboard->Lock()) return;
    be_clipboard->Clear();
    if (BMessage* clip = be_clipboard->Data()) clip->AddData("text/plain", B_MIME_TYPE, text.data(), text.size());
    be_clipboard->Commit();
    be_clipboard->Unlock();
}

SourceView::SourceView(const char* name, bool lineNumbers, bool horizontalBar)
    : BScintillaView(name, B_FRAME_EVENTS, horizontalBar, true, B_NO_BORDER)
    , fLineNumbers(lineNumbers)
{
    SetExplicitMinSize(BSize(80, 40));
    SendMessage(SCI_SETCODEPAGE, SC_CP_UTF8);
    SendMessage(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    SendMessage(SCI_SETMARGINWIDTHN, 0, 0);
    SendMessage(SCI_SETMARGINTYPEN, 1, SC_MARGIN_SYMBOL);
    SendMessage(SCI_SETMARGINMASKN, 1, SC_MASK_FOLDERS);
    SendMessage(SCI_SETMARGINSENSITIVEN, 1, 1);
    SendMessage(SCI_SETMARGINWIDTHN, 1, 0);
    SendMessage(SCI_SETMARGINWIDTHN, 2, 0);
    SendMessage(SCI_SETMARGINLEFT, 0, 8);
    SendMessage(SCI_SETMARGINRIGHT, 0, 8);
    SendMessage(SCI_MARKERDEFINE, SC_MARKNUM_FOLDER, SC_MARK_BOXPLUS);
    SendMessage(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPEN, SC_MARK_BOXMINUS);
    SendMessage(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEREND, SC_MARK_BOXPLUSCONNECTED);
    SendMessage(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPENMID, SC_MARK_BOXMINUSCONNECTED);
    SendMessage(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_TCORNER);
    SendMessage(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERSUB, SC_MARK_VLINE);
    SendMessage(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERTAIL, SC_MARK_LCORNER);
    SendMessage(SCI_SETAUTOMATICFOLD, SC_AUTOMATICFOLD_SHOW | SC_AUTOMATICFOLD_CLICK | SC_AUTOMATICFOLD_CHANGE);
    SendMessage(SCI_SETTABWIDTH, 4);
    SendMessage(SCI_SETSCROLLWIDTH, 1);
    SendMessage(SCI_SETSCROLLWIDTHTRACKING, 1);
    SendMessage(SCI_SETENDATLASTLINE, 1);
    SendMessage(SCI_SETLAYOUTCACHE, SC_CACHE_PAGE);
    SendMessage(SCI_SETIDLESTYLING, SC_IDLESTYLING_ALL);
    SendMessage(SCI_SETCARETWIDTH, 1);
    SendMessage(SCI_SETCARETLINEVISIBLE, 0);
    SendMessage(SCI_SETUNDOCOLLECTION, 0);
    SendMessage(SCI_USEPOPUP, 0);
    SendMessage(SCI_SETWRAPVISUALFLAGS, SC_WRAPVISUALFLAG_NONE);
    SendMessage(SCI_SETWRAPINDENTMODE, SC_WRAPINDENT_INDENT);
    if (!horizontalBar) {
        SendMessage(SCI_SETHSCROLLBAR, 0);
        SetWrap(true);
    }
    ApplyPalette();
    SendMessage(SCI_SETREADONLY, 1);
}

void SourceView::AllAttached()
{
    BScintillaView::AllAttached();
    // The Haiku port may mark styles valid before it has a drawing surface
    // (Kiri's Editor::AllAttached()): realize the fonts again.
    ApplyPalette();
    StyleLexer();
    UpdateLineNumbers();
}

void SourceView::ApplyPalette()
{
    const auto& palette = CurrentSourcePalette();
    font_family family;
    font_style style;
    be_fixed_font->GetFamilyAndStyle(&family, &style);
    SendMessage(SCI_STYLESETFONT, STYLE_DEFAULT, Pointer(family));
    SendMessage(SCI_STYLESETSIZE, STYLE_DEFAULT, static_cast<sptr_t>(be_fixed_font->Size()));
    SendMessage(SCI_STYLESETFORE, STYLE_DEFAULT, SciColor(palette.text));
    SendMessage(SCI_STYLESETBACK, STYLE_DEFAULT, SciColor(palette.background));
    SendMessage(SCI_STYLECLEARALL);
    SendMessage(SCI_SETSELFORE, 1, SciColor(palette.text));
    SendMessage(SCI_SETSELBACK, 1, SciColor(palette.selection));
    SendMessage(SCI_SETCARETFORE, SciColor(palette.accent));
    SendMessage(SCI_STYLESETBACK, STYLE_LINENUMBER, SciColor(palette.background));
    SendMessage(SCI_STYLESETFORE, STYLE_LINENUMBER, SciColor(palette.muted));
    SendMessage(SCI_SETFOLDMARGINCOLOUR, 1, SciColor(palette.background));
    SendMessage(SCI_SETFOLDMARGINHICOLOUR, 1, SciColor(palette.background));
    for (int marker = SC_MARKNUM_FOLDEREND; marker <= SC_MARKNUM_FOLDEROPEN; ++marker) {
        SendMessage(SCI_MARKERSETFORE, marker, SciColor(palette.background));
        SendMessage(SCI_MARKERSETBACK, marker, SciColor(palette.muted));
    }
    const auto run = [&](devtools::TextStyle style, rgb_color color, bool bold = false) {
        const int number = kRunStyle + static_cast<int>(style);
        SendMessage(SCI_STYLESETFORE, number, SciColor(color));
        SendMessage(SCI_STYLESETBOLD, number, bold);
    };
    run(devtools::TextStyle::Title, palette.text, true);
    run(devtools::TextStyle::Name, palette.type);
    run(devtools::TextStyle::Muted, palette.muted);
    run(devtools::TextStyle::Good, palette.added, true);
    run(devtools::TextStyle::Warning, palette.number, true);
    run(devtools::TextStyle::Error, palette.removed, true);
}

// The colours of a lexer's styles, as Kiri's Editor::ApplyTheme() gives them.
void SourceView::StyleLexer()
{
    const auto& palette = CurrentSourcePalette();
    const auto style = [this](int number, rgb_color color, bool bold = false) {
        SendMessage(SCI_STYLESETFORE, number, SciColor(color));
        SendMessage(SCI_STYLESETBOLD, number, bold);
    };
    if (fLexer == "hypertext" || fLexer == "xml") {
        for (int s : {SCE_H_TAG, SCE_H_TAGUNKNOWN, SCE_H_TAGEND, SCE_H_XMLSTART, SCE_H_XMLEND}) style(s, palette.type);
        for (int s : {SCE_H_ATTRIBUTE, SCE_H_ATTRIBUTEUNKNOWN}) style(s, palette.keyword);
        for (int s : {SCE_H_DOUBLESTRING, SCE_H_SINGLESTRING, SCE_H_CDATA}) style(s, palette.string);
        style(SCE_H_COMMENT, palette.comment);
        style(SCE_H_NUMBER, palette.number);
        for (int s : {SCE_HJ_KEYWORD, SCE_HJ_WORD}) style(s, palette.keyword);
        for (int s : {SCE_HJ_DOUBLESTRING, SCE_HJ_SINGLESTRING}) style(s, palette.string);
        for (int s : {SCE_HJ_COMMENT, SCE_HJ_COMMENTLINE}) style(s, palette.comment);
    } else if (fLexer == "json") {
        style(SCE_JSON_NUMBER, palette.number);
        style(SCE_JSON_STRING, palette.string);
        style(SCE_JSON_PROPERTYNAME, palette.type);
        style(SCE_JSON_KEYWORD, palette.keyword);
        style(SCE_JSON_OPERATOR, palette.accent);
        style(SCE_JSON_LINECOMMENT, palette.comment);
        style(SCE_JSON_BLOCKCOMMENT, palette.comment);
    } else if (fLexer == "cpp") {
        for (int s : {SCE_C_COMMENT, SCE_C_COMMENTLINE, SCE_C_COMMENTDOC, SCE_C_COMMENTLINEDOC}) style(s, palette.comment);
        for (int s : {SCE_C_WORD, SCE_C_WORD2}) style(s, palette.keyword);
        for (int s : {SCE_C_STRING, SCE_C_CHARACTER, SCE_C_STRINGRAW, SCE_C_REGEX}) style(s, palette.string);
        style(SCE_C_NUMBER, palette.number);
        style(SCE_C_OPERATOR, palette.accent);
    } else if (fLexer != "null") {
        // Lexilla names its styles, which serves every other language.
        const int count = static_cast<int>(SendMessage(SCI_GETNAMEDSTYLES));
        for (int s = 0; s < count && s < kRunStyle; ++s) {
            const auto length = SendMessage(SCI_NAMEOFSTYLE, s, 0);
            if (length <= 0 || length > 1024) continue;
            std::string name(static_cast<size_t>(length) + 1, '\0');
            SendMessage(SCI_NAMEOFSTYLE, s, Pointer(name.data()));
            const auto has = [&](const char* word) { return name.find(word) != std::string::npos; };
            if (has("comment")) style(s, palette.comment);
            else if (has("string") || has("character")) style(s, palette.string);
            else if (has("number")) style(s, palette.number);
            else if (has("keyword") || has("word")) style(s, palette.keyword);
            else if (has("type") || has("class") || has("tag")) style(s, palette.type);
            else if (has("operator")) style(s, palette.accent);
        }
    }
    Invalidate();
}

void SourceView::ReplaceText(const std::string& text)
{
    SendMessage(SCI_SETREADONLY, 0);
    SendMessage(SCI_CLEARALL);
    SendMessage(SCI_ALLOCATE, text.size() + 1);
    SendMessage(SCI_ADDTEXT, text.size(), Pointer(text.data()));
    SendMessage(SCI_SETREADONLY, fEditable ? 0 : 1);
    if (fEditable) SendMessage(SCI_EMPTYUNDOBUFFER);
    SendMessage(SCI_SETSEL, 0, 0);
    SendMessage(SCI_SETXOFFSET, 0);
    SendMessage(SCI_SETSCROLLWIDTH, 1);
    UpdateLineNumbers();
}

void SourceView::UpdateLineNumbers()
{
    if (!fLineNumbers) return;
    const auto digits = std::to_string(SendMessage(SCI_GETLINECOUNT)).size();
    const std::string widest(std::max<size_t>(3, digits), '9');
    const auto width = SendMessage(SCI_TEXTWIDTH, STYLE_LINENUMBER, Pointer(widest.c_str()));
    SendMessage(SCI_SETMARGINWIDTHN, 0, std::max<sptr_t>(36, width + 16));
}

void SourceView::SetSource(const std::string& text, const char* lexer)
{
    fLexer = lexer && *lexer ? lexer : "null";
    // Very large texts are shown plain: colouring them holds the window up.
    const bool large = text.size() > 8 * 1024 * 1024;
    SendMessage(SCI_SETILEXER, 0, reinterpret_cast<sptr_t>(CreateLexer(large ? "null" : fLexer.c_str())));
    // The keywords Kiri gives these lexers.
    const char* keywords = "";
    if (fLexer == "json") keywords = "true false null";
    else if (fLexer == "cpp")
        keywords = "async await break case catch class const continue debugger default delete do else export extends false "
            "finally for function if import in instanceof let new null of return static super switch this throw true try "
            "typeof undefined var void while with yield";
    else if (fLexer == "hypertext" || fLexer == "xml")
        keywords = "html head body title meta link script style div span p a img ul li ol table tr td th form input button "
            "textarea section article header footer nav main h1 h2 h3 h4 h5 h6 br hr svg path g rect circle";
    SendMessage(SCI_SETKEYWORDS, 0, Pointer(keywords));
    if (fLexer == "hypertext" || fLexer == "xml")
        SendMessage(SCI_SETKEYWORDS, 1, Pointer("async await break case catch class const continue debugger default delete do "
            "else export extends false finally for function if import in instanceof let new null of return static super switch "
            "this throw true try typeof var void while with yield"));
    const bool folds = !large && fLexer != "null";
    SendMessage(SCI_SETPROPERTY, reinterpret_cast<uptr_t>("fold"), Pointer(folds ? "1" : "0"));
    SendMessage(SCI_SETPROPERTY, reinterpret_cast<uptr_t>("fold.html"), Pointer("1"));
    SendMessage(SCI_SETPROPERTY, reinterpret_cast<uptr_t>("fold.compact"), Pointer("0"));
    SendMessage(SCI_SETPROPERTY, reinterpret_cast<uptr_t>("lexer.json.allow.comments"), Pointer("1"));
    SendMessage(SCI_SETPROPERTY, reinterpret_cast<uptr_t>("lexer.cpp.track.preprocessor"), Pointer("0"));
    SendMessage(SCI_SETMARGINWIDTHN, 1, folds ? 16 : 0);
    ApplyPalette();
    StyleLexer();
    ReplaceText(text);
}

void SourceView::SetStyled(const devtools::StyledText& styled)
{
    fLexer = "null";
    SendMessage(SCI_SETILEXER, 0, 0);
    SendMessage(SCI_SETMARGINWIDTHN, 1, 0);
    ApplyPalette();
    ReplaceText(styled.text);
    for (const auto& run : styled.runs) {
        if (run.offset + run.length > styled.text.size()) continue;
        SendMessage(SCI_STARTSTYLING, run.offset);
        SendMessage(SCI_SETSTYLING, run.length, kRunStyle + static_cast<int>(run.style));
    }
}

void SourceView::SetEditable(bool editable)
{
    fEditable = editable;
    SendMessage(SCI_SETREADONLY, editable ? 0 : 1);
    SendMessage(SCI_SETUNDOCOLLECTION, editable ? 1 : 0);
    SendMessage(SCI_SETCARETLINEVISIBLE, editable ? 1 : 0);
    if (editable) SendMessage(SCI_SETCARETLINEBACK, SciColor(CurrentSourcePalette().line));
}

void SourceView::SetWrap(bool wrap) { SendMessage(SCI_SETWRAPMODE, wrap ? SC_WRAP_WORD : SC_WRAP_NONE); }
bool SourceView::Wraps() { return SendMessage(SCI_GETWRAPMODE) != SC_WRAP_NONE; }

std::string SourceView::Text()
{
    const auto size = static_cast<size_t>(SendMessage(SCI_GETLENGTH));
    std::string text(size + 1, '\0');
    SendMessage(SCI_GETTEXT, size + 1, Pointer(text.data()));
    text.resize(size);
    return text;
}

void SourceView::Copy()
{
    if (SendMessage(SCI_GETSELECTIONEMPTY)) {
        CopyAll();
        return;
    }
    // With the terminating NUL.
    const auto size = static_cast<size_t>(SendMessage(SCI_GETSELTEXT, 0, 0));
    std::string text(size + 1, '\0');
    SendMessage(SCI_GETSELTEXT, 0, Pointer(text.data()));
    text.resize(std::strlen(text.c_str()));
    CopyToClipboard(text);
}

void SourceView::CopyAll() { CopyToClipboard(Text()); }

void SourceView::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case kCopySelection: Copy(); break;
        case kCopyAll: CopyAll(); break;
        case kSelectAll: SendMessage(SCI_SELECTALL); break;
        case kToggleWrap: SetWrap(!Wraps()); break;
        default: BScintillaView::MessageReceived(message);
    }
}

void SourceView::ContextMenu(BPoint where)
{
    auto* menu = new BPopUpMenu("source", false, false);
    auto* copy = new BMenuItem("Copy", new BMessage(kCopySelection), 'C');
    copy->SetEnabled(!SendMessage(SCI_GETSELECTIONEMPTY));
    menu->AddItem(copy);
    menu->AddItem(new BMenuItem("Copy All", new BMessage(kCopyAll)));
    menu->AddItem(new BMenuItem("Select All", new BMessage(kSelectAll), 'A'));
    menu->AddSeparatorItem();
    auto* wrap = new BMenuItem("Wrap Lines", new BMessage(kToggleWrap));
    wrap->SetMarked(Wraps());
    menu->AddItem(wrap);
    menu->SetTargetForItems(this);
    menu->SetAsyncAutoDestruct(true);
    menu->Go(where, true, true, true);
}
}
#endif
