#include "DOM.h"
#include "NumericParser.h"

#include <algorithm>
#include <cwctype>
#include <functional>
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

bool ClosesOpenParagraph(const std::wstring& tag) {
    static const wchar_t* tags[] = {
        L"address", L"article", L"aside", L"blockquote", L"div", L"dl",
        L"fieldset", L"footer", L"form", L"h1", L"h2", L"h3", L"h4",
        L"h5", L"h6", L"header", L"hgroup", L"hr", L"main", L"menu",
        L"nav", L"ol", L"p", L"pre", L"section", L"table", L"ul"};
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
            child->parent = body;
            body->children.push_back(std::move(child));
        }
    }
}

void CloseOpenElement(std::vector<std::shared_ptr<Node>>& stack,
                      const std::wstring& tag) {
    size_t match = stack.size();
    for (size_t index = stack.size(); index > 1; --index) {
        if (stack[index - 1]->tag == tag) { match = index - 1; break; }
    }
    if (match == stack.size()) return;
    stack.resize(match);
}

void ParseStyleAttribute(const std::wstring& source,
                         FastMap<std::wstring, std::wstring>& out,
                         FastMap<std::wstring, std::wstring>* priorities = nullptr) {
    size_t start = 0;
    while (start < source.size()) {
        size_t end = source.find(L';', start);
        if (end == std::wstring::npos) end = source.size();
        const auto part = source.substr(start, end - start);
        const size_t colon = part.find(L':');
        if (colon != std::wstring::npos) {
            auto name = Trim(part.substr(0, colon));
            if (name.rfind(L"--", 0) != 0) name = ToLower(name);
            auto value = Trim(part.substr(colon + 1));
            const auto lowerValue = ToLower(value);
            constexpr auto important = L"!important";
            if (lowerValue.size() >= 10 &&
                lowerValue.compare(lowerValue.size() - 10, 10, important) == 0) {
                value = Trim(value.substr(0, value.size() - 10));
                if (priorities && !name.empty()) (*priorities)[name] = L"important";
            } else if (priorities && !name.empty()) priorities->erase(name);
            if (!name.empty()) out[name] = value;
        }
        start = end + 1;
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
        if(selector[index]!=L'#'&&selector[index]!=L'.'){++index;continue;}
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

bool MatchSimple(const std::shared_ptr<Node>& node, const std::wstring& source) {
    if (!node || node->type != NodeType::Element) return false;
    // The overwhelming majority of production selectors are tag/class/id
    // compounds.  Match those without copying and repeatedly scanning the
    // selector for every supported pseudo-class.  Functional/state selectors
    // stay on the complete path below.
    if(source.find(L':')==std::wstring::npos&&source.find(L'[')==std::wstring::npos)
        return MatchPlainSimple(node,source);
    auto selector = Trim(source);
    if (selector.empty() || selector == L"*") return true;

    // Supported dynamic pseudo classes and relational/simple functional selectors.
    for (;;) {
        const auto hasPos = selector.find(L":has(");
        if (hasPos == std::wstring::npos) break;
        size_t close=hasPos+5;int depth=1;for(;close<selector.size()&&depth;++close){if(selector[close]==L'(')++depth;else if(selector[close]==L')')--depth;}
        if(depth!=0)return false;const auto relative=Trim(selector.substr(hasPos+5,close-hasPos-6));bool found=false;
        std::function<void(const std::shared_ptr<Node>&)> find=[&](const std::shared_ptr<Node>& current){if(found)return;for(const auto& child:current->children){if(child->type==NodeType::Element&&MatchSimple(child,relative)){found=true;return;}find(child);}};
        find(node);if(!found)return false;selector.erase(hasPos,close-hasPos);
    }
    for (;;) {
        const auto notPos = selector.find(L":not(");
        if (notPos == std::wstring::npos) break;
        const auto close = selector.find(L')', notPos + 5);
        if (close == std::wstring::npos) return false;
        if (MatchSimple(node, selector.substr(notPos + 5, close - notPos - 5))) return false;
        selector.erase(notPos, close - notPos + 1);
    }
    auto consumePseudo = [&](const wchar_t* text, bool state) {
        const std::wstring p(text);
        size_t pos;
        while ((pos = selector.find(p)) != std::wstring::npos) {
            if (!state) return false;
            selector.erase(pos, p.size());
        }
        return true;
    };
    auto isEdgeChild=[&](bool first){
        auto parent=node->parent.lock();if(!parent)return false;
        if(first){for(const auto& child:parent->children)if(child->type==NodeType::Element)return child==node;}
        else{for(auto it=parent->children.rbegin();it!=parent->children.rend();++it)if((*it)->type==NodeType::Element)return *it==node;}
        return false;
    };
    auto isEdgeOfType=[&](bool first){
        auto parent=node->parent.lock();if(!parent)return false;
        if(first){for(const auto& child:parent->children)
            if(child->type==NodeType::Element&&child->tag==node->tag)return child==node;}
        else{for(auto it=parent->children.rbegin();it!=parent->children.rend();++it)
            if((*it)->type==NodeType::Element&&(*it)->tag==node->tag)return *it==node;}
        return false;
    };
    auto elementSiblingCount=[&](bool sameType){
        auto parent=node->parent.lock();if(!parent)return 0;
        int count=0;for(const auto& child:parent->children)
            if(child->type==NodeType::Element&&(!sameType||child->tag==node->tag))++count;
        return count;
    };
    const bool canBeDisabled=node->tag==L"button"||node->tag==L"fieldset"||
        node->tag==L"input"||node->tag==L"optgroup"||node->tag==L"option"||
        node->tag==L"select"||node->tag==L"textarea";
    const bool canBeRequired=node->tag==L"input"||node->tag==L"select"||node->tag==L"textarea";
    const bool isLink=(node->tag==L"a"||node->tag==L"area")&&node->attributes.count(L"href")!=0;
    auto isEmpty=[&](){
        for(const auto& child:node->children)
            if(child->type==NodeType::Element||
               (child->type==NodeType::Text&&!child->text.empty()))return false;
        return true;
    };
    auto childIndex=[&](){
        auto parent=node->parent.lock();if(!parent)return 0;int index=0;
        for(const auto& child:parent->children)if(child->type==NodeType::Element){++index;if(child==node)return index;}
        return 0;
    };
    auto typeIndex=[&](){
        auto parent=node->parent.lock();if(!parent)return 0;int index=0;
        for(const auto& child:parent->children)if(child->type==NodeType::Element&&child->tag==node->tag){++index;if(child==node)return index;}
        return 0;
    };
    auto matchesNth=[](int index,std::wstring expression){
        expression=ToLower(expression);expression.erase(std::remove_if(expression.begin(),expression.end(),IsSpace),expression.end());
        if(index<=0||expression.empty())return false;if(expression==L"odd")return index%2==1;if(expression==L"even")return index%2==0;
        const auto integer=[](const std::wstring& text,int& value){
            size_t used=0;return TryParseInteger(text,value,&used)&&used==text.size();
        };
        const auto n=expression.find(L'n');
        if(n==std::wstring::npos){int exact=0;return integer(expression,exact)&&index==exact;}
        const auto coefficient=expression.substr(0,n);int a=0;
        if(coefficient.empty()||coefficient==L"+")a=1;
        else if(coefficient==L"-")a=-1;
        else if(!integer(coefficient,a))return false;
        int b=0;if(n+1<expression.size()&&!integer(expression.substr(n+1),b))return false;
        if(a==0)return index==b;
        const int delta=index-b;return delta%a==0&&delta/a>=0;
    };
    for(;;){
        const auto position=selector.find(L":nth-child(");if(position==std::wstring::npos)break;
        const auto close=selector.find(L')',position+11);if(close==std::wstring::npos||!matchesNth(childIndex(),selector.substr(position+11,close-position-11)))return false;
        selector.erase(position,close-position+1);
    }
    for(;;){
        const auto position=selector.find(L":nth-of-type(");if(position==std::wstring::npos)break;
        const auto close=selector.find(L')',position+13);if(close==std::wstring::npos||!matchesNth(typeIndex(),selector.substr(position+13,close-position-13)))return false;
        selector.erase(position,close-position+1);
    }
    // :root is a regular pseudo-class and may be compounded with an attribute,
    // class, or id selector (for example :root[data-theme="light"]).  Keeping
    // it in the token stream made the generic parser read "root" as a tag.
    for(;;){
        const auto position=selector.find(L":root");if(position==std::wstring::npos)break;
        const auto end=position+5;
        if((position>0&&(std::iswalnum(selector[position-1])||selector[position-1]==L'-'||selector[position-1]==L'_'))||
           (end<selector.size()&&(std::iswalnum(selector[end])||selector[end]==L'-'||selector[end]==L'_')))return false;
        if(node->tag!=L"html")return false;
        selector.erase(position,5);
    }
    if (!consumePseudo(L":checked", node->checked) ||
        !consumePseudo(L":indeterminate", node->indeterminate) ||
        !consumePseudo(L":disabled", canBeDisabled&&node->disabled) ||
        !consumePseudo(L":enabled", canBeDisabled&&!node->disabled) ||
        !consumePseudo(L":required", canBeRequired&&node->attributes.count(L"required")!=0) ||
        !consumePseudo(L":optional", canBeRequired&&node->attributes.count(L"required")==0) ||
        !consumePseudo(L":link",isLink) ||
        !consumePseudo(L":any-link",isLink) ||
        !consumePseudo(L":hover", node->hovered) ||
        !consumePseudo(L":focus-within", node->focusWithin||node->focused) ||
        !consumePseudo(L":focus-visible", node->focusVisible) ||
        !consumePseudo(L":focus", node->focused) ||
        !consumePseudo(L":empty",isEmpty()) ||
        !consumePseudo(L":first-child",isEdgeChild(true)) ||
        !consumePseudo(L":last-child",isEdgeChild(false)) ||
        !consumePseudo(L":first-of-type",isEdgeOfType(true)) ||
        !consumePseudo(L":last-of-type",isEdgeOfType(false)) ||
        !consumePseudo(L":only-child",elementSiblingCount(false)==1) ||
        !consumePseudo(L":only-of-type",elementSiblingCount(true)==1)) return false;
    if(selector.find(L":active")!=std::wstring::npos||
       selector.find(L":visited")!=std::wstring::npos)return false;
    // Unsupported pseudo-classes invalidate a selector. Removing only the
    // colon would make a state rule match every element with its base selector.
    if(selector.find(L':')!=std::wstring::npos)return false;

    size_t i = 0;
    if (i < selector.size() && (std::iswalpha(selector[i]) || selector[i] == L'_')) {
        const size_t begin = i++;
        while (i < selector.size() && (std::iswalnum(selector[i]) || selector[i] == L'-' || selector[i] == L'_')) ++i;
        if (node->tag != ToLower(selector.substr(begin, i - begin))) return false;
    }
    while (i < selector.size()) {
        if (selector[i] == L'#' || selector[i] == L'.') {
            const wchar_t kind = selector[i++];
            const size_t begin = i;
            while (i < selector.size() && (std::iswalnum(selector[i]) || selector[i] == L'-' || selector[i] == L'_')) ++i;
            const auto word = selector.substr(begin, i - begin);
            if (kind == L'#' && node->Attribute(L"id") != word) return false;
            if (kind == L'.' && !node->HasClass(word)) return false;
        } else if (selector[i] == L'[') {
            const size_t close = selector.find(L']', i + 1);
            if (close == std::wstring::npos) return false;
            const auto expression = Trim(selector.substr(i + 1, close - i - 1));
            const size_t equals=expression.find(L'=');
            size_t operatorPosition=equals;
            std::wstring attributeOperator;
            if(equals!=std::wstring::npos){
                if(equals>0&&std::wstring(L"~|^$*").find(expression[equals-1])!=std::wstring::npos){
                    operatorPosition=equals-1;attributeOperator=expression.substr(equals-1,2);
                }else attributeOperator=L"=";
            }
            if (operatorPosition == std::wstring::npos) {
                if (node->attributes.count(ToLower(expression)) == 0) return false;
            } else {
                auto key = ToLower(Trim(expression.substr(0, operatorPosition)));
                auto value = Trim(expression.substr(operatorPosition + attributeOperator.size()));
                bool caseInsensitive=false;
                if(value.size()>=2&&std::iswspace(value[value.size()-2])&&
                   (value.back()==L'i'||value.back()==L'I')){
                    caseInsensitive=true;value=Trim(value.substr(0,value.size()-1));
                }
                if (value.size() >= 2 &&
                    ((value.front() == L'\'' && value.back() == L'\'') ||
                     (value.front() == L'"' && value.back() == L'"')))
                    value = value.substr(1, value.size() - 2);
                if(node->attributes.count(key)==0)return false;
                auto actual=node->Attribute(key);
                if(caseInsensitive){actual=ToLower(actual);value=ToLower(value);}
                bool matches=false;
                if(attributeOperator==L"=")matches=actual==value;
                else if(attributeOperator==L"^=")matches=actual.rfind(value,0)==0;
                else if(attributeOperator==L"$=")matches=actual.size()>=value.size()&&
                    actual.compare(actual.size()-value.size(),value.size(),value)==0;
                else if(attributeOperator==L"*=")matches=actual.find(value)!=std::wstring::npos;
                else if(attributeOperator==L"~="){
                    std::wistringstream words(actual);std::wstring word;
                    while(words>>word)if(word==value){matches=true;break;}
                }else if(attributeOperator==L"|=")matches=actual==value||
                    (actual.size()>value.size()&&actual.rfind(value+L"-",0)==0);
                if(!matches)return false;
            }
            i = close + 1;
        } else {
            ++i; // Unknown pseudo/state is ignored so one rule cannot abort parsing.
        }
    }
    return true;
}

std::vector<std::wstring> SplitSelector(std::wstring selector) {
    std::vector<std::wstring> result;std::wstring current;int bracket=0;
    auto flush=[&](){auto value=Trim(current);if(!value.empty())result.push_back(value);current.clear();};
    for(wchar_t c:selector){
        if(c==L'['||c==L'(')++bracket;
        if(c==L']'||c==L')')--bracket;
        if(bracket==0&&(c==L'>'||c==L'+'||c==L'~')){flush();result.push_back(std::wstring(1,c));}
        else if(bracket==0&&IsSpace(c))flush();
        else current+=c;
    }
    flush();
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
    int nesting = 0;
    size_t start = 0;
    for (size_t i = 0; i <= selector.size(); ++i) {
        const wchar_t c = i < selector.size() ? selector[i] : L',';
        if (c == L'[' || c == L'(') ++nesting;
        if (c == L']' || c == L')') --nesting;
        if (c != L',' || nesting != 0) continue;
        auto item = Trim(selector.substr(start, i - start));
        start = i + 1;
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

class HtmlParser {
public:
    explicit HtmlParser(const std::wstring& html,bool scriptingEnabled) : html_(html),scriptingEnabled_(scriptingEnabled) {}
    bool QuirksMode() const {return quirksMode_;}
    bool LimitedQuirksMode() const {return limitedQuirksMode_;}

    std::shared_ptr<Node> Parse(bool fragment, std::wstring* error) {
        auto root = std::make_shared<Node>();
        root->type = fragment ? NodeType::Element : NodeType::Document;
        root->tag = fragment ? L"fragment" : L"#document";
        std::vector<std::shared_ptr<Node>> stack{root};

        while (position_ < html_.size()) {
            if (html_[position_] != L'<') {
                const size_t end = html_.find(L'<', position_);
                auto text = DecodeEntities(html_.substr(position_, end - position_));
                position_ = end == std::wstring::npos ? html_.size() : end;
                if (!text.empty()) {
                    if(std::any_of(text.begin(),text.end(),[](wchar_t c){return !IsSpace(c)&&c!=0xfeff;}))initial_=false;
                    auto node = std::make_shared<Node>();
                    node->type = NodeType::Text;
                    node->tag = L"#text";
                    node->text = text;
                    node->parent = stack.back();
                    stack.back()->children.push_back(node);
                }
                continue;
            }
            if (Starts(L"<!--")) {
                const size_t end = html_.find(L"-->", position_ + 4);
                auto node = std::make_shared<Node>();
                node->type = NodeType::Comment;
                node->tag = L"#comment";
                node->text = html_.substr(position_ + 4,
                    (end == std::wstring::npos ? html_.size() : end) - position_ - 4);
                node->parent = stack.back();
                stack.back()->children.push_back(node);
                position_ = end == std::wstring::npos ? html_.size() : end + 3;
                continue;
            }
            if (Starts(L"<!") || Starts(L"<?")) {
                const size_t end = html_.find(L'>', position_ + 2);
                if(initial_&&!fragment&&ToLower(html_.substr(position_,9))==L"<!doctype"){
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
                const auto name = ReadName();
                const size_t end = html_.find(L'>', position_);
                position_ = end == std::wstring::npos ? html_.size() : end + 1;
                CloseOpenElement(stack, name);
                continue;
            }

            ++position_;
            const auto tag = ReadName();
            if (tag.empty()) { ++position_; continue; }
            initial_=false;
            // HTML permits the </p> end tag to be omitted. Starting another
            // paragraph or a block element implicitly closes the open one.
            if (ClosesOpenParagraph(tag)) CloseOpenElement(stack, L"p");
            auto node = std::make_shared<Node>();
            node->type = NodeType::Element;
            node->tag = tag;
            SkipSpace();
            bool selfClosing = false;
            while (position_ < html_.size() && html_[position_] != L'>') {
                if (html_[position_] == L'/' && position_ + 1 < html_.size() && html_[position_ + 1] == L'>') {
                    selfClosing = true;
                    position_ += 2;
                    break;
                }
                const auto key = ReadName();
                if (key.empty()) { ++position_; continue; }
                SkipSpace();
                std::wstring value;
                if (position_ < html_.size() && html_[position_] == L'=') {
                    ++position_; SkipSpace();
                    if (position_ < html_.size() && (html_[position_] == L'\'' || html_[position_] == L'"')) {
                        const wchar_t quote = html_[position_++];
                        const size_t end = html_.find(quote, position_);
                        value = html_.substr(position_, end - position_);
                        position_ = end == std::wstring::npos ? html_.size() : end + 1;
                    } else {
                        const size_t begin = position_;
                        while (position_ < html_.size() && !IsSpace(html_[position_]) && html_[position_] != L'>') ++position_;
                        value = html_.substr(begin, position_ - begin);
                    }
                }
                node->attributes[key] = DecodeEntities(value);
                SkipSpace();
            }
            if (position_ < html_.size() && html_[position_] == L'>') ++position_;
            node->checked = node->attributes.count(L"checked") != 0;
            node->cryptographicNonce = node->Attribute(L"nonce");
            node->disabled = node->attributes.count(L"disabled") != 0;
            const auto style = node->Attribute(L"style");
            if (!style.empty()) ParseStyleAttribute(style, node->inlineStyle,
                                                     &node->inlineStylePriority);
            node->parent = stack.back();
            stack.back()->children.push_back(node);

            if ((tag == L"script" || tag == L"style" || (tag == L"noscript" && scriptingEnabled_)) && !selfClosing) {
                const std::wstring closing = L"</" + tag;
                const size_t end = ToLower(html_).find(closing, position_);
                auto textNode = std::make_shared<Node>();
                textNode->type = NodeType::Text;
                textNode->tag = L"#text";
                textNode->text = html_.substr(position_, end - position_);
                textNode->parent = node;
                node->children.push_back(textNode);
                if (end == std::wstring::npos) position_ = html_.size();
                else {
                    const size_t close = html_.find(L'>', end + closing.size());
                    position_ = close == std::wstring::npos ? html_.size() : close + 1;
                }
            } else if (!selfClosing && !IsVoidTag(tag)) {
                stack.push_back(node);
            }
        }
        if (!fragment) NormalizeDocumentTree(root);
        if (error) error->clear();
        return root;
    }

private:
    bool Starts(const wchar_t* value) const {
        const size_t count = std::wcslen(value);
        return position_ + count <= html_.size() && html_.compare(position_, count, value) == 0;
    }
    void SkipSpace() { while (position_ < html_.size() && IsSpace(html_[position_])) ++position_; }
    std::wstring ReadName() {
        SkipSpace();
        const size_t begin = position_;
        while (position_ < html_.size()) {
            const wchar_t c = html_[position_];
            if (!std::iswalnum(c) && c != L'-' && c != L'_' && c != L':') break;
            ++position_;
        }
        return ToLower(html_.substr(begin, position_ - begin));
    }
    const std::wstring& html_;
    bool scriptingEnabled_;
    size_t position_ = 0;
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

std::wstring DecodeEntities(const std::wstring& value) {
    std::wstring out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != L'&') { out += value[i]; continue; }
        const size_t end = value.find(L';', i + 1);
        if (end == std::wstring::npos || end - i > 12) { out += value[i]; continue; }
        const auto entity = value.substr(i + 1, end - i - 1);
        if (entity == L"lt") out += L'<';
        else if (entity == L"gt") out += L'>';
        else if (entity == L"amp") out += L'&';
        else if (entity == L"quot") out += L'"';
        else if (entity == L"apos" || entity == L"#39") out += L'\'';
        else if (entity == L"nbsp") out += static_cast<wchar_t>(0x00a0);
        else if (!entity.empty() && entity[0] == L'#') {
            const int base=entity.size()>1&&(entity[1]==L'x'||entity[1]==L'X')?16:10;
            const auto digits=entity.substr(base==16?2:1);size_t used=0;unsigned long long codePoint=0;
            if(TryParseUnsignedInteger(digits,codePoint,&used,base)&&used==digits.size()&&codePoint<=0xffff)
                out+=static_cast<wchar_t>(codePoint);
            else out+=L'&'+entity+L';';
        } else { out += L'&' + entity + L';'; }
        i = end;
    }
    return out;
}

std::wstring Node::Attribute(const std::wstring& name) const {
    // Parsed and DOM-authored attribute keys are stored lowercase. The common
    // path uses lowercase literals, so avoid allocating a temporary string for
    // every lookup and only normalize genuinely mixed-case input.
    const auto direct = attributes.find(name);
    if (direct != attributes.end()) return direct->second;
    if (xml) return L"";
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
        inlineStyle.clear(); inlineStylePriority.clear();
        ParseStyleAttribute(value, inlineStyle, &inlineStylePriority);
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
    if (key == L"style") { inlineStyle.clear(); inlineStylePriority.clear(); }
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
                                                            std::wstring* error) {
    HtmlParser parser(html,scriptingEnabled_);
    auto fragment = parser.Parse(true, error);
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
    if (parts.empty()) return false;
    auto current=node;int index=static_cast<int>(parts.size())-1;
    if(parts[index]==L">"||parts[index]==L"+"||!MatchSimple(current,parts[index]))return false;
    --index;
    while(index>=0){
        bool direct=false,adjacent=false,generalSibling=false;
        if(parts[index]==L">"||parts[index]==L"+"||parts[index]==L"~"){
            direct=parts[index]==L">";adjacent=parts[index]==L"+";
            generalSibling=parts[index]==L"~";--index;if(index<0)return false;
        }
        if(adjacent||generalSibling){
            auto parent=current?current->parent.lock():nullptr;
            std::vector<std::shared_ptr<Node>> previous;
            if(parent)for(const auto& sibling:parent->children){
                if(sibling==current)break;
                if(sibling->type==NodeType::Element)previous.push_back(sibling);
            }
            current.reset();
            for(auto it=previous.rbegin();it!=previous.rend();++it)
                if(adjacent||MatchSimple(*it,parts[index])){current=*it;break;}
        }else current=current?current->parent.lock():nullptr;
        if(direct||adjacent){if(!current||!MatchSimple(current,parts[index]))return false;}
        else if(generalSibling){if(!current)return false;}
        else{while(current&&!MatchSimple(current,parts[index]))current=current->parent.lock();if(!current)return false;}
        --index;
    }
    return true;
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
    return node;
}

void Document::SetInnerHtml(const std::shared_ptr<Node>& node, const std::wstring& html,
                            bool reindex) {
    if (!node) return;
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
            auto text = std::make_shared<Node>();
            text->type = NodeType::Text;
            text->tag = L"#text";
            text->text = rcdata ? DecodeEntities(html) : html;
            children.push_back(std::move(text));
        }
    } else {
        children = ParseFragment(html);
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
    for (const auto& entry : ownedNodes_)
        if (const auto node = entry.second.lock(); node && node->ownerDocument == this)
            node->ownerDocument = nullptr;
    ownedNodes_.clear();
    ids_.clear();idCounts_.clear();nameCounts_.clear();tags_.clear();classes_.clear();
    Walk(root_, [&](const auto& node) {
        node->ownerDocument = this;
        ownedNodes_[node.get()] = node;
        if(node->shadowRoot){
            std::function<void(const std::shared_ptr<Node>&)> bindShadow=[&](const auto& current){
                current->ownerDocument=this;ownedNodes_[current.get()]=current;
                for(const auto& child:current->children)bindShadow(child);
                if(current->shadowRoot)bindShadow(current->shadowRoot);
            };
            bindShadow(node->shadowRoot);
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
    auto root=node;while(auto parent=root->parent.lock())root=std::move(parent);
    if(root!=root_){
        std::function<void(const std::shared_ptr<Node>&)> bind=[&](const auto& current){
            current->ownerDocument=this;ownedNodes_[current.get()]=current;
            for(const auto& child:current->children)bind(child);
            if(current->shadowRoot)bind(current->shadowRoot);
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
