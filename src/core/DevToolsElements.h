#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// The Elements panel's model: the page's DOM as the Web Inspector protocol's
// DOM domain gives it, followed as the page changes it, and the CSS of the
// selected element from the CSS domain. It belongs to the developer tools'
// Session (DevTools.h), which sends its commands to the page and gives it
// their answers and the page's DOM, CSS and Inspector events. Nothing here
// needs Haiku; src/ui/DevToolsElements.cpp shows it and tests/DevToolsTests.cpp
// covers it.
namespace summit::devtools {

// Node types, as the DOM numbers them.
enum DOMNodeType {
    kElementNode = 1, kTextNode = 3, kCDATANode = 4, kProcessingInstructionNode = 7, kCommentNode = 8,
    kDocumentNode = 9, kDocumentTypeNode = 10, kDocumentFragmentNode = 11
};

struct DOMAttribute {
    std::string name, value;
};

struct DOMNode {
    int id = 0;
    // The node it is shown under: its parent, the host of a shadow root or
    // pseudo element, the frame of a frame's document; 0 for the document.
    int parent = 0;
    int type = 0;
    std::string name, localName, value;
    std::vector<DOMAttribute> attributes;
    // Children that count (the protocol leaves out text of white space only),
    // and whether the page has told which they are.
    int childCount = 0;
    bool childrenKnown = false;
    std::vector<int> children;
    std::vector<int> shadowRoots, pseudoElements;
    int contentDocument = 0, templateContent = 0;
    // "before" or "after"; "open", "closed" or "user-agent".
    std::string pseudoType, shadowRootType;
    std::string documentURL, publicId, systemId;
    bool IsElement() const { return type == kElementNode; }
    const std::string* FindAttribute(std::string_view name) const;
    // The tag as shown: lower case for HTML.
    std::string Tag() const;
    // "div#main.note", as breadcrumbs and inherited styles name an element.
    std::string Label() const;
};

// One line of the tree: a node, or the closing tag of an expanded element.
struct TreeRow {
    int node = 0;
    int depth = 0;
    bool closing = false;
};

// The parts of a line, each drawn in its own colour.
enum class Piece { Punctuation, Tag, AttributeName, AttributeValue, Text, Comment, Muted, Pseudo };
struct RowPart {
    std::string text;
    Piece kind;
};

struct Declaration {
    std::string name, value;
    // As the style sheet has it ("color: red;", "/* color: red; */"), or
    // empty for a style the page made without source text.
    std::string text;
    bool important = false, enabled = true, parsedOk = true;
    // A declaration of the same property takes precedence over it: later in
    // the same rule, in a rule that wins, or nearer the element.
    bool overridden = false;
};

struct StyleBlock {
    enum Kind { Inline, Attributes, Rule };
    Kind kind = Rule;
    // "element.style", "Attributes style", or the rule's selectors.
    std::string selector;
    // @media and the like around the rule, innermost first.
    std::vector<std::string> groupings;
    // user-agent, user, author or inspector.
    std::string origin;
    std::string sourceURL;
    // Of the selector in the resource, from 1; 0 if unknown.
    int line = 0;
    // What CSS.setStyleText needs; empty for what cannot be edited.
    std::string styleSheetId;
    int ordinal = -1;
    // The rule's own (CSS.setRuleSelector); -1 for what is not a rule of the page.
    int ruleOrdinal = -1;
    // The ancestor whose style this is, for inherited blocks; 0 for the node's own.
    int inheritedFrom = 0;
    // "::before" for the rules of a pseudo element.
    std::string pseudo;
    std::vector<Declaration> declarations;
    bool Editable() const { return !styleSheetId.empty() && ordinal >= 0; }
    bool SelectorEditable() const { return kind == Rule && !styleSheetId.empty() && ruleOrdinal >= 0; }
    // "style.css:12", "user agent style sheet", or empty.
    std::string Location() const;
};

// The selected element's boxes, in CSS pixels; sides are top, right,
// bottom, left.
struct BoxModel {
    bool known = false;
    double width = 0, height = 0;
    double margin[4] = { }, border[4] = { }, padding[4] = { };
    std::string position, offsets[4], boxSizing, display;
};

struct NodeStyles {
    int node = 0;
    std::string error;
    std::vector<StyleBlock> blocks;
    // Every computed property, by name.
    std::vector<std::pair<std::string, std::string>> computed;
    BoxModel box;
};

// The selected element's accessibility properties, from
// DOM.getAccessibilityPropertiesForNode: role, label and states, as text.
// Asking turns accessibility on in the page's process until the tools
// close (the engine turns it off then on Haiku), so it is asked for only
// while the Accessibility tab shows.
struct NodeAccessibility {
    int node = 0;
    std::string error;
    std::vector<std::pair<std::string, std::string>> properties;
};

// The event listeners of the selected element and of the nodes above it
// that its events reach (DOM.getEventListenersForNode), nearest first.
struct NodeListener {
    std::string type, node, handler, location;
    bool capture = false, once = false, passive = false, attribute = false, disabled = false;
};
struct NodeListeners {
    int node = 0;
    std::string error;
    std::vector<NodeListener> listeners;
};

struct DOMChanges {
    // The lines of the tree must be made again.
    bool tree = false;
    // A node shown changed (its attributes or text).
    bool nodes = false;
    bool selection = false, styles = false, search = false, picking = false;
    // An element was chosen in the page (by the picker or Inspect Element):
    // the tools come forward.
    bool picked = false;
    // OuterHTML() or Source() arrived.
    bool outerHTML = false, source = false;
    // Accessibility() arrived.
    bool accessibility = false;
    // Listeners() arrived.
    bool listeners = false;
    // What went wrong with something asked of the page.
    std::string notice;
    bool Any() const
    {
        return tree || nodes || selection || styles || search || picking || picked || outerHTML || source || accessibility || listeners || !notice.empty();
    }
};

// The names of the attributes in text as an attribute editor takes it
// ("class=a id='b' hidden"), in lower case.
std::vector<std::string> AttributeNames(std::string_view text);
// Marks the declarations that are overridden, given blocks in the order they
// are shown (the element's own from the strongest, then each ancestor's). A
// shorthand (in longhands, with its parts) is overridden when every part is.
void MarkOverridden(std::vector<StyleBlock>& blocks, const std::map<std::string, std::vector<std::string>>& longhands = { });
// The text of a style after one change: a declaration switched on or off,
// replaced (empty text removes it), or a new one added at the end.
enum class StyleEdit { Toggle, Replace, Add };
std::string EditStyleText(const StyleBlock& block, size_t index, StyleEdit edit, const std::string& text = { });
// A CSS colour value (#rgb, #rrggbbaa, rgb()/rgba() with numbers or
// percentages, the basic named colours) as red, green, blue, alpha; empty
// for anything else. CSSColorText writes one back as #rrggbb(aa).
std::optional<std::array<uint8_t, 4>> ParseCSSColor(const std::string& value);
std::string CSSColorText(const std::array<uint8_t, 4>& color);

class DOMModel {
public:
    // Reads the page's document. Until it is called the model sends nothing;
    // the Elements panel calls it when it is first shown.
    void Enable();
    bool Enabled() const { return fEnabled; }
    // The document has been read.
    bool Loaded() const { return fDocument != 0; }
    DOMChanges TakeChanges() { return std::exchange(fChanges, { }); }
    bool HasChanges() const { return fChanges.Any(); }

    int Document() const { return fDocument; }
    const DOMNode* Find(int id) const;
    size_t NodeCount() const { return fNodes.size(); }
    // What is shown under a node: shadow roots, template content, ::before,
    // a frame's document, the children, ::after.
    std::vector<int> ShownChildren(int id) const;
    bool Expandable(int id) const;
    bool IsExpanded(int id) const { return fExpanded.count(id) > 0; }
    void SetExpanded(int id, bool expanded);
    // The node and everything under it, read from the page as needed.
    void ExpandAll(int id);
    void CollapseAll(int id);
    // An element whose only child is a short text shows it on its own line
    // (<title>Example</title>).
    bool InlineText(int id, std::string* text = nullptr) const;
    std::vector<TreeRow> Rows() const;
    std::vector<RowPart> DescribeRow(const TreeRow& row) const;
    // "html › body › div#main".
    std::string Breadcrumbs(int id) const;
    // A selector for the node: "#main > p:nth-child(2)".
    std::string Selector(int id) const;
    // The attributes as an editor shows them: name="value" ...
    std::string AttributesText(int id) const;
    // The path WebKit's DOM.pushNodeByPathToFrontend takes ("1,HTML,1,BODY").
    std::string PathOf(int id) const;

    int Selected() const { return fSelected; }
    // Selects a node and asks for its styles.
    void Select(int id);
    // Expands what is around the node so that its line is shown.
    void Reveal(int id);

    // Changes to the page. The page's DOM events then change the tree.
    void SetAttributesText(int id, const std::string& text);
    // Sets the attributes the text has and leaves the others.
    void AddAttributes(int id, const std::string& text);
    void SetNodeText(int id, const std::string& text);
    void SetTagName(int id, const std::string& name);
    void SetOuterHTML(int id, const std::string& html);
    void Remove(int id);
    void Undo();
    void Redo();
    void ScrollIntoView(int id);
    // Asks for a node's outer HTML, for a purpose of the caller's.
    void RequestOuterHTML(int id, int purpose);
    struct OuterHTMLAnswer {
        int node = 0, purpose = 0;
        std::string html, error;
    };
    const OuterHTMLAnswer& OuterHTML() const { return fOuterHTML; }

    // The page's highlight of a node (or none), and the element picker.
    void Highlight(int id);
    void HideHighlight();
    void SetPicking(bool picking);
    bool Picking() const { return fPicking; }
    // Selects the element at a point of the page's viewport, in CSS pixels
    // (Inspect Element); waits for the document if it has not been read.
    void InspectAt(double x, double y);

    // The selected element's styles, once read; they are read again when the
    // page changes them.
    const NodeStyles& Styles() const { return fStyles; }
    bool StylesLoading() const { return fStylesOutstanding > 0; }
    // The page changed the styles shown; they are read again in a moment
    // (TakeCommands() asks for them).
    bool StylesWaiting() const;
    void RefreshStyles();
    // Pseudo-classes forced on an element (CSS.forcePseudoState: "hover",
    // "focus"...), as if it were hovered or focused; its styles are read
    // again. The page forgets them when the tools close or it moves on.
    void SetForcedPseudoClasses(int id, std::vector<std::string> classes);
    // The selected element's computed value of a property (custom ones
    // included), or empty.
    std::string ComputedValue(const std::string& property) const;
    // Changes a rule's selector (CSS.setRuleSelector), then reads the styles again.
    void SetRuleSelector(size_t block, const std::string& selector);
    // Adds an empty rule for selector to the page's inspector style sheet
    // (made for the main frame the first time: CSS.createStyleSheet), as other
    // browsers' New Style Rule does; the styles are read again.
    void AddRule(const std::string& selector);
    std::vector<std::string> ForcedPseudoClasses(int id) const;
    // Asks for the selected element's accessibility properties.
    void RequestAccessibility();
    const NodeAccessibility& Accessibility() const { return fAccessibility; }
    // Asks for the selected element's event listeners.
    void RequestListeners();
    const NodeListeners& Listeners() const { return fListeners; }
    void ToggleDeclaration(size_t block, size_t index);
    // Empty text removes the declaration.
    void EditDeclaration(size_t block, size_t index, const std::string& text);
    void AddDeclaration(size_t block, const std::string& text);
    // Whether a property, or a shorthand of it, is declared for the node or
    // inherited by it: what the computed list shows unless asked for all.
    bool Declared(const std::string& property) const;

    // The text of a block's style sheet, to read it at the block's line.
    void RequestSheetSource(size_t block);
    struct SheetSource {
        std::string title, text, error;
        // In the text, from 1.
        int line = 0;
    };
    const SheetSource& Source() const { return fSource; }

    // Finds text, a selector or an XPath in the document; steps through
    // what was found, selecting it.
    void Search(const std::string& query);
    void SearchStep(int direction);
    const std::string& SearchQuery() const { return fSearchQuery; }
    size_t SearchCount() const { return fSearchResults.size(); }
    // The result selected, from 0; only meaningful with results.
    size_t SearchIndex() const { return fSearchIndex; }
    bool Searching() const { return fSearchWaiting; }

    // For the session. Commands carry tags; the session sends them to the
    // page and gives back the answers (the inner message's text).
    struct Command {
        uint64_t tag;
        std::string method, parameters;
    };
    std::vector<Command> TakeCommands();
    void Answered(uint64_t tag, std::string_view message);
    void Failed(uint64_t tag, const std::string& error);
    // An event of the page: DOM.*, CSS.*, Inspector.inspect and the Page
    // domain's load events.
    void Event(std::string_view message);
    // The page is in another process now: its document is read again.
    void TargetChanged();

private:
    friend struct DOMReader;
    struct Pending {
        enum Kind {
            Ignore, Document, Children, ExpandAll, Attributes, Matched, Inline, Computed, Resolve, Box, SetStyle, Edit,
            TagName, Remove, Search, SearchResults, RequestNode, Evaluate, Path, OuterHTML, SheetText, Properties, Accessibility, Listeners,
            RuleFrame, RuleSheet, AddRule
        };
        Kind kind;
        int node = 0;
        uint64_t generation = 0;
        int purpose = 0;
        std::string text;
    };
    uint64_t Send(const std::string& method, const std::string& parameters, Pending::Kind kind = Pending::Ignore, int node = 0);
    void Reset(bool forget);
    void RequestDocument();
    DOMNode* Node(int id);
    void Erase(int id);
    void Detach(int id);
    void StartStyles();
    void StylesArrived();
    void SetSelection(int id, bool styles);
    void SelectDefault();
    void Removed(int id);
    bool IsInheritable(const std::string& property) const;

    bool fEnabled = false, fDocumentWaiting = false, fPicking = false, fAgentsEnabled = false;
    int fDocument = 0, fSelected = 0, fHighlighted = 0;
    std::unordered_map<int, DOMNode> fNodes;
    std::set<int> fExpanded;
    // Where the selection goes when the node it names is removed.
    std::map<int, int> fAfterRemoval;
    std::map<int, std::vector<std::string>> fForced;
    // The selection to find again in the next document, and how often to try.
    std::string fRestorePath;
    int fRestoreTries = 0;
    bool fHasPoint = false;
    double fPointX = 0, fPointY = 0;

    NodeStyles fStyles, fNextStyles;
    NodeAccessibility fAccessibility;
    NodeListeners fListeners;
    // The inspector style sheet New Style Rule adds to, once made.
    std::string fInspectorSheet;
    int fStylesOutstanding = 0;
    bool fStylesStale = false;
    double fStylesStarted = 0;
    std::set<std::string> fDeclared;
    // The CSS answers that make up fNextStyles, kept until all have come.
    std::string fMatchedJSON, fInlineJSON;
    // Style sheets the page has: their resource, where in it they start, and
    // whether they are a <style> of the document.
    struct Sheet {
        std::string url;
        int startLine = 0;
        bool inlined = false;
    };
    std::map<std::string, Sheet> fSheets;
    std::set<std::string> fInherited;
    std::map<std::string, std::vector<std::string>> fLonghands;
    bool fPropertiesAsked = false;

    std::string fSearchQuery, fSearchID;
    std::vector<int> fSearchResults;
    size_t fSearchIndex = 0;
    bool fSearchWaiting = false;

    OuterHTMLAnswer fOuterHTML;
    SheetSource fSource;

    std::vector<Command> fCommands;
    std::map<uint64_t, Pending> fPending;
    uint64_t fNextTag = 1, fGeneration = 1, fStylesGeneration = 1;
    DOMChanges fChanges;
};
}
