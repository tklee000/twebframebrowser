#include "CSS.h"
#include "NumericParser.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <functional>
#include <sstream>

namespace TWebFrame::Internal {
namespace {

std::wstring StripComments(const std::wstring& css) {
    std::wstring out;
    for (size_t i = 0; i < css.size();) {
        if (i + 1 < css.size() && css[i] == L'/' && css[i + 1] == L'*') {
            const size_t end = css.find(L"*/", i + 2);
            i = end == std::wstring::npos ? css.size() : end + 2;
        } else out += css[i++];
    }
    return out;
}

std::vector<std::wstring> Split(const std::wstring& value, wchar_t delimiter) {
    std::vector<std::wstring> result;
    int nesting = 0; wchar_t quote = 0; size_t start = 0;
    for (size_t i = 0; i <= value.size(); ++i) {
        const wchar_t c = i < value.size() ? value[i] : delimiter;
        if (quote) { if (c == quote && (i == 0 || value[i - 1] != L'\\')) quote = 0; }
        else if (c == L'\'' || c == L'"') quote = c;
        else if (c == L'(' || c == L'[') ++nesting;
        else if (c == L')' || c == L']') --nesting;
        else if (c == delimiter && nesting == 0) {
            auto item = Trim(value.substr(start, i - start));
            if (!item.empty()) result.push_back(item);
            start = i + 1;
        }
    }
    return result;
}

std::vector<std::wstring> SplitWhitespace(const std::wstring& value) {
    std::vector<std::wstring> result;
    std::wstring current;
    int nesting = 0;wchar_t quote=0;
    for (wchar_t c : value) {
        if(quote){current+=c;if(c==quote)quote=0;continue;}
        if(c==L'\''||c==L'"'){quote=c;current+=c;continue;}
        if (c == L'(' || c == L'[') ++nesting;
        if (c == L')' || c == L']') --nesting;
        if (std::iswspace(c) && nesting == 0) {
            if (!current.empty()) { result.push_back(current); current.clear(); }
        } else current += c;
    }
    if (!current.empty()) result.push_back(current);
    return result;
}

std::wstring BorderColorFromShorthand(const std::wstring& value) {
    constexpr unsigned int invalid = 0x01020304u;
    for (const auto& token : SplitWhitespace(value)) {
        const auto lowered = ToLower(Trim(token));
        if (lowered == L"currentcolor" ||
            StyleSheet::Color(token, invalid) != invalid) {
            return token;
        }
    }
    return value;
}

struct BackgroundShorthand {
    std::wstring image = L"none";
    std::wstring position = L"0% 0%";
    std::wstring size = L"auto";
    std::wstring repeat = L"repeat";
    std::wstring color = L"transparent";
};

bool IsBackgroundImageToken(const std::wstring& token) {
    const auto lowered=ToLower(Trim(token));
    return lowered.rfind(L"url(",0)==0||lowered.find(L"gradient(")!=std::wstring::npos||
           lowered==L"none";
}

bool IsBackgroundRepeatToken(const std::wstring& token) {
    const auto lowered=ToLower(Trim(token));
    return lowered==L"repeat"||lowered==L"repeat-x"||lowered==L"repeat-y"||
           lowered==L"no-repeat"||lowered==L"space"||lowered==L"round";
}

size_t BackgroundSizeSlash(const std::wstring& token) {
    int nesting=0;wchar_t quote=0;
    for(size_t index=0;index<token.size();++index){
        const auto character=token[index];
        if(quote){if(character==quote&&(index==0||token[index-1]!=L'\\'))quote=0;continue;}
        if(character==L'\''||character==L'"'){quote=character;continue;}
        if(character==L'('||character==L'['){++nesting;continue;}
        if(character==L')'||character==L']'){--nesting;continue;}
        if(character==L'/'&&nesting==0)return index;
    }
    return std::wstring::npos;
}

BackgroundShorthand ParseBackgroundLayer(const std::wstring& value,bool finalLayer) {
    BackgroundShorthand result;std::vector<std::wstring> position,size,repeat;
    bool afterSlash=false;constexpr unsigned int invalid=0x01020304u;
    for(const auto& original:SplitWhitespace(value)){
        auto token=original;
        const auto slash=BackgroundSizeSlash(token);
        std::vector<std::wstring> pieces;
        if(slash!=std::wstring::npos){
            if(slash)pieces.push_back(token.substr(0,slash));
            pieces.push_back(L"/");
            if(slash+1<token.size())pieces.push_back(token.substr(slash+1));
        }else pieces.push_back(token);
        for(const auto& piece:pieces){
            if(piece==L"/"){afterSlash=true;continue;}
            const auto lowered=ToLower(Trim(piece));if(lowered.empty())continue;
            if(IsBackgroundImageToken(piece)){result.image=piece;continue;}
            if(IsBackgroundRepeatToken(piece)){repeat.push_back(piece);continue;}
            if(lowered==L"scroll"||lowered==L"fixed"||lowered==L"local"||
               lowered==L"border-box"||lowered==L"padding-box"||lowered==L"content-box")continue;
            if(finalLayer&&StyleSheet::Color(piece,invalid)!=invalid){result.color=piece;continue;}
            (afterSlash?size:position).push_back(piece);
        }
    }
    const auto join=[](const std::vector<std::wstring>& values,const wchar_t* fallback){
        if(values.empty())return std::wstring(fallback);std::wstring joined;
        for(const auto& value:values){if(!joined.empty())joined+=L' ';joined+=value;}return joined;
    };
    result.position=join(position,L"0% 0%");result.size=join(size,L"auto");
    result.repeat=join(repeat,L"repeat");return result;
}

BackgroundShorthand ParseBackgroundShorthand(const std::wstring& value) {
    const auto layers=Split(value,L',');BackgroundShorthand result;
    std::vector<std::wstring> images,positions,sizes,repeats;
    for(size_t index=0;index<layers.size();++index){
        const auto layer=ParseBackgroundLayer(layers[index],index+1==layers.size());
        images.push_back(layer.image);positions.push_back(layer.position);
        sizes.push_back(layer.size);repeats.push_back(layer.repeat);
        if(index+1==layers.size())result.color=layer.color;
    }
    const auto join=[](const std::vector<std::wstring>& values){std::wstring joined;
        for(const auto& value:values){if(!joined.empty())joined+=L", ";joined+=value;}return joined;};
    if(!images.empty()){result.image=join(images);result.position=join(positions);
        result.size=join(sizes);result.repeat=join(repeats);}
    return result;
}

std::wstring SelectorSubjectKey(const std::vector<std::wstring>& parts) {
    if(parts.empty())return {};
    const auto& subject=parts.back();
    std::wstring id,className,tag;
    size_t index=0;
    if(index<subject.size()&&(std::iswalpha(subject[index])||subject[index]==L'_')){
        const auto begin=index++;
        while(index<subject.size()&&(std::iswalnum(subject[index])||
              subject[index]==L'-'||subject[index]==L'_'))++index;
        tag=ToLower(subject.substr(begin,index-begin));
    }
    int nesting=0;
    for(size_t position=0;position<subject.size();++position){
        const auto c=subject[position];
        if(c==L'['||c==L'('){++nesting;continue;}
        if(c==L']'||c==L')'){--nesting;continue;}
        if(nesting||c!=L'#'&&c!=L'.')continue;
        const auto begin=position+1;auto end=begin;
        while(end<subject.size()&&(std::iswalnum(subject[end])||
              subject[end]==L'-'||subject[end]==L'_'))++end;
        if(end==begin)continue;
        if(c==L'#')id=subject.substr(begin,end-begin);
        else if(className.empty())className=subject.substr(begin,end-begin);
        position=end-1;
    }
    if(!id.empty())return L"#"+id;
    if(!className.empty())return L"."+className;
    return tag.empty()?std::wstring{}:L"<"+tag;
}

void CollectSelectorAttributes(const std::wstring& selector,
                               FastMap<std::wstring,bool>& attributes) {
    for(size_t position=0;(position=selector.find(L'[',position))!=std::wstring::npos;){
        const auto close=selector.find(L']',position+1);
        if(close==std::wstring::npos)break;
        size_t begin=position+1;
        while(begin<close&&std::iswspace(selector[begin]))++begin;
        size_t end=begin;
        while(end<close&&(std::iswalnum(selector[end])||selector[end]==L'-'||
              selector[end]==L'_'||selector[end]==L':'))++end;
        if(end>begin)attributes[ToLower(selector.substr(begin,end-begin))]=true;
        position=close+1;
    }
}

std::vector<std::wstring> ExpandEdges(const std::wstring& value) {
    const auto parts = SplitWhitespace(value);
    if (parts.empty()) return {};
    if (parts.size() == 1) return {parts[0], parts[0], parts[0], parts[0]};
    if (parts.size() == 2) return {parts[0], parts[1], parts[0], parts[1]};
    if (parts.size() == 3) return {parts[0], parts[1], parts[2], parts[1]};
    return {parts[0], parts[1], parts[2], parts[3]};
}

std::wstring ResolveViewportUnits(std::wstring value,float viewportWidth,float viewportHeight) {
    for(size_t position=0;position<value.size();){
        size_t unitLength=0;float reference=0;
        if(position+1<value.size()&&value[position]==L'v'&&value[position+1]==L'w'){
            unitLength=2;reference=viewportWidth;
        }else if(position+1<value.size()&&value[position]==L'v'&&value[position+1]==L'h'){
            unitLength=2;reference=viewportHeight;
        }else if(position+3<value.size()&&value.compare(position,4,L"vmin")==0){
            unitLength=4;reference=std::min(viewportWidth,viewportHeight);
        }else if(position+3<value.size()&&value.compare(position,4,L"vmax")==0){
            unitLength=4;reference=std::max(viewportWidth,viewportHeight);
        }
        if(!unitLength){++position;continue;}
        size_t begin=position;
        while(begin>0&&(std::iswdigit(value[begin-1])||value[begin-1]==L'.'))--begin;
        if(begin>0&&(value[begin-1]==L'+'||value[begin-1]==L'-')&&
           (begin==1||value[begin-2]==L'('||value[begin-2]==L','||std::iswspace(value[begin-2])))--begin;
        float number=0;
        if(TryParseFloat(value.substr(begin,position-begin),number)){
            const auto replacement=std::to_wstring(number*reference/100.0f)+L"px";
            value.replace(begin,position+unitLength-begin,replacement);
            position=begin+replacement.size();
        }else position+=unitLength;
    }
    return value;
}

int Specificity(const std::wstring& selector) {
    int ids = 0, classes = 0, tags = 0;
    bool inAttribute = false;
    for (size_t i = 0; i < selector.size(); ++i) {
        const wchar_t c = selector[i];
        if (c == L'#') ++ids;
        else if (c == L'.' || c == L':' || c == L'[') { ++classes; inAttribute = c == L'['; }
        else if (c == L']') inAttribute = false;
        else if (!inAttribute && (i == 0 || std::iswspace(selector[i - 1]) || selector[i - 1] == L',' || selector[i - 1] == L'>') && std::iswalpha(c)) ++tags;
    }
    return ids * 100 + classes * 10 + tags;
}

void SetDefault(const std::shared_ptr<Node>& node, ComputedStyle& style) {
    auto set = [&](const wchar_t* key, const wchar_t* value) {
        if (!style.values->count(key)) (*style.values)[key] = value;
    };
    set(L"box-sizing", L"content-box");
    set(L"position", L"static");
    set(L"color", L"#000000");
    set(L"font-family", L"Segoe UI");
    set(L"font-size", L"16px");
    set(L"font-style", L"normal");
    set(L"font-weight", L"400");
    set(L"tab-size", L"8");
    set(L"text-align", L"left");
    set(L"overflow", L"visible");
    set(L"pointer-events", L"auto");
    set(L"visibility", L"visible");
    set(L"flex-grow", L"0");
    set(L"flex-shrink", L"1");
    set(L"flex-direction", L"row");
    set(L"flex-wrap", L"nowrap");
    set(L"list-style-position", L"outside");
    set(L"list-style-type", L"disc");
    std::wstring display = L"block";
    if (!node || node->type == NodeType::Text) display = L"inline";
    else if (node->tag == L"a" || node->tag == L"abbr" || node->tag == L"b" ||
             node->tag == L"bdi" || node->tag == L"bdo" || node->tag == L"br" ||
             node->tag == L"cite" || node->tag == L"code" || node->tag == L"data" ||
             node->tag == L"del" || node->tag == L"dfn" || node->tag == L"em" ||
             node->tag == L"i" || node->tag == L"ins" || node->tag == L"kbd" || node->tag == L"font" ||
             node->tag == L"label" || node->tag == L"mark" ||
             node->tag == L"q" || node->tag == L"s" || node->tag == L"samp" ||
             node->tag == L"small" || node->tag == L"span" || node->tag == L"strong" ||
             node->tag == L"sub" || node->tag == L"sup" || node->tag == L"time" ||
             node->tag == L"u" || node->tag == L"var" || node->tag == L"wbr") display = L"inline";
    else if (node->tag == L"button" || node->tag == L"canvas" || node->tag == L"embed" ||
             node->tag == L"iframe" || node->tag == L"img" || node->tag == L"input" ||
             node->tag == L"object" || node->tag == L"select" || node->tag == L"textarea" ||
             node->tag == L"video" || node->tag == L"svg")
        display = L"inline-block";
    else if (node->tag == L"table") display = L"table";
    else if (node->tag == L"thead") display = L"table-header-group";
    else if (node->tag == L"tbody") display = L"table-row-group";
    else if (node->tag == L"tfoot") display = L"table-footer-group";
    else if (node->tag == L"tr") display = L"table-row";
    else if (node->tag == L"td" || node->tag == L"th") display = L"table-cell";
    else if (node->tag == L"li") display = L"list-item";
    else if (node->tag == L"head" || node->tag == L"style" || node->tag == L"script" || node->tag == L"meta" || node->tag == L"title" || node->tag == L"col" || node->tag == L"colgroup" || node->tag == L"option" || node->tag == L"datalist" || node->tag == L"template" || (node->tag == L"noscript" && node->ownerDocument && node->ownerDocument->ScriptingEnabled()) || (node->tag == L"dialog" && !node->attributes.count(L"open")) || node->attributes.count(L"hidden")) display = L"none";
    set(L"display", display.c_str());
    if (node && node->tag == L"dialog" && node->attributes.count(L"open")) {
        (*style.values)[L"position"] = L"fixed";
        (*style.values)[L"left"] = L"50%"; (*style.values)[L"top"] = L"50%";
        (*style.values)[L"transform"] = L"translate(-50%, -50%)";
        (*style.values)[L"z-index"] = L"10000";
    }
    if (node && node->tag == L"body") {
        set(L"margin", L"8px");
        // Preserve the legacy HTML body background hint as a low-priority
        // presentational rule. Author CSS still wins through the normal cascade.
        const auto background=node->Attribute(L"background");
        if(!background.empty()&&!style.values->count(L"background-image"))
            (*style.values)[L"background-image"]=L"url(\""+background+L"\")";
    }
    if (node && node->tag == L"p") set(L"margin", L"1em 0");
    if (node && (node->tag == L"ul" || node->tag == L"ol")) {
        set(L"margin", L"1em 0");
        set(L"padding-left", L"40px");
        (*style.values)[L"list-style-type"] = node->tag == L"ol" ? L"decimal" : L"disc";
    }
    if (node && node->tag.size() == 2 && node->tag[0] == L'h' &&
        node->tag[1] >= L'1' && node->tag[1] <= L'6') {
        static const wchar_t* sizes[] = {L"2em",L"1.5em",L"1.17em",L"1em",L"0.83em",L"0.67em"};
        static const wchar_t* margins[] = {L"0.67em",L"0.83em",L"1em",L"1.33em",L"1.67em",L"2.33em"};
        (*style.values)[L"font-size"] = sizes[node->tag[1]-L'1'];
        (*style.values)[L"font-weight"] = L"700";
        (*style.values)[L"margin"] = std::wstring(margins[node->tag[1]-L'1']) + L" 0";
    }
    if (node && (node->tag == L"b" || node->tag == L"strong"))
        (*style.values)[L"font-weight"] = L"700";
    if (node && (node->tag == L"em" || node->tag == L"i" || node->tag == L"var" ||
                 node->tag == L"cite" || node->tag == L"dfn"))
        (*style.values)[L"font-style"] = L"italic";
    if (node && ((node->tag == L"a" && !node->Attribute(L"href").empty()) ||
                 node->tag == L"ins" || node->tag == L"u"))
        (*style.values)[L"text-decoration"] = L"underline";
    if (node && (node->tag == L"del" || node->tag == L"s"))
        (*style.values)[L"text-decoration"] = L"line-through";
    if (node && (node->tag == L"code" || node->tag == L"kbd" ||
                 node->tag == L"samp" || node->tag == L"pre"))
        (*style.values)[L"font-family"] = L"monospace";
    if (node && node->tag == L"hr") {
        set(L"height", L"1px"); set(L"margin", L"0.5em 0");
        set(L"border", L"0"); set(L"background", L"#808080");
    }
    if (node && (node->tag == L"button" || node->tag == L"input" || node->tag == L"select")) {
        // Chromium's native form controls use the small-control font instead
        // of inheriting the surrounding document font shorthand.
        (*style.values)[L"font-family"] = L"Arial";
        (*style.values)[L"font-size"] = L"13.3333px";
        (*style.values)[L"font-weight"] = L"400";
        (*style.values)[L"line-height"] = L"normal";
    }
    if(node&&(node->tag==L"button"||node->tag==L"select"||
       (node->tag==L"input"&&(node->Attribute(L"type")==L"button"||node->Attribute(L"type")==L"submit"||node->Attribute(L"type")==L"reset"))))
        (*style.values)[L"box-sizing"]=L"border-box";
    if(node&&node->tag==L"input"&&(node->Attribute(L"type")==L"button"||node->Attribute(L"type")==L"submit"||node->Attribute(L"type")==L"reset"))
        (*style.values)[L"text-align"]=L"center";
    if (node && node->tag == L"button") {
        set(L"padding", L"2px 6px"); set(L"border", L"1px solid #767676");
        set(L"background", L"#f0f0f0");
        // Keep native-style button labels on one line while still collapsing
        // indentation and newlines from formatted HTML source.
        set(L"white-space", L"nowrap");
        // A button's user-agent style specifies centered text on the element
        // itself.  It therefore takes precedence over an inherited text-align,
        // while an author rule applied later in the cascade can still override it.
        (*style.values)[L"text-align"] = L"center";
    }
    if (node && node->tag == L"select") {
        set(L"border", L"1px solid #767676"); set(L"background", L"white");
    }
    if (node && node->tag == L"input" &&
        (node->Attribute(L"type") == L"checkbox" || node->Attribute(L"type") == L"radio")) {
        // Chromium's user-agent stylesheet keeps this asymmetric margin even
        // when author CSS supplies an explicit checkbox/radio width and height.
        set(L"margin", L"3px 3px 3px 4px");
    }
    if (node && node->tag == L"th") (*style.values)[L"font-weight"] = L"700";
    if(node&&(node->tag==L"td"||node->tag==L"th")){
        std::wstring cellPadding=L"1px";
        for(auto ancestor=node->parent.lock();ancestor;ancestor=ancestor->parent.lock())
            if(ancestor->tag==L"table"){
                const auto authored=Trim(ancestor->Attribute(L"cellpadding"));
                if(!authored.empty())cellPadding=authored;
                break;
            }
        set(L"padding",cellPadding.c_str());
    }
    if(node&&(node->tag==L"thead"||node->tag==L"tbody"||node->tag==L"tfoot"||
       node->tag==L"tr"||node->tag==L"td"||node->tag==L"th")){
        const auto parent=node->parent.lock();
        const bool rowGroup=node->tag==L"thead"||node->tag==L"tbody"||node->tag==L"tfoot";
        const auto valign=ToLower(Trim(node->Attribute(L"valign")));
        if(valign==L"top"||valign==L"middle"||valign==L"bottom"||valign==L"baseline")
            (*style.values)[L"vertical-align"]=valign;
        else set(L"vertical-align",rowGroup||(node->tag==L"tr"&&parent&&parent->tag==L"table")?
            L"middle":L"inherit");
    }
    if(node&&(node->tag==L"div"||node->tag==L"center"||node->tag==L"thead"||
       node->tag==L"tbody"||node->tag==L"tfoot"||node->tag==L"tr"||node->tag==L"td"||node->tag==L"th")){
        auto align=ToLower(Trim(node->Attribute(L"align")));
        if(node->tag==L"center"||align==L"middle")align=L"center";
        // Legacy HTML alignment also aligns narrower descendant blocks.
        // Keep that distinct from ordinary CSS text-align; author CSS can
        // replace this presentational hint through the normal cascade.
        if(align==L"left"||align==L"center"||align==L"right")
            (*style.values)[L"text-align"]=L"-webkit-"+align;
        else if(align==L"justify")(*style.values)[L"text-align"]=align;
    }
    if(node&&(node->tag==L"table"||node->tag==L"tr"||node->tag==L"td"||node->tag==L"th")){
        const auto height=Trim(node->Attribute(L"height"));
        if(!height.empty())set(L"height",height.c_str());
    }
    if (node && node->tag == L"input" && node->Attribute(L"type") != L"checkbox" &&
        node->Attribute(L"type") != L"radio") {
        set(L"border", L"1px solid #767676"); set(L"background", L"white");
    }
}

} // namespace

std::wstring ComputedStyle::Get(const std::wstring& name, const std::wstring& fallback) const {
    const bool alreadyLower=std::none_of(name.begin(),name.end(),[](wchar_t character){
        return std::iswupper(character)!=0;
    });
    auto it=values->end();
    if(alreadyLower||name.rfind(L"--",0)==0)it=values->find(name);
    else{const auto lowered=ToLower(name);it=values->find(lowered);}
    return it == values->end() ? fallback : it->second;
}

bool ComputedStyle::Is(const std::wstring& name, const std::wstring& value) const {
    const auto actual=Get(name);
    return actual.size()==value.size()&&std::equal(actual.begin(),actual.end(),value.begin(),
        [](wchar_t left,wchar_t right){return std::towlower(left)==std::towlower(right);});
}

SelectPopupPalette ResolveSelectPopupPalette(const ComputedStyle& style,
                                             unsigned int effectiveBackground) {
    auto color=[&](const wchar_t* property,unsigned int fallback){
        return StyleSheet::Color(style.Get(property),fallback);
    };
    auto scheme=ToLower(Trim(style.Get(L"color-scheme",L"light")));
    const auto separator=scheme.find_first_of(L" \t");
    if(separator!=std::wstring::npos)scheme.resize(separator);
    const bool dark=scheme==L"dark";
    return {
        color(L"--select-menu-border",dark?0xffa8a8a8:0xffd8d8d8),
        color(L"--select-menu-background",effectiveBackground),
        color(L"--select-menu-selected-background",dark?0xffc6c6c6:0xff767676),
        color(L"--select-menu-color",StyleSheet::Color(style.Get(L"color"),0xfff0f0f0)),
        color(L"--select-menu-selected-color",dark?0xff1b1b1b:0xffffffff),
        color(L"--select-menu-disabled-color",0xff777777)
    };
}

bool StyleSheet::Parse(const std::wstring& source, std::wstring* error) {
    ++version_;
    rules_.clear(); ruleIndex_.clear(); universalRuleIndexes_.clear();
    keyframes_.clear();shadowStyles_.clear();
    selectorAttributes_.clear(); nthChildSubjects_.clear();hasUniversalNthChild_=false;
    rootVariables_.clear(); pseudoRules_.clear(); hoverRuleIndexes_.clear();
    hoverRequiresBroadInvalidation_=false; mutationRequiresBroadInvalidation_=false;
    usesNthChild_ = false;
    usesViewportFontSize_ = false;
    const auto css = StripComments(source);
    int order = 0;
    auto ruleEnd=[](const std::wstring& text,size_t brace,size_t& end){
        int depth=1;wchar_t quote=0;end=brace+1;
        for(;end<text.size()&&depth;++end){
            const wchar_t c=text[end];
            if(quote){if(c==quote&&text[end-1]!=L'\\')quote=0;continue;}
            if(c==L'\''||c==L'"')quote=c;else if(c==L'{')++depth;else if(c==L'}')--depth;
        }
        return depth==0;
    };
    std::function<bool(const std::wstring&,const std::vector<std::wstring>&)> parseBlock;
    parseBlock=[&](const std::wstring& text,const std::vector<std::wstring>& mediaQueries){
        size_t position=0;
        while(position<text.size()){
            while(position<text.size()&&std::iswspace(text[position]))++position;
            if(position>=text.size())break;
            if(text[position]==L'@'){
                const size_t brace=text.find(L'{',position),semicolon=text.find(L';',position);
                if(semicolon!=std::wstring::npos&&(brace==std::wstring::npos||semicolon<brace)){position=semicolon+1;continue;}
                if(brace==std::wstring::npos)break;
                size_t end=0;if(!ruleEnd(text,brace,end)){if(error)*error=L"Unclosed CSS at-rule";return false;}
                const auto prelude=ToLower(Trim(text.substr(position,brace-position)));
                if(prelude.rfind(L"@media",0)==0){
                    auto nestedQueries=mediaQueries;nestedQueries.push_back(Trim(prelude.substr(6)));
                    if(!parseBlock(text.substr(brace+1,end-brace-2),nestedQueries))return false;
                }else if(prelude.rfind(L"@keyframes ",0)==0||prelude.rfind(L"@-webkit-keyframes ",0)==0){
                    CssKeyframes animation;animation.mediaQueries=mediaQueries;
                    const auto rawPrelude=Trim(text.substr(position,brace-position));
                    animation.name=Trim(rawPrelude.substr(rawPrelude.find_first_of(L" \t\r\n")));
                    const auto body=text.substr(brace+1,end-brace-2);size_t framePosition=0;
                    while(framePosition<body.size()){
                        const auto frameBrace=body.find(L'{',framePosition);if(frameBrace==std::wstring::npos)break;
                        size_t frameEnd=0;if(!ruleEnd(body,frameBrace,frameEnd))break;
                        std::vector<CssDeclaration> declarations;
                        for(const auto& part:Split(body.substr(frameBrace+1,frameEnd-frameBrace-2),L';')){
                            const auto colon=part.find(L':');if(colon==std::wstring::npos)continue;
                            CssDeclaration declaration;declaration.name=Trim(part.substr(0,colon));
                            if(declaration.name.rfind(L"--",0)!=0)declaration.name=ToLower(declaration.name);
                            declaration.value=Trim(part.substr(colon+1));
                            if(declaration.value.find(L"!important")==std::wstring::npos&&!declaration.name.empty())declarations.push_back(std::move(declaration));
                        }
                        for(auto selector:Split(body.substr(framePosition,frameBrace-framePosition),L',')){
                            selector=ToLower(Trim(selector));float offset=0;size_t consumed=0;
                            if(selector==L"from")offset=0;
                            else if(selector==L"to")offset=1;
                            else if(TryParseFloat(selector,offset,&consumed)&&selector.substr(consumed)==L"%"&&offset>=0&&offset<=100)offset/=100;
                            else continue;
                            animation.frames.push_back({offset,declarations});
                        }
                        framePosition=frameEnd;
                    }
                    std::stable_sort(animation.frames.begin(),animation.frames.end(),[](const CssKeyframe& a,const CssKeyframe& b){return a.offset<b.offset;});
                    if(!animation.name.empty()&&!animation.frames.empty())keyframes_.push_back(std::move(animation));
                }
                position=end;continue;
            }
            const size_t brace=text.find(L'{',position);if(brace==std::wstring::npos)break;
            size_t end=0;if(!ruleEnd(text,brace,end)){if(error)*error=L"Unclosed CSS rule";return false;}
            const auto selectors=Split(text.substr(position,brace-position),L',');
            const auto body=text.substr(brace+1,end-brace-2);std::vector<CssDeclaration> declarations;
            for(const auto& part:Split(body,L';')){
                const size_t colon=part.find(L':');if(colon==std::wstring::npos)continue;
                CssDeclaration declaration;declaration.name=Trim(part.substr(0,colon));
                if(declaration.name.rfind(L"--",0)!=0)declaration.name=ToLower(declaration.name);
                declaration.value=Trim(part.substr(colon+1));
                const auto important=declaration.value.rfind(L"!important");
                if(important!=std::wstring::npos&&Trim(declaration.value.substr(important))==L"!important"){
                    declaration.important=true;declaration.value=Trim(declaration.value.substr(0,important));
                }
                if(declaration.name==L"font-size"){
                    const auto lowered=ToLower(declaration.value);
                    usesViewportFontSize_=usesViewportFontSize_||lowered.find(L"vw")!=std::wstring::npos||
                        lowered.find(L"vh")!=std::wstring::npos||lowered.find(L"vmin")!=std::wstring::npos||
                        lowered.find(L"vmax")!=std::wstring::npos;
                }
                if(!declaration.name.empty())declarations.push_back(declaration);
            }
            for(const auto& selector:selectors){
                CssRule rule;
                rule.selector=selector;
                rule.declarations=declarations;
                rule.specificity=Specificity(selector);
                rule.order=order++;
                rule.mediaQueries=mediaQueries;
                auto pseudoSuffix=selector.rfind(L"::");size_t pseudoSeparator=2;
                if(pseudoSuffix==std::wstring::npos){
                    const auto colon=selector.rfind(L':');
                    const auto suffix=colon==std::wstring::npos?L"":ToLower(Trim(selector.substr(colon+1)));
                    if(suffix==L"before"||suffix==L"after"||suffix==L"first-line"||suffix==L"first-letter"){
                        pseudoSuffix=colon;pseudoSeparator=1;
                    }
                }
                if(pseudoSuffix!=std::wstring::npos)
                    rule.pseudo=ToLower(Trim(selector.substr(pseudoSuffix+pseudoSeparator)));
                if(pseudoSuffix!=std::wstring::npos)rule.specificity=Specificity(selector.substr(0,pseudoSuffix))+1;
                rule.selectorParts=Document::CompileSelector(
                    pseudoSuffix==std::wstring::npos?selector:selector.substr(0,pseudoSuffix));
                CollectSelectorAttributes(selector,selectorAttributes_);
                for(const auto& part:rule.selectorParts)
                    if(part.find(L":nth-child(")!=std::wstring::npos||
                       part.find(L":nth-of-type(")!=std::wstring::npos){
                        const auto key=SelectorSubjectKey({part});
                        if(key.empty())hasUniversalNthChild_=true;
                        else nthChildSubjects_[key]=true;
                    }
                rule.hasCustomDeclarations=std::any_of(declarations.begin(),declarations.end(),
                    [](const CssDeclaration& declaration){return declaration.name.rfind(L"--",0)==0;});
                const auto subjectKey=SelectorSubjectKey(rule.selectorParts);
                rules_.push_back(std::move(rule));
                const auto ruleIndex=rules_.size()-1;
                if(selector.find(L":hover")!=std::wstring::npos){
                    hoverRuleIndexes_.push_back(ruleIndex);
                    // A hovered node can invalidate outside its own subtree
                    // through sibling combinators or relational :has(). Keep
                    // those uncommon selectors on the conservative path.
                    hoverRequiresBroadInvalidation_=hoverRequiresBroadInvalidation_||
                        selector.find(L'+')!=std::wstring::npos||
                        selector.find(L'~')!=std::wstring::npos||
                        selector.find(L":has(")!=std::wstring::npos;
                }
                mutationRequiresBroadInvalidation_=mutationRequiresBroadInvalidation_||
                    selector.find(L'+')!=std::wstring::npos||
                    selector.find(L'~')!=std::wstring::npos||
                    selector.find(L":has(")!=std::wstring::npos;
                if(subjectKey.empty())universalRuleIndexes_.push_back(ruleIndex);
                else ruleIndex_[subjectKey].push_back(ruleIndex);
                if(selector.find(L":nth-child(")!=std::wstring::npos)usesNthChild_=true;
                if(!rules_.back().pseudo.empty()){
                    const auto& pseudo=rules_.back().pseudo;
                    if(std::find(pseudoRules_.begin(),pseudoRules_.end(),pseudo)==pseudoRules_.end())
                        pseudoRules_.push_back(pseudo);
                }
                if(mediaQueries.empty()&&Trim(selector)==L":root")for(const auto& declaration:declarations)
                    if(declaration.name.rfind(L"--",0)==0)rootVariables_[declaration.name]=declaration.value;
            }
            position=end;
        }
        return true;
    };
    if(!parseBlock(css,{}))return false;
    if (error) error->clear();
    return true;
}

std::vector<const CssRule*> StyleSheet::CandidateRules(const std::shared_ptr<Node>& node) const {
    std::vector<const CssRule*> result;
    if(!node||node->type!=NodeType::Element)return result;
    auto append=[&](const std::wstring& key){
        const auto found=ruleIndex_.find(key);
        if(found==ruleIndex_.end())return;
        for(const auto index:found->second)result.push_back(&rules_[index]);
    };
    result.reserve(universalRuleIndexes_.size()+24);
    for(const auto index:universalRuleIndexes_)result.push_back(&rules_[index]);
    append(L"<"+node->tag);
    const auto id=node->Attribute(L"id");
    if(!id.empty())append(L"#"+id);
    const auto classes=node->attributes.find(L"class");
    if(classes!=node->attributes.end()){
        const auto& value=classes->second;size_t position=0;
        while(position<value.size()){
            while(position<value.size()&&std::iswspace(value[position]))++position;
            const size_t start=position;
            while(position<value.size()&&!std::iswspace(value[position]))++position;
            if(position>start)append(L"."+value.substr(start,position-start));
        }
    }
    return result;
}

bool StyleSheet::AttributeAffectsStyle(const std::wstring& name) const {
    if(name==L"id"||name==L"class"||name==L"hidden"||name==L"open"||
       name==L"href"||name==L"fill"||name==L"fill-opacity"||
       name==L"fill-rule"||name==L"stroke"||name==L"stroke-opacity"||
       name==L"stroke-width"||name==L"stroke-linecap"||name==L"stroke-linejoin")return true;
    return selectorAttributes_.count(name)!=0;
}

bool StyleSheet::UsesNthChildFor(const std::shared_ptr<Node>& node) const {
    if(!usesNthChild_||!node||node->type!=NodeType::Element)return false;
    if(hasUniversalNthChild_||nthChildSubjects_.count(L"<"+node->tag))return true;
    const auto id=node->Attribute(L"id");
    if(!id.empty()&&nthChildSubjects_.count(L"#"+id))return true;
    const auto classes=node->attributes.find(L"class");
    if(classes!=node->attributes.end()){
        const auto& value=classes->second;size_t position=0;
        while(position<value.size()){
            while(position<value.size()&&std::iswspace(value[position]))++position;
            const size_t start=position;
            while(position<value.size()&&!std::iswspace(value[position]))++position;
            if(position>start&&nthChildSubjects_.count(L"."+value.substr(start,position-start)))return true;
        }
    }
    return false;
}

bool StyleSheet::HasPseudoRules(std::wstring_view pseudo) const {
    return std::any_of(pseudoRules_.begin(),pseudoRules_.end(),
        [&](const std::wstring& candidate){return candidate==pseudo;});
}

bool StyleSheet::HasPseudoRulesFor(const std::shared_ptr<Node>& node,std::wstring_view pseudo) const {
    if(!node||node->type!=NodeType::Element||!HasPseudoRules(pseudo))return false;
    for(const auto* rule:CandidateRules(node))
        if(rule->pseudo==pseudo&&RuleApplies(*rule,viewportWidth_,viewportHeight_)&&
           Document::MatchesSelector(node,rule->selectorParts))return true;
    return false;
}

bool StyleSheet::HoverStateAffects(const std::shared_ptr<Node>& node) const {
    if(!node||node->type!=NodeType::Element)return false;
    const bool previous=node->hovered;
    for(const auto index:hoverRuleIndexes_){
        if(index>=rules_.size())continue;
        const auto& rule=rules_[index];
        if(!RuleApplies(rule,viewportWidth_,viewportHeight_))continue;
        for(const auto& part:rule.selectorParts){
            if(part.find(L":hover")==std::wstring::npos)continue;
            node->hovered=false;
            const bool withoutHover=Document::MatchesSelector(node,std::vector<std::wstring>{part});
            node->hovered=true;
            const bool withHover=Document::MatchesSelector(node,std::vector<std::wstring>{part});
            node->hovered=previous;
            if(withoutHover!=withHover)return true;
        }
    }
    node->hovered=previous;
    return false;
}

bool StyleSheet::MediaQueryMatches(const std::wstring& source,double viewportWidth,double viewportHeight,
                                  double displayWidth,double displayHeight,double devicePixelRatio){
    if(displayWidth<=0)displayWidth=viewportWidth;
    if(displayHeight<=0)displayHeight=viewportHeight;
    const auto MediaLength=[](std::wstring value,double widthReference,double heightReference)->double{
        value=ToLower(Trim(value));size_t used=0;float number=0;
        if(!TryParseFloat(value,number,&used))return std::numeric_limits<double>::quiet_NaN();
        const auto unit=Trim(value.substr(used));
        if(unit.empty()||unit==L"px")return number;
        if(unit==L"em"||unit==L"rem")return number*16.0;
        if(unit==L"vw")return number*widthReference/100.0;
        if(unit==L"vh")return number*heightReference/100.0;
        return std::numeric_limits<double>::quiet_NaN();
    };
    const auto EvaluateMediaFeature=[&](std::wstring expression){
        expression=ToLower(Trim(expression));
        const auto colon=expression.find(L':');
        auto name=Trim(expression.substr(0,colon));
        const auto value=colon==std::wstring::npos?L"":Trim(expression.substr(colon+1));
        auto compareLength=[&](const std::wstring& base,double actual){
            const double expected=MediaLength(value,viewportWidth,viewportHeight);
            if(!std::isfinite(expected))return false;
            if(name==L"min-"+base)return actual+0.001>=expected;
            if(name==L"max-"+base)return actual<=expected+0.001;
            return name==base&&std::abs(actual-expected)<0.001;
        };
        if(name==L"width"||name==L"min-width"||name==L"max-width"||
           name==L"device-width"||name==L"min-device-width"||name==L"max-device-width"){
            const std::wstring base=name.find(L"device-")!=std::wstring::npos?L"device-width":L"width";
            return compareLength(base,base==L"device-width"?displayWidth:viewportWidth);
        }
        if(name==L"height"||name==L"min-height"||name==L"max-height"||
           name==L"device-height"||name==L"min-device-height"||name==L"max-device-height"){
            const std::wstring base=name.find(L"device-")!=std::wstring::npos?L"device-height":L"height";
            return compareLength(base,base==L"device-height"?displayHeight:viewportHeight);
        }
        if(name==L"orientation")return value==(viewportWidth>=viewportHeight?L"landscape":L"portrait");
        if(name==L"prefers-reduced-motion")return value.empty()||value==L"no-preference";
        if(name==L"prefers-color-scheme")return value.empty()||value==L"light";
        if(name==L"hover")return value.empty()||value==L"hover";
        if(name==L"any-hover")return value.empty()||value==L"hover";
        if(name==L"pointer"||name==L"any-pointer")return value.empty()||value==L"fine";
        if(name==L"color")return value.empty()||value==L"8";
        if(name==L"resolution"||name==L"min-resolution"||name==L"max-resolution"){
            size_t used=0;float number=0;if(!TryParseFloat(value,number,&used))return false;
            const auto unit=Trim(ToLower(value.substr(used)));
            const double expected=unit==L"dpi"?number/96.0:unit==L"dpcm"?number*2.54/96.0:
                (unit==L"dppx"||unit==L"x"||unit.empty()?number:std::numeric_limits<double>::quiet_NaN());
            if(!std::isfinite(expected))return false;
            if(name==L"min-resolution")return devicePixelRatio+0.0001>=expected;
            if(name==L"max-resolution")return devicePixelRatio<=expected+0.0001;
            return std::abs(devicePixelRatio-expected)<0.0001;
        }
        if(name==L"-webkit-min-device-pixel-ratio"||name==L"-webkit-max-device-pixel-ratio"){
            size_t used=0;float expected=0;if(!TryParseFloat(value,expected,&used))return false;
            return name.find(L"min-")!=std::wstring::npos?
                devicePixelRatio+0.0001>=expected:devicePixelRatio<=expected+0.0001;
        }
        return false;
    };

        size_t clauseStart=0;int nesting=0;
        for(size_t index=0;index<=source.size();++index){
            const wchar_t character=index<source.size()?source[index]:L',';
            if(character==L'(')++nesting;else if(character==L')')--nesting;
            if(character!=L','||nesting!=0)continue;
            auto clause=ToLower(Trim(source.substr(clauseStart,index-clauseStart)));clauseStart=index+1;
            bool negate=false;
            if(clause.rfind(L"not ",0)==0){negate=true;clause=Trim(clause.substr(4));}
            if(clause.rfind(L"only ",0)==0)clause=Trim(clause.substr(5));
            const auto firstFeature=clause.find(L'(');
            auto mediaType=Trim(clause.substr(0,firstFeature));
            while(mediaType.size()>=3&&mediaType.rfind(L"and")==mediaType.size()-3)
                mediaType=Trim(mediaType.substr(0,mediaType.size()-3));
            bool matches=mediaType.empty()||mediaType==L"all"||mediaType==L"screen";
            size_t position=firstFeature;
            while(matches&&position!=std::wstring::npos&&position<clause.size()){
                int depth=1;size_t end=position+1;
                for(;end<clause.size()&&depth;++end){if(clause[end]==L'(')++depth;else if(clause[end]==L')')--depth;}
                if(depth!=0){matches=false;break;}
                matches=EvaluateMediaFeature(clause.substr(position+1,end-position-2));
                position=clause.find(L'(',end);
            }
            if(negate)matches=!matches;
            if(matches)return true;
        }
        return false;
    }

bool StyleSheet::RuleApplies(const CssRule& rule,float width,float height) const {
    if(!rule.mediaEnabled)return false;
    for(const auto& query:rule.mediaQueries)
        if(!MediaQueryMatches(query,width,height,displayWidth_,displayHeight_,displayScale_))return false;
    return true;
}

void StyleSheet::SetDisplay(float width,float height,float scale) noexcept {
    if(std::abs(width-displayWidth_)<0.001f&&std::abs(height-displayHeight_)<0.001f&&
       std::abs(scale-displayScale_)<0.001f)return;
    displayWidth_=width;displayHeight_=height;displayScale_=scale;++version_;
}

void StyleSheet::SetViewport(float width,float height) noexcept {
    width=std::max(0.0f,width);height=std::max(0.0f,height);
    if(std::abs(width-viewportWidth_)<0.001f&&std::abs(height-viewportHeight_)<0.001f)return;
    // Most viewport lengths stay authored and resolve during layout. Font size
    // is a computed value inherited by descendants, so styles containing a
    // viewport-relative font size must be rebuilt when the viewport changes.
    bool mediaActivationChanged=false;
    for(const auto& rule:rules_){
        const bool wasActive=RuleApplies(rule,viewportWidth_,viewportHeight_);
        const bool isActive=RuleApplies(rule,width,height);
        if(wasActive!=isActive){mediaActivationChanged=true;break;}
    }
    viewportWidth_=width;viewportHeight_=height;
    if(mediaActivationChanged||usesViewportFontSize_)++version_;
}

std::wstring StyleSheet::ResolveVariables(const std::wstring& input,
                                          const FastMap<std::wstring, std::wstring>& vars,
                                          int depth) const {
    if (depth > 8) return input;
    std::wstring value = input;
    size_t position = 0;
    while ((position = value.find(L"var(", position)) != std::wstring::npos) {
        const size_t end = value.find(L')', position + 4);
        if (end == std::wstring::npos) break;
        auto expression = value.substr(position + 4, end - position - 4);
        const size_t comma = expression.find(L',');
        const auto name = Trim(expression.substr(0, comma));
        auto it = vars.find(name);
        const auto replacement = it != vars.end() ? ResolveVariables(it->second, vars, depth + 1)
            : (comma == std::wstring::npos ? L"" : Trim(expression.substr(comma + 1)));
        value.replace(position, end - position + 1, replacement);
        position += replacement.size();
    }
    return value;
}

std::shared_ptr<StyleSheet> StyleSheet::ShadowStyles(const std::shared_ptr<Node>& node) const {
    if(node&&!shadowScope_){
        auto root=node;while(const auto ancestor=root->parent.lock())root=ancestor;
        if(!root->shadowHost.expired()){
            std::wstring css;
            std::function<void(const std::shared_ptr<Node>&)> collect=[&](const auto& current){
                if(current->tag==L"style")css+=current->InnerText()+L"\n";
                for(const auto& child:current->children)collect(child);
            };
            collect(root);
            shadowStyles_.erase(std::remove_if(shadowStyles_.begin(),shadowStyles_.end(),[](const ShadowStyleCache& entry){return entry.root.expired();}),shadowStyles_.end());
            auto cached=std::find_if(shadowStyles_.begin(),shadowStyles_.end(),[&](const ShadowStyleCache& entry){return entry.root.lock()==root;});
            if(cached==shadowStyles_.end()){
                shadowStyles_.push_back({root,{},std::make_shared<StyleSheet>()});cached=shadowStyles_.end()-1;
                cached->sheet->shadowScope_=true;
            }
            if(cached->css!=css||cached->sheet->Version()==0){cached->css=css;cached->sheet->Parse(css);}
            cached->sheet->SetViewport(viewportWidth_,viewportHeight_);
            cached->sheet->SetDisplay(displayWidth_,displayHeight_,displayScale_);
            return cached->sheet;
        }
    }
    return {};
}

const CssKeyframes* StyleSheet::FindKeyframes(const std::shared_ptr<Node>& node,const std::wstring& name) const {
    if(const auto scoped=ShadowStyles(node))return scoped->FindKeyframes(node,name);
    for(auto frame=keyframes_.rbegin();frame!=keyframes_.rend();++frame){
        if(frame->name!=name)continue;
        bool applies=true;
        for(const auto& query:frame->mediaQueries)
            if(!MediaQueryMatches(query,viewportWidth_,viewportHeight_,displayWidth_,displayHeight_,displayScale_)){applies=false;break;}
        if(applies)return &*frame;
    }
    return nullptr;
}

ComputedStyle StyleSheet::Compute(const std::shared_ptr<Node>& node, const ComputedStyle* parent,
                                  const std::wstring& pseudo) const {
    if(const auto scoped=ShadowStyles(node))return scoped->Compute(node,parent,pseudo);
    ComputedStyle result;
    if (parent) for (const auto* inherited : {L"color", L"color-scheme", L"font-family", L"font-size", L"font-style", L"font-weight", L"letter-spacing", L"line-height", L"list-style-image", L"list-style-position", L"list-style-type", L"tab-size", L"text-align", L"text-decoration", L"text-decoration-line", L"white-space", L"pointer-events", L"visibility", L"fill", L"fill-opacity", L"fill-rule", L"stroke", L"stroke-opacity", L"stroke-width", L"stroke-linecap", L"stroke-linejoin", L"stroke-miterlimit", L"stroke-dasharray", L"stroke-dashoffset"}) {
        const auto value = parent->Get(inherited);
        if (!value.empty()) (*result.values)[inherited] = value;
    }
    if(pseudo.empty()){
        SetDefault(node,result);
        // Rows and cells inherit vertical alignment through their UA rule,
        // rather than treating every cell as independently middle-aligned.
        if(result.Is(L"vertical-align",L"inherit"))
            (*result.values)[L"vertical-align"]=parent?parent->Get(L"vertical-align",L"middle"):L"middle";
    }
    else{
        auto generated=std::make_shared<Node>();generated->tag=L"span";SetDefault(generated,result);
        // Form-control placeholder text has a user-agent color even when the
        // document does not declare ::placeholder. Keep it in the common
        // pseudo-style cascade so author color/inherit/initial rules can still
        // override it normally instead of making the painter special-case an
        // individual input or page.
        if(pseudo==L"placeholder")(*result.values)[L"color"]=L"#757575";
        // Browser user-agent styles give modal dialogs a translucent backdrop.
        // Author ::backdrop rules participate in the normal cascade below.
        if(pseudo==L"backdrop")(*result.values)[L"background-color"]=L"rgba(0,0,0,.1)";
    }
    // Preserve the user-agent/inherited starting point so a winning CSS-wide
    // `inherit` declaration can replace an earlier author declaration rather
    // than leaving that declaration behind when the parent has no value.
    const auto baseValues = *result.values;
    struct Winner { bool important; int specificity; int order; };
    FastMap<std::wstring, Winner> winners;
    auto wins = [](const Winner& candidate, const Winner& current) {
        return candidate.important > current.important ||
            (candidate.important == current.important &&
             (candidate.specificity > current.specificity ||
              (candidate.specificity == current.specificity && candidate.order >= current.order)));
    };
    const auto ruleApplies=[&](const CssRule& rule){return RuleApplies(rule,viewportWidth_,viewportHeight_);};
    auto variables=rootVariables_;
    if(parent){
        for(const auto& pair:*parent->values)
            if(pair.first.rfind(L"--",0)==0)variables[pair.first]=pair.second;
    }else if(node){
        // Layout may start at <body>; retain custom properties set dynamically
        // on its <html> ancestor through document.documentElement.style.
        std::vector<std::shared_ptr<Node>> ancestors;
        for(auto ancestor=node->parent.lock();ancestor;ancestor=ancestor->parent.lock())ancestors.push_back(ancestor);
        for(auto it=ancestors.rbegin();it!=ancestors.rend();++it)
            for(const auto& pair:(*it)->inlineStyle)
                if(pair.first.rfind(L"--",0)==0)variables[pair.first]=pair.second;
    }
    const auto candidateRules=CandidateRules(node);
    std::vector<const CssRule*> matchedRules;
    matchedRules.reserve(candidateRules.size());
    for(const auto* rulePointer:candidateRules){
        const auto& rule=*rulePointer;
        if(ruleApplies(rule)&&
           (pseudo.empty()?rule.pseudo.empty():rule.pseudo==pseudo)&&
           Document::MatchesSelector(node,rule.selectorParts))matchedRules.push_back(rulePointer);
    }
    FastMap<std::wstring,Winner> customWinners;
    for(const auto* rulePointer:matchedRules){
        const auto& rule=*rulePointer;
        if(!rule.hasCustomDeclarations)continue;
        for(const auto& declaration:rule.declarations){
            if(declaration.name.rfind(L"--",0)!=0)continue;
            const Winner candidate{declaration.important,rule.specificity,rule.order};
            const auto previous=customWinners.find(declaration.name);
            if(previous==customWinners.end()||wins(candidate,previous->second)){
                customWinners[declaration.name]=candidate;
                variables[declaration.name]=declaration.value;
            }
        }
    }
    for(const auto& pair:node->inlineStyle){
        if(pair.first.rfind(L"--",0)!=0)continue;
        const Winner candidate{node->inlineStylePriority.count(pair.first)!=0,1000,0};const auto previous=customWinners.find(pair.first);
        if(previous==customWinners.end()||wins(candidate,previous->second))variables[pair.first]=pair.second;
    }
    for(const auto& pair:variables)(*result.values)[pair.first]=pair.second;
    // SVG presentation attributes are author declarations with zero
    // specificity. Resolve them only after inherited and element-local custom
    // properties are known; otherwise values such as fill="var(--surface)"
    // reach the painter unresolved and fall back to the current text color.
    if(pseudo.empty()&&node)for(const auto* presentation:{L"fill",L"fill-opacity",L"fill-rule",
        L"stroke",L"stroke-opacity",L"stroke-width",L"stroke-linecap",L"stroke-linejoin",
        L"stroke-miterlimit",L"stroke-dasharray",L"stroke-dashoffset"}){
        const auto value=node->Attribute(presentation);
        if(!value.empty())(*result.values)[presentation]=ResolveVariables(value,variables);
    }
    auto setProperty = [&](const std::wstring& name, const std::wstring& value,
                           const Winner& candidate) {
        const auto current = winners.find(name);
        if (current != winners.end() && !wins(candidate, current->second)) return;
        winners[name] = candidate;
        auto resolved = ResolveVariables(value, variables);
        if(name==L"font-size"){
            const auto lowered=ToLower(Trim(resolved));
            const bool hasViewportUnit=lowered.find(L"vw")!=std::wstring::npos||
                lowered.find(L"vh")!=std::wstring::npos||lowered.find(L"vmin")!=std::wstring::npos||
                lowered.find(L"vmax")!=std::wstring::npos;
            if(hasViewportUnit){
                float parentSize=16;
                if(parent)parentSize=StyleSheet::Length(parent->Get(L"font-size",L"16px"),16,16,16,16);
                const auto viewportResolved=ResolveViewportUnits(lowered,viewportWidth_,viewportHeight_);
                resolved=std::to_wstring(StyleSheet::Length(viewportResolved,parentSize,
                    viewportWidth_,parentSize,parentSize))+L"px";
            }
        }
        if (ToLower(Trim(resolved)) == L"inherit") {
            const auto inherited = parent ? parent->Get(name) : L"";
            if (!inherited.empty()) (*result.values)[name] = inherited;
            else {
                const auto initial = baseValues.find(name);
                if (initial != baseValues.end()) (*result.values)[name] = initial->second;
                else (*result.values)[name] = L"";
            }
        } else (*result.values)[name] = std::move(resolved);
    };
    auto applyDeclaration = [&](const std::wstring& name, const std::wstring& value,
                                const Winner& candidate) {
        if(name==L"margin"||name==L"padding"||name.rfind(L"margin-",0)==0||name.rfind(L"padding-",0)==0){
            const bool padding=name.rfind(L"padding",0)==0;
            const bool unitless=node&&node->ownerDocument&&node->ownerDocument->QuirksMode();
            for(const auto& token:SplitWhitespace(ResolveVariables(value,variables))){
                float number=0;size_t used=0;
                if(TryParseFloat(token,number,&used)&&
                   ((padding&&number<0)||(!unitless&&used==token.size()&&number!=0)))return;
            }
        }
        // Keep the authored property for script/debug inspection and also write
        // the longhands consumed by layout. Longhand winners are compared
        // independently, matching CSS shorthand reset and cascade behavior.
        setProperty(name, value, candidate);

        if (name == L"background") {
            const auto resolvedBackground=ResolveVariables(value,variables);
            const auto background=ParseBackgroundShorthand(resolvedBackground);
            setProperty(L"background-color",background.color,candidate);
            setProperty(L"background-image",background.image,candidate);
            setProperty(L"background-position",background.position,candidate);
            setProperty(L"background-size",background.size,candidate);
            setProperty(L"background-repeat",background.repeat,candidate);
        } else if (name == L"margin" || name == L"padding") {
            const auto edges = ExpandEdges(value);
            if (edges.size() == 4) {
                static const wchar_t* sides[] = {L"top", L"right", L"bottom", L"left"};
                for (size_t i = 0; i < 4; ++i)
                    setProperty(name + L"-" + sides[i], edges[i], candidate);
            }
        } else if (name == L"border") {
            const auto color = BorderColorFromShorthand(
                ResolveVariables(value, variables));
            for (const auto* side : {L"top", L"right", L"bottom", L"left"}) {
                setProperty(L"border-" + std::wstring(side) + L"-width", value, candidate);
                setProperty(L"border-" + std::wstring(side) + L"-color", color, candidate);
                setProperty(L"border-" + std::wstring(side) + L"-style", value, candidate);
            }
        } else if (name == L"border-width" || name == L"border-color" ||
                   name == L"border-style") {
            const auto edges = ExpandEdges(value);
            if (edges.size() == 4) {
                const auto suffix = name == L"border-width" ? L"-width" :
                    (name == L"border-color" ? L"-color" : L"-style");
                static const wchar_t* sides[] = {L"top", L"right", L"bottom", L"left"};
                for (size_t i = 0; i < 4; ++i)
                    setProperty(L"border-" + std::wstring(sides[i]) + suffix, edges[i], candidate);
            }
        } else if (name == L"border-top" || name == L"border-right" ||
                   name == L"border-bottom" || name == L"border-left") {
            setProperty(name + L"-width", value, candidate);
            setProperty(name + L"-color", BorderColorFromShorthand(
                ResolveVariables(value, variables)), candidate);
            setProperty(name + L"-style", value, candidate);
        } else if (name == L"outline") {
            const auto resolvedOutline=ResolveVariables(value,variables);
            std::wstring width=L"medium",style=L"none",color=L"currentcolor";
            constexpr unsigned int invalid=0x01020304u;
            for(const auto& token:SplitWhitespace(resolvedOutline)){
                const auto lowered=ToLower(Trim(token));
                if(lowered==L"none"||lowered==L"hidden"||lowered==L"dotted"||
                   lowered==L"dashed"||lowered==L"solid"||lowered==L"double"||
                   lowered==L"groove"||lowered==L"ridge"||lowered==L"inset"||
                   lowered==L"outset"||lowered==L"auto")style=token;
                else if(lowered==L"currentcolor"||lowered==L"invert"||
                        StyleSheet::Color(token,invalid)!=invalid)color=token;
                else width=token;
            }
            setProperty(L"outline-width",width,candidate);
            setProperty(L"outline-style",style,candidate);
            setProperty(L"outline-color",color,candidate);
        } else if (name == L"gap") {
            const auto parts = SplitWhitespace(value);
            if (!parts.empty()) {
                setProperty(L"row-gap", parts[0], candidate);
                setProperty(L"column-gap", parts.size() > 1 ? parts[1] : parts[0], candidate);
            }
        } else if (name == L"overflow") {
            const auto parts = SplitWhitespace(value);
            if (!parts.empty()) {
                setProperty(L"overflow-x", parts[0], candidate);
                setProperty(L"overflow-y", parts.size() > 1 ? parts[1] : parts[0], candidate);
            }
        } else if (name == L"list-style") {
            const auto parts=SplitWhitespace(value);
            std::wstring type=L"disc",position=L"outside",image=L"none";
            for(const auto& part:parts){
                const auto lowered=ToLower(part);
                if(lowered==L"inside"||lowered==L"outside")position=part;
                else if(lowered.rfind(L"url(",0)==0)image=part;
                else type=part;
            }
            setProperty(L"list-style-type",type,candidate);
            setProperty(L"list-style-position",position,candidate);
            setProperty(L"list-style-image",image,candidate);
        } else if (name == L"place-items") {
            const auto parts=SplitWhitespace(value);
            if(!parts.empty()){
                setProperty(L"align-items",parts[0],candidate);
                setProperty(L"justify-items",parts.size()>1?parts[1]:parts[0],candidate);
            }
        } else if (name == L"place-content") {
            const auto parts=SplitWhitespace(value);
            if(!parts.empty()){
                size_t split=1;
                if((ToLower(parts[0])==L"safe"||ToLower(parts[0])==L"unsafe")&&parts.size()>1)
                    split=2;
                std::wstring alignValue=parts[0];
                for(size_t index=1;index<split;++index)alignValue+=L" "+parts[index];
                std::wstring justifyValue=alignValue;
                if(split<parts.size()){
                    justifyValue=parts[split];
                    for(size_t index=split+1;index<parts.size();++index)
                        justifyValue+=L" "+parts[index];
                }
                setProperty(L"align-content",alignValue,candidate);
                setProperty(L"justify-content",justifyValue,candidate);
            }
        } else if (name == L"inset") {
            const auto edges=ExpandEdges(value);
            if(edges.size()==4){
                static const wchar_t* sides[]={L"top",L"right",L"bottom",L"left"};
                for(size_t i=0;i<4;++i)setProperty(sides[i],edges[i],candidate);
            }
        } else if (name == L"flex") {
            auto parts = SplitWhitespace(value);
            std::wstring grow = L"0", shrink = L"1", basis = L"auto";
            if (value == L"none") { grow = L"0"; shrink = L"0"; }
            else if (value == L"auto") { grow = L"1"; shrink = L"1"; }
            else if (value == L"initial") { grow = L"0"; shrink = L"1"; }
            else if (!parts.empty()) {
                grow = parts[0];
                shrink = parts.size() > 1 ? parts[1] : L"1";
                basis = parts.size() > 2 ? parts[2] : L"0%";
            }
            setProperty(L"flex-grow", grow, candidate);
            setProperty(L"flex-shrink", shrink, candidate);
            setProperty(L"flex-basis", basis, candidate);
        } else if (name == L"flex-flow") {
            const auto parts=SplitWhitespace(value);std::wstring direction=L"row",wrap=L"nowrap";
            for(const auto& part:parts){const auto token=ToLower(part);
                if(token==L"row"||token==L"row-reverse"||token==L"column"||token==L"column-reverse")direction=part;
                else if(token==L"nowrap"||token==L"wrap"||token==L"wrap-reverse")wrap=part;
            }
            setProperty(L"flex-direction",direction,candidate);
            setProperty(L"flex-wrap",wrap,candidate);
        } else if (name == L"font") {
            const auto shorthand=ToLower(Trim(value));
            if(shorthand==L"inherit"){
                for(const auto* property:{L"font-family",L"font-size",L"font-style",L"font-weight",L"line-height"})
                    setProperty(property,L"inherit",candidate);
            }else{
                const auto parts=SplitWhitespace(value);size_t sizeIndex=parts.size();
                auto isSize=[](const std::wstring& token){
                    const auto lowered=ToLower(token);
                    return lowered==L"xx-small"||lowered==L"x-small"||lowered==L"small"||
                        lowered==L"medium"||lowered==L"large"||lowered==L"x-large"||lowered==L"xx-large"||
                        lowered.find(L"px")!=std::wstring::npos||lowered.find(L"pt")!=std::wstring::npos||
                        lowered.find(L"em")!=std::wstring::npos||lowered.find(L"rem")!=std::wstring::npos||
                        lowered.find(L"%")!=std::wstring::npos;
                };
                for(size_t i=0;i<parts.size();++i)if(isSize(parts[i])){sizeIndex=i;break;}
                if(sizeIndex<parts.size()){
                    std::wstring size=parts[sizeIndex],lineHeight=L"normal";
                    const auto slash=size.find(L'/');
                    if(slash!=std::wstring::npos){lineHeight=size.substr(slash+1);size=size.substr(0,slash);}
                    else if(sizeIndex+1<parts.size()&&parts[sizeIndex+1].rfind(L"/",0)==0){lineHeight=parts[sizeIndex+1].substr(1);++sizeIndex;}
                    std::wstring style=L"normal",weight=L"400";
                    for(size_t i=0;i<sizeIndex;++i){const auto token=ToLower(parts[i]);
                        if(token==L"italic"||token==L"oblique")style=token;
                        if(token==L"bold"||token==L"bolder"||token==L"lighter"||(token.size()==3&&token[0]>=L'1'&&token[0]<=L'9'&&token[1]==L'0'&&token[2]==L'0'))weight=parts[i];}
                    std::wstring family;for(size_t i=sizeIndex+1;i<parts.size();++i){if(!family.empty())family+=L" ";family+=parts[i];}
                    setProperty(L"font-size",size,candidate);setProperty(L"line-height",lineHeight,candidate);
                    setProperty(L"font-style",style,candidate);setProperty(L"font-weight",weight,candidate);
                    if(!family.empty())setProperty(L"font-family",family,candidate);
                }
            }
        }
    };
    for (const auto* rulePointer : matchedRules) {
        const auto& rule=*rulePointer;
        for (const auto& declaration : rule.declarations) {
            if (declaration.name.rfind(L"--", 0) == 0) continue;
            const Winner candidate{declaration.important, rule.specificity, rule.order};
            applyDeclaration(declaration.name, declaration.value, candidate);
        }
    }
    if(pseudo.empty())for (const auto& pair : node->inlineStyle) {
        if(pair.first.rfind(L"--",0)==0)continue;
        applyDeclaration(pair.first, pair.second,
                         {node->inlineStylePriority.count(pair.first)!=0, 1000, 0});
    }
    // HTML hidden-state inputs are non-rendered controls. Chromium enforces
    // this as a user-agent !important rule, so even a broad author rule such
    // as `input { display:block }` must not expose their submitted values.
    // Keep the rule in the common style cascade rather than teaching table or
    // form layout about one particular page.
    if(pseudo.empty()&&node&&node->tag==L"input"&&
       ToLower(Trim(node->Attribute(L"type")))==L"hidden")
        (*result.values)[L"display"]=L"none";
    // Percentages and font-relative lengths compute against this element's
    // font, then descendants inherit the resulting length. Unitless numbers
    // stay numbers and are multiplied by each descendant's own font size.
    const auto lineHeight=ToLower(Trim(result.Get(L"line-height")));
    float number=0;size_t consumed=0;
    if(TryParseFloat(lineHeight,number,&consumed)&&consumed<lineHeight.size()){
        const float fontSize=Length(result.Get(L"font-size",L"16px"),16,viewportWidth_,16,16);
        (*result.values)[L"line-height"]=std::to_wstring(Length(lineHeight,fontSize,viewportWidth_,fontSize,fontSize))+L"px";
    }
    return result;
}

float StyleSheet::Length(const std::wstring& raw, float reference, float viewport, float fallback, float fontSize) {
    auto value = Trim(ToLower(raw));
    if (value.empty() || value == L"auto" || value == L"none") return fallback;
    auto mathArguments = [&](const wchar_t* name) {
        const std::wstring prefix = std::wstring(name) + L"(";
        if (value.rfind(prefix, 0) != 0 || value.back() != L')')
            return std::vector<std::wstring>{};
        return Split(value.substr(prefix.size(), value.size() - prefix.size() - 1), L',');
    };
    if (value.rfind(L"min(", 0) == 0) {
        const auto arguments = mathArguments(L"min");
        if (arguments.empty()) return fallback;
        float result = Length(arguments.front(), reference, viewport, fallback, fontSize);
        for (size_t index = 1; index < arguments.size(); ++index)
            result = std::min(result, Length(arguments[index], reference, viewport, fallback, fontSize));
        return result;
    }
    if (value.rfind(L"max(", 0) == 0) {
        const auto arguments = mathArguments(L"max");
        if (arguments.empty()) return fallback;
        float result = Length(arguments.front(), reference, viewport, fallback, fontSize);
        for (size_t index = 1; index < arguments.size(); ++index)
            result = std::max(result, Length(arguments[index], reference, viewport, fallback, fontSize));
        return result;
    }
    if (value.rfind(L"clamp(", 0) == 0) {
        const auto arguments = mathArguments(L"clamp");
        if (arguments.size() != 3) return fallback;
        const float minimum = Length(arguments[0], reference, viewport, fallback, fontSize);
        const float preferred = Length(arguments[1], reference, viewport, fallback, fontSize);
        const float maximum = Length(arguments[2], reference, viewport, fallback, fontSize);
        return std::max(minimum, std::min(preferred, maximum));
    }
    if (value.rfind(L"calc(", 0) == 0 && value.back() == L')') {
        auto expression = value.substr(5, value.size() - 6);
        float total = 0.0f; int sign = 1; size_t start = 0; int nesting = 0;
        for (size_t i = 0; i <= expression.size(); ++i) {
            if (i < expression.size() && expression[i] == L'(') ++nesting;
            else if (i < expression.size() && expression[i] == L')') --nesting;
            if (i == expression.size() || (nesting == 0 && (expression[i] == L'+' || expression[i] == L'-'))) {
                total += sign * Length(expression.substr(start, i - start), reference, viewport, 0, fontSize);
                if (i < expression.size()) sign = expression[i] == L'-' ? -1 : 1;
                start = i + 1;
            }
        }
        return total;
    }
    size_t used=0;float number=0;
    if(!TryParseFloat(value,number,&used))return fallback;
    const auto unit=value.substr(used);
    if(unit==L"%")return reference*number/100.0f;
    if(unit==L"vh"||unit==L"vw")return viewport*number/100.0f;
    if(unit==L"em")return fontSize*number;
    if(unit==L"rem")return 16.0f*number;
    if(unit==L"pt")return number*(96.0f/72.0f);
    if(unit==L"pc")return number*16.0f;
    if(unit==L"in")return number*96.0f;
    if(unit==L"cm")return number*(96.0f/2.54f);
    if(unit==L"mm")return number*(96.0f/25.4f);
    if(unit==L"q")return number*(96.0f/101.6f);
    return unit.empty()||unit==L"px"?number:fallback;
}

unsigned int StyleSheet::Color(const std::wstring& raw, unsigned int fallback) {
    auto value = ToLower(Trim(raw));
    if (value == L"transparent" || value == L"none") return 0;
    static const FastMap<std::wstring, unsigned int> named = {
        {L"white",0xffffffffu},{L"black",0xff000000u},{L"red",0xffff0000u},
        {L"blue",0xff0000ffu},{L"green",0xff008000u},{L"gray",0xff808080u},
        {L"grey",0xff808080u},{L"yellow",0xffffff00u},{L"purple",0xff800080u},
        {L"fuchsia",0xffff00ffu},{L"magenta",0xffff00ffu},{L"lime",0xff00ff00u},
        {L"aqua",0xff00ffffu},{L"cyan",0xff00ffffu},{L"navy",0xff000080u},
        {L"teal",0xff008080u},{L"olive",0xff808000u},{L"maroon",0xff800000u},
        {L"silver",0xffc0c0c0u},{L"orange",0xffffa500u},
        {L"rebeccapurple",0xff663399u},{L"transparent",0u}};
    const auto namedIt = named.find(value);
    if (namedIt != named.end()) return namedIt->second;
    if (value.rfind(L"color-mix(", 0) == 0 && value.back() == L')') {
        const auto arguments=Split(value.substr(10,value.size()-11),L',');
        if(arguments.size()==3&&ToLower(Trim(arguments[0]))==L"in srgb"){
            struct Stop { unsigned int color=0;float weight=-1;bool valid=false; };
            auto parseStop=[&](const std::wstring& source){
                Stop stop;auto parts=SplitWhitespace(source);if(parts.empty())return stop;
                if(parts.size()>1&&parts.back().find(L'%')!=std::wstring::npos){
                    size_t used=0;float weight=0;
                    if(!TryParseFloat(parts.back(),weight,&used)||parts.back().substr(used)!=L"%")return stop;
                    stop.weight=weight/100.0f;parts.pop_back();
                }
                std::wstring colorText;for(const auto& part:parts){if(!colorText.empty())colorText+=L" ";colorText+=part;}
                constexpr unsigned int invalid=0x01020304u;
                stop.color=Color(colorText,invalid);stop.valid=stop.color!=invalid;return stop;
            };
            auto first=parseStop(arguments[1]),second=parseStop(arguments[2]);
            if(first.valid&&second.valid){
                if(first.weight<0&&second.weight<0)first.weight=second.weight=0.5f;
                else if(first.weight<0)first.weight=1.0f-second.weight;
                else if(second.weight<0)second.weight=1.0f-first.weight;
                first.weight=std::max(0.0f,first.weight);second.weight=std::max(0.0f,second.weight);
                const float sum=first.weight+second.weight;
                if(sum>0){const float alphaMultiplier=std::min(1.0f,sum);first.weight/=sum;second.weight/=sum;
                    auto channel=[](unsigned int color,int shift){return static_cast<float>((color>>shift)&255)/255.0f;};
                    const float a1=channel(first.color,24),a2=channel(second.color,24);
                    const float mixedAlpha=a1*first.weight+a2*second.weight,alpha=mixedAlpha*alphaMultiplier;
                    auto mix=[&](int shift){return mixedAlpha>0?(channel(first.color,shift)*a1*first.weight+channel(second.color,shift)*a2*second.weight)/mixedAlpha:0.0f;};
                    auto byte=[](float number){return static_cast<unsigned int>(std::lround(std::max(0.0f,std::min(1.0f,number))*255.0f));};
                    return (byte(alpha)<<24)|(byte(mix(16))<<16)|(byte(mix(8))<<8)|byte(mix(0));
                }
            }
        }
        return fallback;
    }
    if (!value.empty() && value[0] == L'#') {
        auto hex=value.substr(1);
        if(hex.size()==3)hex=std::wstring{hex[0],hex[0],hex[1],hex[1],hex[2],hex[2]};
        if(hex.size()==4)hex=std::wstring{hex[0],hex[0],hex[1],hex[1],hex[2],hex[2],hex[3],hex[3]};
        unsigned long long parsed=0;size_t used=0;
        if((hex.size()==6||hex.size()==8)&&TryParseUnsignedInteger(hex,parsed,&used,16)&&used==hex.size()){
            const auto rgba=static_cast<unsigned int>(parsed);
            if(hex.size()==6)return 0xff000000u|rgba;
            return ((rgba&0xffu)<<24)|(rgba>>8);
        }
        return fallback;
    }
    if (value.rfind(L"rgb", 0) == 0) {
        const auto a = value.find(L'('), b = value.find(L')');
        if (a != std::wstring::npos && b != std::wstring::npos) {
            auto arguments=value.substr(a+1,b-a-1);
            if(arguments.find(L',')==std::wstring::npos)
                for(size_t slash=0;(slash=arguments.find(L'/',slash))!=std::wstring::npos;slash+=3)
                    arguments.replace(slash,1,L" / ");
            auto parts=arguments.find(L',')==std::wstring::npos?
                SplitWhitespace(arguments):Split(arguments,L',');
            parts.erase(std::remove(parts.begin(),parts.end(),L"/"),parts.end());
            if(parts.size()>=3){
                auto channel=[](const std::wstring& token,unsigned int& result){
                    const auto text=Trim(token);size_t used=0;float number=0;
                    if(!TryParseFloat(text,number,&used))return false;
                    const bool percent=used<text.size()&&text.substr(used)==L"%";
                    if(used!=text.size()&&!percent)return false;
                    const float value=percent?number*2.55f:number;
                    result=static_cast<unsigned int>(std::lround(std::max(0.0f,std::min(255.0f,value))));
                    return true;
                };
                auto alpha=[](const std::wstring& token,unsigned int& result){
                    const auto text=Trim(token);size_t used=0;float number=0;
                    if(!TryParseFloat(text,number,&used))return false;
                    const bool percent=used<text.size()&&text.substr(used)==L"%";
                    if(used!=text.size()&&!percent)return false;
                    const float value=percent?number/100.0f:number;
                    result=static_cast<unsigned int>(std::lround(std::max(0.0f,std::min(1.0f,value))*255.0f));
                    return true;
                };
                unsigned int r=0,g=0,bl=0,opacity=255;
                if(channel(parts[0],r)&&channel(parts[1],g)&&channel(parts[2],bl)&&
                   (parts.size()<=3||alpha(parts[3],opacity)))
                    return (opacity<<24)|(r<<16)|(g<<8)|bl;
            }
        }
    }
    return fallback;
}

} // namespace TWebFrame::Internal
