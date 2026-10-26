#pragma once
#if SUMMIT_MODERN_WEBKIT
#include "SourceView.h"
#include "core/DevTools.h"
#include <OutlineListView.h>
#include <View.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

class BButton;
class BCardLayout;
class BCheckBox;
class BColumnListView;
class BFilePanel;
class BListView;
class BMenuField;
class BMessageRunner;
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

// Objects as a tree: opening a row asks the page for the object's properties
// (Runtime.getDisplayableProperties), and an object property opens in turn.
// The Console shows an entry's objects in one, the Sources panel a paused
// frame's scopes (DevToolsConsole.cpp).
class ObjectTree : public BOutlineListView {
public:
    ObjectTree(devtools::Session& session, DevToolsHost& host);
    // Rows for these objects; a single one opens at once.
    void ShowObjects(const std::vector<devtools::ConsoleObject>& objects);
    void Clear();
    // The page answered for these objects.
    void Answered(const std::vector<std::string>& objects);
    // Opens the first shown row whose text starts with prefix (tests).
    bool OpenRow(const std::string& prefix);
    // The rows shown, indented by depth.
    std::vector<std::string> Rows();
    void MouseDown(BPoint where) override;
    void KeyDown(const char* bytes, int32 count) override;

private:
    struct Item;
    void AddObject(const std::string& text, const std::string& objectId, Item* parent);
    void Open(Item* item);
    void LoadExpanded();
    void Fill(Item* item);
    devtools::Session& fSession;
    DevToolsHost& fHost;
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
    void ShowFrames(const devtools::Request& request);
    void ShowSummary();
    bool AtEnd() const;
    // The requests listed, in the list's order (HAR export).
    std::vector<const devtools::Request*> ShownRequests() const;
    bool ExportHar(const std::string& path, std::string& error) const;
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
    // Copy as cURL, Replay, Block This Address, Unblock All.
    BButton* fRequestMenu;
    BCheckBox* fDisableCache;
    BMenuField* fThrottling;
    BButton* fExportHar;
    std::unique_ptr<BFilePanel> fHarPanel;
    BColumnListView* fList;
    BStringView* fSummary;
    BCardLayout* fDetailCards;
    BTabView* fDetails;
    SourceView* fHeaders;
    BodyView* fRequestBody;
    BodyView* fResponseBody;
    SourceView* fMessages;
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
    void SaveCookie();
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

// The page's scripts, their breakpoints, and the call stack and scopes while
// the page is paused (the inspector's Debugger domain, DevToolsSources.cpp).
// The debugger is turned on when the panel is first shown.
// Performance: a recording of the page's main-thread work (the inspector's
// Timeline): time by kind of work, the longest records with what was in
// them and where they came from, and the frames drawn
// (DevToolsPerformance.cpp).
class PerformancePanel : public BView {
public:
    PerformancePanel(devtools::Session& session, DevToolsHost& host);
    void AttachedToWindow() override;
    void MessageReceived(BMessage* message) override;
    void Apply(const devtools::Changes& changes);
    bool Command(const std::string& action, const std::string& argument, std::string& error);
    std::string StateJSON();

private:
    class Row;
    void Show();
    void ShowStatus();
    void ShowRecord();
    devtools::Session& fSession;
    DevToolsHost& fHost;
    bool fWasRecording = false;
    // The longest records' summaries, as JSON, by row.
    std::vector<std::string> fLongest;
    BButton* fRecord;
    BStringView* fStatus;
    BColumnListView* fTypes;
    BColumnListView* fRecords;
    BodyView* fDetail;
};

class SourcesPanel : public BView {
public:
    SourcesPanel(devtools::Session& session, DevToolsHost& host);
    void AttachedToWindow() override;
    void MessageReceived(BMessage* message) override;
    void Apply(const devtools::Changes& changes);
    void SetShown(bool shown);
    bool Command(const std::string& action, const std::string& argument, std::string& error);
    std::string StateJSON();

private:
    class Code;
    void RebuildScripts();
    void SelectScript(const std::string& id);
    void ShowSource();
    void ShowMarkers();
    void ShowPause();
    void SelectFrame(int index);
    void ToggleBreakpoint(int line);
    const devtools::Script* Selected() const;
    // Pretty printing: the shown text of a script, and moving between a
    // shown line and a place in the page's document.
    bool Pretty() const;
    // The original source shown (of the selected script's map), or -1.
    int OriginalIndex() const;
    const devtools::PrettyScript& Formatted(const devtools::Script& script);
    int ShownLine(const devtools::Script& script, int line, int column);
    devtools::SourcePosition DocumentPlace(const devtools::Script& script, int shownLine);
    std::map<std::string, devtools::PrettyScript> fFormatted;
    // Pretty printing follows each script (on for minified ones) until it
    // is chosen by hand.
    bool fPrettyChosen = false;
    devtools::Session& fSession;
    DevToolsHost& fHost;
    std::string fSelectedScript;
    std::vector<std::string> fScriptIDs;
    int fFrame = -1;
    bool fShownOnce = false;
    BButton* fPauseResume;
    BButton* fStepOver;
    BButton* fStepInto;
    BButton* fStepOut;
    BCheckBox* fContentScripts;
    BCheckBox* fPrettyPrint;
    BStringView* fStatus;
    BListView* fScripts;
    Code* fCode;
    BListView* fFrames;
    ObjectTree* fScopes;
};
}
#endif
