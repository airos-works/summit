#pragma once
#if SUMMIT_MODERN_WEBKIT
#include "core/DevTools.h"
#include <ScintillaView.h>
#include <string>

namespace summit {
// The colours of the developer tools' text: Kiri's Daylight theme, or its
// Obsidian theme where the system's documents are dark, so that a source
// looks in the browser as it does in the editor.
struct SourcePalette {
    rgb_color background, panel, text, muted, border, accent, selection, line,
        comment, keyword, string, number, type, added, removed;
    bool dark;
    // A colour of the palette over its background: weight 0 is the
    // background, 1 the colour.
    rgb_color Tint(rgb_color color, float weight) const;
};
const SourcePalette& CurrentSourcePalette();
// Scintilla's colours are 0xBBGGRR.
int SciColor(rgb_color color);

// Text that is read, selected and copied, not edited: in the fixed font,
// coloured by a Lexilla lexer or by the runs of a StyledText. Scintilla is
// what Kiri shows its sources with; lexers, keywords and the colours of their
// styles are chosen as Kiri's editor chooses them.
class SourceView : public BScintillaView {
public:
    // Without a horizontal bar the text is wrapped.
    explicit SourceView(const char* name, bool lineNumbers = true, bool horizontalBar = true);
    void AllAttached() override;
    void MessageReceived(BMessage* message) override;
    void ContextMenu(BPoint where) override;
    // lexer is a Lexilla name ("json", "xml", "hypertext", "cpp", "css") or "null".
    void SetSource(const std::string& text, const char* lexer);
    void SetStyled(const devtools::StyledText& text);
    void SetWrap(bool wrap);
    bool Wraps();
    // Text that can be typed into (Edit as HTML), with undo.
    void SetEditable(bool editable);
    std::string Text();
    // The selection, or all of the text without one, to the clipboard.
    void Copy();
    void CopyAll();
    // The inner view, which has the focus and takes the keys.
    BView* TextView() { return Target(); }

protected:
    // The default style and what does not depend on the text. Subclasses
    // define their own styles after calling it.
    virtual void ApplyPalette();
    void ReplaceText(const std::string& text);
    void UpdateLineNumbers();
    // Styles of StyledText runs begin here; lexers use those below 40.
    static constexpr int kRunStyle = 64;
    const bool fLineNumbers;

private:
    void StyleLexer();
    std::string fLexer = "null";
    bool fEditable = false;
};

// Puts text on the clipboard as text/plain.
void CopyToClipboard(const std::string& text);
}
#endif
