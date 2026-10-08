#include "DOM.h"
#include "CSSSyntax.h"
#include "NumericParser.h"
#include "HtmlNamedEntities.h"

#include <algorithm>
#include <cwctype>
#include <functional>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <windows.h>
#include <objbase.h>
#include <xmllite.h>
#include <shlwapi.h>
#include <wrl/client.h>
#pragma comment(lib, "xmllite.lib")
#pragma comment(lib, "shlwapi.lib")

namespace TWebFrame::Internal {

namespace {

bool IsSpace(wchar_t c) { return std::iswspace(c) != 0; }

std::wstring AdjustSvgAttribute(const std::wstring& name){
    // HTML tree construction adjusts these tokenized lowercase SVG names.
    // https://html.spec.whatwg.org/multipage/parsing.html#adjust-svg-attributes
    static const FastMap<std::wstring,std::wstring> names=[](){
        FastMap<std::wstring,std::wstring> result;
        for(const auto* adjusted:{L"attributeName",L"attributeType",L"baseFrequency",L"baseProfile",
            L"calcMode",L"clipPathUnits",L"diffuseConstant",L"edgeMode",L"filterUnits",L"glyphRef",
            L"gradientTransform",L"gradientUnits",L"kernelMatrix",L"kernelUnitLength",L"keyPoints",
            L"keySplines",L"keyTimes",L"lengthAdjust",L"limitingConeAngle",L"markerHeight",L"markerUnits",
            L"markerWidth",L"maskContentUnits",L"maskUnits",L"numOctaves",L"pathLength",L"patternContentUnits",
            L"patternTransform",L"patternUnits",L"pointsAtX",L"pointsAtY",L"pointsAtZ",L"preserveAlpha",
            L"preserveAspectRatio",L"primitiveUnits",L"refX",L"refY",L"repeatCount",L"repeatDur",
            L"requiredExtensions",L"requiredFeatures",L"specularConstant",L"specularExponent",L"spreadMethod",
            L"startOffset",L"stdDeviation",L"stitchTiles",L"surfaceScale",L"systemLanguage",L"tableValues",
            L"targetX",L"targetY",L"textLength",L"viewBox",L"viewTarget",L"xChannelSelector",L"yChannelSelector",L"zoomAndPan"})
            result.emplace(ToLower(adjusted),adjusted);
        return result;
    }();const auto found=names.find(name);return found==names.end()?name:found->second;
}

template<typename Callback>
void ForEachClassToken(const std::wstring& value, Callback&& callback) {
    size_t position = 0;
    while (position < value.size()) {
        while (position < value.size() && IsSpace(value[position])) ++position;
        const size_t start = position;
        while (position < value.size() && !IsSpace(value[position])) ++position;
        if (start != position) callback(value.substr(start, position - start));
    }
}

bool IsVoidTag(const std::wstring& tag) {
    static const wchar_t* tags[] = {
        L"area", L"base", L"br", L"col", L"embed", L"hr", L"img", L"input",
        L"link", L"meta", L"param", L"source", L"track", L"wbr"};
    for (const auto* candidate : tags) if (tag == candidate) return true;
    return false;
}

bool IsMetadataElement(const std::shared_ptr<Node>& node) {
    if (!node || node->type != NodeType::Element) return false;
    static const wchar_t* tags[] = {
        L"base", L"basefont", L"bgsound", L"link", L"meta", L"noframes",
        L"style", L"template", L"title"};
    for (const auto* candidate : tags) if (node->tag == candidate) return true;
    return false;
}

bool IsWhitespaceText(const std::shared_ptr<Node>& node) {
    return node && node->type == NodeType::Text && Trim(node->text).empty();
}

bool IsWithinScope(const std::shared_ptr<Node>& node,
                   const std::shared_ptr<Node>& scope,
                   bool includeScope = true) {
    if (!node) return false;
    if (!scope) return true;
    for (auto current = includeScope ? node : node->parent.lock(); current;
         current = current->parent.lock())
        if (current == scope) return true;
    return false;
}

bool DocumentOrderLess(const std::shared_ptr<Node>& left,
                       const std::shared_ptr<Node>& right) {
    if (left == right) return false;
    std::vector<const Node*> leftPath, rightPath;
    for (auto current = left; current; current = current->parent.lock())
        leftPath.push_back(current.get());
    for (auto current = right; current; current = current->parent.lock())
        rightPath.push_back(current.get());
    std::reverse(leftPath.begin(), leftPath.end());
    std::reverse(rightPath.begin(), rightPath.end());
    if (leftPath.empty() || rightPath.empty() || leftPath.front() != rightPath.front())
        return left.get() < right.get();
    size_t index = 0;
    while (index < leftPath.size() && index < rightPath.size() &&
           leftPath[index] == rightPath[index]) ++index;
    if (index == leftPath.size()) return true;
    if (index == rightPath.size()) return false;
    const auto* parent = leftPath[index - 1];
    for (const auto& child : parent->children) {
        if (child.get() == leftPath[index]) return true;
        if (child.get() == rightPath[index]) return false;
    }
    return left.get() < right.get();
}

template<typename Predicate>
std::vector<std::shared_ptr<Node>> OrderedIndexedNodes(
    const std::vector<std::shared_ptr<Node>>& indexed,
    const std::shared_ptr<Node>& scope, bool includeScope, Predicate&& matches) {
    std::vector<std::shared_ptr<Node>> result;
    result.reserve(indexed.size());
    for (const auto& node : indexed)
        if (IsWithinScope(node, scope, includeScope) && matches(node)) result.push_back(node);
    std::sort(result.begin(), result.end(), DocumentOrderLess);
    return result;
}

std::shared_ptr<Node> MakeElement(const wchar_t* tag,
                                  const std::shared_ptr<Node>& parent) {
    auto node = std::make_shared<Node>();
    node->type = NodeType::Element;
    node->tag = tag;
    node->parent = parent;
    return node;
}

void AppendChildren(const std::shared_ptr<Node>& parent,
                    std::vector<std::shared_ptr<Node>> children) {
    for (auto& child : children) {
        child->parent = parent;
        parent->children.push_back(std::move(child));
    }
}

// A browser always exposes an html/head/body tree, including for HTML source
// that omits all three optional tags.  Keeping the parser's lightweight token
// handling and normalizing its document result here gives layout the real body
// box (and therefore the user-agent body margin) without making fragment
// parsing invent document-only elements.
void NormalizeDocumentTree(const std::shared_ptr<Node>& document) {
    if (!document || document->type != NodeType::Document) return;

    std::shared_ptr<Node> html;
    for (const auto& child : document->children)
        if (child->type == NodeType::Element && child->tag == L"html") {
            html = child;
            break;
        }

    if (!html) {
        html = MakeElement(L"html", document);
        auto looseChildren = std::move(document->children);
        document->children.clear();
        document->children.push_back(html);
        AppendChildren(html, std::move(looseChildren));
    } else {
        std::vector<std::shared_ptr<Node>> documentSiblings;
        for (auto& child : document->children)
            if (child != html) documentSiblings.push_back(std::move(child));
        document->children.clear();
        html->parent = document;
        document->children.push_back(html);
        AppendChildren(html, std::move(documentSiblings));
    }

    std::shared_ptr<Node> head;
    std::shared_ptr<Node> body;
    std::vector<std::shared_ptr<Node>> looseChildren;
    for (auto& child : html->children) {
        if (!head && child->type == NodeType::Element && child->tag == L"head") {
            head = child;
        } else if (!body && child->type == NodeType::Element && child->tag == L"body") {
            body = child;
        } else {
            looseChildren.push_back(std::move(child));
        }
    }
    html->children.clear();
    if (!head) head = MakeElement(L"head", html);
    if (!body) body = MakeElement(L"body", html);
    head->parent = html;
    body->parent = html;
    html->children.push_back(head);
    html->children.push_back(body);

    bool bodyStarted = !body->children.empty();
    for (auto& child : looseChildren) {
        const bool metadata = IsMetadataElement(child);
        if (!bodyStarted && (metadata || IsWhitespaceText(child))) {
            child->parent = head;
            head->children.push_back(std::move(child));
        } else {
            if (!IsWhitespaceText(child)) bodyStarted = true;
            // Text after </body></html> is reprocessed in the body. Adjacent
            // token text belongs to the body's existing last text node.
            if(child->type==NodeType::Text&&!body->children.empty()&&
               body->children.back()->type==NodeType::Text){
                body->children.back()->text+=child->text;
                continue;
            }
            child->parent = body;
            body->children.push_back(std::move(child));
        }
    }
}

bool IsHeading(const std::wstring& tag) {
    return tag.size() == 2 && tag[0] == L'h' && tag[1] >= L'1' && tag[1] <= L'6';
}

enum HtmlTreeFlags : unsigned {
    TreeSpecial = 1, TreeScope = 2, TreeBlockEnd = 4, TreeClosesParagraph = 8,
    TreeImpliedEnd = 16, TreeSelect = 32, TreeRuby = 64, TreeTemplate = 128,
    TreeOptionalStart = 256, TreeForm = 512, TreeInput = 1024,
    TreeFormatting = 2048, TreeFormattingMarker = 4096, TreeReconstruct = 8192,
    TreeTableStructure = 16384, TreeTableContainer = 32768
};

unsigned HtmlTreeClassification(const std::wstring& tag, const std::wstring& namespaceUri = {}) {
    if (!namespaceUri.empty()) {
        // These foreign integration points are both special and scope barriers.
        if (namespaceUri == L"http://www.w3.org/2000/svg")
            return tag == L"foreignobject" || tag == L"desc" || tag == L"title" ?
                TreeSpecial | TreeScope : 0;
        return 0;
    }
    static const FastMap<std::wstring, unsigned> flags = [] {
        FastMap<std::wstring, unsigned> result;
        for (const auto* tag : {L"address", L"applet", L"area", L"article", L"aside", L"base",
            L"basefont", L"bgsound", L"blockquote", L"body", L"br", L"button", L"caption",
            L"center", L"col", L"colgroup", L"dd", L"details", L"dir", L"div", L"dl", L"dt",
            L"embed", L"fieldset", L"figcaption", L"figure", L"footer", L"form", L"frame",
            L"frameset", L"h1", L"h2", L"h3", L"h4", L"h5", L"h6", L"head", L"header",
            L"hgroup", L"hr", L"html", L"iframe", L"img", L"input", L"keygen", L"li", L"link",
            L"listing", L"main", L"marquee", L"menu", L"meta", L"nav", L"noembed", L"noframes",
            L"noscript", L"object", L"ol", L"p", L"param", L"plaintext", L"pre", L"script",
            L"search", L"section", L"select", L"source", L"style", L"summary", L"table", L"tbody",
            L"td", L"template", L"textarea", L"tfoot", L"th", L"thead", L"title", L"tr", L"track",
            L"ul", L"wbr", L"xmp"}) result[tag] |= TreeSpecial;
        for (const auto* tag : {L"applet", L"caption", L"html", L"table", L"td", L"th",
            L"marquee", L"object", L"select", L"template"}) result[tag] |= TreeScope;
        for (const auto* tag : {L"address", L"article", L"aside", L"blockquote", L"button", L"center",
            L"details", L"dialog", L"dir", L"div", L"dl", L"fieldset", L"figcaption", L"figure",
            L"footer", L"header", L"hgroup", L"listing", L"main", L"menu", L"nav", L"ol",
            L"pre", L"search", L"section", L"select", L"summary", L"ul"}) result[tag] |= TreeBlockEnd;
        for (const auto* tag : {L"address", L"article", L"aside", L"blockquote", L"div", L"dl",
            L"center", L"details", L"dialog", L"dir", L"fieldset", L"figcaption", L"figure", L"footer",
            L"form", L"h1", L"h2", L"h3", L"h4", L"h5", L"h6", L"header", L"hgroup", L"hr",
            L"main", L"menu", L"nav", L"ol", L"p", L"pre", L"listing", L"search", L"section",
            L"summary", L"table", L"ul", L"li", L"dd", L"dt", L"xmp", L"plaintext"}) result[tag] |= TreeClosesParagraph;
        for (const auto* tag : {L"dd", L"dt", L"li", L"optgroup", L"option", L"p",
            L"rb", L"rp", L"rt", L"rtc"}) result[tag] |= TreeImpliedEnd;
        result[L"select"] |= TreeSelect;
        result[L"ruby"] |= TreeRuby;
        result[L"template"] |= TreeTemplate;
        result[L"form"] |= TreeForm;
        result[L"input"] |= TreeInput;
        for (const auto* tag : {L"caption", L"col", L"colgroup", L"tbody", L"td", L"tfoot", L"th", L"thead", L"tr"})
            result[tag] |= TreeTableStructure;
        for (const auto* tag : {L"table", L"tbody", L"tfoot", L"thead", L"tr"}) result[tag] |= TreeTableContainer;
        for (const auto* tag : {L"option", L"optgroup", L"rb", L"rtc", L"rp", L"rt", L"hr"})
            result[tag] |= TreeOptionalStart;
        for (const auto* tag : {L"a", L"b", L"big", L"code", L"em", L"font", L"i", L"nobr",
            L"s", L"small", L"strike", L"strong", L"tt", L"u"}) result[tag] |= TreeFormatting;
        for (const auto* tag : {L"applet", L"marquee", L"object", L"caption", L"td", L"th"})
            result[tag] |= TreeFormattingMarker;
        for (const auto* tag : {L"button", L"applet", L"marquee", L"object", L"area", L"br",
            L"embed", L"img", L"keygen", L"wbr", L"input", L"xmp", L"select", L"option",
            L"optgroup", L"svg", L"math"}) result[tag] |= TreeReconstruct;
        return result;
    }();
    const auto found = flags.find(tag);
    return found == flags.end() ? 0 : found->second;
}

// Prefix state is restored by truncating the stack, or refreshed after removing
// a non-current form. In particular, an absent
// paragraph never requires walking every ancestor of a deeply nested block.
// These are parser-local indices; they cannot become stale after DOM edits.
class HtmlOpenElements {
    struct FormattingEntry {
        std::shared_ptr<Node> node; // A null entry is a scope marker.
        size_t stackIndex = size_t(-1);
    };
public:
    enum class Scope { General, Button, ListItem, Table, Unbounded };
    enum class TableMode { Body, Table, Section, Row, Cell, Caption, Column };
    enum class TableToken { Body, Insert, Ignore, Form };
    explicit HtmlOpenElements(const std::shared_ptr<Node>& root) { push_back(root); }
    void SeedTableFragment(std::wstring_view context) {
        auto& entry = entries_.front();
        if (context == L"table") entry.mode = TableMode::Table;
        else if (context == L"tbody" || context == L"thead" || context == L"tfoot") entry.mode = TableMode::Section;
        else if (context == L"tr") entry.mode = TableMode::Row;
        else if (context == L"caption") entry.mode = TableMode::Caption;
        else if (context == L"colgroup") entry.mode = TableMode::Column;
        // The context element is not an open element in a fragment parse.
        // In particular, td/th contexts reset to in-body at the fake root.
    }
    size_t size() const { return entries_.size(); }
    const std::shared_ptr<Node>& back() const { return entries_.back().node; }
    const std::shared_ptr<Node>& operator[](size_t index) const { return entries_[index].node; }
    void resize(size_t count) { entries_.resize(count); }
    void push_back(const std::shared_ptr<Node>& node) {
        push_back(node, HtmlTreeClassification(node->tag, node->namespaceUri));
    }
    void push_back(const std::shared_ptr<Node>& node, unsigned flags) {
        Entry entry;
        entry.node = node;
        entry.flags = flags;
        const auto index = entries_.size();
        RefreshPrefix(entry, index);
        entries_.push_back(std::move(entry));
        if ((flags & TreeForm) && !HasContext(TreeTemplate)) {
            form_ = node;
            formIndex_ = index;
        }
    }
    bool HasContext(unsigned context) const { return (entries_.back().contexts & context) != 0; }
    bool IgnoreFormStart() const { return form_ && !HasContext(TreeTemplate); }
    bool InTableContext() const { return Mode() != TableMode::Body; }
    bool TableTextTarget() const {
        return (entries_.back().flags & TreeTableContainer) != 0;
    }
    void InsertNode(const std::shared_ptr<Node>& node) {
        if (!foster_ || !TableContainer(back())) {
            node->parent = back(); back()->children.push_back(node); return;
        }
        const auto location = InsertionLocation(back(), foster_);
        node->parent = location.first;
        if (!location.second) location.first->children.push_back(node);
        else {
            auto& children = location.first->children;
            children.insert(std::find(children.begin(), children.end(), location.second), node);
        }
    }
    void InsertText(std::wstring text) {
        if (text.empty()) return;
        if (!foster_ || !TableContainer(back())) {
            auto& children = back()->children;
            if (!children.empty() && children.back()->type == NodeType::Text) { children.back()->text += text; return; }
            auto node = std::make_shared<Node>();
            node->type = NodeType::Text; node->tag = L"#text"; node->text = std::move(text);
            node->parent = back(); children.push_back(std::move(node)); return;
        }
        const auto location = InsertionLocation(back(), foster_);
        auto& children = location.first->children;
        const auto position = location.second ? std::find(children.begin(), children.end(), location.second) : children.end();
        if (position != children.begin() && (*(position - 1))->type == NodeType::Text) {
            (*(position - 1))->text += text;
            return;
        }
        auto node = std::make_shared<Node>();
        node->type = NodeType::Text; node->tag = L"#text"; node->text = std::move(text);
        node->parent = location.first; children.insert(position, std::move(node));
    }
    void Characters(std::wstring text, bool tableBuffer = false) {
        foster_ = false;
        if (Mode() == TableMode::Column && back()->namespaceUri.empty()) {
            const auto first = std::find_if(text.begin(), text.end(), [](wchar_t c) { return !TableSpace(c); });
            InsertText(std::wstring(text.begin(), first));
            if (first == text.end()) return;
            if (back()->tag != L"colgroup") {
                std::wstring whitespace;
                std::copy_if(first,text.end(),std::back_inserter(whitespace),TableSpace);
                InsertText(std::move(whitespace)); return;
            }
            text.erase(text.begin(), first);
            resize(size() - 1);
            tableBuffer = true;
        }
        if (tableBuffer) {
            foster_ = std::any_of(text.begin(), text.end(), [](wchar_t c) { return !TableSpace(c); });
            if (!foster_) { InsertText(std::move(text)); return; }
        } else if (Mode() == TableMode::Table || Mode() == TableMode::Section || Mode() == TableMode::Row) foster_ = true;
        ReconstructFormatting();
        InsertText(std::move(text));
        foster_ = false;
    }
    void SetTableForm(const std::shared_ptr<Node>& node) { form_ = node; formIndex_ = size_t(-1); }
    TableToken PrepareTableToken(const std::wstring& tag, bool end, const Node* token = nullptr) {
        foster_ = false;
        // The mode and table-scope boundary are prefix state: well-formed
        // documents do not scan their ancestors for every text/start token.
        for (;;) {
            const auto mode = Mode();
            const bool section = tag == L"tbody" || tag == L"thead" || tag == L"tfoot";
            const bool cell = tag == L"td" || tag == L"th";
            const bool structure = section || cell || tag == L"tr" || tag == L"caption" ||
                tag == L"col" || tag == L"colgroup";
            if (mode != TableMode::Body && (mode != TableMode::Caption || entries_.back().modeIndex) && HasContext(TreeSelect) &&
                (section || cell || tag == L"tr" || tag == L"table" || tag == L"caption")) {
                if (end && Find(tag, Scope::Table) == size()) return TableToken::Ignore;
                const auto select = Find(L"select", Scope::Unbounded);
                if (select != size()) { resize(select); continue; }
            }
            if (mode == TableMode::Body) return structure ? TableToken::Ignore : TableToken::Body;
            if (mode == TableMode::Cell) {
                if (end && cell) {
                    const auto index = Find(tag, Scope::Table);
                    if (index != size()) { GenerateImpliedEndTags(tag); resize(index); ClearFormattingMarker(); }
                    return TableToken::Ignore;
                }
                if ((!end && structure) || (end && (section || tag == L"tr" || tag == L"table"))) {
                    if (end && Find(tag, Scope::Table) == size()) return TableToken::Ignore;
                    const auto index = entries_.back().modeIndex;
                    GenerateImpliedEndTags(); resize(index); ClearFormattingMarker(); continue;
                }
                if (end && (tag == L"body" || tag == L"caption" || tag == L"col" || tag == L"colgroup" || tag == L"html")) return TableToken::Ignore;
                return TableToken::Body;
            }
            if (mode == TableMode::Caption) {
                if ((end && tag == L"caption") || (!end && structure) || (end && tag == L"table")) {
                    const auto index = Find(L"caption", Scope::Table);
                    if (index == size()) return TableToken::Ignore;
                    GenerateImpliedEndTags(); resize(index); ClearFormattingMarker();
                    if (end && tag == L"caption") return TableToken::Ignore;
                    continue;
                }
                if (end && (section || cell || tag == L"tr" || tag == L"body" || tag == L"col" || tag == L"colgroup" || tag == L"html")) return TableToken::Ignore;
                return TableToken::Body;
            }
            if (mode == TableMode::Column) {
                if (!end && tag == L"col") return TableToken::Insert;
                if (end && tag == L"col") return TableToken::Ignore;
                if (tag == L"template") return TableToken::Body;
                if (back()->tag != L"colgroup") return TableToken::Ignore;
                resize(size() - 1);
                if (end && tag == L"colgroup") return TableToken::Ignore;
                continue;
            }
            if (mode == TableMode::Row) {
                if (!end && cell) { ClearTo(TableMode::Row); return TableToken::Insert; }
                if ((end && tag == L"tr") || (!end && (section || tag == L"tr" || tag == L"caption" || tag == L"col" || tag == L"colgroup")) ||
                    (end && (section || tag == L"table"))) {
                    if ((end && section && Find(tag, Scope::Table) == size()) || Find(L"tr", Scope::Table) == size()) return TableToken::Ignore;
                    ClearTo(TableMode::Row); resize(size() - 1);
                    if (end && tag == L"tr") return TableToken::Ignore;
                    continue;
                }
                if (end && (cell || tag == L"body" || tag == L"caption" || tag == L"col" || tag == L"colgroup" || tag == L"html")) return TableToken::Ignore;
            }
            if (mode == TableMode::Section) {
                if (!end && tag == L"tr") { ClearTo(TableMode::Section); return TableToken::Insert; }
                if (!end && cell) { ClearTo(TableMode::Section); Implicit(L"tr"); continue; }
                if ((end && section) || (!end && (section || tag == L"caption" || tag == L"col" || tag == L"colgroup")) || (end && tag == L"table")) {
                    if (end && section && Find(tag, Scope::Table) == size()) return TableToken::Ignore;
                    if (!entries_.back().modeIndex) return TableToken::Ignore;
                    ClearTo(TableMode::Section); resize(size() - 1);
                    if (end && section) return TableToken::Ignore;
                    continue;
                }
                if (end && (cell || tag == L"tr" || tag == L"body" || tag == L"caption" || tag == L"col" || tag == L"colgroup" || tag == L"html")) return TableToken::Ignore;
            }
            // Row/section fallback uses the same in-table rules, preserving
            // its mode for ordinary fostered children still on the stack.
            if (!end && (tag == L"caption" || tag == L"colgroup" || section)) {
                ClearTo(TableMode::Table); return TableToken::Insert;
            }
            if (!end && tag == L"col") { ClearTo(TableMode::Table); Implicit(L"colgroup"); continue; }
            if (!end && (cell || tag == L"tr")) { ClearTo(TableMode::Table); Implicit(L"tbody"); continue; }
            if (tag == L"table") {
                const auto index = Find(tag, Scope::Table);
                if (index == size()) return TableToken::Ignore;
                resize(index);
                if (end) return TableToken::Ignore;
                continue;
            }
            if (end && (structure || tag == L"body" || tag == L"html")) return TableToken::Ignore;
            if (!end && (tag == L"style" || tag == L"script" || tag == L"template")) return TableToken::Insert;
            if (!end && tag == L"input" && token && ToLower(token->Attribute(L"type")) == L"hidden") return TableToken::Insert;
            if (!end && tag == L"form") return IgnoreFormStart() || HasContext(TreeTemplate) ? TableToken::Ignore : TableToken::Form;
            foster_ = true;
            return TableToken::Body;
        }
    }
    void ReconstructFormatting() {
        if (formatting_.empty() || !formatting_.back().node || OnStack(formatting_.back())) return;
        size_t first = formatting_.size() - 1;
        while (first && formatting_[first - 1].node && !OnStack(formatting_[first - 1])) --first;
        for (size_t index = first; index < formatting_.size(); ++index) {
            auto node = CloneFormatting(formatting_[index].node);
            InsertNode(node);
            formatting_[index] = {node, size()};
            push_back(node);
        }
    }
    void AddFormatting(const std::shared_ptr<Node>& node) {
        size_t matches = 0, earliest = formatting_.size();
        // Attribute order is immaterial. Compare literal token attributes, not
        // computed styles, so distinct markup is never collapsed by Noah's Ark.
        for (size_t count = formatting_.size(); count; --count) {
            const auto& candidate = formatting_[count - 1].node;
            if (!candidate) break;
            if (candidate->tag == node->tag && candidate->namespaceUri == node->namespaceUri &&
                candidate->attributes.size() == node->attributes.size() &&
                std::all_of(node->attributes.begin(), node->attributes.end(), [&](const auto& attribute) {
                    const auto found = candidate->attributes.find(attribute.first);
                    return found != candidate->attributes.end() && found->second == attribute.second;
                })) {
                ++matches; earliest = count - 1;
            }
        }
        if (matches >= 3) formatting_.erase(formatting_.begin() + earliest);
        formatting_.push_back({node, size() - 1});
    }
    void AddFormattingMarker() { formatting_.push_back({}); }
    void ClearFormattingMarker() {
        while (!formatting_.empty()) {
            const bool marker = !formatting_.back().node;
            formatting_.pop_back();
            if (marker) return;
        }
    }
    void PrepareFormattingStart(const std::wstring& tag) {
        if (tag == L"a") {
            const auto index = FindFormatting(tag);
            if (index != formatting_.size()) {
                auto previous = formatting_[index].node;
                AdoptFormatting(tag);
                const auto oldList = FindFormattingNode(previous);
                if (oldList != formatting_.size()) formatting_.erase(formatting_.begin() + oldList);
                const auto oldStack = IndexOf(previous);
                if (oldStack != size()) Erase(oldStack);
            }
        }
        ReconstructFormatting();
        if (tag == L"nobr" && Find(tag, Scope::General) != size()) {
            AdoptFormatting(tag);
            ReconstructFormatting();
        }
    }
    void GenerateImpliedEndTags(std::wstring_view except = {}) {
        while (size() > 1 && (entries_.back().flags & TreeImpliedEnd) && back()->tag != except)
            resize(size() - 1);
    }
    void CloseForm() {
        if (HasContext(TreeTemplate)) {
            const auto index = Find(L"form", Scope::General);
            if (index != size()) resize(index);
            return;
        }
        // The pointer survives other stack pops. A failed end token clears it
        // too, allowing a later form start after a scope barrier.
        auto node = std::move(form_);
        const auto index = formIndex_;
        formIndex_ = 0;
        if (!node || index >= size() || entries_[index].node != node ||
            index < entries_.back().general) return;
        GenerateImpliedEndTags();
        Erase(index);
    }
    bool PrepareSelectStart() {
        if (!HasContext(TreeSelect)) return true;
        const auto index = Find(L"select", Scope::General);
        if (index != size()) resize(index);
        return false;
    }
    void PrepareOptionalStart(const std::wstring& tag) {
        if (tag == L"option" || tag == L"optgroup") {
            if (HasContext(TreeSelect)) GenerateImpliedEndTags(tag == L"option" ? L"optgroup" : L"");
            else if (back()->namespaceUri.empty() && back()->tag == L"option") resize(size() - 1);
        } else if ((tag == L"rb" || tag == L"rtc" || tag == L"rp" || tag == L"rt") && HasContext(TreeRuby)) {
            GenerateImpliedEndTags(tag == L"rp" || tag == L"rt" ? L"rtc" : L"");
        } else if (tag == L"hr" && HasContext(TreeSelect)) GenerateImpliedEndTags();
    }
    bool CloseParagraph() {
        const auto index = entries_.back().paragraph;
        if (!index || index < entries_.back().button) return false;
        resize(index);
        return true;
    }
    size_t Find(const std::wstring& tag, Scope scope, bool heading = false) const {
        const auto& top = entries_.back();
        const size_t barrier = scope == Scope::Button ? top.button :
            scope == Scope::ListItem ? top.listItem : scope == Scope::General ? top.general : scope == Scope::Table ? top.tableScope : 0;
        for (size_t count = size(); count > 1; --count) {
            const size_t index = count - 1;
            if (index < barrier) break;
            const auto& node = entries_[index].node;
            if ((scope == Scope::Unbounded || node->namespaceUri.empty()) &&
                (heading ? IsHeading(node->tag) : node->tag == tag)) return index;
        }
        return size();
    }
    void CloseListItem(bool definition) {
        for (size_t count = size(); count > 1; --count) {
            const auto& entry = entries_[count - 1];
            const auto& node = entry.node;
            const auto& tag = node->tag;
            if (node->namespaceUri.empty() && (definition ? tag == L"dt" || tag == L"dd" : tag == L"li")) {
                resize(count - 1);
                return;
            }
            if ((entry.flags & TreeSpecial) &&
                !(node->namespaceUri.empty() && (tag == L"address" || tag == L"div" || tag == L"p"))) return;
        }
    }
    void CloseEndTag(const std::wstring& tag, bool htmlDispatch = false, bool ordinary = false) {
        const auto& current = back();
        // The current node and most recent formatting entry are the common
        // well-formed path. Neither classification nor an ancestor scan is
        // needed, while expired Noah entries still take the ordinary fallback.
        if (size() > 1 && current->namespaceUri.empty() && current->tag == tag && tag != L"form") {
            const auto flags = entries_.back().flags;
            if (!(flags & (TreeFormatting | TreeFormattingMarker))) { resize(size() - 1); return; }
            if ((flags & TreeFormatting) && !formatting_.empty() && formatting_.back().node == current) {
                resize(size() - 1); formatting_.pop_back(); return;
            }
            if (flags & TreeFormattingMarker) { resize(size() - 1); ClearFormattingMarker(); return; }
        }
        if (!ordinary && (htmlDispatch || current->namespaceUri.empty()) &&
            (HtmlTreeClassification(tag) & TreeFormatting)) {
            if (!AdoptFormatting(tag)) CloseEndTag(tag, htmlDispatch, true);
            return;
        }
        if ((htmlDispatch || current->namespaceUri.empty()) &&
            (tag == L"applet" || tag == L"marquee" || tag == L"object" ||
             tag == L"caption" || tag == L"td" || tag == L"th")) {
            const auto index = Find(tag, Scope::General);
            if (index != size()) { resize(index); ClearFormattingMarker(); }
            return;
        }
        // An ordinary fallback may encounter a formatting node that Noah's
        // Ark already removed. Forms keep their independent pointer lifetime.
        if (size() > 1 && current->namespaceUri.empty() && current->tag == tag && tag != L"form") {
            resize(size() - 1);
            return;
        }
        // Integration points reprocess HTML start/character tokens, but their
        // own end tags still use foreign-content handling. Otherwise a title
        // or desc can swallow following SVG siblings and the document tail.
        const bool foreign = !htmlDispatch && !current->namespaceUri.empty();
        if (foreign) {
            // A foreign end token falls back to HTML dispatch upon reaching
            // an HTML ancestor. It must retain integration-point scope guards.
            for (size_t count = size(); count > 1; --count) {
                const auto& node = entries_[count - 1].node;
                if (node->namespaceUri.empty()) { CloseEndTag(tag, true); return; }
                if (node->tag == tag) { resize(count - 1); return; }
            }
        } else if (tag == L"html" || tag == L"head" || tag == L"body" ||
            tag == L"table" || tag == L"tbody" || tag == L"thead" || tag == L"tfoot" ||
            tag == L"tr" || tag == L"td" || tag == L"th" || tag == L"template") {
            // Other insertion modes retain their existing handling. This
            // bounded in-body improvement is not a full tree builder.
            const auto index = Find(tag, Scope::Unbounded);
            if (index != size()) resize(index);
        } else if (tag == L"form") {
            CloseForm();
        } else if (tag == L"p") {
            if (!CloseParagraph()) {
                // A stray </p> inserts and immediately closes an empty p.
                auto node = MakeElement(L"p", back());
                back()->children.push_back(std::move(node));
            }
        } else if (tag == L"br") {
            ReconstructFormatting();
            back()->children.push_back(MakeElement(L"br", back()));
        } else {
            const bool scoped = (HtmlTreeClassification(tag) & TreeBlockEnd) ||
                tag == L"li" || tag == L"dd" || tag == L"dt" || IsHeading(tag);
            if (scoped) {
                const auto index = Find(tag, tag == L"li" ? Scope::ListItem : Scope::General, IsHeading(tag));
                if (index != size()) resize(index);
            } else {
                // Ordinary end tags cannot jump across a special HTML node.
                for (size_t count = size(); count > 1; --count) {
                    const auto& entry = entries_[count - 1];
                    if (entry.node->namespaceUri.empty() && entry.node->tag == tag) { resize(count - 1); return; }
                    if (entry.flags & TreeSpecial) return;
                }
            }
        }
    }
private:
    static bool TableSpace(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L'\f'; }
    TableMode Mode() const { return entries_.back().mode; }
    static bool TableContainer(const std::shared_ptr<Node>& node) {
        return node->namespaceUri.empty() && (node->tag == L"table" || node->tag == L"tbody" ||
            node->tag == L"thead" || node->tag == L"tfoot" || node->tag == L"tr");
    }
    std::pair<std::shared_ptr<Node>, std::shared_ptr<Node>> InsertionLocation(
        const std::shared_ptr<Node>& target, bool foster) const {
        if (!foster || !TableContainer(target)) return {target, {}};
        const auto index = entries_.back().tableScope;
        if (index) {
            const auto table = entries_[index].node;
            if (table->tag != L"table") return {target, {}};
            if (auto parent = table->parent.lock()) return {parent, table};
            return {entries_[index - 1].node, {}};
        }
        return {entries_.front().node, {}};
    }
    void ClearTo(TableMode mode) {
        if (Mode() == mode) { resize(size_t(entries_.back().modeIndex) + 1); return; }
        for (size_t count = size(); count > 1; --count) {
            const auto& entry = entries_[count - 1];
            if (entry.modeIndex == count - 1 && entry.mode == mode) { resize(count); return; }
            if (entry.node->namespaceUri.empty() && (entry.node->tag == L"html" || entry.node->tag == L"template")) { resize(count); return; }
        }
    }
    void Implicit(const wchar_t* tag) {
        auto node = MakeElement(tag, back());
        InsertNode(node); push_back(node);
    }
    bool OnStack(const FormattingEntry& entry) const {
        return entry.stackIndex < size() && entries_[entry.stackIndex].node == entry.node;
    }
    size_t IndexOf(const std::shared_ptr<Node>& node) const {
        for (size_t count = size(); count; --count)
            if (entries_[count - 1].node == node) return count - 1;
        return size();
    }
    size_t FindFormatting(const std::wstring& tag) const {
        for (size_t count = formatting_.size(); count; --count) {
            const auto& node = formatting_[count - 1].node;
            if (!node) break;
            if (node->tag == tag) return count - 1;
        }
        return formatting_.size();
    }
    size_t FindFormattingNode(const std::shared_ptr<Node>& node) const {
        for (size_t count = formatting_.size(); count; --count)
            if (formatting_[count - 1].node == node) return count - 1;
        return formatting_.size();
    }
    static std::shared_ptr<Node> CloneFormatting(const std::shared_ptr<Node>& original) {
        auto node = std::make_shared<Node>();
        node->tag = original->tag;
        node->namespaceUri = original->namespaceUri;
        node->qualifiedName = original->qualifiedName;
        node->attributes = original->attributes;
        node->inlineStyle = original->inlineStyle;
        node->inlineStylePriority = original->inlineStylePriority;
        node->inlineStyleOrder = original->inlineStyleOrder;
        node->cryptographicNonce = original->cryptographicNonce;
        node->checked = original->checked;
        node->disabled = original->disabled;
        return node;
    }
    static void DetachNode(const std::shared_ptr<Node>& node) {
        if (auto parent = node->parent.lock()) {
            auto& children = parent->children;
            children.erase(std::remove(children.begin(), children.end(), node), children.end());
        }
        node->parent.reset();
    }
    static void AppendNode(const std::shared_ptr<Node>& parent, const std::shared_ptr<Node>& node) {
        DetachNode(node);
        node->parent = parent;
        parent->children.push_back(node);
    }
    void Erase(size_t index) {
        entries_.erase(entries_.begin() + index);
        for (auto& entry : formatting_) {
            if (entry.stackIndex == index) entry.stackIndex = size_t(-1);
            else if (entry.stackIndex != size_t(-1) && entry.stackIndex > index) --entry.stackIndex;
        }
        if (formIndex_ == index) formIndex_ = size_t(-1);
        else if (formIndex_ != size_t(-1) && formIndex_ > index) --formIndex_;
        for (size_t cursor = index; cursor < size(); ++cursor) RefreshPrefix(entries_[cursor], cursor);
    }
    void Insert(size_t index, const std::shared_ptr<Node>& node) {
        Entry entry; entry.node = node; entry.flags = HtmlTreeClassification(node->tag, node->namespaceUri);
        entries_.insert(entries_.begin() + index, std::move(entry));
        for (auto& formatting : formatting_)
            if (formatting.stackIndex != size_t(-1) && formatting.stackIndex >= index) ++formatting.stackIndex;
        if (formIndex_ != size_t(-1) && formIndex_ >= index) ++formIndex_;
        for (size_t cursor = index; cursor < size(); ++cursor) RefreshPrefix(entries_[cursor], cursor);
    }
    bool AdoptFormatting(const std::wstring& tag) {
        // WHATWG adoption agency: bounded outer repair, three-node inner
        // formatting retention, bookmark and exact DOM/stack reparenting.
        if (size() > 1 && back()->namespaceUri.empty() && back()->tag == tag &&
            FindFormattingNode(back()) == formatting_.size()) { resize(size() - 1); return true; }
        for (unsigned outer = 0; outer < 8; ++outer) {
            const size_t listIndex = FindFormatting(tag);
            if (listIndex == formatting_.size()) return false;
            const auto formattingNode = formatting_[listIndex].node;
            if (!OnStack(formatting_[listIndex])) {
                formatting_.erase(formatting_.begin() + listIndex); return true;
            }
            const size_t stackIndex = formatting_[listIndex].stackIndex;
            if (stackIndex < entries_.back().general) return true;
            size_t blockIndex = stackIndex + 1;
            while (blockIndex < size() && !(entries_[blockIndex].flags & TreeSpecial)) ++blockIndex;
            if (blockIndex == size()) {
                resize(stackIndex);
                formatting_.erase(formatting_.begin() + listIndex); return true;
            }
            const auto furthestBlock = entries_[blockIndex].node;
            const auto commonAncestor = entries_[stackIndex - 1].node;
            size_t bookmark = listIndex;
            auto lastNode = furthestBlock;
            size_t cursor = blockIndex;
            for (unsigned inner = 1; ; ++inner) {
                --cursor;
                const auto oldNode = entries_[cursor].node;
                if (oldNode == formattingNode) break;
                size_t activeIndex = FindFormattingNode(oldNode);
                if (inner > 3 && activeIndex != formatting_.size()) {
                    if (activeIndex < bookmark) --bookmark;
                    formatting_.erase(formatting_.begin() + activeIndex);
                    activeIndex = formatting_.size();
                }
                if (activeIndex == formatting_.size()) { Erase(cursor); continue; }
                auto replacement = CloneFormatting(oldNode);
                formatting_[activeIndex] = {replacement, cursor};
                entries_[cursor].node = replacement;
                if (lastNode == furthestBlock) bookmark = activeIndex + 1;
                AppendNode(replacement, lastNode);
                lastNode = std::move(replacement);
            }
            // Table common ancestors use the adjusted location even though
            // the remaining table insertion modes are implemented separately.
            const auto location = InsertionLocation(commonAncestor, true);
            const auto& target = location.first;
            const auto& before = location.second;
            if (!IsWithinScope(target, lastNode)) {
                DetachNode(lastNode); lastNode->parent = target;
                const auto position = std::find(target->children.begin(), target->children.end(), before);
                target->children.insert(position, lastNode);
            }
            auto replacement = CloneFormatting(formattingNode);
            AppendChildren(replacement, std::move(furthestBlock->children));
            furthestBlock->children.clear();
            AppendNode(furthestBlock, replacement);
            const size_t oldIndex = FindFormattingNode(formattingNode);
            if (oldIndex < bookmark) --bookmark;
            formatting_.erase(formatting_.begin() + oldIndex);
            const size_t oldStack = IndexOf(formattingNode);
            Erase(oldStack);
            const size_t newStack = IndexOf(furthestBlock) + 1;
            Insert(newStack, replacement);
            formatting_.insert(formatting_.begin() + bookmark, {replacement, newStack});
        }
        return true;
    }
    struct Entry {
        std::shared_ptr<Node> node;
        unsigned paragraph = 0, general = 0, button = 0, listItem = 0;
        // Table prefixes stay compact even in documents without tables. The
        // nearest table-scope barrier already identifies the foster table.
        unsigned tableScope = 0, modeIndex = 0;
        TableMode mode = TableMode::Body;
        unsigned flags = 0, contexts = 0;
    };
    void RefreshPrefix(Entry& entry, size_t index) {
        if (index > (std::numeric_limits<unsigned>::max)())
            throw std::length_error("HTML open-elements index exceeds its compact representation");
        const unsigned compactIndex = static_cast<unsigned>(index);
        if (index) {
            const auto& previous = entries_[index - 1];
            entry.paragraph = previous.paragraph;
            entry.general = previous.general;
            entry.button = previous.button;
            entry.listItem = previous.listItem;
            entry.contexts = previous.contexts;
            entry.tableScope = previous.tableScope;
            entry.mode = previous.mode; entry.modeIndex = previous.modeIndex;
        } else {
            entry.paragraph = entry.general = entry.button = entry.listItem = entry.contexts = 0;
            entry.tableScope = entry.modeIndex = 0; entry.mode = TableMode::Body;
        }
        if (entry.flags & TreeScope) {
            entry.general = entry.button = entry.listItem = compactIndex;
            entry.contexts &= ~(TreeSelect | TreeRuby);
        }
        entry.contexts |= entry.flags & (TreeSelect | TreeRuby | TreeTemplate);
        if (entry.node->namespaceUri.empty()) {
            const auto& tag = entry.node->tag;
            if ((entry.flags & TreeScope) && (tag == L"html" || tag == L"table" || tag == L"template")) entry.tableScope = compactIndex;
            if (entry.flags & (TreeTableStructure | TreeTableContainer)) {
                if (tag == L"table") { entry.mode = TableMode::Table; entry.modeIndex = compactIndex; }
                else if (tag == L"tbody" || tag == L"thead" || tag == L"tfoot") { entry.mode = TableMode::Section; entry.modeIndex = compactIndex; }
                else if (tag == L"tr") { entry.mode = TableMode::Row; entry.modeIndex = compactIndex; }
                else if (tag == L"td" || tag == L"th") { entry.mode = TableMode::Cell; entry.modeIndex = compactIndex; }
                else if (tag == L"caption") { entry.mode = TableMode::Caption; entry.modeIndex = compactIndex; }
                else if (tag == L"colgroup") { entry.mode = TableMode::Column; entry.modeIndex = compactIndex; }
            }
            if (entry.node->tag == L"p") entry.paragraph = compactIndex;
            if (entry.node->tag == L"button") entry.button = compactIndex;
            if (entry.node->tag == L"ol" || entry.node->tag == L"ul") entry.listItem = compactIndex;
        }
    }
    std::vector<Entry> entries_;
    std::vector<FormattingEntry> formatting_;
    std::shared_ptr<Node> form_;
    size_t formIndex_ = 0;
    bool foster_ = false;
};

void ParseStyleAttribute(const std::wstring& source,
                         FastMap<std::wstring, std::wstring>& out,
                         FastMap<std::wstring, std::wstring>* priorities = nullptr,
                         FastMap<std::wstring, size_t>* order = nullptr) {
    size_t declarationOrder = 0;
    for(const auto& declaration:CssSyntax::Declarations(source)){
        ++declarationOrder;
        if(priorities&&priorities->count(declaration.name)&&!declaration.important)continue;
        out[declaration.name]=declaration.value;
        if(order)(*order)[declaration.name]=declarationOrder;
        if(priorities){
            if(declaration.important)(*priorities)[declaration.name]=L"important";
            else priorities->erase(declaration.name);
        }
    }
}

bool MatchPlainSimple(const std::shared_ptr<Node>& node,std::wstring_view selector) {
    while(!selector.empty()&&IsSpace(selector.front()))selector.remove_prefix(1);
    while(!selector.empty()&&IsSpace(selector.back()))selector.remove_suffix(1);
    if(selector.empty()||selector==L"*")return true;
    const auto identifier=[](wchar_t character){
        return std::iswalnum(character)||character==L'-'||character==L'_';
    };
    const auto equalsInsensitive=[](const std::wstring& value,std::wstring_view expected){
        return value.size()==expected.size()&&std::equal(value.begin(),value.end(),expected.begin(),
            [](wchar_t left,wchar_t right){return std::towlower(left)==std::towlower(right);});
    };
    size_t index=0;
    if(std::iswalpha(selector[index])||selector[index]==L'_'){
        const size_t begin=index++;
        while(index<selector.size()&&identifier(selector[index]))++index;
        if(!equalsInsensitive(node->tag,selector.substr(begin,index-begin)))return false;
    }else if(selector[index]==L'*')++index;
    while(index<selector.size()){
        if(selector[index]!=L'#'&&selector[index]!=L'.')return false;
        const wchar_t kind=selector[index++];const size_t begin=index;
        while(index<selector.size()&&identifier(selector[index]))++index;
        const auto token=selector.substr(begin,index-begin);
        if(token.empty())return false;
        if(kind==L'#'){
            const auto found=node->attributes.find(L"id");
            if(found==node->attributes.end()||found->second.size()!=token.size()||
               found->second.compare(0,token.size(),token.data(),token.size())!=0)return false;
        }else{
            const auto found=node->attributes.find(L"class");
            if(found==node->attributes.end())return false;
            const auto& classes=found->second;size_t position=0;bool matched=false;
            while(position<classes.size()){
                while(position<classes.size()&&IsSpace(classes[position]))++position;
                const size_t start=position;
                while(position<classes.size()&&!IsSpace(classes[position]))++position;
                if(position-start==token.size()&&
                   classes.compare(start,token.size(),token.data(),token.size())==0){matched=true;break;}
            }
            if(!matched)return false;
        }
    }
    return true;
}

#include "CSSSelectors.inl"

std::vector<std::wstring> SplitSelector(std::wstring selector) {
    std::vector<std::wstring> result;
    size_t start=0;
    while(start<selector.size()){
        while(start<selector.size()&&CssSyntax::Space(selector[start]))++start;
        if(start==selector.size())break;
        const auto c=selector[start];
        if(c==L'>'||c==L'+'||c==L'~'){result.emplace_back(1,c);++start;continue;}
        auto end=CssSyntax::Find(selector,L" >+~\t\r\n\f",start);
        if(end==std::wstring::npos)end=selector.size();
        result.push_back(selector.substr(start,end-start));start=end;
    }
    return result;
}
void Walk(const std::shared_ptr<Node>& node,
          const std::function<void(const std::shared_ptr<Node>&)>& fn) {
    if (!node) return;
    fn(node);
    for (const auto& child : node->children) Walk(child, fn);
}

bool WalkUntil(const std::shared_ptr<Node>& node,
               const std::function<bool(const std::shared_ptr<Node>&)>& fn) {
    if (!node) return false;
    if (fn(node)) return true;
    for (const auto& child : node->children)
        if (WalkUntil(child, fn)) return true;
    return false;
}

struct CompiledQuerySelector {
    enum class ScopeMode { None, ScopeOnly, DirectChild, Descendant };
    enum class IndexKind { None, Id, Class, Tag };
    ScopeMode scopeMode = ScopeMode::None;
    IndexKind indexKind = IndexKind::None;
    std::wstring indexKey;
    std::vector<std::wstring> parts;
};

void CompileQueryIndexKey(CompiledQuerySelector& selector) {
    if (selector.parts.empty()) return;
    const auto& simple = selector.parts.back();
    if(simple.find(L'\\')!=std::wstring::npos)return;
    std::wstring firstClass;
    int brackets = 0, parentheses = 0;
    for (size_t index = 0; index < simple.size();) {
        const wchar_t character = simple[index];
        if (character == L'[') { ++brackets; ++index; continue; }
        if (character == L']') { brackets = std::max(0, brackets - 1); ++index; continue; }
        if (character == L'(') { ++parentheses; ++index; continue; }
        if (character == L')') { parentheses = std::max(0, parentheses - 1); ++index; continue; }
        if ((character == L'#' || character == L'.') && brackets == 0 && parentheses == 0) {
            const wchar_t kind = character;
            const size_t start = ++index;
            while (index < simple.size() && (std::iswalnum(simple[index]) ||
                   simple[index] == L'-' || simple[index] == L'_')) ++index;
            if (start == index) continue;
            const auto key = simple.substr(start, index - start);
            if (kind == L'#') {
                selector.indexKind = CompiledQuerySelector::IndexKind::Id;
                selector.indexKey = key;
                return;
            }
            if (firstClass.empty()) firstClass = key;
            continue;
        }
        ++index;
    }
    if (!firstClass.empty()) {
        selector.indexKind = CompiledQuerySelector::IndexKind::Class;
        selector.indexKey = std::move(firstClass);
        return;
    }
    size_t end = 0;
    if (!simple.empty() && (std::iswalpha(simple.front()) || simple.front() == L'_')) {
        end = 1;
        while (end < simple.size() && (std::iswalnum(simple[end]) ||
               simple[end] == L'-' || simple[end] == L'_')) ++end;
    }
    if (end) {
        selector.indexKind = CompiledQuerySelector::IndexKind::Tag;
        selector.indexKey = ToLower(simple.substr(0, end));
    }
}

std::vector<CompiledQuerySelector> CompileQuerySelectors(const std::wstring& selector,
                                                         bool hasScope) {
    std::vector<CompiledQuerySelector> result;
    for (const auto& item : CssSyntax::Split(CssSyntax::Comments(selector),L',')) {
        CompiledQuerySelector compiled;
        if (hasScope && item.rfind(L":scope", 0) == 0) {
            auto remainder = Trim(item.substr(6));
            if (remainder.empty()) compiled.scopeMode = CompiledQuerySelector::ScopeMode::ScopeOnly;
            else if (remainder.front() == L'>') {
                compiled.scopeMode = CompiledQuerySelector::ScopeMode::DirectChild;
                remainder = Trim(remainder.substr(1));
                compiled.parts = SplitSelector(remainder);
            } else {
                compiled.scopeMode = CompiledQuerySelector::ScopeMode::Descendant;
                compiled.parts = SplitSelector(remainder);
            }
        } else {
            compiled.parts = SplitSelector(item);
        }
        CompileQueryIndexKey(compiled);
        result.push_back(std::move(compiled));
    }
    return result;
}

bool MatchesQuerySelector(const std::shared_ptr<Node>& node,
                          const std::shared_ptr<Node>& scope,
                          const CompiledQuerySelector& selector) {
    using ScopeMode = CompiledQuerySelector::ScopeMode;
    if (selector.scopeMode == ScopeMode::ScopeOnly) return node == scope;
    if (selector.scopeMode == ScopeMode::DirectChild)
        return node->parent.lock() == scope && Document::MatchesSelector(node, selector.parts);
    if (selector.scopeMode == ScopeMode::Descendant)
        return node != scope && Document::MatchesSelector(node, selector.parts);
    return Document::MatchesSelector(node, selector.parts);
}

void AppendInnerText(const Node& node, std::wstring& output) {
    if (node.type == NodeType::Text || node.type == NodeType::CData) { output += node.text; return; }
    if (node.type == NodeType::Comment) return;
    if (node.tag == L"br") { output += L'\n'; return; }
    for (const auto& child : node.children) AppendInnerText(*child, output);
}

// Preprocess the input stream before tokenization, including attributes,
// comments and raw text. Character references are decoded afterwards so
// &#13; remains a carriage return. Most application input needs no copy.
std::wstring HtmlInputWithNormalizedLineEndings(const std::wstring& html) {
    const auto first = html.find(L'\r');
    if (first == std::wstring::npos) return {};
    std::wstring normalized;
    normalized.reserve(html.size());
    normalized.append(html, 0, first);
    for (size_t index = first; index < html.size(); ++index) {
        if (html[index] == L'\r') {
            normalized += L'\n';
            if (index + 1 < html.size() && html[index + 1] == L'\n') ++index;
        } else normalized += html[index];
    }
    return normalized;
}

class HtmlParser {
public:
    explicit HtmlParser(const std::wstring& html,bool scriptingEnabled)
        : normalizedHtml_(HtmlInputWithNormalizedLineEndings(html)),
          html_(normalizedHtml_.empty() ? html : normalizedHtml_),scriptingEnabled_(scriptingEnabled) {}
    bool QuirksMode() const {return quirksMode_;}
    bool LimitedQuirksMode() const {return limitedQuirksMode_;}

    std::shared_ptr<Node> Parse(bool fragment, std::wstring* error, std::wstring_view fragmentContext = {}) {
        auto root = std::make_shared<Node>();
        root->type = fragment ? NodeType::Element : NodeType::Document;
        root->tag = fragment ? L"fragment" : L"#document";
        HtmlOpenElements stack(root);
        if (fragment) stack.SeedTableFragment(fragmentContext);
        std::wstring tableText;
        const auto flushTableText = [&]() {
            if (!tableText.empty()) { stack.Characters(std::move(tableText), true); tableText.clear(); }
        };
        const auto characters = [&](std::wstring text) {
            if (stack.TableTextTarget()) tableText += text;
            else {
                flushTableText();
                if (!stack.InTableContext()) {
                    if (!ForeignDataContext(stack.back())) stack.ReconstructFormatting();
                    AppendText(stack.back(), std::move(text));
                } else if (ForeignDataContext(stack.back())) AppendText(stack.back(), std::move(text));
                else stack.Characters(std::move(text));
            }
        };

        while (position_ < html_.size()) {
            // Only the token immediately following pre/listing can lose its
            // first LF. A comment or child element cancels the convenience.
            const bool ignoreLeadingLf = ignoreLeadingLf_;
            ignoreLeadingLf_ = false;
            if (html_[position_] != L'<') {
                const size_t end = html_.find(L'<', position_);
                auto text = html_.substr(position_, end - position_);
                if (text.find(L'&') != std::wstring::npos) text = DecodeEntities(text);
                FilterDataNulls(stack.back(), text);
                if (ignoreLeadingLf && !text.empty() && text.front() == L'\n')
                    text.erase(text.begin());
                position_ = end == std::wstring::npos ? html_.size() : end;
                if (!text.empty()) {
                    if(std::any_of(text.begin(),text.end(),[](wchar_t c){return !IsSpace(c)&&c!=0xfeff;}))initial_=false;
                    characters(std::move(text));
                }
                continue;
            }
            if (Starts(L"<!--")) {
                flushTableText();
                position_ += 4;
                AppendComment(stack.back(), ReadComment());
                continue;
            }
            if (Starts(L"<!") || Starts(L"<?")) {
                flushTableText();
                if (Starts(L"<![CDATA[") && ForeignDataContext(stack.back())) {
                    position_ += 9;
                    const size_t end = html_.find(L"]]>", position_);
                    auto text = html_.substr(position_, end - position_);
                    FilterDataNulls(stack.back(), text);
                    AppendText(stack.back(), std::move(text));
                    position_ = end == std::wstring::npos ? html_.size() : end + 3;
                    continue;
                }
                const bool doctype = AsciiStarts(L"<!doctype");
                if (Starts(L"<!") && !doctype) {
                    position_ += 2;
                    AppendComment(stack.back(), ReadBogusComment());
                    continue;
                }
                const size_t end = html_.find(L'>', position_ + 2);
                if(initial_&&!fragment&&doctype){
                    const auto declaration=ToLower(Trim(html_.substr(position_+9,
                        (end==std::wstring::npos?html_.size():end)-position_-9)));
                    quirksMode_=declaration.rfind(L"html",0)!=0||
                        (declaration.size()>4&&!IsSpace(declaration[4]));
                    if(!quirksMode_){
                        size_t token=4;
                        while(token<declaration.size()&&IsSpace(declaration[token]))++token;
                        if(declaration.compare(token,6,L"public")==0&&
                           token+6<declaration.size()&&IsSpace(declaration[token+6])){
                            token+=6;
                            const auto quotedIdentifier=[&](){
                                while(token<declaration.size()&&IsSpace(declaration[token]))++token;
                                if(token>=declaration.size()||
                                   (declaration[token]!=L'\''&&declaration[token]!=L'"'))return std::wstring{};
                                const wchar_t quote=declaration[token++];const size_t begin=token;
                                const size_t finish=declaration.find(quote,token);
                                token=finish==std::wstring::npos?declaration.size():finish+1;
                                return declaration.substr(begin,token-begin-(finish==std::wstring::npos?0:1));
                            };
                            const auto publicId=quotedIdentifier();
                            const auto systemId=quotedIdentifier();
                            const bool xhtmlLegacy=publicId.rfind(L"-//w3c//dtd xhtml 1.0 transitional//",0)==0||
                                publicId.rfind(L"-//w3c//dtd xhtml 1.0 frameset//",0)==0;
                            const bool htmlLegacy=publicId.rfind(L"-//w3c//dtd html 4.01 transitional//",0)==0||
                                publicId.rfind(L"-//w3c//dtd html 4.01 frameset//",0)==0;
                            limitedQuirksMode_=xhtmlLegacy||(htmlLegacy&&!systemId.empty());
                            if(htmlLegacy&&systemId.empty())quirksMode_=true;
                        }
                    }
                    initial_=false;
                }
                position_ = end == std::wstring::npos ? html_.size() : end + 1;
                continue;
            }
            if (Starts(L"</")) {
                position_ += 2;
                if (position_ == html_.size()) {
                    characters(L"</");
                } else if (html_[position_] == L'>') {
                    ++position_;
                    ignoreLeadingLf_ = ignoreLeadingLf;
                } else if (!AsciiAlpha(html_[position_])) {
                    flushTableText();
                    // An invalid end opener becomes a bogus comment, without
                    // skipping whitespace to find a different closing name.
                    AppendComment(stack.back(), ReadBogusComment());
                } else {
                    const auto name = ReadTagName();
                    bool selfClosing = false;
                    // End tokens share attribute/quote states, even though
                    // the tree builder ignores their attributes and slash.
                    if (ReadTagAttributes(position_, nullptr, selfClosing)) {
                        flushTableText();
                        if (ForeignDataContext(stack.back()) ||
                            (!stack.InTableContext() &&
                                ((name.front() != L't' && name.front() != L'c') ||
                                    !(HtmlTreeClassification(name) & TreeTableStructure))) ||
                            stack.PrepareTableToken(name, true) != HtmlOpenElements::TableToken::Ignore)
                            stack.CloseEndTag(name);
                    }
                }
                continue;
            }

            ++position_;
            if (position_ >= html_.size() || !AsciiAlpha(html_[position_])) {
                // In data state an invalid start-tag opener emits '<' and
                // reconsumes the following character. Skipping it loses a
                // valid sibling after sequences such as <<div>.
                characters(L"<");
                initial_ = false;
                continue;
            }
            const auto tag = ReadTagName();
            const bool svg = tag == L"svg" || (stack.back()->namespaceUri == L"http://www.w3.org/2000/svg" &&
                stack.back()->tag != L"foreignobject" && stack.back()->tag != L"desc" && stack.back()->tag != L"title");
            if (!svg && tag == L"form" && stack.IgnoreFormStart()) {
                // Consume the complete token without allocating a discarded
                // element or decoding attributes the tree builder ignores.
                bool selfClosing = false;
                if (ReadTagAttributes(position_, nullptr, selfClosing)) flushTableText();
                continue;
            }
            auto node = std::make_shared<Node>();
            node->type = NodeType::Element;
            node->tag = tag;
            if(svg)
                node->namespaceUri=L"http://www.w3.org/2000/svg";
            // Internal lookup keys stay ASCII lowercase; the DOM's qualified
            // name retains SVG's canonical spelling at this integration point.
            if (svg && tag == L"foreignobject") node->qualifiedName = L"foreignObject";
            bool selfClosing = false;
            if (!ReadTagAttributes(position_, node.get(), selfClosing)) continue;
            flushTableText();
            initial_=false;
            const unsigned treeFlags = HtmlTreeClassification(tag, node->namespaceUri);
            const auto tableToken = (stack.InTableContext() || (treeFlags & TreeTableStructure)) &&
                !ForeignDataContext(stack.back()) ?
                stack.PrepareTableToken(tag, false, node.get()) : HtmlOpenElements::TableToken::Body;
            if (tableToken == HtmlOpenElements::TableToken::Ignore) continue;
            // Apply tree mutations only after the tag token is emitted. EOF
            // inside a pending tag must not close a paragraph or open tbody.
            if (node->namespaceUri.empty() && tableToken == HtmlOpenElements::TableToken::Body) {
                if ((treeFlags & TreeSelect) && !stack.PrepareSelectStart()) continue;
                if (treeFlags & TreeInput) stack.PrepareSelectStart();
                if (tag == L"li" || tag == L"dd" || tag == L"dt") stack.CloseListItem(tag != L"li");
                if ((treeFlags & TreeClosesParagraph) && (tag != L"table" || !quirksMode_)) stack.CloseParagraph();
                if (treeFlags & TreeOptionalStart) stack.PrepareOptionalStart(tag);
                if (IsHeading(tag) && stack.back()->namespaceUri.empty() && IsHeading(stack.back()->tag))
                    stack.resize(stack.size() - 1);
                if (tag == L"button") {
                    const auto index = stack.Find(tag, HtmlOpenElements::Scope::General);
                    if (index != stack.size()) stack.resize(index);
                }
            }
            if (!ForeignDataContext(stack.back()) && tableToken == HtmlOpenElements::TableToken::Body) {
                if (treeFlags & TreeFormatting) stack.PrepareFormattingStart(tag);
                else if ((treeFlags & TreeReconstruct) ||
                    !(treeFlags & (TreeSpecial | TreeClosesParagraph | TreeOptionalStart)) &&
                    (node->namespaceUri.empty() || tag == L"svg")) stack.ReconstructFormatting();
            }
            // HTML non-void elements ignore a trailing slash. Foreign content
            // keeps its explicit self-closing behavior (for example SVG).
            if (node->namespaceUri.empty()) selfClosing = false;
            node->checked = node->attributes.count(L"checked") != 0;
            node->cryptographicNonce = node->Attribute(L"nonce");
            node->disabled = node->attributes.count(L"disabled") != 0;
            const auto style = node->Attribute(L"style");
            if (!style.empty()) ParseStyleAttribute(style, node->inlineStyle,
                                                     &node->inlineStylePriority, &node->inlineStyleOrder);
            stack.InsertNode(node);
            if (tableToken == HtmlOpenElements::TableToken::Form) { stack.SetTableForm(node); continue; }

            const bool rcdata = tag == L"title" || tag == L"textarea";
            const bool rawText = tag == L"script" || tag == L"style" || tag == L"xmp" ||
                tag == L"iframe" || tag == L"noembed" || tag == L"noframes" ||
                (tag == L"noscript" && scriptingEnabled_);
            const bool plaintext = tag == L"plaintext";
            if (node->namespaceUri.empty() && (rcdata || rawText || plaintext) && !selfClosing) {
                size_t closeEnd = html_.size();
                const size_t end = plaintext ? html_.size() : tag == L"script" ?
                    FindScriptEndTag(closeEnd) : FindTextEndTag(tag, closeEnd);
                auto text = html_.substr(position_, end - position_);
                // NULL tokens in these text modes become U+FFFD. Do this
                // before references: directly assigned DOM text stays literal.
                std::replace(text.begin(), text.end(), L'\0', L'\xfffd');
                if (rcdata) text = DecodeEntities(text);
                if (tag == L"textarea" && !text.empty() && text.front() == L'\n')
                    text.erase(text.begin());
                if (!text.empty()) {
                    auto textNode = std::make_shared<Node>();
                    textNode->type = NodeType::Text;
                    textNode->tag = L"#text";
                    textNode->text = std::move(text);
                    textNode->parent = node;
                    node->children.push_back(std::move(textNode));
                }
                position_ = closeEnd;
            } else if (!selfClosing && !IsVoidTag(tag)) {
                stack.push_back(node, treeFlags);
                if (treeFlags & TreeFormatting) stack.AddFormatting(node);
                if (treeFlags & TreeFormattingMarker) stack.AddFormattingMarker();
                ignoreLeadingLf_ = node->namespaceUri.empty() &&
                    (tag == L"pre" || tag == L"listing");
            }
        }
        flushTableText();
        if (!fragment) NormalizeDocumentTree(root);
        if (error) error->clear();
        return root;
    }

private:
    static bool ForeignDataContext(const std::shared_ptr<Node>& parent) {
        // SVG integration points use the HTML text/declaration behavior.
        return !parent->namespaceUri.empty() && parent->tag != L"foreignobject" &&
            parent->tag != L"title" && parent->tag != L"desc";
    }
    static void FilterDataNulls(const std::shared_ptr<Node>& parent, std::wstring& text) {
        const size_t first = text.find(L'\0');
        if (first == std::wstring::npos) return;
        if (ForeignDataContext(parent)) std::replace(text.begin() + first, text.end(), L'\0', L'\xfffd');
        else text.erase(std::remove(text.begin() + first, text.end(), L'\0'), text.end());
    }
    static void AppendComment(const std::shared_ptr<Node>& parent, std::wstring text) {
        const size_t null = text.find(L'\0');
        if (null != std::wstring::npos) std::replace(text.begin() + null, text.end(), L'\0', L'\xfffd');
        auto node = std::make_shared<Node>();
        node->type = NodeType::Comment; node->tag = L"#comment"; node->text = std::move(text);
        node->parent = parent; parent->children.push_back(std::move(node));
    }
    std::wstring ReadBogusComment() {
        const size_t end = html_.find(L'>', position_);
        auto text = html_.substr(position_, end - position_);
        position_ = end == std::wstring::npos ? html_.size() : end + 1;
        return text;
    }
    std::wstring ReadComment() {
        enum class State { Start, StartDash, Data, EndDash, End, EndBang };
        State state = State::Start;
        std::wstring text;
        // Pending terminator characters belong to states, not the comment.
        // EOF therefore emits the accumulated text without '-'/'--'/'--!'.
        while (position_ < html_.size()) {
            const wchar_t c = html_[position_];
            switch (state) {
            case State::Start:
                if (c == L'-') { ++position_; state = State::StartDash; }
                else if (c == L'>') { ++position_; return text; }
                else state = State::Data;
                break;
            case State::StartDash:
                if (c == L'-') { ++position_; state = State::End; }
                else if (c == L'>') { ++position_; return text; }
                else { text += L'-'; state = State::Data; }
                break;
            case State::Data: {
                // Less-than/bang states append '<!' and then reconsume in
                // these same dash/end states. Since parse errors have no DOM
                // side effect, copy those literal characters with the span.
                // NULL replacement is shared by normal and bogus comments.
                const size_t boundary = html_.find(L'-', position_);
                const size_t end = boundary == std::wstring::npos ? html_.size() : boundary;
                text.append(html_, position_, end - position_); position_ = end;
                if (position_ == html_.size()) return text;
                ++position_; state = State::EndDash;
                break;
            }
            case State::EndDash:
                if (c == L'-') { ++position_; state = State::End; }
                else { text += L'-'; state = State::Data; }
                break;
            case State::End:
                if (c == L'>') { ++position_; return text; }
                else if (c == L'!') { ++position_; state = State::EndBang; }
                else if (c == L'-') {
                    const size_t boundary = html_.find_first_not_of(L'-', position_);
                    const size_t end = boundary == std::wstring::npos ? html_.size() : boundary;
                    text.append(html_, position_, end - position_); position_ = end;
                }
                else { text += L"--"; state = State::Data; }
                break;
            case State::EndBang:
                if (c == L'-') { text += L"--!"; ++position_; state = State::EndDash; }
                else if (c == L'>') { ++position_; return text; }
                else { text += L"--!"; state = State::Data; }
                break;
            }
        }
        return text;
    }
    static void AppendText(const std::shared_ptr<Node>& parent, std::wstring text) {
        if (text.empty()) return;
        // Ignored tokens and invalid openers do not split a DOM text node.
        // Reuse its storage instead of allocating another node per chunk.
        if (!parent->children.empty() && parent->children.back()->type == NodeType::Text) {
            parent->children.back()->text += text;
            return;
        }
        auto node = std::make_shared<Node>();
        node->type = NodeType::Text; node->tag = L"#text"; node->text = std::move(text);
        node->parent = parent; parent->children.push_back(std::move(node));
    }
    static bool TextDelimiter(wchar_t c) {
        return c == L'>' || c == L'/' || c == L' ' || c == L'\t' || c == L'\n' || c == L'\f';
    }
    static bool AsciiAlpha(wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
    }
    bool AppropriateEndTag(size_t candidate, const std::wstring& tag, size_t& closeEnd) const {
        size_t index = candidate + 2;
        if (candidate + 1 >= html_.size() || html_[candidate + 1] != L'/' ||
            index + tag.size() >= html_.size()) return false;
        for (size_t matched = 0; matched < tag.size(); ++matched) {
            wchar_t character = html_[index + matched];
            if (character >= L'A' && character <= L'Z') character += L'a' - L'A';
            if (character != tag[matched]) return false;
        }
        index += tag.size();
        if (!TextDelimiter(html_[index])) return false;
        bool selfClosing = false;
        // Reuse the exact attribute states, including malformed leading '='
        // and quotes in unquoted values. Pending appropriate tags at EOF are
        // discarded rather than returned as raw text.
        ReadTagAttributes(index, nullptr, selfClosing);
        closeEnd = index;
        return true;
    }
    size_t FindScriptEndTag(size_t& closeEnd) const {
        enum class State { Data, Escaped, EscapedDash, EscapedDashDash,
            DoubleEscaped, DoubleDash, DoubleDashDash };
        State state = State::Data;
        size_t index = position_;
        const std::wstring script = L"script";
        while (index < html_.size()) {
            if (state == State::Data) {
                index = html_.find(L'<', index);
                if (index == std::wstring::npos) break;
                if (AppropriateEndTag(index, script, closeEnd)) return index;
                if (html_.compare(index, 4, L"<!--") == 0) {
                    state = State::EscapedDashDash;
                    index += 4;
                } else ++index;
                continue;
            }
            const bool doubled = state == State::DoubleEscaped || state == State::DoubleDash || state == State::DoubleDashDash;
            const State base = doubled ? State::DoubleEscaped : State::Escaped;
            const wchar_t c = html_[index++];
            if (c == L'-') {
                state = state == base ? (doubled ? State::DoubleDash : State::EscapedDash) :
                    (doubled ? State::DoubleDashDash : State::EscapedDashDash);
            } else if (c == L'>') {
                state = state == State::EscapedDashDash || state == State::DoubleDashDash ? State::Data : base;
            } else if (c == L'<') {
                state = base;
                if (!doubled && AppropriateEndTag(index - 1, script, closeEnd)) return index - 1;
                // Double escape start/end uses only ASCII letters and an HTML
                // delimiter. The literal </script> exits double escaping; it
                // does not close the element until a later end-tag token.
                if (doubled) {
                    if (index >= html_.size() || html_[index] != L'/') continue;
                    ++index;
                }
                const size_t begin = index;
                bool matches = true;
                while (index < html_.size() && AsciiAlpha(html_[index])) {
                    wchar_t letter = html_[index];
                    if (letter >= L'A' && letter <= L'Z') letter += L'a' - L'A';
                    const size_t offset = index - begin;
                    matches = matches && offset < script.size() && letter == script[offset];
                    ++index;
                }
                if (index > begin && index < html_.size() && TextDelimiter(html_[index])) {
                    if (matches && index - begin == script.size())
                        state = doubled ? State::Escaped : State::DoubleEscaped;
                    ++index;
                }
            } else state = base;
        }
        return html_.size();
    }
    // Scan only the text belonging to this element. Lowercasing the complete
    // document at every script/style made many raw elements quadratic. End
    // names require an HTML delimiter, so </stylex> stays literal text.
    size_t FindTextEndTag(const std::wstring& tag, size_t& closeEnd) const {
        size_t candidate = position_;
        while ((candidate = html_.find(L'<', candidate)) != std::wstring::npos) {
            if (AppropriateEndTag(candidate, tag, closeEnd)) return candidate;
            ++candidate;
        }
        return html_.size();
    }
    bool Starts(const wchar_t* value) const {
        const size_t count = std::wcslen(value);
        return position_ + count <= html_.size() && html_.compare(position_, count, value) == 0;
    }
    bool AsciiStarts(const wchar_t* value) const {
        for (size_t i = 0; value[i]; ++i) {
            if (position_ + i >= html_.size()) return false;
            wchar_t c = html_[position_ + i];
            if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
            if (c != value[i]) return false;
        }
        return true;
    }
    static bool HtmlSpace(wchar_t c) {
        // CR is already normalized by input preprocessing. Unicode spaces and
        // vertical tab are ordinary tag/name/value characters.
        return c == L' ' || c == L'\t' || c == L'\n' || c == L'\f';
    }
    static wchar_t NameCharacter(wchar_t c) {
        return c >= L'A' && c <= L'Z' ? c + (L'a' - L'A') : c == 0 ? L'\xfffd' : c;
    }
    void SkipHtmlSpace(size_t& cursor) const { while (cursor < html_.size() && HtmlSpace(html_[cursor])) ++cursor; }
    std::wstring ReadTagName() {
        const size_t begin = position_;
        while (position_ < html_.size() && !TextDelimiter(html_[position_])) ++position_;
        auto name = html_.substr(begin, position_ - begin);
        std::transform(name.begin(), name.end(), name.begin(), NameCharacter);
        return name;
    }
    bool ReadTagAttributes(size_t& cursor, Node* node, bool& selfClosing) const {
        while (cursor < html_.size()) {
            SkipHtmlSpace(cursor);
            if (cursor == html_.size()) break;
            if (html_[cursor] == L'>') { ++cursor; return true; }
            if (html_[cursor] == L'/') {
                ++cursor;
                if (cursor < html_.size() && html_[cursor] == L'>') {
                    ++cursor; selfClosing = true; return true;
                }
                // Unexpected solidus: reconsume the next character before a
                // new attribute. A slash in an unquoted value stays literal.
                continue;
            }
            const size_t nameBegin = cursor++;
            // The first '=' is part of a malformed attribute name; later '='
            // characters start its value. Quotes and '<' also remain in names.
            while (cursor < html_.size() && !HtmlSpace(html_[cursor]) &&
                   html_[cursor] != L'/' && html_[cursor] != L'>' && html_[cursor] != L'=') ++cursor;
            std::wstring name;
            bool store = false;
            if (node) {
                name = html_.substr(nameBegin, cursor - nameBegin);
                std::transform(name.begin(), name.end(), name.begin(), NameCharacter);
                if (!node->namespaceUri.empty()) name = AdjustSvgAttribute(name);
                store = !node->attributes.count(name);
            }
            SkipHtmlSpace(cursor);
            size_t valueBegin = cursor, valueEnd = cursor;
            if (cursor < html_.size() && html_[cursor] == L'=') {
                ++cursor; SkipHtmlSpace(cursor);
                if (cursor == html_.size()) break;
                if (html_[cursor] == L'\'' || html_[cursor] == L'"') {
                    const wchar_t quote = html_[cursor++];
                    valueBegin = cursor;
                    valueEnd = html_.find(quote, cursor);
                    if (valueEnd == std::wstring::npos) { cursor = html_.size(); break; }
                    cursor = valueEnd + 1;
                } else {
                    valueBegin = cursor;
                    while (cursor < html_.size() && !HtmlSpace(html_[cursor]) && html_[cursor] != L'>') ++cursor;
                    valueEnd = cursor;
                }
            }
            if (store) {
                auto value = html_.substr(valueBegin, valueEnd - valueBegin);
                // Physical NULL is replaced before decoding references; direct
                // DOM assignment and decoded characters are not retokenized.
                std::replace(value.begin(), value.end(), L'\0', L'\xfffd');
                if (value.find(L'&') != std::wstring::npos) value = DecodeEntities(value, true);
                node->attributes.emplace(std::move(name), std::move(value));
            }
        }
        // Every tag/attribute state discards its pending token at EOF.
        return false;
    }
    const std::wstring normalizedHtml_;
    const std::wstring& html_;
    bool scriptingEnabled_;
    size_t position_ = 0;
    bool ignoreLeadingLf_ = false;
    bool quirksMode_ = true, limitedQuirksMode_ = false, initial_ = true;
};

} // namespace

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return std::towlower(c); });
    return value;
}

std::wstring Trim(const std::wstring& value) {
    size_t a = 0, b = value.size();
    while (a < b && IsSpace(value[a])) ++a;
    while (b > a && IsSpace(value[b - 1])) --b;
    return value.substr(a, b - a);
}

std::wstring DecodeEntities(const std::wstring& value, bool inAttribute) {
    size_t ampersand = value.find(L'&');
    if (ampersand == std::wstring::npos) return value;
    std::wstring out;
    out.reserve(value.size());
    const auto appendCodePoint = [&](std::uint32_t code) {
        if (code <= 0xffff) out += static_cast<wchar_t>(code);
        else {
            code -= 0x10000;
            out += static_cast<wchar_t>(0xd800 + (code >> 10));
            out += static_cast<wchar_t>(0xdc00 + (code & 0x3ff));
        }
    };
    size_t copied = 0;
    while (ampersand != std::wstring::npos) {
        out.append(value, copied, ampersand - copied);
        size_t end = ampersand + 1;
        bool decoded = false;
        if (end < value.size() && value[end] == L'#') {
            ++end;
            const unsigned int base = end < value.size() && (value[end] == L'x' || value[end] == L'X') ? 16 : 10;
            if (base == 16) ++end;
            const size_t digits = end;
            std::uint32_t code = 0;
            while (end < value.size()) {
                const auto c = value[end];
                const unsigned int digit = c >= L'0' && c <= L'9' ? c - L'0' :
                    c >= L'a' && c <= L'f' ? c - L'a' + 10 :
                    c >= L'A' && c <= L'F' ? c - L'A' + 10 : 16;
                if (digit >= base) break;
                // Saturate outside Unicode while still consuming every digit.
                // Arbitrarily long input cannot overflow or cause a rescan.
                code = std::min<std::uint32_t>(0x110000, code * base + digit);
                ++end;
            }
            if (end != digits) {
                if (end < value.size() && value[end] == L';') ++end;
                if (!code || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) code = 0xfffd;
                else if (code >= 0x80 && code <= 0x9f) {
                    static constexpr std::uint16_t replacements[] = {
                        0x20ac,0x81,0x201a,0x192,0x201e,0x2026,0x2020,0x2021,
                        0x2c6,0x2030,0x160,0x2039,0x152,0x8d,0x17d,0x8f,
                        0x90,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,
                        0x2dc,0x2122,0x161,0x203a,0x153,0x9d,0x17e,0x178};
                    code = replacements[code - 0x80];
                }
                appendCodePoint(code);
                decoded = true;
            }
        } else if (end < value.size() && value[end] < 128) {
            // Semicolon-terminated common names cannot have a longer match or
            // trigger attribute ambiguity. Bypass trie branches for this hot
            // path without special-casing any document or caller.
            std::uint32_t common = 0;
            size_t commonLength = 0;
            switch (value[end]) {
            case L'a':
                if (value.compare(end, 4, L"amp;") == 0) { common = L'&'; commonLength = 4; }
                else if (value.compare(end, 5, L"apos;") == 0) { common = L'\''; commonLength = 5; }
                break;
            case L'l': if (value.compare(end, 3, L"lt;") == 0) { common = L'<'; commonLength = 3; } break;
            case L'g': if (value.compare(end, 3, L"gt;") == 0) { common = L'>'; commonLength = 3; } break;
            case L'q': if (value.compare(end, 5, L"quot;") == 0) { common = L'"'; commonLength = 5; } break;
            case L'n': if (value.compare(end, 5, L"nbsp;") == 0) { common = 0xa0; commonLength = 5; } break;
            }
            if (common) {
                appendCodePoint(common);
                copied = end + commonLength;
                ampersand = value.find(L'&', copied);
                continue;
            }
            // Longest exact name, including the 106 legacy semicolonless
            // spellings. Static trie storage needs no allocation or startup map.
            auto index = HtmlNamedEntities::roots[value[end]];
            std::uint16_t match = 0;
            size_t matchEnd = end;
            while (index) {
                const auto& node = HtmlNamedEntities::nodes[index];
                ++end;
                if (node.value) { match = node.value; matchEnd = end; }
                if (end == value.size()) break;
                index = node.child;
                while (index && HtmlNamedEntities::nodes[index].character < value[end])
                    index = HtmlNamedEntities::nodes[index].sibling;
                if (index && HtmlNamedEntities::nodes[index].character != value[end]) index = 0;
            }
            if (match) {
                const wchar_t next = matchEnd < value.size() ? value[matchEnd] : 0;
                const bool ambiguousAttribute = inAttribute && value[matchEnd - 1] != L';' &&
                    ((next >= L'a' && next <= L'z') || (next >= L'A' && next <= L'Z') ||
                     (next >= L'0' && next <= L'9') || next == L'=');
                if (!ambiguousAttribute) {
                    const auto& replacement = HtmlNamedEntities::values[match];
                    appendCodePoint(replacement.first);
                    if (replacement.second) appendCodePoint(replacement.second);
                    end = matchEnd;
                    decoded = true;
                }
            }
        }
        if (!decoded) { out += L'&'; end = ampersand + 1; }
        copied = end;
        ampersand = value.find(L'&', copied);
    }
    out.append(value, copied, value.size() - copied);
    return out;
}

std::wstring Node::Attribute(const std::wstring& name) const {
    // Parsed and DOM-authored attribute keys are stored lowercase. The common
    // path uses lowercase literals, so avoid allocating a temporary string for
    // every lookup and only normalize genuinely mixed-case input.
    const auto direct = attributes.find(name);
    if (direct != attributes.end()) return direct->second;
    if (xml) return L"";
    if(namespaceUri==L"http://www.w3.org/2000/svg"){
        const auto adjusted=attributes.find(AdjustSvgAttribute(ToLower(name)));
        if(adjusted!=attributes.end())return adjusted->second;
    }
    if (std::none_of(name.begin(), name.end(), [](wchar_t character) {
            return std::iswupper(character) != 0;
        })) return L"";
    const auto normalized = attributes.find(ToLower(name));
    return normalized == attributes.end() ? L"" : normalized->second;
}

void Node::SetAttribute(const std::wstring& name, const std::wstring& value) {
    const auto key = xml ? name : ToLower(name);
    const auto oldClass = key == L"class" ? Attribute(L"class") : L"";
    const auto oldName = key == L"name" ? Attribute(L"name") : L"";
    attributes[key] = value;
    if (key == L"nonce") cryptographicNonce = value;
    if (key == L"checked") checked = true;
    if (key == L"disabled") disabled = true;
    if (key == L"style") {
        inlineStyle.clear(); inlineStylePriority.clear(); inlineStyleOrder.clear();
        ParseStyleAttribute(value, inlineStyle, &inlineStylePriority, &inlineStyleOrder);
    }
    if (key == L"class" && ownerDocument)
        ownerDocument->UpdateElementClass(shared_from_this(), oldClass, value);
    if (key == L"name" && ownerDocument)
        ownerDocument->UpdateElementName(shared_from_this(), oldName, value);
}

void Node::RemoveAttribute(const std::wstring& name) {
    const auto key = xml ? name : ToLower(name);
    const auto oldClass = key == L"class" ? Attribute(L"class") : L"";
    const auto oldName = key == L"name" ? Attribute(L"name") : L"";
    attributes.erase(key);
    if (key == L"nonce") cryptographicNonce.clear();
    if (key == L"checked") checked = false;
    if (key == L"disabled") disabled = false;
    if (key == L"style") { inlineStyle.clear(); inlineStylePriority.clear(); inlineStyleOrder.clear(); }
    if (key == L"class" && ownerDocument)
        ownerDocument->UpdateElementClass(shared_from_this(), oldClass, L"");
    if (key == L"name" && ownerDocument)
        ownerDocument->UpdateElementName(shared_from_this(), oldName, L"");
}

bool Node::HasClass(const std::wstring& name) const {
    const auto found = attributes.find(L"class");
    if (found == attributes.end()) return false;
    const auto& value = found->second;
    size_t position = 0;
    while (position < value.size()) {
        while (position < value.size() && IsSpace(value[position])) ++position;
        const size_t start = position;
        while (position < value.size() && !IsSpace(value[position])) ++position;
        if (position - start == name.size() && value.compare(start, name.size(), name) == 0)
            return true;
    }
    return false;
}

void Node::AddClass(const std::wstring& name) {
    if (HasClass(name)) return;
    auto value = Attribute(L"class");
    const auto oldClass = value;
    if (!value.empty()) value += L' ';
    attributes[L"class"] = value + name;
    if (ownerDocument)
        ownerDocument->UpdateElementClass(shared_from_this(), oldClass, attributes[L"class"]);
}

void Node::RemoveClass(const std::wstring& name) {
    const auto found = attributes.find(L"class");
    if (found == attributes.end()) return;
    const auto& source = found->second;
    if (!HasClass(name)) return;
    const auto oldClass = source;
    std::wstring value;
    value.reserve(source.size());
    size_t position = 0;
    while (position < source.size()) {
        while (position < source.size() && IsSpace(source[position])) ++position;
        const size_t start = position;
        while (position < source.size() && !IsSpace(source[position])) ++position;
        if (start == position ||
            (position - start == name.size() && source.compare(start, name.size(), name) == 0))
            continue;
        if (!value.empty()) value += L' ';
        value.append(source, start, position - start);
    }
    attributes[L"class"] = std::move(value);
    if (ownerDocument)
        ownerDocument->UpdateElementClass(shared_from_this(), oldClass, attributes[L"class"]);
}

void Node::ToggleClass(const std::wstring& name, bool force, bool hasForce) {
    const bool add = hasForce ? force : !HasClass(name);
    if (add) AddClass(name); else RemoveClass(name);
}

std::wstring Node::InnerText() const {
    std::wstring result;
    AppendInnerText(*this, result);
    return result;
}

void Node::SetInnerText(const std::wstring& value) {
    std::function<void(const std::shared_ptr<Node>&)> disconnect = [&](const auto& node) {
        node->ownerDocument = nullptr;
        for (const auto& child : node->children) disconnect(child);
    };
    for (auto& child : children) { disconnect(child); child->parent.reset(); }
    children.clear();
    auto child = std::make_shared<Node>();
    child->type = NodeType::Text; child->tag = L"#text"; child->text = value;
    child->parent = shared_from_this();
    children.push_back(child);
}

std::shared_ptr<Node> Node::TemplateContents() {
    if(!templateContent){
        templateContent=std::make_shared<Node>();templateContent->tag=L"#document-fragment";
        templateContent->ownerDocument=ownerDocument;
        templateContent->children=std::move(children);
        for(const auto& child:templateContent->children)child->parent=templateContent;
    }
    return templateContent;
}

static void NormalizeTemplateContents(const std::shared_ptr<Node>& node){
    if(!node)return;
    for(const auto& child:node->children)NormalizeTemplateContents(child);
    if(node->tag==L"template"&&!node->xml)node->TemplateContents();
}

std::shared_ptr<Node> Node::Closest(const std::wstring& selector) {
    auto current = shared_from_this();
    while (current) {
        if (Document::MatchesSelector(current, selector)) return current;
        current = current->parent.lock();
    }
    return {};
}

Document::Document() {
    root_ = std::make_shared<Node>();
    root_->type = NodeType::Document; root_->tag = L"#document";
    root_->ownerDocument = this;
    ownedNodes_[root_.get()] = root_;
}

Document::~Document() {
    for (const auto& entry : ownedNodes_)
        if (const auto node = entry.second.lock(); node && node->ownerDocument == this)
            node->ownerDocument = nullptr;
}

void Document::AdoptParsed(Document& source) {
    if (&source == this) return;
    for (const auto& entry : ownedNodes_)
        if (const auto node = entry.second.lock(); node && node->ownerDocument == this)
            node->ownerDocument = nullptr;
    ownedNodes_.clear();ids_.clear();idCounts_.clear();nameCounts_.clear();
    tags_.clear();classes_.clear();
    root_ = std::move(source.root_);
    quirksMode_ = source.quirksMode_;
    limitedQuirksMode_ = source.limitedQuirksMode_;
    xml_ = source.xml_;
    scriptingEnabled_ = source.scriptingEnabled_;
    if (!root_) {
        root_ = std::make_shared<Node>();
        root_->type = NodeType::Document;root_->tag = L"#document";
    }
    Reindex();
}

bool Document::Parse(const std::wstring& html, std::wstring* error) {
    xml_ = false;
    HtmlParser parser(html,scriptingEnabled_);
    root_ = parser.Parse(false, error);
    quirksMode_ = parser.QuirksMode();
    limitedQuirksMode_ = parser.LimitedQuirksMode();
    Reindex();
    return root_ != nullptr;
}

bool Document::ParseXml(const std::wstring& xml, std::wstring* error) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IXmlReader> reader;
    // Input has already been decoded by the resource loader. Feed UTF-16 to
    // XmlLite explicitly, independent of the original declaration's encoding.
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(xml.data()),
                                   static_cast<UINT>(xml.size()*sizeof(wchar_t))));
    ComPtr<IXmlReaderInput> input;
    HRESULT status=CreateXmlReader(__uuidof(IXmlReader),&reader,nullptr);
    if(SUCCEEDED(status))status=CreateXmlReaderInputWithEncodingName(stream.Get(),nullptr,L"utf-16",FALSE,nullptr,&input);
    if(SUCCEEDED(status))status=reader->SetInput(input.Get());
    auto root=std::make_shared<Node>();root->type=NodeType::Document;root->tag=L"#document";root->xml=true;
    std::vector<std::shared_ptr<Node>> stack{root};
    XmlNodeType type{};
    while(SUCCEEDED(status)&&(status=reader->Read(&type))==S_OK){
        if(type==XmlNodeType_EndElement){if(stack.size()>1)stack.pop_back();continue;}
        if(type==XmlNodeType_XmlDeclaration||type==XmlNodeType_ProcessingInstruction)continue;
        auto node=std::make_shared<Node>();node->xml=true;
        const wchar_t* value=nullptr;UINT count=0;
        if(type==XmlNodeType_Element){
            node->type=NodeType::Element;reader->GetQualifiedName(&value,&count);node->tag.assign(value,count);
            if(reader->MoveToFirstAttribute()==S_OK){
                do{const wchar_t* name=nullptr;UINT length=0;reader->GetQualifiedName(&name,&length);
                    const std::wstring key(name,length);reader->GetValue(&value,&count);node->attributes[key]=std::wstring(value,count);
                }while(reader->MoveToNextAttribute()==S_OK);
                reader->MoveToElement();
            }
            node->cryptographicNonce=node->Attribute(L"nonce");
        }else if(type==XmlNodeType_CDATA||type==XmlNodeType_Text||type==XmlNodeType_Whitespace||type==XmlNodeType_Comment){
            node->type=type==XmlNodeType_CDATA?NodeType::CData:type==XmlNodeType_Comment?NodeType::Comment:NodeType::Text;
            node->tag=type==XmlNodeType_CDATA?L"#cdata-section":type==XmlNodeType_Comment?L"#comment":L"#text";
            reader->GetValue(&value,&count);node->text.assign(value,count);
        }else continue;
        node->parent=stack.back();stack.back()->children.push_back(node);
        if(type==XmlNodeType_Element&&!reader->IsEmptyElement())stack.push_back(node);
    }
    const bool hasRoot=std::any_of(root->children.begin(),root->children.end(),[](const auto& node){return node->type==NodeType::Element;});
    if(FAILED(status)||stack.size()!=1||!hasRoot){if(error)*error=L"Malformed XML document";return false;}
    root_=std::move(root);xml_=true;quirksMode_=false;limitedQuirksMode_=false;Reindex();
    if(error)error->clear();return true;
}

std::vector<std::shared_ptr<Node>> Document::ParseFragment(const std::wstring& html,
                                                            std::wstring* error,
                                                            const std::shared_ptr<Node>& context) {
    HtmlParser parser(html,scriptingEnabled_);
    auto fragment = parser.Parse(true, error, context && context->namespaceUri.empty() ? context->tag : L"");
    NormalizeTemplateContents(fragment);
    return fragment ? fragment->children : std::vector<std::shared_ptr<Node>>{};
}

std::shared_ptr<Node> Document::Body() const {
    return QuerySelector(L"body");
}

std::shared_ptr<Node> Document::GetElementById(const std::wstring& id) const {
    const auto it = ids_.find(id);
    return it == ids_.end() ? nullptr : it->second.lock();
}

std::vector<std::shared_ptr<Node>> Document::GetElementsByName(const std::wstring& name) const {
    std::vector<std::shared_ptr<Node>> result;
    // Named window/document property lookup is attempted for every otherwise
    // unknown property.  Avoid walking the whole tree when no such name exists;
    // retain the tree walk for present names so results remain in document order.
    if (name.empty() || nameCounts_.find(name) == nameCounts_.end()) return result;
    Walk(root_, [&](const auto& node) { if (node->Attribute(L"name") == name) result.push_back(node); });
    return result;
}

std::vector<std::shared_ptr<Node>> Document::GetElementsByTagName(
    const std::wstring& requestedTag, const std::shared_ptr<Node>& scope,
    bool includeScope) const {
    const auto tag = xml_ ? requestedTag : ToLower(requestedTag);
    std::vector<std::shared_ptr<Node>> result;
    if (tag != L"*" && (!scope || IsWithinScope(scope,root_))) {
        const auto found = tags_.find(tag);
        if (found == tags_.end()) return result;
        // For the small buckets used by repeated library lookups, visiting the
        // index is substantially cheaper than walking a comment-heavy page.
        // Large buckets keep the linear tree walk, which naturally preserves
        // document order without an O(k log k) sort.
        constexpr size_t kDirectIndexLimit = 64;
        if (found->second.size() <= kDirectIndexLimit) {
            std::vector<std::shared_ptr<Node>> indexed;
            indexed.reserve(found->second.size());
            for (const auto& entry : found->second)
                if (const auto node = entry.second.lock()) indexed.push_back(node);
            return OrderedIndexedNodes(indexed, scope, includeScope,
                                       [](const auto&) { return true; });
        }
    }
    Walk(scope ? scope : root_, [&](const auto& node) {
        if ((!includeScope && node == scope) || node->type != NodeType::Element) return;
        if (tag == L"*" || node->tag == tag) result.push_back(node);
    });
    return result;
}

std::vector<std::shared_ptr<Node>> Document::GetElementsByClassName(
    const std::wstring& classNames, const std::shared_ptr<Node>& scope,
    bool includeScope) const {
    std::vector<std::wstring> names;
    ForEachClassToken(classNames, [&](const auto& token) { names.push_back(token); });
    std::vector<std::shared_ptr<Node>> result;
    if (names.empty()) return result;
    const NodeIndexBucket* smallest = nullptr;
    if(!scope || IsWithinScope(scope,root_))for (const auto& name : names) {
        const auto found = classes_.find(name);
        if (found == classes_.end()) return result;
        if (!smallest || found->second.size() < smallest->size()) smallest = &found->second;
    }
    constexpr size_t kDirectIndexLimit = 64;
    const auto matches = [&](const auto& node) {
        return std::all_of(names.begin(), names.end(),
                           [&](const auto& name) { return node->HasClass(name); });
    };
    if (smallest && smallest->size() <= kDirectIndexLimit) {
        std::vector<std::shared_ptr<Node>> indexed;
        indexed.reserve(smallest->size());
        for (const auto& entry : *smallest)
            if (const auto node = entry.second.lock()) indexed.push_back(node);
        return OrderedIndexedNodes(indexed, scope, includeScope, matches);
    }
    Walk(scope ? scope : root_, [&](const auto& node) {
        if ((!includeScope && node == scope) || node->type != NodeType::Element) return;
        if (matches(node)) result.push_back(node);
    });
    return result;
}

bool Document::MatchesSelector(const std::shared_ptr<Node>& node, const std::wstring& selector) {
    // Element.matches() and Element.closest() accept a selector list.  Reuse
    // the same comma-aware compilation as querySelector(), and match when any
    // list item applies instead of treating the comma as part of a tag name.
    for(const auto& compiled:CompileQuerySelectors(selector,false))
        if(MatchesSelector(node,compiled.parts))return true;
    return false;
}

std::vector<std::wstring> Document::CompileSelector(const std::wstring& selector) {
    return SplitSelector(selector);
}

bool Document::MatchesSelector(const std::shared_ptr<Node>& node,
                               const std::vector<std::wstring>& parts) {
    return !parts.empty()&&MatchParts(node,parts,static_cast<int>(parts.size())-1);
}

std::shared_ptr<Node> Document::QuerySelector(const std::wstring& selector,
                                               const std::shared_ptr<Node>& scope) const {
    const auto selectors = CompileQuerySelectors(selector, scope != nullptr);
    std::unordered_set<const Node*> candidates;
    bool filterCandidates = !selectors.empty() && (!scope || IsWithinScope(scope,root_));
    for (const auto& item : selectors) {
        if (item.scopeMode == CompiledQuerySelector::ScopeMode::ScopeOnly) {
            if (scope) candidates.insert(scope.get());
            continue;
        }
        if (item.indexKind == CompiledQuerySelector::IndexKind::Id) {
            const auto count = idCounts_.find(item.indexKey);
            const auto indexed = ids_.find(item.indexKey);
            if (count == idCounts_.end() || count->second != 1 || indexed == ids_.end()) {
                if (count != idCounts_.end() && count->second > 1) filterCandidates = false;
                continue;
            }
            if (const auto node = indexed->second.lock()) candidates.insert(node.get());
        } else if (item.indexKind == CompiledQuerySelector::IndexKind::Class ||
                   item.indexKind == CompiledQuerySelector::IndexKind::Tag) {
            const auto& index = item.indexKind == CompiledQuerySelector::IndexKind::Class ? classes_ : tags_;
            const auto found = index.find(item.indexKey);
            if (found == index.end()) continue;
            for (const auto& entry : found->second)
                if (entry.second.lock()) candidates.insert(entry.first);
        } else {
            filterCandidates = false;
        }
    }
    if (filterCandidates && candidates.empty()) return {};
    if (filterCandidates && candidates.size() <= 64) {
        std::vector<std::shared_ptr<Node>> indexed;
        indexed.reserve(candidates.size());
        for (const auto* candidate : candidates) {
            const auto found = ownedNodes_.find(candidate);
            if (found != ownedNodes_.end())
                if (const auto node = found->second.lock(); IsWithinScope(node, scope))
                    indexed.push_back(node);
        }
        std::sort(indexed.begin(), indexed.end(), DocumentOrderLess);
        for (const auto& node : indexed)
            for (const auto& item : selectors)
                if (MatchesQuerySelector(node, scope, item)) return node;
        return {};
    }
    std::shared_ptr<Node> result;
    WalkUntil(scope ? scope : root_, [&](const auto& node) {
        if (filterCandidates && candidates.count(node.get()) == 0) return false;
        for (const auto& item : selectors) {
            if (MatchesQuerySelector(node, scope, item)) { result = node; return true; }
        }
        return false;
    });
    return result;
}

std::vector<std::shared_ptr<Node>> Document::QuerySelectorAll(
    const std::wstring& selector, const std::shared_ptr<Node>& scope) const {
    std::vector<std::shared_ptr<Node>> result;
    // Parse selector groups once instead of once for every visited node.
    const auto selectors = CompileQuerySelectors(selector, scope != nullptr);
    std::unordered_set<const Node*> candidates;
    bool filterCandidates = !selectors.empty() && (!scope || IsWithinScope(scope,root_));
    for (const auto& item : selectors) {
        if (item.scopeMode == CompiledQuerySelector::ScopeMode::ScopeOnly) {
            if (scope) candidates.insert(scope.get());
            continue;
        }
        if (item.indexKind == CompiledQuerySelector::IndexKind::Id) {
            const auto count = idCounts_.find(item.indexKey);
            const auto indexed = ids_.find(item.indexKey);
            if (count == idCounts_.end() || count->second != 1 || indexed == ids_.end()) {
                if (count != idCounts_.end() && count->second > 1) filterCandidates = false;
                continue;
            }
            if (const auto node = indexed->second.lock()) candidates.insert(node.get());
        } else if (item.indexKind == CompiledQuerySelector::IndexKind::Class ||
                   item.indexKind == CompiledQuerySelector::IndexKind::Tag) {
            const auto& index = item.indexKind == CompiledQuerySelector::IndexKind::Class ? classes_ : tags_;
            const auto found = index.find(item.indexKey);
            if (found == index.end()) continue;
            for (const auto& entry : found->second)
                if (entry.second.lock()) candidates.insert(entry.first);
        } else {
            filterCandidates = false;
        }
    }
    if (filterCandidates && candidates.empty()) return result;
    if (filterCandidates && candidates.size() <= 64) {
        std::vector<std::shared_ptr<Node>> indexed;
        indexed.reserve(candidates.size());
        for (const auto* candidate : candidates) {
            const auto found = ownedNodes_.find(candidate);
            if (found != ownedNodes_.end())
                if (const auto node = found->second.lock(); IsWithinScope(node, scope))
                    indexed.push_back(node);
        }
        std::sort(indexed.begin(), indexed.end(), DocumentOrderLess);
        for (const auto& node : indexed)
            for (const auto& item : selectors)
                if (MatchesQuerySelector(node, scope, item)) { result.push_back(node); break; }
        return result;
    }
    Walk(scope ? scope : root_, [&](const auto& node) {
        if (filterCandidates && candidates.count(node.get()) == 0) return;
        for (const auto& item : selectors) {
            if (MatchesQuerySelector(node, scope, item)) { result.push_back(node); break; }
        }
    });
    return result;
}

std::shared_ptr<Node> Document::CreateElement(const std::wstring& tag) const {
    auto node = std::make_shared<Node>();
    node->type = NodeType::Element; node->tag = xml_?tag:ToLower(tag);node->xml=xml_;
    if(!xml_&&node->tag==L"template")node->TemplateContents();
    return node;
}

void Document::SetInnerHtml(const std::shared_ptr<Node>& node, const std::wstring& html,
                            bool reindex) {
    if (!node) return;
    if(node->tag==L"template"&&!node->xml){SetInnerHtml(node->TemplateContents(),html,reindex);return;}
    std::vector<std::shared_ptr<Node>> children;
    // The innerHTML setter parses in the context of the target element.  Raw
    // text elements do not treat '<' or '&' as markup; RCDATA elements only
    // expand character references.  Parsing these values as an ordinary HTML
    // fragment corrupts JavaScript comparisons such as `a < b` and CSS text.
    static const std::unordered_set<std::wstring> rawTextElements = {
        L"script", L"style", L"xmp", L"iframe", L"noembed", L"noframes", L"plaintext"
    };
    const bool rawText = node->type == NodeType::Element &&
                         rawTextElements.count(node->tag) != 0;
    const bool rcdata = node->type == NodeType::Element &&
                       (node->tag == L"title" || node->tag == L"textarea");
    if (rawText || rcdata) {
        if (!html.empty()) {
            const auto normalized = HtmlInputWithNormalizedLineEndings(html);
            auto input = normalized.empty() ? html : normalized;
            // WebView2's RCDATA fragment context ignores literal NULL tokens;
            // RAWTEXT/script/plaintext contexts replace them. Keep this apart
            // from document RCDATA and from direct DOM string assignment.
            if (rcdata) input.erase(std::remove(input.begin(), input.end(), L'\0'), input.end());
            else std::replace(input.begin(), input.end(), L'\0', L'\xfffd');
            if (!input.empty()) {
                auto text = std::make_shared<Node>();
                text->type = NodeType::Text;
                text->tag = L"#text";
                text->text = rcdata ? DecodeEntities(input) : input;
                children.push_back(std::move(text));
            }
        }
    } else {
        children = ParseFragment(html,nullptr,node);
    }
    for (auto& child : children) child->parent = node;
    std::function<void(const std::shared_ptr<Node>&)> disconnect = [&](const auto& current) {
        current->ownerDocument = nullptr;
        for (const auto& child : current->children) disconnect(child);
    };
    for (auto& child : node->children) { disconnect(child); child->parent.reset(); }
    node->children = std::move(children);
    if (reindex) Reindex();
}

std::wstring Document::StyleText() const {
    std::wstring result;
    for (const auto& style : QuerySelectorAll(L"style")) result += style->InnerText() + L"\n";
    return result;
}

std::wstring Document::ScriptText() const {
    std::wstring result;
    for (const auto& script : QuerySelectorAll(L"script")) result += script->InnerText() + L"\n";
    return result;
}

void Document::Reindex() {
    ++fullReindexCount_;
    NormalizeTemplateContents(root_);
    for (const auto& entry : ownedNodes_)
        if (const auto node = entry.second.lock(); node && node->ownerDocument == this)
            node->ownerDocument = nullptr;
    ownedNodes_.clear();
    ids_.clear();idCounts_.clear();nameCounts_.clear();tags_.clear();classes_.clear();
    Walk(root_, [&](const auto& node) {
        node->ownerDocument = this;
        ownedNodes_[node.get()] = node;
        if(node->shadowRoot||node->templateContent){
            std::function<void(const std::shared_ptr<Node>&)> bindShadow=[&](const auto& current){
                current->ownerDocument=this;ownedNodes_[current.get()]=current;
                for(const auto& child:current->children)bindShadow(child);
                if(current->shadowRoot)bindShadow(current->shadowRoot);
                if(current->templateContent)bindShadow(current->templateContent);
            };
            if(node->shadowRoot)bindShadow(node->shadowRoot);
            if(node->templateContent)bindShadow(node->templateContent);
        }
        if (node->type == NodeType::Element) {
            tags_[node->tag][node.get()] = node;
            const auto classes = node->attributes.find(L"class");
            if (classes != node->attributes.end())
                ForEachClassToken(classes->second, [&](const auto& token) {
                    classes_[token][node.get()] = node;
                });
        }
        const auto id = node->Attribute(L"id");
        if (!id.empty()) { ids_[id] = node; ++idCounts_[id]; }
        const auto name = node->Attribute(L"name");
        if (!name.empty()) ++nameCounts_[name];
    });
}

bool Document::UpdateElementId(const std::shared_ptr<Node>& node,
                               const std::wstring& oldId,
                               const std::wstring& newId) {
    if(!node||oldId==newId)return true;
    auto root=node;while(auto parent=root->parent.lock())root=std::move(parent);
    if(root!=root_)return true;
    if(!oldId.empty()){
        const auto count=idCounts_.find(oldId);
        const auto indexed=ids_.find(oldId);
        if(count==idCounts_.end()||count->second!=1||indexed==ids_.end()||
           indexed->second.lock()!=node)return false;
    }
    if(!newId.empty()){
        const auto count=idCounts_.find(newId);
        if(count!=idCounts_.end()&&count->second!=0)return false;
    }
    if(!oldId.empty()){ids_.erase(oldId);idCounts_.erase(oldId);}
    if(!newId.empty()){ids_[newId]=node;idCounts_[newId]=1;}
    return true;
}

void Document::UpdateElementClass(const std::shared_ptr<Node>& node,
                                  const std::wstring& oldClass,
                                  const std::wstring& newClass) {
    if (!node || oldClass == newClass) return;
    auto root = node;
    while (auto parent = root->parent.lock()) root = std::move(parent);
    if (root != root_) return;
    std::unordered_set<std::wstring> oldTokens, newTokens;
    ForEachClassToken(oldClass, [&](const auto& token) { oldTokens.insert(token); });
    ForEachClassToken(newClass, [&](const auto& token) { newTokens.insert(token); });
    for (const auto& token : oldTokens) if (newTokens.count(token) == 0) {
        const auto found = classes_.find(token);
        if (found != classes_.end()) found->second.erase(node.get());
    }
    for (const auto& token : newTokens) if (oldTokens.count(token) == 0)
        classes_[token][node.get()] = node;
}

void Document::UpdateElementName(const std::shared_ptr<Node>& node,
                                 const std::wstring& oldName,
                                 const std::wstring& newName) {
    if (!node || oldName == newName) return;
    auto root = node;
    while (auto parent = root->parent.lock()) root = std::move(parent);
    if (root != root_) return;
    if (!oldName.empty()) {
        const auto found = nameCounts_.find(oldName);
        if (found != nameCounts_.end()) {
            if (found->second > 1) --found->second;
            else nameCounts_.erase(oldName);
        }
    }
    if (!newName.empty()) ++nameCounts_[newName];
}

bool Document::IndexSubtree(const std::shared_ptr<Node>& node) {
    if(!node)return true;
    NormalizeTemplateContents(node);
    auto root=node;while(auto parent=root->parent.lock())root=std::move(parent);
    if(root!=root_){
        std::function<void(const std::shared_ptr<Node>&)> bind=[&](const auto& current){
            current->ownerDocument=this;ownedNodes_[current.get()]=current;
            for(const auto& child:current->children)bind(child);
            if(current->shadowRoot)bind(current->shadowRoot);
            if(current->templateContent)bind(current->templateContent);
        };
        bind(node);return true;
    }
    std::vector<std::pair<std::wstring,std::shared_ptr<Node>>> entries;
    FastMap<std::wstring,size_t> pending;
    Walk(node,[&](const auto& current){
        const auto id=current->Attribute(L"id");
        if(!id.empty()){entries.emplace_back(id,current);++pending[id];}
    });
    for(const auto& item:pending){
        if(item.second!=1)return false;
        const auto existing=idCounts_.find(item.first);
        if(existing!=idCounts_.end()&&existing->second!=0)return false;
    }
    Walk(node,[&](const auto& current){
        current->ownerDocument=this;
        ownedNodes_[current.get()]=current;
        if(current->type!=NodeType::Element)return;
        tags_[current->tag][current.get()]=current;
        const auto classes=current->attributes.find(L"class");
        if(classes!=current->attributes.end())ForEachClassToken(classes->second,[&](const auto& token){
            classes_[token][current.get()]=current;
        });
        const auto name=current->attributes.find(L"name");
        if(name!=current->attributes.end()&&!name->second.empty())++nameCounts_[name->second];
    });
    for(const auto& item:entries){ids_[item.first]=item.second;idCounts_[item.first]=1;}
    return true;
}

bool Document::UnindexSubtree(const std::shared_ptr<Node>& node) {
    if(!node)return true;
    auto root=node;while(auto parent=root->parent.lock())root=std::move(parent);
    if(root!=root_)return true;
    std::vector<std::pair<std::wstring,std::shared_ptr<Node>>> entries;
    FastMap<std::wstring,size_t> pending;
    FastMap<std::wstring,std::shared_ptr<Node>> pendingNodes;
    Walk(node,[&](const auto& current){
        const auto id=current->Attribute(L"id");
        if(!id.empty()){entries.emplace_back(id,current);++pending[id];pendingNodes[id]=current;}
    });
    for(const auto& item:pending){
        if(item.second!=1)return false;
        const auto count=idCounts_.find(item.first);
        const auto indexed=ids_.find(item.first);
        if(count==idCounts_.end()||count->second!=1||indexed==ids_.end()||
           indexed->second.lock()!=pendingNodes.find(item.first)->second)return false;
    }
    Walk(node,[&](const auto& current){
        if(current->ownerDocument==this)current->ownerDocument=nullptr;
        ownedNodes_.erase(current.get());
        if(current->type!=NodeType::Element)return;
        const auto tag=tags_.find(current->tag);if(tag!=tags_.end())tag->second.erase(current.get());
        const auto classes=current->attributes.find(L"class");
        if(classes!=current->attributes.end())ForEachClassToken(classes->second,[&](const auto& token){
            const auto found=classes_.find(token);if(found!=classes_.end())found->second.erase(current.get());
        });
        const auto name=current->attributes.find(L"name");
        if(name!=current->attributes.end()&&!name->second.empty()){
            const auto found=nameCounts_.find(name->second);
            if(found!=nameCounts_.end()){
                if(found->second>1)--found->second;else nameCounts_.erase(name->second);
            }
        }
    });
    for(const auto& item:entries){ids_.erase(item.first);idCounts_.erase(item.first);}
    return true;
}

} // namespace TWebFrame::Internal
