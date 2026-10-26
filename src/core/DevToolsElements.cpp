#include "DevToolsElements.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>

namespace summit::devtools {
using nlohmann::json;
namespace {
// What the page draws over a node: Web Inspector's colours. Without the
// label of tag, size and role (showInfo): to find the role the engine turns
// on accessibility for the whole web process, and this port's accessibility
// notifications (AXObjectCachePlayStation.cpp's postPlatformNotification,
// which tested !!object.document()) then crashed it on the page's next change
// of a node that has left its document. The engine is fixed since 19
// October; the label stays off because accessibility for the whole web
// process costs time on every change of the page.
constexpr const char* kHighlightConfig = "{\"showInfo\":false,\"contentColor\":{\"r\":111,\"g\":168,\"b\":220,\"a\":0.66},"
    "\"paddingColor\":{\"r\":147,\"g\":196,\"b\":125,\"a\":0.55},\"borderColor\":{\"r\":255,\"g\":229,\"b\":153,\"a\":0.66},"
    "\"marginColor\":{\"r\":246,\"g\":178,\"b\":107,\"a\":0.66}}";
// The page's objects the panel asks for are released together.
constexpr const char* kObjectGroup = "summit-elements";
// The element at a point, looking into frames of the same origin and open
// shadow roots.
constexpr const char* kElementAtPoint = "(function (x, y) { let root = document, element = null;"
    " for (let depth = 0; depth < 16; ++depth) { let found = null; try { found = root.elementFromPoint(x, y); } catch (e) { }"
    " if (!found || found === element) break; element = found;"
    " if (found.shadowRoot && found.shadowRoot.elementFromPoint) { root = found.shadowRoot; continue; }"
    " if ((found.localName === 'iframe' || found.localName === 'frame') && found.contentDocument) {"
    " const box = found.getBoundingClientRect(); x -= box.left + found.clientLeft; y -= box.top + found.clientTop;"
    " root = found.contentDocument; continue; } break; } return element; })";
// The size of an element's border box, for the box model of inline elements.
constexpr const char* kBorderBox = "function () { const box = this.getBoundingClientRect(); return [box.width, box.height]; }";
constexpr const char* kScrollIntoView = "function () { if (this.scrollIntoViewIfNeeded) this.scrollIntoViewIfNeeded(true);"
    " else if (this.scrollIntoView) this.scrollIntoView({block: 'center'}); }";
// Styles are read again at most this often when the page keeps changing them.
constexpr double kStyleInterval = 0.3;

const json& Member(const json& object, const char* key)
{
    static const json nothing;
    if (!object.is_object()) return nothing;
    const auto found = object.find(key);
    return found == object.end() ? nothing : *found;
}
std::string Text(const json& value) { return value.is_string() ? value.get<std::string>() : std::string(); }
double Number(const json& value, double otherwise = 0) { return value.is_number() ? value.get<double>() : otherwise; }
int Integer(const json& value) { return static_cast<int>(Number(value)); }
bool Flag(const json& value) { return value.is_boolean() && value.get<bool>(); }
std::string Dump(const json& value) { return value.dump(-1, ' ', false, json::error_handler_t::replace); }
std::string Quoted(const std::string& text) { return Dump(json(text)); }

std::string LowerCase(std::string text)
{
    for (auto& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return text;
}

std::string Trim(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r\n\f");
    if (first == std::string_view::npos) return { };
    const auto last = text.find_last_not_of(" \t\r\n\f");
    return std::string(text.substr(first, last - first + 1));
}

// Runs of white space become one space; the text is cut at limit bytes.
std::string Collapse(std::string_view text, size_t limit)
{
    std::string result;
    bool space = false;
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
            space = !result.empty();
            continue;
        }
        if (space) result += ' ';
        space = false;
        result += c;
        if (result.size() >= limit) {
            // Not in the middle of a character.
            while (!result.empty() && (static_cast<unsigned char>(result.back()) & 0xC0) == 0x80) result.pop_back();
            if (!result.empty() && (static_cast<unsigned char>(result.back()) & 0x80)) result.pop_back();
            return result + "…";
        }
    }
    return result;
}

bool IsVoid(const std::string& tag)
{
    static const char* const kVoid[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param",
        "source", "track", "wbr"};
    return std::any_of(std::begin(kVoid), std::end(kVoid), [&](const char* name) { return tag == name; });
}

bool IsComment(const std::string& text) { return text.size() >= 4 && text.compare(0, 2, "/*") == 0 && text.compare(text.size() - 2, 2, "*/") == 0; }

std::string Uncomment(const std::string& text)
{
    if (!IsComment(text)) return text;
    return Trim(std::string_view(text).substr(2, text.size() - 4));
}

std::string WithSemicolon(std::string text)
{
    text = Trim(text);
    if (!text.empty() && !IsComment(text) && text.back() != ';') text += ';';
    return text;
}

// One part a declaration contributes to its style's text.
std::vector<std::string> StyleParts(const StyleBlock& block)
{
    std::vector<std::string> parts;
    for (const auto& declaration : block.declarations) {
        if (!declaration.text.empty()) parts.push_back(WithSemicolon(declaration.text));
        else parts.push_back(declaration.name + ": " + declaration.value + (declaration.important ? " !important;" : ";"));
    }
    return parts;
}

// The number at the start of a computed length ("8px", "-2.5px").
double Pixels(const std::string& value)
{
    if (value.empty()) return 0;
    char* end = nullptr;
    const double number = std::strtod(value.c_str(), &end);
    return end == value.c_str() || !std::isfinite(number) ? 0 : number;
}

double Seconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Properties that are inherited, for when the page has not said which.
bool InheritedByDefault(const std::string& name)
{
    static const std::set<std::string> kInherited = {"azimuth", "border-collapse", "border-spacing", "caption-side", "color",
        "color-scheme", "cursor", "direction", "empty-cells", "font", "font-family", "font-feature-settings", "font-kerning",
        "font-size", "font-size-adjust", "font-stretch", "font-style", "font-synthesis", "font-variant", "font-variant-caps",
        "font-variant-east-asian", "font-variant-ligatures", "font-variant-numeric", "font-variation-settings", "font-weight",
        "hyphens", "letter-spacing", "line-break", "line-height", "list-style", "list-style-image", "list-style-position",
        "list-style-type", "orphans", "overflow-wrap", "paint-order", "pointer-events", "quotes", "tab-size", "text-align",
        "text-align-last", "text-indent", "text-justify", "text-rendering", "text-shadow", "text-transform",
        "text-underline-position", "visibility", "white-space", "widows", "word-break", "word-spacing", "word-wrap",
        "writing-mode", "-webkit-text-fill-color", "-webkit-text-stroke", "-webkit-font-smoothing", "fill", "fill-opacity",
        "stroke", "stroke-width", "stroke-opacity", "accent-color", "caret-color"};
    return kInherited.count(name) > 0;
}

// "@media screen and (min-width: 600px)".
std::string GroupingLabel(const json& grouping)
{
    const auto type = Text(Member(grouping, "type"));
    const auto text = Text(Member(grouping, "text"));
    if (type == "media-rule" || type == "media-import-rule" || type == "media-link-node" || type == "media-style-node")
        return text.empty() ? std::string() : "@media " + text;
    if (type == "supports-rule") return "@supports " + text;
    if (type == "layer-rule" || type == "layer-import-rule") return text.empty() ? "@layer" : "@layer " + text;
    if (type == "container-rule") return "@container " + text;
    if (type == "scope-rule") return text.empty() ? "@scope" : "@scope " + text;
    if (type == "starting-style-rule") return "@starting-style";
    return { };
}

// The file of an address: its last path segment, else its host.
std::string ShortURL(const std::string& url)
{
    auto text = std::string_view(url);
    text = text.substr(0, text.find_first_of("?#"));
    while (text.size() > 1 && text.back() == '/') text.remove_suffix(1);
    const auto slash = text.rfind('/');
    if (slash == std::string_view::npos || slash + 1 >= text.size()) return std::string(text);
    return std::string(text.substr(slash + 1));
}

std::string EscapeAttribute(const std::string& value)
{
    std::string result;
    for (const char c : value) {
        if (c == '&') result += "&amp;";
        else if (c == '"') result += "&quot;";
        else result += c;
    }
    return result;
}
}

const std::string* DOMNode::FindAttribute(std::string_view wanted) const
{
    for (const auto& attribute : attributes)
        if (attribute.name == wanted) return &attribute.value;
    return nullptr;
}

std::string DOMNode::Tag() const
{
    if (!localName.empty()) return localName;
    return type == kElementNode ? LowerCase(name) : name;
}

std::string DOMNode::Label() const
{
    if (!IsElement()) return name;
    if (!pseudoType.empty()) return "::" + pseudoType;
    std::string label = Tag();
    if (const auto* id = FindAttribute("id"); id && !id->empty()) label += "#" + *id;
    if (const auto* classes = FindAttribute("class")) {
        size_t count = 0;
        for (size_t start = 0; start < classes->size();) {
            const auto first = classes->find_first_not_of(" \t\n\r\f", start);
            if (first == std::string::npos) break;
            const auto end = std::min(classes->find_first_of(" \t\n\r\f", first), classes->size());
            if (++count > 3) {
                label += ".…";
                break;
            }
            label += "." + classes->substr(first, end - first);
            start = end;
        }
    }
    return label;
}

std::string StyleBlock::Location() const
{
    if (origin == "user-agent") return "user agent style sheet";
    if (sourceURL.empty()) return origin == "inspector" ? "inspector style sheet" : std::string();
    auto location = ShortURL(sourceURL);
    if (line > 0) location += ":" + std::to_string(line);
    return location;
}

std::vector<std::string> AttributeNames(std::string_view text)
{
    std::vector<std::string> names;
    size_t i = 0;
    const auto space = [&](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; };
    while (i < text.size()) {
        while (i < text.size() && (space(text[i]) || text[i] == '/')) ++i;
        if (i >= text.size()) break;
        const size_t start = i;
        // The HTML parser lets a name begin with "=".
        if (text[i] == '=') ++i;
        while (i < text.size() && !space(text[i]) && text[i] != '/' && text[i] != '>' && text[i] != '=') ++i;
        if (i == start) {
            ++i;
            continue;
        }
        auto name = LowerCase(std::string(text.substr(start, i - start)));
        while (i < text.size() && space(text[i])) ++i;
        if (i < text.size() && text[i] == '=') {
            ++i;
            while (i < text.size() && space(text[i])) ++i;
            if (i < text.size() && (text[i] == '"' || text[i] == '\'')) {
                const char quote = text[i++];
                while (i < text.size() && text[i] != quote) ++i;
                if (i < text.size()) ++i;
            } else
                while (i < text.size() && !space(text[i]) && text[i] != '>') ++i;
        }
        if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(std::move(name));
    }
    return names;
}

void MarkOverridden(std::vector<StyleBlock>& blocks, const std::map<std::string, std::vector<std::string>>& longhands)
{
    using Place = std::pair<size_t, size_t>;
    // What a declaration sets: a shorthand its longhands, else the property.
    const auto sets = [&](const Declaration& declaration) {
        auto name = LowerCase(declaration.name);
        const auto found = longhands.find(name);
        return found == longhands.end() || found->second.empty() ? std::vector<std::string> {name} : found->second;
    };
    // The element's own blocks, each pseudo element's, each ancestor's: a
    // cascade each, in the order they come.
    const auto groupOf = [](const StyleBlock& block) {
        return block.inheritedFrom ? "a" + std::to_string(block.inheritedFrom) : "p" + block.pseudo;
    };
    std::vector<std::string> order;
    std::map<std::string, std::vector<size_t>> groups;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const auto key = groupOf(blocks[i]);
        if (!groups.count(key)) order.push_back(key);
        groups[key].push_back(i);
    }
    // Properties set by the element, then by each nearer ancestor.
    std::set<std::string> taken;
    for (const auto& key : order) {
        const bool inherited = key[0] == 'a';
        std::map<std::string, Place> winners;
        for (const auto i : groups[key]) {
            for (size_t j = 0; j < blocks[i].declarations.size(); ++j) {
                const auto& declaration = blocks[i].declarations[j];
                if (!declaration.enabled || !declaration.parsedOk || declaration.overridden) continue;
                for (const auto& name : sets(declaration)) {
                    const auto found = winners.find(name);
                    // The strongest comes first; only !important beats it.
                    if (found == winners.end()) winners[name] = {i, j};
                    else if (declaration.important && !blocks[found->second.first].declarations[found->second.second].important)
                        found->second = {i, j};
                }
            }
        }
        // A declaration counts while it sets one of its properties.
        for (const auto i : groups[key]) {
            for (size_t j = 0; j < blocks[i].declarations.size(); ++j) {
                auto& declaration = blocks[i].declarations[j];
                if (!declaration.enabled || !declaration.parsedOk || declaration.overridden) continue;
                bool counts = false;
                for (const auto& name : sets(declaration)) {
                    const auto found = winners.find(name);
                    if (found != winners.end() && found->second == Place {i, j} && !(inherited && taken.count(name))) counts = true;
                }
                declaration.overridden = !counts;
            }
        }
        if (inherited || key == "p")
            for (const auto& [name, place] : winners) taken.insert(name);
    }
}

std::string EditStyleText(const StyleBlock& block, size_t index, StyleEdit edit, const std::string& text)
{
    auto parts = StyleParts(block);
    if (edit == StyleEdit::Add) parts.push_back(WithSemicolon(text));
    else if (index < parts.size()) {
        if (edit == StyleEdit::Toggle)
            parts[index] = IsComment(parts[index]) ? WithSemicolon(Uncomment(parts[index])) : "/* " + parts[index] + " */";
        else if (Trim(text).empty()) parts.erase(parts.begin() + static_cast<std::ptrdiff_t>(index));
        else parts[index] = WithSemicolon(text);
    }
    std::string result;
    for (const auto& part : parts) {
        if (part.empty()) continue;
        if (!result.empty()) result += ' ';
        result += part;
    }
    return result;
}

// Reads the protocol into the model; a friend so that the header needs no
// JSON types.
struct DOMReader {
    // A node the page describes, with what it says of its descendants. A
    // node known already keeps what the description leaves out.
    static int AddNode(DOMModel& model, const json& object, int parent)
    {
        const int id = Integer(Member(object, "nodeId"));
        if (id <= 0) return 0;
        DOMNode node;
        if (const auto existing = model.fNodes.find(id); existing != model.fNodes.end()) node = std::move(existing->second);
        node.id = id;
        node.parent = parent;
        node.type = Integer(Member(object, "nodeType"));
        node.name = Text(Member(object, "nodeName"));
        node.localName = Text(Member(object, "localName"));
        node.value = Text(Member(object, "nodeValue"));
        if (const auto& attributes = Member(object, "attributes"); attributes.is_array()) {
            node.attributes.clear();
            for (size_t i = 0; i + 1 < attributes.size(); i += 2)
                node.attributes.push_back({Text(attributes[i]), Text(attributes[i + 1])});
        }
        if (const auto& count = Member(object, "childNodeCount"); count.is_number()) {
            node.childCount = std::max(0, Integer(count));
            if (!node.childCount && !node.childrenKnown) node.childrenKnown = true;
        }
        if (const auto& children = Member(object, "children"); children.is_array()) {
            node.children = Replace(model, node.children, children, id);
            node.childrenKnown = true;
            node.childCount = static_cast<int>(node.children.size());
        }
        if (const auto& document = Member(object, "contentDocument"); document.is_object()) {
            const auto added = AddNode(model, document, id);
            if (node.contentDocument && node.contentDocument != added) model.Erase(node.contentDocument);
            node.contentDocument = added;
        }
        if (const auto& content = Member(object, "templateContent"); content.is_object()) {
            const auto added = AddNode(model, content, id);
            if (node.templateContent && node.templateContent != added) model.Erase(node.templateContent);
            node.templateContent = added;
        }
        if (const auto& roots = Member(object, "shadowRoots"); roots.is_array()) node.shadowRoots = Replace(model, node.shadowRoots, roots, id);
        if (const auto& pseudo = Member(object, "pseudoElements"); pseudo.is_array())
            node.pseudoElements = Replace(model, node.pseudoElements, pseudo, id);
        if (const auto pseudoType = Text(Member(object, "pseudoType")); !pseudoType.empty()) node.pseudoType = pseudoType;
        if (const auto rootType = Text(Member(object, "shadowRootType")); !rootType.empty()) node.shadowRootType = rootType;
        if (const auto url = Text(Member(object, "documentURL")); !url.empty()) node.documentURL = url;
        node.publicId = Text(Member(object, "publicId"));
        node.systemId = Text(Member(object, "systemId"));
        model.fNodes[id] = std::move(node);
        return id;
    }

    // A list of nodes in place of another: those no longer in it go.
    static std::vector<int> Replace(DOMModel& model, const std::vector<int>& old, const json& objects, int parent)
    {
        std::vector<int> ids;
        for (const auto& object : objects)
            if (const int id = AddNode(model, object, parent)) ids.push_back(id);
        for (const int id : old)
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) model.Erase(id);
        return ids;
    }

    static StyleBlock ReadRule(const DOMModel& model, const json& rule)
    {
        StyleBlock block;
        block.kind = StyleBlock::Rule;
        block.selector = Text(Member(Member(rule, "selectorList"), "text"));
        block.origin = Text(Member(rule, "origin"));
        block.sourceURL = Text(Member(rule, "sourceURL"));
        if (!block.sourceURL.empty() && Member(rule, "sourceLine").is_number()) block.line = Integer(Member(rule, "sourceLine")) + 1;
        if (const auto& groupings = Member(rule, "groupings"); groupings.is_array())
            for (const auto& grouping : groupings)
                if (auto label = GroupingLabel(grouping); !label.empty()) block.groupings.push_back(std::move(label));
        const auto& style = Member(rule, "style");
        // The browser's own rules cannot be changed.
        if (block.origin != "user-agent") {
            ReadStyleID(style, block);
            if (const auto& id = Member(rule, "ruleId"); id.is_object() && Text(Member(id, "styleSheetId")) == block.styleSheetId)
                block.ruleOrdinal = Member(id, "ordinal").is_number() ? Integer(Member(id, "ordinal")) : -1;
        }
        // A <style>'s rules count their lines from its start, the document's from the top.
        if (const auto sheet = model.fSheets.find(block.styleSheetId); block.line > 0 && sheet != model.fSheets.end() && sheet->second.inlined)
            block.line += sheet->second.startLine;
        ReadDeclarations(model, style, block);
        return block;
    }

    static void ReadStyleID(const json& style, StyleBlock& block)
    {
        const auto& id = Member(style, "styleId");
        if (!id.is_object()) return;
        block.styleSheetId = Text(Member(id, "styleSheetId"));
        block.ordinal = Member(id, "ordinal").is_number() ? Integer(Member(id, "ordinal")) : -1;
    }

    // The declarations as written; for a style without source text, its
    // properties, with the shorthands the page gave instead of their parts.
    static void ReadDeclarations(const DOMModel& model, const json& style, StyleBlock& block)
    {
        const auto& properties = Member(style, "cssProperties");
        if (!properties.is_array()) return;
        const bool written = std::any_of(properties.begin(), properties.end(),
            [](const json& property) { return !Text(Member(property, "text")).empty(); });
        std::map<std::string, std::string> shorthandOf, shorthandValue;
        if (!written) {
            if (const auto& entries = Member(style, "shorthandEntries"); entries.is_array()) {
                for (const auto& entry : entries) {
                    const auto name = Text(Member(entry, "name"));
                    const auto found = model.fLonghands.find(name);
                    if (found == model.fLonghands.end()) continue;
                    shorthandValue[name] = Text(Member(entry, "value"));
                    for (const auto& longhand : found->second) shorthandOf.emplace(longhand, name);
                }
            }
        }
        std::set<std::string> shown;
        for (const auto& property : properties) {
            Declaration declaration;
            declaration.name = Text(Member(property, "name"));
            declaration.value = Text(Member(property, "value"));
            declaration.text = Text(Member(property, "text"));
            if (written && declaration.text.empty()) continue;
            if (!written) {
                if (const auto found = shorthandOf.find(declaration.name); found != shorthandOf.end()) {
                    if (!shown.insert(found->second).second) continue;
                    declaration.name = found->second;
                    declaration.value = shorthandValue[found->second];
                } else if (Flag(Member(property, "implicit")))
                    continue;
            }
            declaration.important = Text(Member(property, "priority")) == "important";
            const auto status = Text(Member(property, "status"));
            declaration.enabled = status != "disabled";
            declaration.overridden = status == "inactive";
            declaration.parsedOk = !Member(property, "parsedOk").is_boolean() || Flag(Member(property, "parsedOk"));
            block.declarations.push_back(std::move(declaration));
        }
    }

    static void BuildStyles(DOMModel& model)
    {
        const auto matched = json::parse(model.fMatchedJSON, nullptr, false);
        const auto inlined = json::parse(model.fInlineJSON, nullptr, false);
        std::vector<StyleBlock> blocks;
        if (const auto& style = Member(inlined, "inlineStyle"); style.is_object()) {
            StyleBlock block;
            block.kind = StyleBlock::Inline;
            block.selector = "element.style";
            block.origin = "inline";
            ReadStyleID(style, block);
            ReadDeclarations(model, style, block);
            blocks.push_back(std::move(block));
        }
        // The protocol lists the rules from the weakest; they are shown from
        // the strongest, with presentational attributes above the browser's own.
        std::vector<StyleBlock> author, agent;
        if (const auto& rules = Member(matched, "matchedCSSRules"); rules.is_array()) {
            for (auto match = rules.rbegin(); match != rules.rend(); ++match) {
                auto block = ReadRule(model, Member(*match, "rule"));
                (block.origin == "user-agent" ? agent : author).push_back(std::move(block));
            }
        }
        for (auto& block : author) blocks.push_back(std::move(block));
        if (const auto& style = Member(inlined, "attributesStyle"); style.is_object()) {
            StyleBlock block;
            block.kind = StyleBlock::Attributes;
            block.selector = "Attributes style";
            block.origin = "attributes";
            ReadDeclarations(model, style, block);
            if (!block.declarations.empty()) blocks.push_back(std::move(block));
        }
        for (auto& block : agent) blocks.push_back(std::move(block));
        if (const auto& pseudo = Member(matched, "pseudoElements"); pseudo.is_array()) {
            for (const auto& entry : pseudo) {
                const auto name = "::" + Text(Member(entry, "pseudoId"));
                const auto& matches = Member(entry, "matches");
                if (!matches.is_array()) continue;
                for (auto match = matches.rbegin(); match != matches.rend(); ++match) {
                    auto block = ReadRule(model, Member(*match, "rule"));
                    block.pseudo = name;
                    blocks.push_back(std::move(block));
                }
            }
        }
        // Inherited styles come from the nearest ancestor out.
        std::vector<int> ancestors;
        if (const auto* node = model.Find(model.fNextStyles.node)) {
            for (const auto* up = model.Find(node->parent); up; up = model.Find(up->parent))
                if (up->IsElement()) ancestors.push_back(up->id);
        }
        if (const auto& inherited = Member(matched, "inherited"); inherited.is_array()) {
            for (size_t k = 0; k < inherited.size(); ++k) {
                const int from = k < ancestors.size() ? ancestors[k] : -static_cast<int>(k + 1);
                std::vector<StyleBlock> level;
                if (const auto& style = Member(inherited[k], "inlineStyle"); style.is_object()) {
                    StyleBlock block;
                    block.kind = StyleBlock::Inline;
                    block.selector = "style attribute";
                    block.origin = "inline";
                    ReadStyleID(style, block);
                    ReadDeclarations(model, style, block);
                    level.push_back(std::move(block));
                }
                std::vector<StyleBlock> levelAgent;
                if (const auto& rules = Member(inherited[k], "matchedCSSRules"); rules.is_array()) {
                    for (auto match = rules.rbegin(); match != rules.rend(); ++match) {
                        auto block = ReadRule(model, Member(*match, "rule"));
                        (block.origin == "user-agent" ? levelAgent : level).push_back(std::move(block));
                    }
                }
                for (auto& block : levelAgent) level.push_back(std::move(block));
                for (auto& block : level) {
                    auto& list = block.declarations;
                    list.erase(std::remove_if(list.begin(), list.end(),
                        [&](const Declaration& declaration) { return !model.IsInheritable(LowerCase(declaration.name)); }), list.end());
                    if (list.empty()) continue;
                    block.inheritedFrom = from;
                    blocks.push_back(std::move(block));
                }
            }
        }
        MarkOverridden(blocks, model.fLonghands);
        model.fNextStyles.blocks = std::move(blocks);
    }

    static void ReadComputed(DOMModel& model, const json& result)
    {
        auto& styles = model.fNextStyles;
        styles.computed.clear();
        if (const auto& properties = Member(result, "computedStyle"); properties.is_array())
            for (const auto& property : properties)
                styles.computed.emplace_back(Text(Member(property, "name")), Text(Member(property, "value")));
        std::sort(styles.computed.begin(), styles.computed.end());
    }

    static void Event(DOMModel& model, const std::string& method, const json& parameters)
    {
        auto& changes = model.fChanges;
        if (method == "Inspector.inspect") {
            const auto& object = Member(parameters, "object");
            const auto objectId = Text(Member(object, "objectId"));
            if (model.fPicking) {
                model.fPicking = false;
                model.fHighlighted = 0;
                changes.picking = true;
            }
            if (objectId.empty() || Text(Member(object, "subtype")) != "node" || !model.Loaded()) return;
            model.Send("DOM.requestNode", "{\"objectId\":" + Quoted(objectId) + "}", DOMModel::Pending::RequestNode);
            model.Send("Runtime.releaseObject", "{\"objectId\":" + Quoted(objectId) + "}");
            return;
        }
        if (method == "Page.domContentEventFired" || method == "Page.loadEventFired") {
            if (model.Loaded() && !model.fRestorePath.empty())
                model.Send("DOM.pushNodeByPathToFrontend", "{\"path\":" + Quoted(model.fRestorePath) + "}", DOMModel::Pending::Path);
            return;
        }
        if (method == "CSS.styleSheetAdded") {
            const auto& header = Member(parameters, "header");
            const auto id = Text(Member(header, "styleSheetId"));
            if (!id.empty())
                model.fSheets[id] = {Text(Member(header, "sourceURL")), Integer(Member(header, "startLine")), Flag(Member(header, "isInline"))};
            return;
        }
        if (method == "CSS.styleSheetRemoved") {
            model.fSheets.erase(Text(Member(parameters, "styleSheetId")));
            return;
        }
        if (method == "CSS.styleSheetChanged") {
            const auto id = Text(Member(parameters, "styleSheetId"));
            for (const auto& block : model.fStyles.blocks)
                if (block.styleSheetId == id) model.fStylesStale = true;
            return;
        }
        if (method == "CSS.mediaQueryResultChanged") {
            if (model.fSelected) model.fStylesStale = true;
            return;
        }
        if (!model.Loaded()) {
            if (method == "DOM.documentUpdated" && model.fEnabled && !model.fDocumentWaiting) model.RequestDocument();
            return;
        }
        if (method == "DOM.documentUpdated") {
            if (model.fRestorePath.empty()) model.fRestorePath = model.PathOf(model.fSelected);
            model.fRestoreTries = 0;
            model.Reset(false);
            model.RequestDocument();
        } else if (method == "DOM.setChildNodes") {
            auto* parent = model.Node(Integer(Member(parameters, "parentId")));
            const auto& nodes = Member(parameters, "nodes");
            if (!parent || !nodes.is_array()) return;
            const int id = parent->id;
            const auto old = parent->children;
            auto children = Replace(model, old, nodes, id);
            if (auto* again = model.Node(id)) {
                again->children = std::move(children);
                again->childrenKnown = true;
                again->childCount = static_cast<int>(again->children.size());
            }
            changes.tree = true;
        } else if (method == "DOM.childNodeInserted") {
            const int parentId = Integer(Member(parameters, "parentNodeId"));
            if (!model.Node(parentId)) return;
            const int id = AddNode(model, Member(parameters, "node"), parentId);
            auto* parent = model.Node(parentId);
            if (!id || !parent) return;
            auto& children = parent->children;
            children.erase(std::remove(children.begin(), children.end(), id), children.end());
            const int previous = Integer(Member(parameters, "previousNodeId"));
            auto at = children.begin();
            if (previous) {
                const auto found = std::find(children.begin(), children.end(), previous);
                at = found == children.end() ? children.end() : found + 1;
            }
            children.insert(at, id);
            parent->childrenKnown = true;
            parent->childCount = static_cast<int>(children.size());
            changes.tree = true;
        } else if (method == "DOM.childNodeRemoved" || method == "DOM.willDestroyDOMNode") {
            model.Removed(Integer(Member(parameters, "nodeId")));
        } else if (method == "DOM.shadowRootPopped") {
            model.Removed(Integer(Member(parameters, "rootId")));
        } else if (method == "DOM.pseudoElementRemoved") {
            model.Removed(Integer(Member(parameters, "pseudoElementId")));
        } else if (method == "DOM.shadowRootPushed" || method == "DOM.pseudoElementAdded") {
            const bool shadow = method == "DOM.shadowRootPushed";
            const int hostId = Integer(Member(parameters, shadow ? "hostId" : "parentId"));
            if (!model.Node(hostId)) return;
            const int id = AddNode(model, Member(parameters, shadow ? "root" : "pseudoElement"), hostId);
            auto* host = model.Node(hostId);
            if (!id || !host) return;
            auto& list = shadow ? host->shadowRoots : host->pseudoElements;
            if (std::find(list.begin(), list.end(), id) == list.end()) {
                // ::before goes first, ::after last.
                const auto* added = model.Find(id);
                if (!shadow && added && added->pseudoType == "before") list.insert(list.begin(), id);
                else list.push_back(id);
            }
            changes.tree = true;
        } else if (method == "DOM.attributeModified" || method == "DOM.attributeRemoved") {
            auto* node = model.Node(Integer(Member(parameters, "nodeId")));
            if (!node) return;
            const auto name = Text(Member(parameters, "name"));
            auto& attributes = node->attributes;
            const auto found = std::find_if(attributes.begin(), attributes.end(), [&](const DOMAttribute& a) { return a.name == name; });
            if (method == "DOM.attributeRemoved") {
                if (found != attributes.end()) attributes.erase(found);
            } else if (found != attributes.end()) found->value = Text(Member(parameters, "value"));
            else attributes.push_back({name, Text(Member(parameters, "value"))});
            changes.nodes = true;
            if (node->id == model.fSelected) model.fStylesStale = true;
        } else if (method == "DOM.inlineStyleInvalidated") {
            if (const auto& ids = Member(parameters, "nodeIds"); ids.is_array())
                for (const auto& id : ids)
                    if (model.Find(Integer(id)))
                        model.Send("DOM.getAttributes", "{\"nodeId\":" + std::to_string(Integer(id)) + "}",
                            DOMModel::Pending::Attributes, Integer(id));
        } else if (method == "DOM.characterDataModified") {
            auto* node = model.Node(Integer(Member(parameters, "nodeId")));
            if (!node) return;
            node->value = Text(Member(parameters, "characterData"));
            changes.nodes = true;
        } else if (method == "DOM.childNodeCountUpdated") {
            auto* node = model.Node(Integer(Member(parameters, "nodeId")));
            if (!node) return;
            // The page says this of a node whose children it has not given:
            // what is known of them is no longer.
            const int id = node->id;
            node->childCount = std::max(0, Integer(Member(parameters, "childNodeCount")));
            for (const int child : std::vector<int>(node->children)) model.Erase(child);
            if (auto* again = model.Node(id)) {
                again->children.clear();
                again->childrenKnown = !again->childCount;
                if (again->childCount && model.IsExpanded(id))
                    model.Send("DOM.requestChildNodes", "{\"nodeId\":" + std::to_string(id) + "}", DOMModel::Pending::Children, id);
            }
            changes.tree = true;
        } else if (method == "DOM.inspect") {
            const int id = Integer(Member(parameters, "nodeId"));
            if (!model.Find(id)) return;
            model.fPicking = false;
            changes.picking = changes.picked = true;
            model.Reveal(id);
            model.Select(id);
        }
    }

    static void Answered(DOMModel& model, const DOMModel::Pending& pending, const json& message)
    {
        using Pending = DOMModel::Pending;
        const auto& result = Member(message, "result");
        const auto error = Text(Member(Member(message, "error"), "message"));
        const bool failed = Member(message, "error").is_object();
        auto& changes = model.fChanges;
        const bool styles = pending.kind == Pending::Matched || pending.kind == Pending::Inline || pending.kind == Pending::Computed
            || (pending.kind == Pending::Resolve && !pending.purpose) || pending.kind == Pending::Box;
        if (styles ? pending.generation != model.fStylesGeneration
            : pending.kind != Pending::Properties && pending.generation != model.fGeneration)
            return;
        switch (pending.kind) {
            case Pending::Ignore: break;
            case Pending::Accessibility: {
                if (pending.node != model.fSelected) break;
                NodeAccessibility accessibility;
                accessibility.node = pending.node;
                accessibility.error = failed ? (error.empty() ? std::string("The page did not answer") : error) : std::string();
                // Role and label first, then the states the element has.
                const auto& properties = Member(result, "properties");
                std::vector<std::pair<std::string, std::string>> rest;
                if (properties.is_object()) {
                    for (const auto& [key, value] : properties.items()) {
                        std::string text;
                        if (value.is_string()) text = value.get<std::string>();
                        else if (value.is_boolean()) text = value.get<bool>() ? "true" : "false";
                        else if (value.is_number()) text = value.dump();
                        else if (value.is_array()) text = std::to_string(value.size()) + (value.size() == 1 ? " node" : " nodes");
                        else continue;
                        if (key == "role" || key == "label") accessibility.properties.emplace_back(key, text);
                        else rest.emplace_back(key, text);
                    }
                }
                std::sort(accessibility.properties.begin(), accessibility.properties.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                accessibility.properties.insert(accessibility.properties.end(), rest.begin(), rest.end());
                model.fAccessibility = std::move(accessibility);
                changes.accessibility = true;
                break;
            }
            case Pending::Listeners: {
                if (pending.node != model.fSelected) break;
                NodeListeners listeners;
                listeners.node = pending.node;
                listeners.error = failed ? (error.empty() ? std::string("The page did not answer") : error) : std::string();
                if (const auto& list = Member(result, "listeners"); list.is_array()) {
                    for (const auto& item : list) {
                        NodeListener listener;
                        listener.type = Text(Member(item, "type"));
                        listener.capture = Flag(Member(item, "useCapture"));
                        listener.once = Flag(Member(item, "once"));
                        listener.passive = Flag(Member(item, "passive"));
                        listener.attribute = Flag(Member(item, "isAttribute"));
                        listener.disabled = Flag(Member(item, "disabled"));
                        listener.handler = Text(Member(item, "handlerName"));
                        if (listener.handler.empty()) listener.handler = "(anonymous)";
                        if (Flag(Member(item, "onWindow"))) listener.node = "window";
                        else if (const auto& id = Member(item, "nodeId"); id.is_number()) {
                            const auto* node = model.Find(id.get<int>());
                            listener.node = node ? node->Label() : "node " + std::to_string(id.get<int>());
                        }
                        if (const auto& location = Member(item, "location"); location.is_object())
                            listener.location = "script " + Text(Member(location, "scriptId")) + ", line "
                                + std::to_string(static_cast<int>(Number(Member(location, "lineNumber"))) + 1);
                        listener.own = Integer(Member(item, "nodeId")) == pending.node;
                        listeners.listeners.push_back(std::move(listener));
                    }
                }
                // The element's own first, then those of the nodes above it.
                std::stable_partition(listeners.listeners.begin(), listeners.listeners.end(), [](const NodeListener& listener) { return listener.own; });
                model.fListeners = std::move(listeners);
                changes.listeners = true;
                break;
            }
            case Pending::Properties:
                if (const auto& properties = Member(result, "cssProperties"); properties.is_array()) {
                    for (const auto& property : properties) {
                        const auto name = Text(Member(property, "name"));
                        if (Flag(Member(property, "inherited"))) {
                            model.fInherited.insert(name);
                            if (const auto& aliases = Member(property, "aliases"); aliases.is_array())
                                for (const auto& alias : aliases) model.fInherited.insert(Text(alias));
                        }
                        if (const auto& longhands = Member(property, "longhands"); longhands.is_array() && !longhands.empty()) {
                            auto& list = model.fLonghands[name];
                            for (const auto& longhand : longhands) list.push_back(Text(longhand));
                        }
                    }
                    if (model.fSelected) model.fStylesStale = true;
                }
                break;
            case Pending::Document: {
                model.fDocumentWaiting = false;
                if (failed) {
                    changes.notice = "The page's document could not be read: " + error;
                    break;
                }
                model.fDocument = AddNode(model, Member(result, "root"), 0);
                changes.tree = true;
                const auto* document = model.Find(model.fDocument);
                if (!document) break;
                model.fExpanded.insert(model.fDocument);
                for (const int child : document->children) {
                    const auto* node = model.Find(child);
                    if (node && node->IsElement()) {
                        model.fExpanded.insert(child);
                        for (const int inner : node->children) {
                            const auto* body = model.Find(inner);
                            if (body && body->IsElement() && body->Tag() == "body") model.SetExpanded(inner, true);
                        }
                    }
                }
                if (!model.fRestorePath.empty()) {
                    model.Send("DOM.pushNodeByPathToFrontend", "{\"path\":" + Quoted(model.fRestorePath) + "}", Pending::Path);
                    // Shown meanwhile; the restored selection replaces it.
                    model.SelectDefault();
                } else model.SelectDefault();
                if (model.fHasPoint) model.InspectAt(model.fPointX, model.fPointY);
                if (!model.fSearchQuery.empty()) model.Search(std::string(model.fSearchQuery));
                break;
            }
            case Pending::Children: break;
            case Pending::ExpandAll: {
                // Frames' documents and shadow roots come without their
                // children: they are asked for.
                std::function<void(int)> expand = [&](int id) {
                    if (!model.Expandable(id)) return;
                    model.fExpanded.erase(id);
                    model.SetExpanded(id, true);
                    for (const int child : model.ShownChildren(id)) expand(child);
                };
                expand(pending.node);
                changes.tree = true;
                break;
            }
            case Pending::Attributes:
                if (auto* node = model.Node(pending.node); node && !failed) {
                    if (const auto& attributes = Member(result, "attributes"); attributes.is_array()) {
                        node->attributes.clear();
                        for (size_t i = 0; i + 1 < attributes.size(); i += 2)
                            node->attributes.push_back({Text(attributes[i]), Text(attributes[i + 1])});
                        changes.nodes = true;
                        if (node->id == model.fSelected) model.fStylesStale = true;
                    }
                }
                break;
            case Pending::Matched:
            case Pending::Inline:
            case Pending::Computed:
            case Pending::Box:
                if (failed && model.fNextStyles.error.empty()) model.fNextStyles.error = error;
                if (pending.kind == Pending::Matched) model.fMatchedJSON = Dump(result);
                else if (pending.kind == Pending::Inline) model.fInlineJSON = Dump(result);
                else if (pending.kind == Pending::Computed) ReadComputed(model, result);
                else if (const auto& value = Member(Member(result, "result"), "value"); value.is_array() && value.size() == 2) {
                    model.fNextStyles.box.width = Number(value[0]);
                    model.fNextStyles.box.height = Number(value[1]);
                }
                if (--model.fStylesOutstanding <= 0) model.StylesArrived();
                break;
            case Pending::Resolve: {
                const auto objectId = Text(Member(Member(result, "object"), "objectId"));
                if (objectId.empty()) {
                    if (!pending.purpose && --model.fStylesOutstanding <= 0) model.StylesArrived();
                    break;
                }
                const auto call = "{\"objectId\":" + Quoted(objectId) + ",\"functionDeclaration\":"
                    + Quoted(pending.purpose ? kScrollIntoView : kBorderBox) + ",\"returnByValue\":true}";
                if (pending.purpose) model.Send("Runtime.callFunctionOn", call);
                else {
                    const auto tag = model.Send("Runtime.callFunctionOn", call, Pending::Box, pending.node);
                    model.fPending[tag].generation = model.fStylesGeneration;
                }
                model.Send("Runtime.releaseObject", "{\"objectId\":" + Quoted(objectId) + "}");
                break;
            }
            case Pending::SetStyle:
                if (failed) changes.notice = "The style could not be changed: " + error;
                model.RefreshStyles();
                break;
            case Pending::RuleFrame: {
                // The main frame, for the inspector style sheet.
                const auto frame = Text(Member(Member(Member(result, "frameTree"), "frame"), "id"));
                if (failed || frame.empty()) {
                    changes.notice = "No rule could be added: the page has no frame";
                    break;
                }
                const auto tag = model.Send("CSS.createStyleSheet", "{\"frameId\":" + Quoted(frame) + "}", Pending::RuleSheet);
                model.fPending[tag].text = pending.text;
                break;
            }
            case Pending::RuleSheet:
                model.fInspectorSheet = Text(Member(result, "styleSheetId"));
                if (failed || model.fInspectorSheet.empty()) {
                    changes.notice = "No rule could be added: " + (error.empty() ? std::string("no style sheet") : error);
                    break;
                }
                model.AddRule(pending.text);
                break;
            case Pending::AddRule:
                if (failed) changes.notice = "The rule could not be added: " + error;
                model.RefreshStyles();
                break;
            case Pending::Edit:
            case Pending::Remove:
                if (failed) {
                    changes.notice = error;
                    model.fAfterRemoval.erase(pending.node);
                }
                break;
            case Pending::TagName:
            case Pending::Move:
                if (failed) changes.notice = error;
                else if (const int id = Integer(Member(result, "nodeId")); model.Find(id)) {
                    model.Reveal(id);
                    model.Select(id);
                }
                break;
            case Pending::Search:
                if (pending.text != model.fSearchQuery) {
                    if (!failed) model.Send("DOM.discardSearchResults", "{\"searchId\":" + Quoted(Text(Member(result, "searchId"))) + "}");
                    break;
                }
                model.fSearchID = Text(Member(result, "searchId"));
                if (failed || Integer(Member(result, "resultCount")) <= 0) {
                    model.fSearchWaiting = false;
                    if (failed) changes.notice = error;
                    changes.search = true;
                    break;
                }
                {
                    const auto tag = model.Send("DOM.getSearchResults", "{\"searchId\":" + Quoted(model.fSearchID) + ",\"fromIndex\":0,\"toIndex\":"
                        + std::to_string(std::min(Integer(Member(result, "resultCount")), 5000)) + "}", Pending::SearchResults);
                    model.fPending[tag].text = pending.text;
                }
                break;
            case Pending::SearchResults: {
                if (pending.text != model.fSearchQuery) break;
                model.fSearchWaiting = false;
                model.fSearchResults.clear();
                if (const auto& ids = Member(result, "nodeIds"); ids.is_array())
                    for (const auto& id : ids)
                        if (model.Find(Integer(id))) model.fSearchResults.push_back(Integer(id));
                model.fSearchIndex = 0;
                changes.search = true;
                if (!model.fSearchResults.empty()) {
                    model.Reveal(model.fSearchResults[0]);
                    model.Select(model.fSearchResults[0]);
                }
                break;
            }
            case Pending::Evaluate: {
                const auto& object = Member(result, "result");
                const auto objectId = Text(Member(object, "objectId"));
                if (failed || objectId.empty() || Text(Member(object, "subtype")) != "node") {
                    changes.notice = failed ? error : "There is no element there.";
                    break;
                }
                model.Send("DOM.requestNode", "{\"objectId\":" + Quoted(objectId) + "}", Pending::RequestNode);
                model.Send("Runtime.releaseObjectGroup", "{\"objectGroup\":" + Quoted(kObjectGroup) + "}");
                break;
            }
            case Pending::RequestNode: {
                const int id = Integer(Member(result, "nodeId"));
                if (failed || !model.Find(id)) {
                    if (failed) changes.notice = error;
                    break;
                }
                model.Reveal(id);
                model.Select(id);
                changes.picked = true;
                break;
            }
            case Pending::Path: {
                const int id = Integer(Member(result, "nodeId"));
                if (!failed && model.Find(id)) {
                    model.fRestorePath.clear();
                    model.Reveal(id);
                    model.Select(id);
                } else if (++model.fRestoreTries >= 3)
                    model.fRestorePath.clear();
                break;
            }
            case Pending::OuterHTML:
                model.fOuterHTML = {pending.node, pending.purpose, Text(Member(result, "outerHTML")), failed ? error : std::string()};
                changes.outerHTML = true;
                break;
            case Pending::SheetText:
                model.fSource = {pending.text, Text(Member(result, "text")), failed ? error : std::string(), pending.purpose};
                changes.source = true;
                break;
        }
    }
};

const DOMNode* DOMModel::Find(int id) const
{
    if (!id) return nullptr;
    const auto found = fNodes.find(id);
    return found == fNodes.end() ? nullptr : &found->second;
}

DOMNode* DOMModel::Node(int id) { return const_cast<DOMNode*>(Find(id)); }

uint64_t DOMModel::Send(const std::string& method, const std::string& parameters, Pending::Kind kind, int node)
{
    const auto tag = fNextTag++;
    fCommands.push_back({tag, method, parameters});
    if (kind != Pending::Ignore) {
        Pending pending;
        pending.kind = kind;
        pending.node = node;
        pending.generation = fGeneration;
        fPending[tag] = pending;
    }
    return tag;
}

void DOMModel::Enable()
{
    if (fEnabled) return;
    fEnabled = true;
    RequestDocument();
}

void DOMModel::RequestDocument()
{
    if (fDocumentWaiting) return;
    fDocumentWaiting = true;
    if (!fAgentsEnabled) {
        fAgentsEnabled = true;
        // Inspector: what the picker chose; CSS: the style sheets, and styles.
        Send("Inspector.enable", "{}");
        Send("CSS.enable", "{}");
        if (fInherited.empty()) Send("CSS.getSupportedCSSProperties", "{}", Pending::Properties);
    }
    Send("DOM.getDocument", "{}", Pending::Document);
}

void DOMModel::Reset(bool forgetAgents)
{
    fNodes.clear();
    fExpanded.clear();
    fAfterRemoval.clear();
    fForced.clear();
    // A new document has none of the old one's style sheets.
    fInspectorSheet.clear();
    fDocument = fSelected = fHighlighted = 0;
    fDocumentWaiting = false;
    fStyles = { };
    fNextStyles = { };
    fDeclared.clear();
    fStylesOutstanding = 0;
    fStylesStale = false;
    ++fStylesGeneration;
    ++fGeneration;
    fSearchResults.clear();
    fSearchID.clear();
    fSearchWaiting = false;
    for (auto pending = fPending.begin(); pending != fPending.end();)
        pending = pending->second.kind == Pending::Properties ? std::next(pending) : fPending.erase(pending);
    if (forgetAgents) {
        fAgentsEnabled = false;
        fSheets.clear();
        if (fPicking) {
            fPicking = false;
            fChanges.picking = true;
        }
    }
    fChanges.tree = fChanges.selection = fChanges.styles = fChanges.search = true;
}

void DOMModel::TargetChanged()
{
    if (!fEnabled) {
        fAgentsEnabled = false;
        return;
    }
    if (fRestorePath.empty()) fRestorePath = PathOf(fSelected);
    fRestoreTries = 0;
    Reset(true);
    RequestDocument();
}

void DOMModel::Erase(int id)
{
    const auto found = fNodes.find(id);
    if (found == fNodes.end()) return;
    const DOMNode node = std::move(found->second);
    fNodes.erase(found);
    fExpanded.erase(id);
    for (const int child : node.children) Erase(child);
    for (const int root : node.shadowRoots) Erase(root);
    for (const int pseudo : node.pseudoElements) Erase(pseudo);
    if (node.contentDocument) Erase(node.contentDocument);
    if (node.templateContent) Erase(node.templateContent);
}

void DOMModel::Detach(int id)
{
    const auto* node = Find(id);
    if (!node) return;
    if (auto* parent = Node(node->parent)) {
        const auto drop = [id](std::vector<int>& list) { list.erase(std::remove(list.begin(), list.end(), id), list.end()); };
        const auto before = parent->children.size();
        drop(parent->children);
        if (parent->children.size() != before) parent->childCount = static_cast<int>(parent->children.size());
        drop(parent->shadowRoots);
        drop(parent->pseudoElements);
        if (parent->contentDocument == id) parent->contentDocument = 0;
        if (parent->templateContent == id) parent->templateContent = 0;
    }
    Erase(id);
}

void DOMModel::Removed(int id)
{
    const auto* node = Find(id);
    if (!node) return;
    bool selectionGone = false;
    for (const auto* up = Find(fSelected); up; up = Find(up->parent)) {
        if (up->id == id) {
            selectionGone = true;
            break;
        }
    }
    const int parent = node->parent;
    int after = 0;
    if (const auto found = fAfterRemoval.find(id); found != fAfterRemoval.end()) {
        after = found->second;
        fAfterRemoval.erase(found);
    }
    if (fHighlighted == id) fHighlighted = 0;
    Detach(id);
    fChanges.tree = true;
    if (!selectionGone) return;
    fSelected = 0;
    const int target = Find(after) ? after : parent;
    if (Find(target) && target != fDocument) Select(target);
    else {
        fStyles = { };
        fChanges.selection = fChanges.styles = true;
    }
}

std::vector<int> DOMModel::ShownChildren(int id) const
{
    std::vector<int> shown;
    const auto* node = Find(id);
    if (!node) return shown;
    const auto pseudo = [&](bool before) {
        for (const int child : node->pseudoElements) {
            const auto* element = Find(child);
            if (element && (element->pseudoType == "before") == before) shown.push_back(child);
        }
    };
    shown.insert(shown.end(), node->shadowRoots.begin(), node->shadowRoots.end());
    if (node->templateContent) shown.push_back(node->templateContent);
    pseudo(true);
    if (node->contentDocument) shown.push_back(node->contentDocument);
    shown.insert(shown.end(), node->children.begin(), node->children.end());
    pseudo(false);
    return shown;
}

bool DOMModel::Expandable(int id) const
{
    const auto* node = Find(id);
    if (!node) return false;
    if (!node->childrenKnown && node->childCount > 0) return true;
    return !ShownChildren(id).empty() && !InlineText(id);
}

void DOMModel::SetExpanded(int id, bool expanded)
{
    const auto* node = Find(id);
    if (!node) return;
    if (!expanded) {
        if (fExpanded.erase(id)) fChanges.tree = true;
        return;
    }
    if (!fExpanded.insert(id).second) return;
    fChanges.tree = true;
    if (!node->childrenKnown && node->childCount > 0)
        Send("DOM.requestChildNodes", "{\"nodeId\":" + std::to_string(id) + "}", Pending::Children, id);
}

void DOMModel::ExpandAll(int id)
{
    if (!Find(id)) return;
    Send("DOM.requestChildNodes", "{\"nodeId\":" + std::to_string(id) + ",\"depth\":-1}", Pending::ExpandAll, id);
}

void DOMModel::CollapseAll(int id)
{
    std::function<void(int)> collapse = [&](int node) {
        fExpanded.erase(node);
        for (const int child : ShownChildren(node)) collapse(child);
    };
    collapse(id);
    fChanges.tree = true;
}

bool DOMModel::InlineText(int id, std::string* text) const
{
    const auto* node = Find(id);
    if (!node || !node->IsElement() || !node->childrenKnown || node->children.size() != 1 || !node->shadowRoots.empty()
        || !node->pseudoElements.empty() || node->contentDocument || node->templateContent)
        return false;
    const auto* child = Find(node->children[0]);
    if (!child || child->type != kTextNode || child->value.size() > 160) return false;
    const auto collapsed = Collapse(child->value, 200);
    if (collapsed.size() > 80) return false;
    if (text) *text = collapsed;
    return true;
}

std::vector<TreeRow> DOMModel::Rows() const
{
    std::vector<TreeRow> rows;
    std::function<void(int, int)> add = [&](int id, int depth) {
        rows.push_back({id, depth, false});
        if (!IsExpanded(id) || InlineText(id)) return;
        const auto children = ShownChildren(id);
        if (children.empty()) return;
        for (const int child : children) add(child, depth + 1);
        const auto* node = Find(id);
        if (node && node->IsElement() && node->pseudoType.empty()) rows.push_back({id, depth, true});
    };
    if (Find(fDocument))
        for (const int child : ShownChildren(fDocument)) add(child, 0);
    return rows;
}

std::vector<RowPart> DOMModel::DescribeRow(const TreeRow& row) const
{
    std::vector<RowPart> parts;
    const auto* node = Find(row.node);
    if (!node) return parts;
    const auto add = [&](std::string text, Piece kind) {
        if (!text.empty()) parts.push_back({std::move(text), kind});
    };
    const auto closing = [&] {
        add("</", Piece::Punctuation);
        add(node->Tag(), Piece::Tag);
        add(">", Piece::Punctuation);
    };
    if (row.closing) {
        closing();
        return parts;
    }
    switch (node->type) {
        case kElementNode: {
            if (!node->pseudoType.empty()) {
                add("::" + node->pseudoType, Piece::Pseudo);
                break;
            }
            add("<", Piece::Punctuation);
            add(node->Tag(), Piece::Tag);
            for (const auto& attribute : node->attributes) {
                add(" ", Piece::Punctuation);
                add(attribute.name, Piece::AttributeName);
                if (attribute.value.empty()) continue;
                add("=\"", Piece::Punctuation);
                add(Collapse(attribute.value, 200), Piece::AttributeValue);
                add("\"", Piece::Punctuation);
            }
            add(">", Piece::Punctuation);
            std::string text;
            if (InlineText(row.node, &text)) {
                add(text, Piece::Text);
                closing();
            } else if (Expandable(row.node)) {
                if (!IsExpanded(row.node) || ShownChildren(row.node).empty()) {
                    add("…", Piece::Muted);
                    closing();
                }
            } else if (!IsVoid(node->Tag()))
                closing();
            break;
        }
        case kTextNode: add("\"" + Collapse(node->value, 300) + "\"", Piece::Text); break;
        case kCommentNode: add("<!-- " + Collapse(node->value, 300) + " -->", Piece::Comment); break;
        case kCDATANode: add("<![CDATA[" + Collapse(node->value, 300) + "]]>", Piece::Text); break;
        case kProcessingInstructionNode: add("<?" + node->name + " " + Collapse(node->value, 300) + "?>", Piece::Muted); break;
        case kDocumentTypeNode: {
            std::string text = "<!DOCTYPE " + node->name;
            if (!node->publicId.empty()) text += " PUBLIC \"" + node->publicId + "\"";
            if (!node->systemId.empty()) text += (node->publicId.empty() ? " SYSTEM \"" : " \"") + node->systemId + "\"";
            add(text + ">", Piece::Muted);
            break;
        }
        case kDocumentNode:
            add("#document", Piece::Muted);
            if (!node->documentURL.empty()) add(" (" + Collapse(node->documentURL, 200) + ")", Piece::Muted);
            break;
        case kDocumentFragmentNode:
            add(node->shadowRootType.empty() ? std::string("#document-fragment") : "#shadow-root (" + node->shadowRootType + ")",
                Piece::Muted);
            break;
        default: add(node->name, Piece::Muted); break;
    }
    return parts;
}

std::string DOMModel::Breadcrumbs(int id) const
{
    std::vector<std::string> labels;
    for (const auto* node = Find(id); node && node->type != kDocumentNode; node = Find(node->parent)) {
        if (node->IsElement() || node->id == id) labels.push_back(node->Label());
        if (node->Tag() == "html" && node->IsElement()) break;
    }
    std::string text;
    for (auto label = labels.rbegin(); label != labels.rend(); ++label) {
        if (!text.empty()) text += " › ";
        text += *label;
    }
    return text;
}

std::string DOMModel::Selector(int id) const
{
    std::vector<std::string> parts;
    for (const auto* node = Find(id); node && node->IsElement(); node = Find(node->parent)) {
        if (!node->pseudoType.empty()) {
            parts.push_back("::" + node->pseudoType);
            continue;
        }
        const auto* identifier = node->FindAttribute("id");
        if (identifier && !identifier->empty() && identifier->find_first_of(" \t\n\"'") == std::string::npos) {
            parts.push_back("#" + *identifier);
            break;
        }
        auto part = node->Tag();
        const auto* parent = Find(node->parent);
        // There is one of each of these.
        if (part == "html" || part == "body" || part == "head" || !parent) {
            parts.push_back(part);
            break;
        }
        int index = 0, position = 0, same = 0;
        for (const int sibling : parent->children) {
            const auto* other = Find(sibling);
            if (!other || !other->IsElement()) continue;
            ++index;
            if (other->Tag() == part) ++same;
            if (sibling == node->id) position = index;
        }
        if (same > 1 && position) part += ":nth-child(" + std::to_string(position) + ")";
        parts.push_back(part);
        if (parent->type != kElementNode) break;
    }
    std::string text;
    for (auto part = parts.rbegin(); part != parts.rend(); ++part) {
        if (!text.empty()) text += part->rfind("::", 0) == 0 ? "" : " > ";
        text += *part;
    }
    return text;
}

std::string DOMModel::AttributesText(int id) const
{
    std::string text;
    if (const auto* node = Find(id)) {
        for (const auto& attribute : node->attributes) {
            if (!text.empty()) text += ' ';
            text += attribute.name + "=\"" + EscapeAttribute(attribute.value) + "\"";
        }
    }
    return text;
}

std::string DOMModel::PathOf(int id) const
{
    std::vector<std::pair<size_t, std::string>> steps;
    const auto* node = Find(id);
    while (node && node->parent) {
        const auto* parent = Find(node->parent);
        if (!parent) return { };
        if (parent->contentDocument == node->id) steps.emplace_back(0, node->name);
        else {
            const auto found = std::find(parent->children.begin(), parent->children.end(), node->id);
            if (found == parent->children.end()) {
                // A shadow root or pseudo element: the path stops at its host.
                steps.clear();
                node = parent;
                continue;
            }
            steps.emplace_back(static_cast<size_t>(found - parent->children.begin()), node->name);
        }
        node = parent;
    }
    if (!node || node->id != fDocument || steps.empty()) return { };
    std::string path;
    for (auto step = steps.rbegin(); step != steps.rend(); ++step) {
        if (!path.empty()) path += ',';
        path += std::to_string(step->first) + "," + step->second;
    }
    return path;
}

void DOMModel::Select(int id) { SetSelection(id, true); }

void DOMModel::SetSelection(int id, bool styles)
{
    if (!Find(id) || id == fSelected) return;
    fSelected = id;
    fChanges.selection = true;
    // $0 in the console.
    Send("DOM.setInspectedNode", "{\"nodeId\":" + std::to_string(id) + "}");
    if (styles) StartStyles();
}

void DOMModel::SelectDefault()
{
    const auto* document = Find(fDocument);
    if (!document) return;
    int html = 0, body = 0;
    for (const int child : document->children) {
        const auto* node = Find(child);
        if (node && node->IsElement()) {
            html = child;
            break;
        }
    }
    if (const auto* root = Find(html)) {
        for (const int child : root->children) {
            const auto* node = Find(child);
            if (node && node->IsElement() && node->Tag() == "body") body = child;
        }
    }
    const int chosen = body ? body : html ? html : document->children.empty() ? 0 : document->children[0];
    if (chosen) Select(chosen);
}

void DOMModel::Reveal(int id)
{
    const auto* node = Find(id);
    if (!node) return;
    for (const auto* up = Find(node->parent); up; up = Find(up->parent))
        if (fExpanded.insert(up->id).second) fChanges.tree = true;
}

void DOMModel::StartStyles()
{
    ++fStylesGeneration;
    fStylesStale = false;
    fStylesStarted = Seconds();
    const auto* node = Find(fSelected);
    if (!node || !node->IsElement()) {
        fStylesOutstanding = 0;
        fStyles = { };
        fStyles.node = fSelected;
        fDeclared.clear();
        fChanges.styles = true;
        return;
    }
    fNextStyles = { };
    fNextStyles.node = fSelected;
    fMatchedJSON.clear();
    fInlineJSON.clear();
    fStylesOutstanding = 4;
    const auto id = "{\"nodeId\":" + std::to_string(fSelected);
    const auto ask = [&](const char* method, const std::string& parameters, Pending::Kind kind) {
        const auto tag = Send(method, parameters, kind, fSelected);
        fPending[tag].generation = fStylesGeneration;
    };
    ask("CSS.getMatchedStylesForNode", id + ",\"includePseudo\":true,\"includeInherited\":true}", Pending::Matched);
    ask("CSS.getInlineStylesForNode", id + "}", Pending::Inline);
    ask("CSS.getComputedStyleForNode", id + "}", Pending::Computed);
    ask("DOM.resolveNode", id + ",\"objectGroup\":" + Quoted(kObjectGroup) + "}", Pending::Resolve);
}

void DOMModel::StylesArrived()
{
    fStylesOutstanding = 0;
    DOMReader::BuildStyles(*this);
    std::map<std::string, std::string> computed(fNextStyles.computed.begin(), fNextStyles.computed.end());
    const auto value = [&](const std::string& name) {
        const auto found = computed.find(name);
        return found == computed.end() ? std::string() : found->second;
    };
    auto& box = fNextStyles.box;
    static const char* const kSides[] = {"top", "right", "bottom", "left"};
    for (int side = 0; side < 4; ++side) {
        box.margin[side] = Pixels(value(std::string("margin-") + kSides[side]));
        box.border[side] = Pixels(value(std::string("border-") + kSides[side] + "-width"));
        box.padding[side] = Pixels(value(std::string("padding-") + kSides[side]));
        box.offsets[side] = value(kSides[side]);
    }
    box.position = value("position");
    box.boxSizing = value("box-sizing");
    box.display = value("display");
    // The content box: the computed size where it is a length, else the
    // border box the page measured, less padding and borders.
    const double horizontal = box.padding[1] + box.padding[3] + box.border[1] + box.border[3];
    const double vertical = box.padding[0] + box.padding[2] + box.border[0] + box.border[2];
    const auto size = [&](const std::string& computedSize, double measured, double edges) {
        const bool length = computedSize.size() > 2 && computedSize.compare(computedSize.size() - 2, 2, "px") == 0;
        const double border = length ? Pixels(computedSize) + (box.boxSizing == "border-box" ? 0 : edges) : measured;
        return std::max(0.0, border - edges);
    };
    box.width = size(value("width"), box.width, horizontal);
    box.height = size(value("height"), box.height, vertical);
    box.known = !computed.empty() && box.display != "none";
    fStyles = std::move(fNextStyles);
    fNextStyles = { };
    fMatchedJSON.clear();
    fInlineJSON.clear();
    // What the computed list shows when it does not show everything.
    fDeclared.clear();
    for (const auto& block : fStyles.blocks) {
        if (!block.pseudo.empty()) continue;
        for (const auto& declaration : block.declarations) {
            if (!declaration.enabled) continue;
            const auto name = LowerCase(declaration.name);
            fDeclared.insert(name);
            if (const auto found = fLonghands.find(name); found != fLonghands.end())
                fDeclared.insert(found->second.begin(), found->second.end());
        }
    }
    fChanges.styles = true;
}

std::optional<std::array<uint8_t, 4>> ParseCSSColor(const std::string& value)
{
    auto text = LowerCase(Trim(value));
    if (text.empty()) return std::nullopt;
    const auto hexDigit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    if (text[0] == '#') {
        const auto digits = text.substr(1);
        if (std::any_of(digits.begin(), digits.end(), [&](char c) { return hexDigit(c) < 0; })) return std::nullopt;
        std::array<uint8_t, 4> color {0, 0, 0, 255};
        if (digits.size() == 3 || digits.size() == 4) {
            for (size_t i = 0; i < digits.size(); ++i) color[i] = static_cast<uint8_t>(hexDigit(digits[i]) * 17);
            return color;
        }
        if (digits.size() == 6 || digits.size() == 8) {
            for (size_t i = 0; i < digits.size() / 2; ++i)
                color[i] = static_cast<uint8_t>(hexDigit(digits[2 * i]) * 16 + hexDigit(digits[2 * i + 1]));
            return color;
        }
        return std::nullopt;
    }
    if (text.rfind("rgb(", 0) == 0 || text.rfind("rgba(", 0) == 0) {
        const auto open = text.find('('), close = text.rfind(')');
        if (close == std::string::npos || close < open) return std::nullopt;
        auto inner = text.substr(open + 1, close - open - 1);
        std::replace(inner.begin(), inner.end(), ',', ' ');
        std::replace(inner.begin(), inner.end(), '/', ' ');
        std::vector<std::string> parts;
        for (size_t from = 0; from < inner.size();) {
            const auto start = inner.find_first_not_of(' ', from);
            if (start == std::string::npos) break;
            const auto end = std::min(inner.find(' ', start), inner.size());
            parts.push_back(inner.substr(start, end - start));
            from = end;
        }
        if (parts.size() != 3 && parts.size() != 4) return std::nullopt;
        std::array<uint8_t, 4> color {0, 0, 0, 255};
        for (size_t i = 0; i < parts.size(); ++i) {
            char* end = nullptr;
            const double number = std::strtod(parts[i].c_str(), &end);
            if (end == parts[i].c_str()) return std::nullopt;
            const bool percent = *end == '%';
            const double scaled = percent ? number / 100 * 255 : i < 3 ? number : number * 255;
            color[i] = static_cast<uint8_t>(std::clamp(std::round(scaled), 0.0, 255.0));
        }
        return color;
    }
    static const std::map<std::string, std::array<uint8_t, 4>> named = {
        {"transparent", {0, 0, 0, 0}}, {"black", {0, 0, 0, 255}}, {"white", {255, 255, 255, 255}}, {"red", {255, 0, 0, 255}},
        {"green", {0, 128, 0, 255}}, {"blue", {0, 0, 255, 255}}, {"yellow", {255, 255, 0, 255}}, {"orange", {255, 165, 0, 255}},
        {"purple", {128, 0, 128, 255}}, {"gray", {128, 128, 128, 255}}, {"grey", {128, 128, 128, 255}}, {"silver", {192, 192, 192, 255}},
        {"maroon", {128, 0, 0, 255}}, {"navy", {0, 0, 128, 255}}, {"teal", {0, 128, 128, 255}}, {"olive", {128, 128, 0, 255}},
        {"lime", {0, 255, 0, 255}}, {"aqua", {0, 255, 255, 255}}, {"cyan", {0, 255, 255, 255}}, {"fuchsia", {255, 0, 255, 255}},
        {"magenta", {255, 0, 255, 255}}};
    if (const auto found = named.find(text); found != named.end()) return found->second;
    return std::nullopt;
}

std::string CSSColorText(const std::array<uint8_t, 4>& color)
{
    char text[16];
    if (color[3] == 255) std::snprintf(text, sizeof text, "#%02x%02x%02x", color[0], color[1], color[2]);
    else std::snprintf(text, sizeof text, "#%02x%02x%02x%02x", color[0], color[1], color[2], color[3]);
    return text;
}

std::string DOMModel::ComputedValue(const std::string& property) const
{
    // The list is sorted by name.
    const auto& computed = fStyles.computed;
    const auto found = std::lower_bound(computed.begin(), computed.end(), property,
        [](const auto& entry, const std::string& name) { return entry.first < name; });
    return found != computed.end() && found->first == property ? found->second : std::string();
}

bool DOMModel::IsInheritable(const std::string& property) const
{
    if (property.rfind("--", 0) == 0) return true;
    return fInherited.empty() ? InheritedByDefault(property) : fInherited.count(property) > 0;
}

bool DOMModel::Declared(const std::string& property) const { return fDeclared.count(LowerCase(property)) > 0; }

void DOMModel::SetForcedPseudoClasses(int id, std::vector<std::string> classes)
{
    if (!Find(id)) return;
    std::sort(classes.begin(), classes.end());
    classes.erase(std::unique(classes.begin(), classes.end()), classes.end());
    std::string list;
    for (const auto& name : classes) list += (list.empty() ? "" : ",") + Quoted(name);
    Send("CSS.forcePseudoState", "{\"nodeId\":" + std::to_string(id) + ",\"forcedPseudoClasses\":[" + list + "]}");
    if (classes.empty()) fForced.erase(id);
    else fForced[id] = std::move(classes);
    if (id == fSelected) RefreshStyles();
}

std::vector<std::string> DOMModel::ForcedPseudoClasses(int id) const
{
    const auto found = fForced.find(id);
    return found == fForced.end() ? std::vector<std::string>() : found->second;
}

void DOMModel::SetRuleSelector(size_t block, const std::string& selector)
{
    if (block >= fStyles.blocks.size() || !fStyles.blocks[block].SelectorEditable() || Trim(selector).empty()) return;
    const auto& style = fStyles.blocks[block];
    Send("CSS.setRuleSelector", "{\"ruleId\":{\"styleSheetId\":" + Quoted(style.styleSheetId) + ",\"ordinal\":"
        + std::to_string(style.ruleOrdinal) + "},\"selector\":" + Quoted(Trim(selector)) + "}", Pending::SetStyle);
}

void DOMModel::AddRule(const std::string& selector)
{
    if (Trim(selector).empty()) return;
    if (fInspectorSheet.empty()) {
        const auto tag = Send("Page.getResourceTree", "{}", Pending::RuleFrame);
        fPending[tag].text = Trim(selector);
        return;
    }
    Send("CSS.addRule", "{\"styleSheetId\":" + Quoted(fInspectorSheet) + ",\"selector\":" + Quoted(Trim(selector)) + "}",
        Pending::AddRule);
}

void DOMModel::RefreshStyles()
{
    if (!fSelected) return;
    if (fStylesOutstanding) fStylesStale = true;
    else StartStyles();
}

void DOMModel::RequestListeners()
{
    fListeners = { };
    fListeners.node = fSelected;
    if (!fSelected) return;
    Send("DOM.getEventListenersForNode", "{\"nodeId\":" + std::to_string(fSelected) + "}", Pending::Listeners, fSelected);
}

void DOMModel::RequestAccessibility()
{
    if (!fSelected) return;
    fAccessibility = { };
    fAccessibility.node = fSelected;
    Send("DOM.getAccessibilityPropertiesForNode", "{\"nodeId\":" + std::to_string(fSelected) + "}", Pending::Accessibility, fSelected);
}

void DOMModel::ToggleDeclaration(size_t block, size_t index)
{
    if (block >= fStyles.blocks.size()) return;
    auto& style = fStyles.blocks[block];
    if (!style.Editable() || index >= style.declarations.size()) return;
    const auto text = EditStyleText(style, index, StyleEdit::Toggle);
    // Shown at once; the styles read again confirm it.
    style.declarations[index].enabled = !style.declarations[index].enabled;
    fChanges.styles = true;
    Send("CSS.setStyleText", "{\"styleId\":{\"styleSheetId\":" + Quoted(style.styleSheetId) + ",\"ordinal\":"
        + std::to_string(style.ordinal) + "},\"text\":" + Quoted(text) + "}", Pending::SetStyle);
}

void DOMModel::EditDeclaration(size_t block, size_t index, const std::string& text)
{
    if (block >= fStyles.blocks.size()) return;
    const auto& style = fStyles.blocks[block];
    if (!style.Editable() || index >= style.declarations.size()) return;
    Send("CSS.setStyleText", "{\"styleId\":{\"styleSheetId\":" + Quoted(style.styleSheetId) + ",\"ordinal\":"
        + std::to_string(style.ordinal) + "},\"text\":" + Quoted(EditStyleText(style, index, StyleEdit::Replace, text)) + "}",
        Pending::SetStyle);
}

void DOMModel::AddDeclaration(size_t block, const std::string& text)
{
    if (block >= fStyles.blocks.size() || Trim(text).empty()) return;
    const auto& style = fStyles.blocks[block];
    if (!style.Editable()) return;
    Send("CSS.setStyleText", "{\"styleId\":{\"styleSheetId\":" + Quoted(style.styleSheetId) + ",\"ordinal\":"
        + std::to_string(style.ordinal) + "},\"text\":" + Quoted(EditStyleText(style, 0, StyleEdit::Add, text)) + "}",
        Pending::SetStyle);
}

void DOMModel::RequestSheetSource(size_t block)
{
    if (block >= fStyles.blocks.size()) return;
    const auto& style = fStyles.blocks[block];
    const auto sheet = fSheets.find(style.styleSheetId);
    auto title = style.sourceURL.empty() ? std::string("Style sheet") : style.sourceURL;
    if (sheet != fSheets.end() && sheet->second.inlined) title = "Style sheet in " + title;
    if (style.styleSheetId.empty() || style.kind != StyleBlock::Rule) {
        fSource = {title, { }, "The browser's own style sheets cannot be shown.", 0};
        fChanges.source = true;
        return;
    }
    // A <style> in the document starts at a line of it.
    int line = style.line;
    if (sheet != fSheets.end() && line > 0) line = std::max(1, line - sheet->second.startLine);
    const auto tag = Send("CSS.getStyleSheetText", "{\"styleSheetId\":" + Quoted(style.styleSheetId) + "}", Pending::SheetText);
    fPending[tag].text = title;
    fPending[tag].purpose = line;
}

void DOMModel::SetAttributesText(int id, const std::string& text)
{
    const auto* node = Find(id);
    if (!node || !node->IsElement()) return;
    const auto names = AttributeNames(text);
    for (const auto& attribute : node->attributes)
        if (std::find(names.begin(), names.end(), LowerCase(attribute.name)) == names.end())
            Send("DOM.removeAttribute", "{\"nodeId\":" + std::to_string(id) + ",\"name\":" + Quoted(attribute.name) + "}", Pending::Edit);
    if (!Trim(text).empty())
        Send("DOM.setAttributesAsText", "{\"nodeId\":" + std::to_string(id) + ",\"text\":" + Quoted(text) + "}", Pending::Edit);
    Send("DOM.markUndoableState", "{}");
}

void DOMModel::AddAttributes(int id, const std::string& text)
{
    const auto* node = Find(id);
    if (!node || !node->IsElement() || Trim(text).empty()) return;
    Send("DOM.setAttributesAsText", "{\"nodeId\":" + std::to_string(id) + ",\"text\":" + Quoted(text) + "}", Pending::Edit);
    Send("DOM.markUndoableState", "{}");
}

void DOMModel::SetNodeText(int id, const std::string& text)
{
    if (!Find(id)) return;
    Send("DOM.setNodeValue", "{\"nodeId\":" + std::to_string(id) + ",\"value\":" + Quoted(text) + "}", Pending::Edit);
    Send("DOM.markUndoableState", "{}");
}

void DOMModel::SetTagName(int id, const std::string& name)
{
    const auto* node = Find(id);
    const auto tag = Trim(name);
    if (!node || !node->IsElement() || tag.empty() || tag == node->Tag()) return;
    Send("DOM.setNodeName", "{\"nodeId\":" + std::to_string(id) + ",\"name\":" + Quoted(tag) + "}", Pending::TagName, id);
    Send("DOM.markUndoableState", "{}");
}

void DOMModel::SetOuterHTML(int id, const std::string& html)
{
    if (!Find(id)) return;
    Send("DOM.setOuterHTML", "{\"nodeId\":" + std::to_string(id) + ",\"outerHTML\":" + Quoted(html) + "}", Pending::Edit);
    Send("DOM.markUndoableState", "{}");
}

void DOMModel::Remove(int id)
{
    const auto* node = Find(id);
    if (!node || id == fDocument) return;
    // The selection moves to the next node, else the one before, else the parent.
    const auto siblings = ShownChildren(node->parent);
    const auto at = std::find(siblings.begin(), siblings.end(), id);
    int after = node->parent;
    if (at != siblings.end() && at + 1 != siblings.end()) after = *(at + 1);
    else if (at != siblings.end() && at != siblings.begin()) after = *(at - 1);
    fAfterRemoval[id] = after;
    Send("DOM.removeNode", "{\"nodeId\":" + std::to_string(id) + "}", Pending::Remove, id);
    Send("DOM.markUndoableState", "{}");
}

bool DOMModel::Move(int id, int parent, int before)
{
    const auto* node = Find(id);
    const auto* into = Find(parent);
    if (!node || !into || id == fDocument || node->parent == fDocument || !into->IsElement() || IsVoid(into->Tag())) return false;
    // Not into itself or anything under it.
    for (const auto* above = into; above; above = Find(above->parent))
        if (above->id == id) return false;
    if (before == id) return false;
    if (before && std::find(into->children.begin(), into->children.end(), before) == into->children.end()) return false;
    // Already there.
    if (node->parent == parent) {
        const auto& siblings = into->children;
        const auto at = std::find(siblings.begin(), siblings.end(), id);
        if (at != siblings.end() && ((before && at + 1 != siblings.end() && *(at + 1) == before) || (!before && at + 1 == siblings.end())))
            return false;
    }
    std::string parameters = "{\"nodeId\":" + std::to_string(id) + ",\"targetNodeId\":" + std::to_string(parent);
    if (before) parameters += ",\"insertBeforeNodeId\":" + std::to_string(before);
    Send("DOM.moveTo", parameters + "}", Pending::Move, id);
    Send("DOM.markUndoableState", "{}");
    return true;
}

bool DOMModel::DropTarget(const TreeRow& row, int zone, int* parent, int* before) const
{
    const auto* node = Find(row.node);
    if (!node) return false;
    // A closing tag: into the element as its last child, or after it.
    if (row.closing && zone < 0) zone = 0;
    if (zone == 0) {
        if (!node->IsElement() || IsVoid(node->Tag())) return false;
        *parent = node->id;
        *before = 0;
        return true;
    }
    const auto* above = Find(node->parent);
    if (!above || !above->IsElement()) return false;
    *parent = above->id;
    if (zone < 0) {
        *before = node->id;
        return true;
    }
    const auto& siblings = above->children;
    const auto at = std::find(siblings.begin(), siblings.end(), node->id);
    *before = at != siblings.end() && at + 1 != siblings.end() ? *(at + 1) : 0;
    return true;
}

void DOMModel::Undo() { Send("DOM.undo", "{}", Pending::Edit); }
void DOMModel::Redo() { Send("DOM.redo", "{}", Pending::Edit); }

void DOMModel::ScrollIntoView(int id)
{
    if (!Find(id)) return;
    const auto tag = Send("DOM.resolveNode", "{\"nodeId\":" + std::to_string(id) + ",\"objectGroup\":" + Quoted(kObjectGroup) + "}",
        Pending::Resolve, id);
    fPending[tag].purpose = 1;
}

void DOMModel::RequestOuterHTML(int id, int purpose)
{
    if (!Find(id)) return;
    const auto tag = Send("DOM.getOuterHTML", "{\"nodeId\":" + std::to_string(id) + "}", Pending::OuterHTML, id);
    fPending[tag].purpose = purpose;
}

void DOMModel::Highlight(int id)
{
    if (id == fHighlighted || !Find(id)) return;
    fHighlighted = id;
    Send("DOM.highlightNode", "{\"nodeId\":" + std::to_string(id) + ",\"highlightConfig\":" + kHighlightConfig + "}");
}

void DOMModel::HideHighlight()
{
    if (!fHighlighted) return;
    fHighlighted = 0;
    Send("DOM.hideHighlight", "{}");
}

void DOMModel::SetPicking(bool picking)
{
    if (picking == fPicking) return;
    if (!fEnabled) Enable();
    fPicking = picking;
    fHighlighted = 0;
    fChanges.picking = true;
    Send("DOM.setInspectModeEnabled", std::string("{\"enabled\":") + (picking ? "true" : "false") + ",\"highlightConfig\":"
        + kHighlightConfig + "}");
}

void DOMModel::InspectAt(double x, double y)
{
    if (!fEnabled) Enable();
    if (!Loaded()) {
        fHasPoint = true;
        fPointX = x;
        fPointY = y;
        return;
    }
    fHasPoint = false;
    char point[64];
    std::snprintf(point, sizeof point, "(%.2f, %.2f)", x, y);
    Send("Runtime.evaluate", "{\"expression\":" + Quoted(std::string(kElementAtPoint) + point) + ",\"objectGroup\":"
        + Quoted(kObjectGroup) + ",\"returnByValue\":false}", Pending::Evaluate);
}

void DOMModel::Search(const std::string& query)
{
    if (!fSearchID.empty()) Send("DOM.discardSearchResults", "{\"searchId\":" + Quoted(fSearchID) + "}");
    fSearchID.clear();
    fSearchQuery = query;
    fSearchResults.clear();
    fSearchIndex = 0;
    fSearchWaiting = false;
    fChanges.search = true;
    if (Trim(query).empty()) return;
    if (!Loaded()) {
        // Run once the document has been read.
        Enable();
        return;
    }
    fSearchWaiting = true;
    const auto tag = Send("DOM.performSearch", "{\"query\":" + Quoted(query) + ",\"caseSensitive\":false}", Pending::Search);
    fPending[tag].text = query;
}

void DOMModel::SearchStep(int direction)
{
    if (fSearchResults.empty()) return;
    const auto count = fSearchResults.size();
    fSearchIndex = (fSearchIndex + count + (direction < 0 ? count - 1 : 1)) % count;
    fChanges.search = true;
    Reveal(fSearchResults[fSearchIndex]);
    Select(fSearchResults[fSearchIndex]);
}

std::vector<DOMModel::Command> DOMModel::TakeCommands()
{
    // Styles the page changed are read again, not more often than this.
    if (fStylesStale && !fStylesOutstanding && fSelected && Seconds() - fStylesStarted >= kStyleInterval) StartStyles();
    return std::exchange(fCommands, { });
}

bool DOMModel::StylesWaiting() const { return fStylesStale && !fStylesOutstanding && fSelected; }

void DOMModel::Answered(uint64_t tag, std::string_view message)
{
    const auto found = fPending.find(tag);
    if (found == fPending.end()) return;
    const auto pending = found->second;
    fPending.erase(found);
    DOMReader::Answered(*this, pending, json::parse(message, nullptr, false));
}

void DOMModel::Failed(uint64_t tag, const std::string& error)
{
    const auto found = fPending.find(tag);
    if (found == fPending.end()) return;
    const auto pending = found->second;
    fPending.erase(found);
    DOMReader::Answered(*this, pending, json {{"error", {{"message", error}}}});
}

void DOMModel::Event(std::string_view text)
{
    if (!fEnabled) return;
    const auto message = json::parse(text, nullptr, false);
    if (!message.is_object()) return;
    DOMReader::Event(*this, Text(Member(message, "method")), Member(message, "params"));
}
}
