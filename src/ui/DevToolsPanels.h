#pragma once
#if SUMMIT_MODERN_WEBKIT
#include "SourceView.h"
#include "core/DevTools.h"
#include <View.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

class BButton;
class BCardLayout;
class BCheckBox;
class BColumnListView;
class BMenuField;
class BMessageRunner;
class BOutlineListView;
class BRow;
class BSplitView;
class BStringView;
class BTabView;
class BTextControl;

// The two panels of the Developer Tools window (DevToolsWindow.h). They show
// one devtools::Session, which the window owns and feeds; all of it happens
// on the window's thread.
namespace summit {
// What a panel asks of the window after it has changed the session: send
// what the session wants sent, and show what changed in both panels.
class DevToolsHost {
public:
    virtual void Sync() = 0;
protected:
    ~DevToolsHost() = default;
};

// A request's or response's body: formatted when it is JSON, XML or HTML,
// coloured by its kind, with a button that copies what is shown.
class BodyView : public BView {
public:
    explicit BodyView(const char* name);
    void AttachedToWindow() override;
    void MessageReceived(BMessage* message) override;
    // The body's bytes as they were sent, and its Content-Type.
    void ShowBody(const std::string& bytes, const std::string& mimeType);
    // No body to show, and why.
    void ShowNote(const std::string& note);
    // What Copy copies: the text as it is shown.
    std::string ShownText();
    // As the check boxes and the button do.
    void SetFormatted(bool formatted);
    void SetWrapped(bool wrapped);
    void CopyShown();
    // {"info", "note", "text", "formatted", "shows"}, for tests.
    std::string StateJSON();

private:
    void Update();
    class ImageView;
    std::string fBytes, fMimeType;
    devtools::BodyKind fKind = devtools::BodyKind::Text;
    bool fHasBody = false;
    BCheckBox* fFormat;
    BCheckBox* fWrap;
    BButton* fCopy;
    BStringView* fInfo;
    SourceView* fSource;
    ImageView* fImage;
    BStringView* fNote;
    BCardLayout* fCards;
};

class NetworkPanel : public BView {
public:
    NetworkPanel(devtools::Session& session, DevToolsHost& host);
    void AttachedToWindow() override;
    void MessageReceived(BMessage* message) override;
    void Apply(const devtools::Changes& changes);
    // kDeveloperToolsCommand: false if the action is not the panel's; error
    // says what was wrong with one that is.
    bool Command(const std::string& action, const std::string& argument, std::string& error);
    std::string StateJSON();

private:
    class Row;
    void Select(Row* row);
    bool Shows(const devtools::Request& request) const;
    void Rebuild();
    void Fill(Row& row, const devtools::Request& request);
    void ShowSelection();
    void ShowBody(const devtools::Request& request);
    void ShowSummary();
    bool AtEnd() const;
    devtools::Session& fSession;
    DevToolsHost& fHost;
    std::map<uint64, Row*> fRows;
    uint64 fSelected = 0;
    std::string fFilterText;
    // Of the types menu; 0 is every type.
    int32 fCategory = 0;
    BButton* fClear;
    BCheckBox* fPreserve;
    BTextControl* fFilter;
    BMenuField* fTypes;
    BColumnListView* fList;
    BStringView* fSummary;
    BCardLayout* fDetailCards;
    BTabView* fDetails;
    SourceView* fHeaders;
    BodyView* fRequestBody;
    BodyView* fResponseBody;
};

// Local and session storage of the page's origins, and its cookies: read
// from the page when shown, followed as the page changes them, edited and
// deleted through it.
class StoragePanel : public BView {
public:
    StoragePanel(devtools::Session& session, DevToolsHost& host);
    void AttachedToWindow() override;
    void MessageReceived(BMessage* message) override;
    void Apply(const devtools::Changes& changes);
    // Whether the panel is the one shown: only then are areas read.
    void SetShown(bool shown);
    bool Command(const std::string& action, const std::string& argument, std::string& error);
    std::string StateJSON();

private:
    class AreaItem;
    class Row;
    void RebuildAreas();
    void SelectArea(devtools::StorageKind kind, const std::string& origin);
    void Load(bool again);
    void Fill();
    void ShowItem();
    void Delete();
    const devtools::StorageArea* Area() const;
    devtools::Session& fSession;
    DevToolsHost& fHost;
    bool fShown = false;
    bool fHasArea = false;
    devtools::StorageKind fKind = devtools::StorageKind::Local;
    std::string fOrigin;
    // The key (or cookie name, domain and path) of the selected row.
    std::string fSelected;
    std::string fFilterText;
    BButton* fRefresh;
    BButton* fDelete;
    BButton* fClear;
    BTextControl* fFilter;
    BOutlineListView* fAreas;
    BCardLayout* fTables;
    BColumnListView* fItems;
    BColumnListView* fCookies;
    BStringView* fSummary;
    BodyView* fValue;
    BView* fEditor;
    BTextControl* fKey;
    BTextControl* fNewValue;
    BButton* fSave;
};

// The page's DOM as a tree, followed as the page changes it, and the CSS of
// the selected element: matched rules, computed styles and its boxes. Nodes
// and styles are edited through the page; the element picker and Inspect
// Element choose a node in the page. DevToolsElements.cpp.
class ElementsPanel : public BView {
public:
    ElementsPanel(devtools::Session& session, DevToolsHost& host);
    void AttachedToWindow() override;
    void MessageReceived(BMessage* message) override;
    void Apply(const devtools::Changes& changes);
    // Whether the panel is the one shown: the page's document is read the
    // first time it is, and the page's highlight goes when it is not.
    void SetShown(bool shown);
    // The element picker, as its button and Shift+Alt+C do.
    void SetPicking(bool picking);
    bool Picking() const;
    // Inspect Element: the element at a point of the page, in CSS pixels.
    void InspectAt(float x, float y);
    void FocusSearch();
    // An element was chosen in the page since the last call: the tools
    // should come forward.
    bool TakePicked();
    bool Command(const std::string& action, const std::string& argument, std::string& error);
    std::string StateJSON();

private:
    class Tree;
    class Styles;
    class BoxModel;
    class HTMLEditor;
    void ShowSelection();
    void ShowForced();
    void ShowStyles();
    void ShowComputed();
    // The Accessibility tab: asked for only while it shows.
    void AskAccessibility();
    void ShowListeners();
    void ShowAccessibility();
    void ShowSearch();
    void ShowPicking();
    void Notice(const std::string& text);
    void OpenSource();
    devtools::Session& fSession;
    DevToolsHost& fHost;
    bool fShown = false, fPicked = false;
    std::string fComputedFilter;
    BButton* fPick;
    BTextControl* fSearch;
    // Finds a moment after the last key typed into fSearch.
    std::unique_ptr<BMessageRunner> fSearchTimer;
    BStringView* fSearchCount;
    BButton* fSearchPrevious;
    BButton* fSearchNext;
    Tree* fTree;
    BStringView* fCrumbs;
    BStringView* fNotice;
    BTabView* fSide;
    BTextControl* fStyleFilter;
    // :hov: the pseudo-classes forced on the selected element.
    BButton* fForce;
    BButton* fNewRule;
    Styles* fStyles;
    BoxModel* fBox;
    BTextControl* fComputedSearch;
    BCheckBox* fComputedAll;
    BColumnListView* fComputed;
    BColumnListView* fAccessibility;
    BColumnListView* fListeners;
};

class ConsolePanel : public BView {
public:
    ConsolePanel(devtools::Session& session, DevToolsHost& host);
    void AttachedToWindow() override;
    void MessageReceived(BMessage* message) override;
    void Apply(const devtools::Changes& changes);
    // A line of the input's history: older for -1, newer for 1.
    void StepHistory(int direction);
    bool Command(const std::string& action, const std::string& argument, std::string& error);
    std::string StateJSON();

private:
    class Log;
    class ObjectTree;
    bool Shows(const devtools::ConsoleEntry& entry) const;
    // Shows the objects of the entry (its serial) in the object tree.
    void Inspect(uint64 serial);
    void Rebuild();
    void ShowCounts();
    devtools::Session& fSession;
    DevToolsHost& fHost;
    devtools::LevelFilter fLevels;
    std::string fFilterText;
    std::vector<std::string> fHistory;
    size_t fHistoryIndex = 0;
    // The serial of the last entry the log shows, to redraw it when it repeats.
    uint64 fLastShown = 0;
    BButton* fClear;
    BCheckBox* fPreserve;
    BCheckBox* fErrors;
    BCheckBox* fWarnings;
    BCheckBox* fInfo;
    BCheckBox* fDebug;
    BTextControl* fFilter;
    BStringView* fCounts;
    Log* fLog;
    BSplitView* fSplit;
    ObjectTree* fObjects;
    BTextControl* fInput;
};
}
#endif
