#pragma once
#if SUMMIT_MODERN_WEBKIT
#include "core/PageSource.h"
#include <Window.h>
#include <memory>
#include <string>

class BButton;
class BCheckBox;
class BMessageRunner;
class BStringView;
class BTextControl;
class BWebKitInspectorSession;

namespace summit {
class SourceView;

// A text read in the fixed font, coloured by what it is, with line numbers,
// line wrapping and find: View › View Page Source, and the style sheets the
// Elements panel opens at a rule. docs/developer-tools.md describes it.
class SourceWindow final : public BWindow {
public:
    // near: the window it belongs to, which it is placed over.
    SourceWindow(const std::string& title, BRect near);
    ~SourceWindow() override;
    // lexer as SourceView takes it; line (from 1) is shown at the top and selected.
    void ShowText(const std::string& text, const char* lexer, int line = 0, const std::string& info = { });
    // No text, and why.
    void ShowNote(const std::string& note);
    // View Page Source: asks the page for its document as the server sent it.
    // Before Show(); the session ends once the page has answered. pageTarget
    // ("page-N"), if known, is the page's target, which the engine tells only
    // the page's first connection.
    void FetchPageSource(std::shared_ptr<BWebKitInspectorSession> inspector, const std::string& pageTarget = { });
    void MessageReceived(BMessage* message) override;

private:
    void Drain();
    void Finish();
    // Selects the next (1) or previous (-1) match of the find field.
    void Find(int direction, bool fromSelection = true);
    // Marks every match and counts them.
    void MarkMatches();
    SourceView* fSource;
    BTextControl* fFind;
    BButton* fPrevious;
    BButton* fNext;
    BStringView* fMatches;
    BCheckBox* fWrap;
    BStringView* fInfo;
    std::string fQuery;
    size_t fMatchCount = 0;
    std::shared_ptr<BWebKitInspectorSession> fInspector;
    devtools::PageSourceRequest fRequest;
    std::unique_ptr<BMessageRunner> fPulse;
    bigtime_t fAsked = 0;
};
}
#endif
