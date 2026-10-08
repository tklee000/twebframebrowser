#include "CSS.h"
#include "CSSSyntax.h"
#include "NumericParser.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <functional>
#include <sstream>

namespace TWebFrame::Internal {
namespace {

std::vector<std::wstring> Split(const std::wstring& value, wchar_t delimiter) {
    return CssSyntax::Split(value, delimiter);
}

std::vector<std::wstring> SplitWhitespace(const std::wstring& value) {
    return CssSyntax::Words(value);
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
    return L"currentcolor";
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
            if(finalLayer&&(lowered==L"currentcolor"||StyleSheet::Color(piece,invalid)!=invalid)){result.color=piece;continue;}
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
    // Escapes and functional selectors need the general candidate path.
    if(subject.find(L'\\')!=std::wstring::npos)return {};
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
    wchar_t quote=0;
    for(size_t position=0;position<selector.size();++position){
        const auto c=selector[position];
        if(c==L'\\'){position=CssSyntax::EscapeEnd(selector,position)-1;continue;}
        if(quote){if(c==quote)quote=0;continue;}
        if(c==L'\''||c==L'"'){quote=c;continue;}
        if(c!=L'[')continue;
        const auto close=CssSyntax::Close(selector,position);
        if(close>=selector.size())break;
        size_t begin=position+1;
        while(begin<close&&std::iswspace(selector[begin]))++begin;
        const auto end=CssSyntax::IdentifierEnd(selector,begin);
        if(end>begin)attributes[ToLower(CssSyntax::Decode(std::wstring_view(selector).substr(begin,end-begin)))]=true;
        position=close;
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

std::wstring LogicalProperty(const std::wstring& name, const std::wstring& direction,
                             const std::wstring& writingMode) {
    if(name.find(L"inline")==std::wstring::npos&&name.find(L"block")==std::wstring::npos)return name;
    const bool vertical=writingMode==L"vertical-rl"||writingMode==L"vertical-lr"||
        writingMode==L"sideways-rl"||writingMode==L"sideways-lr";
    bool reverse=direction==L"rtl";
    if(writingMode==L"sideways-lr")reverse=!reverse;
    const std::wstring inlineStart=vertical?(reverse?L"bottom":L"top"):(reverse?L"right":L"left");
    const std::wstring inlineEnd=vertical?(reverse?L"top":L"bottom"):(reverse?L"left":L"right");
    const std::wstring blockStart=vertical?((writingMode==L"vertical-lr"||writingMode==L"sideways-lr")?L"left":L"right"):L"top";
    const std::wstring blockEnd=vertical?((writingMode==L"vertical-lr"||writingMode==L"sideways-lr")?L"right":L"left"):L"bottom";
    for(const auto* prefix:{L"",L"min-",L"max-"}){
        if(name==std::wstring(prefix)+L"inline-size")return std::wstring(prefix)+(vertical?L"height":L"width");
        if(name==std::wstring(prefix)+L"block-size")return std::wstring(prefix)+(vertical?L"width":L"height");
    }
    for(const auto* prefix:{L"margin-",L"padding-",L"inset-",L"border-"}){
        const std::wstring base=prefix;
        if(name.rfind(base,0)!=0)continue;
        const auto tail=name.substr(base.size());
        for(const auto& edge:std::vector<std::pair<std::wstring,std::wstring>>{
            {L"inline-start",inlineStart},{L"inline-end",inlineEnd},
            {L"block-start",blockStart},{L"block-end",blockEnd}}){
            if(tail!=edge.first&&!(base==L"border-"&&tail.rfind(edge.first+L"-",0)==0))continue;
            return (base==L"inset-"?L"":base)+edge.second+tail.substr(edge.first.size());
        }
    }
    return name;
}

// Resolve dimension tokens, leaving strings, identifiers and URL payloads intact.
// Used-value callers need not guess the root font from a local element's font.
std::wstring ResolveRootFontUnits(const std::wstring& value,float rootFont) {
    if(ToLower(value).find(L"rem")==std::wstring::npos)return value;
    std::wstring result;
    for(size_t position=0;position<value.size();){
        const auto c=value[position];
        if(c==L'\''||c==L'"'){
            const auto begin=position++;while(position<value.size()){
                if(value[position]==L'\\'){position=CssSyntax::EscapeEnd(value,position);continue;}
                if(value[position++]==c)break;
            }
            result.append(value,begin,position-begin);continue;
        }
        float number=0;size_t used=0;
        if((std::iswdigit(c)||c==L'.'||c==L'+'||c==L'-')&&
           TryParseFloat(value.substr(position),number,&used)&&used){
            const auto unitStart=position+used;
            const auto end=CssSyntax::IdentifierEnd(value,unitStart);
            if(ToLower(value.substr(unitStart,end-unitStart))==L"rem")
                result+=std::to_wstring(number*rootFont)+L"px";
            else result.append(value,position,end-position);
            position=end;continue;
        }
        if(CssSyntax::Name(c)||c==L'\\'){
            const auto end=CssSyntax::IdentifierEnd(value,position);
            auto next=end;
            if(ToLower(CssSyntax::Decode(std::wstring_view(value).substr(position,end-position)))==L"url"&&
               next<value.size()&&value[next]==L'(')next=std::min(value.size(),CssSyntax::Close(value,next)+1);
            result.append(value,position,next-position);position=next;continue;
        }
        result+=c;++position;
    }
    return result;
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
    static thread_local unsigned depth=0;
    if(depth>=128)return 0;
    struct Scope {unsigned& value;explicit Scope(unsigned& v):value(v){++value;}~Scope(){--value;}} scope(depth);
    // Reserve independent fields: ten classes must never outrank one ID.
    int ids = 0, classes = 0, tags = 0;
    bool typePosition = true;
    for (size_t i = 0; i < selector.size(); ++i) {
        const wchar_t c = selector[i];
        if(CssSyntax::Space(c)||c==L'>'||c==L'+'||c==L'~'){typePosition=true;continue;}
        if(c==L'['){++classes;i=CssSyntax::Close(selector,i);typePosition=false;continue;}
        if(c==L'#'||c==L'.'){
            if(c==L'#')++ids;else ++classes;
            i=CssSyntax::IdentifierEnd(selector,i+1)-1;typePosition=false;continue;
        }
        if(c==L':'){
            const bool element=i+1<selector.size()&&selector[i+1]==L':';
            const size_t start=i+(element?2:1),end=CssSyntax::IdentifierEnd(selector,start);
            const auto name=ToLower(CssSyntax::Decode(std::wstring_view(selector).substr(start,end-start)));
            const bool legacy=name==L"before"||name==L"after"||name==L"first-letter"||name==L"first-line";
            if(element||legacy)++tags;
            else if(name!=L"where"&&name!=L"is"&&name!=L"not"&&name!=L"has")++classes;
            i=end-1;
            if(end<selector.size()&&selector[end]==L'('){
                const auto close=CssSyntax::Close(selector,end);
                auto arguments=selector.substr(end+1,close-end-1);int maximum=0;
                if(name==L"is"||name==L"not"||name==L"has"){
                    for(const auto& item:Split(arguments,L','))maximum=std::max(maximum,Specificity(item));
                }else if(name==L"nth-child"||name==L"nth-last-child"){
                    const auto of=ToLower(arguments).find(L" of ");
                    if(of!=std::wstring::npos)for(const auto& item:Split(arguments.substr(of+4),L','))maximum=std::max(maximum,Specificity(item));
                }
                ids+=maximum/1000000;classes+=(maximum/1000)%1000;tags+=maximum%1000;i=close;
            }
            typePosition=false;continue;
        }
        if(CssSyntax::Name(c)||c==L'\\'){
            const auto end=CssSyntax::IdentifierEnd(selector,i);
            if(typePosition)++tags;i=end-1;
        }
        typePosition=false;
    }
    return std::min(ids,999)*1000000+std::min(classes,999)*1000+std::min(tags,999);
}

void CopyInheritedStyle(const ComputedStyle& parent,ComputedStyle& result) {
    result.rootFontSize=parent.rootFontSize;
    result.deviceScale=parent.deviceScale;
    for(const auto* inherited:{L"border-collapse", L"border-spacing", L"caption-side", L"color", L"color-scheme", L"font-family", L"font-size", L"font-style", L"font-weight", L"font-kerning", L"letter-spacing", L"word-spacing", L"overflow-wrap", L"word-break", L"hyphens", L"line-height", L"list-style-image", L"list-style-position", L"list-style-type", L"tab-size", L"direction", L"writing-mode", L"text-orientation", L"text-align", L"text-decoration", L"text-decoration-line", L"white-space", L"pointer-events", L"visibility", L"fill", L"fill-opacity", L"fill-rule", L"stroke", L"stroke-opacity", L"stroke-width", L"stroke-linecap", L"stroke-linejoin", L"stroke-miterlimit", L"stroke-dasharray", L"stroke-dashoffset"}){
        const auto value=parent.Get(inherited);
        if(!value.empty())(*result.values)[inherited]=value;
    }
}

void SetDefault(const std::shared_ptr<Node>& node, ComputedStyle& style) {
    auto set = [&](const wchar_t* key, const wchar_t* value) {
        if (!style.values->count(key)) (*style.values)[key] = value;
    };
    set(L"box-sizing", L"content-box");
    set(L"position", L"static");
    set(L"clip-path", L"none");
    set(L"color", L"#000000");
    set(L"font-family", L"Segoe UI");
    set(L"font-size", L"16px");
    set(L"font-style", L"normal");
    set(L"font-weight", L"400");
    set(L"font-kerning", L"auto");
    set(L"word-spacing", L"normal");
    set(L"overflow-wrap", L"normal");
    set(L"word-break", L"normal");
    set(L"hyphens", L"manual");
    set(L"tab-size", L"8");
    set(L"direction", L"ltr");
    set(L"unicode-bidi", L"normal");
    if(node&&node->type==NodeType::Element&&
       (node->namespaceUri.empty()||node->namespaceUri==L"http://www.w3.org/1999/xhtml")){
        // HTML direction attributes are local UA declarations, applied before
        // author CSS and before resolving logical properties. Invalid tokens
        // keep the inherited direction; HTML keywords do not trim whitespace.
        const auto direction=ToLower(node->Attribute(L"dir"));
        if(direction==L"ltr"||direction==L"rtl")(*style.values)[L"direction"]=direction;
        const auto& tag=node->tag;
        if(direction==L"ltr"||direction==L"rtl"||direction==L"auto"||
           tag==L"address"||tag==L"blockquote"||tag==L"center"||tag==L"div"||
           tag==L"figure"||tag==L"figcaption"||tag==L"footer"||tag==L"form"||
           tag==L"header"||tag==L"hr"||tag==L"legend"||tag==L"listing"||
           tag==L"main"||tag==L"p"||tag==L"plaintext"||tag==L"pre"||tag==L"summary"||
           tag==L"xmp"||tag==L"article"||tag==L"aside"||tag==L"hgroup"||tag==L"nav"||
           tag==L"search"||tag==L"section"||tag==L"table"||tag==L"caption"||
           tag==L"colgroup"||tag==L"col"||tag==L"thead"||tag==L"tbody"||tag==L"tfoot"||
           tag==L"tr"||tag==L"td"||tag==L"th"||tag==L"dir"||tag==L"dd"||tag==L"dl"||
           tag==L"dt"||tag==L"menu"||tag==L"ol"||tag==L"ul"||tag==L"li"||
           tag==L"bdi"||tag==L"output"||(tag.size()==2&&tag[0]==L'h'&&tag[1]>=L'1'&&tag[1]<=L'6'))
            (*style.values)[L"unicode-bidi"]=L"isolate";
        if(tag==L"bdo")(*style.values)[L"unicode-bidi"]=L"isolate-override";
    }
    set(L"text-align", L"start");
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
    else if (node->tag == L"caption") display = L"table-caption";
    else if (node->tag == L"col") display = L"table-column";
    else if (node->tag == L"colgroup") display = L"table-column-group";
    else if (node->tag == L"thead") display = L"table-header-group";
    else if (node->tag == L"tbody") display = L"table-row-group";
    else if (node->tag == L"tfoot") display = L"table-footer-group";
    else if (node->tag == L"tr") display = L"table-row";
    else if (node->tag == L"td" || node->tag == L"th") display = L"table-cell";
    else if (node->tag == L"li") display = L"list-item";
    else if (node->tag == L"head" || node->tag == L"style" || node->tag == L"script" || node->tag == L"meta" || node->tag == L"title" || node->tag == L"option" || node->tag == L"datalist" || node->tag == L"template" || (node->tag == L"noscript" && node->ownerDocument && node->ownerDocument->ScriptingEnabled()) || (node->tag == L"dialog" && !node->attributes.count(L"open")) || (node->namespaceUri.empty() && node->attributes.count(L"hidden"))) display = L"none";
    // The hidden attribute applies to every HTML tag, including controls and
    // inline elements selected earlier in the UA display classification.
    if (node && node->namespaceUri.empty() && node->attributes.count(L"hidden")) display = L"none";
    if (node && node->namespaceUri == L"http://www.w3.org/2000/svg" && node->tag == L"svg") display = L"inline";
    set(L"display", display.c_str());
    if(node&&(node->tag==L"col"||node->tag==L"colgroup")){
        const auto width=node->Attribute(L"width");
        if(!width.empty())set(L"width",width.c_str());
    }
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
    if(node&&node->tag==L"svg"){
        // Inline SVG gets a clipping viewport from the user-agent stylesheet.
        // A stand-alone SVG document root retains the visible initial value.
        if(const auto parent=node->parent.lock();parent&&parent->type!=NodeType::Document)
            (*style.values)[L"overflow"]=L"hidden";
        // SVG presentation dimensions participate below author CSS.
        const auto width=node->Attribute(L"width"),height=node->Attribute(L"height");
        if(!width.empty())set(L"width",width.c_str());
        if(!height.empty())set(L"height",height.c_str());
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
        (*style.values)[L"color"] = node->disabled?L"#545454":L"#000000";
    }
    if(node&&node->tag==L"textarea"){
        (*style.values)[L"font-family"]=L"monospace";(*style.values)[L"font-size"]=L"13.3333px";
        (*style.values)[L"font-weight"]=L"400";(*style.values)[L"line-height"]=L"normal";
        (*style.values)[L"color"]=node->disabled?L"#545454":L"#000000";
        set(L"background",L"white");set(L"border",L"2px inset #767676");set(L"padding",L"2px");
        (*style.values)[L"white-space"]=L"pre-wrap";
        set(L"overflow",L"auto");
    }
    if(node&&node->tag==L"table"){
        // These UA declarations are local to an HTML table, so inherited
        // spacing/collapse cannot outrank them; author declarations still can.
        (*style.values)[L"border-spacing"]=L"2px";
        (*style.values)[L"border-collapse"]=L"separate";
    }
    if(node&&(node->tag==L"table"||node->tag==L"button"||node->tag==L"select"||
       (node->tag==L"input"&&(node->Attribute(L"type")==L"checkbox"||node->Attribute(L"type")==L"radio"||node->Attribute(L"type")==L"search"||node->Attribute(L"type")==L"color"))||
       (node->tag==L"input"&&(node->Attribute(L"type")==L"button"||node->Attribute(L"type")==L"submit"||node->Attribute(L"type")==L"reset"))))
        (*style.values)[L"box-sizing"]=L"border-box";
    if(node&&node->tag==L"input"&&(node->Attribute(L"type")==L"button"||node->Attribute(L"type")==L"submit"||node->Attribute(L"type")==L"reset"))
        (*style.values)[L"text-align"]=L"center";
    if (node && node->tag == L"button") {
        set(L"padding", L"1px 6px"); set(L"border", L"2px outset #000000");
        set(L"background", L"#f0f0f0");
        (*style.values)[L"white-space"] = L"normal";
        // A button's user-agent style specifies centered text on the element
        // itself.  It therefore takes precedence over an inherited text-align,
        // while an author rule applied later in the cascade can still override it.
        (*style.values)[L"text-align"] = L"center";
    }
    if (node && node->tag == L"select") {
        set(L"border", L"1px solid #767676"); set(L"background", L"white");
        (*style.values)[L"white-space"] = L"pre";
    }
    if (node && node->tag == L"option") (*style.values)[L"white-space"] = L"nowrap";
    if (node && node->tag == L"object") (*style.values)[L"overflow"] = L"clip";
    if (node && node->tag == L"input" &&
        (node->Attribute(L"type") == L"checkbox" || node->Attribute(L"type") == L"radio")) {
        // Chromium's user-agent stylesheet keeps this asymmetric margin even
        // when author CSS supplies an explicit checkbox/radio width and height.
        set(L"margin", L"3px 3px 3px 4px");
    }
    if (node && node->tag == L"th") {
        (*style.values)[L"font-weight"] = L"700";
        // The UA internal-center value becomes center only for inherited
        // start alignment. Later author inherit/initial rules still win.
        if(style.Is(L"text-align",L"start"))(*style.values)[L"text-align"]=L"center";
    }
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
        const auto type=node->Attribute(L"type");
        const bool button=type==L"button"||type==L"submit"||type==L"reset";
        set(L"padding",button?L"1px 6px":L"1px 2px");
        set(L"border",button?L"2px outset #000000":L"2px inset #767676");
        set(L"background",button?L"#f0f0f0":L"white");
        (*style.values)[L"white-space"]=button?L"pre":L"normal";
    }
}

} // namespace

std::wstring ComputedStyle::Get(const std::wstring& name, const std::wstring& fallback) const {
    const bool alreadyLower=std::none_of(name.begin(),name.end(),[](wchar_t character){
        return std::iswupper(character)!=0;
    });
    auto it=values->end();
    const bool normalize=!alreadyLower&&name.rfind(L"--",0)!=0;
    const auto normalized=normalize?ToLower(name):std::wstring();
    const auto& lowered=normalize?normalized:name;
    if(lowered.rfind(L"--",0)!=0&&(lowered.find(L"inline")!=std::wstring::npos||lowered.find(L"block")!=std::wstring::npos)){
        const auto direction=values->find(L"direction"),mode=values->find(L"writing-mode");
        it=values->find(LogicalProperty(lowered,direction==values->end()?L"ltr":direction->second,
            mode==values->end()?L"horizontal-tb":mode->second));
    }else it=values->find(lowered);
    return it == values->end() || it->second == std::wstring(1,L'\0') ? fallback : it->second;
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

#include "CSSParser.inl"
#include "CSSConditions.inl"

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
        if(expression==L"screen"||expression==L"all")return true;
        if(expression==L"print")return false;
        const auto featureValue=[&](const std::wstring& name)->double{
            if(name==L"width")return viewportWidth;
            if(name==L"height")return viewportHeight;
            if(name==L"device-width")return displayWidth;
            if(name==L"device-height")return displayHeight;
            if(name==L"resolution")return devicePixelRatio;
            if(name==L"aspect-ratio")return viewportHeight>0?viewportWidth/viewportHeight:0;
            if(name==L"color")return 8;
            return std::numeric_limits<double>::quiet_NaN();
        };
        const auto comparison=expression.find_first_of(L"<>=");
        if(comparison!=std::wstring::npos){
            std::vector<std::wstring> operands,operators;size_t start=0;
            for(size_t i=0;i<expression.size();){
                if(expression[i]!=L'<'&&expression[i]!=L'>'&&expression[i]!=L'='){++i;continue;}
                operands.push_back(Trim(expression.substr(start,i-start)));const auto begin=i++;
                if(i<expression.size()&&expression[i]==L'=')++i;
                operators.push_back(expression.substr(begin,i-begin));start=i;
            }
            operands.push_back(Trim(expression.substr(start)));
            if(operands.size()<2||operands.size()>3)return false;
            std::wstring feature;
            for(const auto& operand:operands)if(std::isfinite(featureValue(operand))){if(!feature.empty())return false;feature=operand;}
            if(feature.empty()||(operands.size()==3&&operands[1]!=feature))return false;
            if(operators.size()==2&&(operators[0][0]!=operators[1][0]||operators[0][0]==L'='))return false;
            const auto operandValue=[&](const std::wstring& operand)->double{
                if(operand==feature)return featureValue(feature);
                if(feature==L"aspect-ratio"){
                    const auto ratio=Split(operand,L'/');float a=0,b=1;size_t used=0;
                    if(ratio.empty()||ratio.size()>2||!TryParseFloat(ratio[0],a,&used)||used!=ratio[0].size())return std::numeric_limits<double>::quiet_NaN();
                    if(ratio.size()==2&&(!TryParseFloat(ratio[1],b,&used)||used!=ratio[1].size()||b<=0))return std::numeric_limits<double>::quiet_NaN();
                    return a/b;
                }
                if(feature==L"resolution"){
                    float number=0;size_t used=0;if(!TryParseFloat(operand,number,&used))return std::numeric_limits<double>::quiet_NaN();
                    const auto unit=operand.substr(used);
                    return unit==L"dpi"?number/96.0:unit==L"dpcm"?number*2.54/96.0:unit==L"dppx"||unit==L"x"?number:std::numeric_limits<double>::quiet_NaN();
                }
                return MediaLength(operand,viewportWidth,viewportHeight);
            };
            for(size_t i=0;i<operators.size();++i){
                const auto a=operandValue(operands[i]),b=operandValue(operands[i+1]);if(!std::isfinite(a)||!std::isfinite(b))return false;
                const auto& op=operators[i];bool matches=false;
                if(op==L"<")matches=a<b;else if(op==L"<=")matches=a<=b;
                else if(op==L">")matches=a>b;else if(op==L">=")matches=a>=b;
                else if(op==L"=")matches=a==b;
                if(!matches)return false;
            }
            return true;
        }
        const auto colon=expression.find(L':');
        auto name=Trim(expression.substr(0,colon));
        const auto value=colon==std::wstring::npos?L"":Trim(expression.substr(colon+1));
        if(colon==std::wstring::npos&&std::isfinite(featureValue(name)))return featureValue(name)!=0;
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

    for(auto clause:Split(CssSyntax::Comments(source),L',')){
        clause=ToLower(Trim(clause));
        if(clause.rfind(L"only ",0)==0)clause=Trim(clause.substr(5));
        const auto afterNot=clause.rfind(L"not ",0)==0?Trim(clause.substr(4)):L"";
        const bool legacyNot=!afterNot.empty()&&afterNot.front()!=L'(';
        if(legacyNot)clause=Trim(clause.substr(4));
        const bool matches=CssCondition(clause,EvaluateMediaFeature);
        if(legacyNot?!matches:matches)return true;
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

#include "CSSVariables.inl"

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

ComputedStyle StyleSheet::AnonymousStyle(const ComputedStyle& parent,const std::wstring& display){
    ComputedStyle result;CopyInheritedStyle(parent,result);SetDefault({},result);
    (*result.values)[L"display"]=display;
    for(const auto& value:*parent.values)
        if(value.first.rfind(L"--",0)==0)(*result.values)[value.first]=value.second;
    return result;
}

ComputedStyle StyleSheet::Compute(const std::shared_ptr<Node>& node, const ComputedStyle* parent,
                                  const std::wstring& pseudo) const {
    if(const auto scoped=ShadowStyles(node))return scoped->Compute(node,parent,pseudo);
    ComputedStyle result;
    result.rootFontSize=parent?parent->rootFontSize:16.0f;
    if(parent)CopyInheritedStyle(*parent,result);
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
    struct Winner { bool important; int specificity; int order; std::vector<int> layer; bool inlineStyle=false; size_t declarationOrder=0; };
    FastMap<std::wstring, Winner> winners;
    struct PropertyCandidate {Winner winner;std::wstring value;};
    FastMap<std::wstring,std::vector<PropertyCandidate>> propertyCandidates;
    auto wins = [](const Winner& candidate, const Winner& current) {
        if(candidate.important!=current.important)return candidate.important;
        if(candidate.inlineStyle!=current.inlineStyle)return candidate.inlineStyle;
        for(size_t i=0;i<std::max(candidate.layer.size(),current.layer.size());++i){
            const int left=i<candidate.layer.size()?candidate.layer[i]:std::numeric_limits<int>::max();
            const int right=i<current.layer.size()?current.layer[i]:std::numeric_limits<int>::max();
            if(left!=right)return candidate.important?left<right:left>right;
        }
        return candidate.important > current.important ||
            (candidate.important == current.important &&
             (candidate.specificity > current.specificity ||
              (candidate.specificity == current.specificity &&
               (candidate.order > current.order || (candidate.order == current.order && candidate.declarationOrder >= current.declarationOrder)))));
    };
    const auto ruleApplies=[&](const CssRule& rule){return RuleApplies(rule,viewportWidth_,viewportHeight_);};
    FastMap<std::wstring,std::wstring> variables;
    if(parent){
        for(const auto& pair:*parent->values)
            if(pair.first.rfind(L"--",0)==0)variables[pair.first]=pair.second;
    }else if(node){
        // Layout may start at <body>; retain custom properties set dynamically
        // on its <html> ancestor through document.documentElement.style.
        std::vector<std::shared_ptr<Node>> ancestors;
        for(auto ancestor=node->parent.lock();ancestor;ancestor=ancestor->parent.lock())ancestors.push_back(ancestor);
        ComputedStyle ancestorStyle;
        bool hasAncestorStyle=false;
        for(auto it=ancestors.rbegin();it!=ancestors.rend();++it){
            if((*it)->type!=NodeType::Element)continue;
            ancestorStyle=Compute(*it,hasAncestorStyle?&ancestorStyle:nullptr);
            hasAncestorStyle=true;
        }
        if(hasAncestorStyle)for(const auto& pair:*ancestorStyle.values)
            if(pair.first.rfind(L"--",0)==0)variables[pair.first]=pair.second;
    }
    const auto candidateRules=CandidateRules(node);
    const auto inheritedVariables=variables;
    std::vector<const CssRule*> matchedRules;
    matchedRules.reserve(candidateRules.size());
    for(const auto* rulePointer:candidateRules){
        const auto& rule=*rulePointer;
        if(ruleApplies(rule)&&
           (pseudo.empty()?rule.pseudo.empty():rule.pseudo==pseudo)&&
           Document::MatchesSelector(node,rule.selectorParts))matchedRules.push_back(rulePointer);
    }
    const bool needsLayerRevert=std::any_of(matchedRules.begin(),matchedRules.end(),[](const auto* rule){
        return std::any_of(rule->declarations.begin(),rule->declarations.end(),[](const auto& item){return ToLower(Trim(item.value))==L"revert-layer";});
    })||std::any_of(node->inlineStyle.begin(),node->inlineStyle.end(),[](const auto& item){return ToLower(Trim(item.second))==L"revert-layer";});
    FastMap<std::wstring,Winner> customWinners;
    FastMap<std::wstring,std::vector<PropertyCandidate>> customCandidates;
    for(const auto* rulePointer:matchedRules){
        const auto& rule=*rulePointer;
        if(!rule.hasCustomDeclarations)continue;
        for(const auto& declaration:rule.declarations){
            if(declaration.name.rfind(L"--",0)!=0)continue;
            const Winner candidate{declaration.important,rule.specificity,rule.order,rule.layer};
            if(needsLayerRevert)customCandidates[declaration.name].push_back({candidate,declaration.value});
            const auto previous=customWinners.find(declaration.name);
            if(previous==customWinners.end()||wins(candidate,previous->second)){
                customWinners[declaration.name]=candidate;
                variables[declaration.name]=declaration.value;
            }
        }
    }
    if(pseudo.empty())for(const auto& pair:node->inlineStyle){
        if(pair.first.rfind(L"--",0)!=0)continue;
        const Winner candidate{node->inlineStylePriority.count(pair.first)!=0,0,0,{},true};const auto previous=customWinners.find(pair.first);
        if(needsLayerRevert)customCandidates[pair.first].push_back({candidate,pair.second});
        if(previous==customWinners.end()||wins(candidate,previous->second))variables[pair.first]=pair.second;
    }
    // Freeze custom properties at their defining element before inheritance.
    // Otherwise a child's --b could change a parent's --a:var(--b).
    const auto rawVariables=variables;
    CssVariableResolver variableResolver(rawVariables);
    for(auto& pair:variables){
        const auto keyword=ToLower(Trim(pair.second));
        if(keyword==L"initial")pair.second=std::wstring(1,L'\0');
        else if(keyword==L"inherit"||keyword==L"unset")pair.second=parent?parent->Get(pair.first,std::wstring(1,L'\0')):std::wstring(1,L'\0');
        else{std::wstring resolved;if(variableResolver.ResolveProperty(pair.first,resolved))pair.second=std::move(resolved);else pair.second=std::wstring(1,L'\0');}
        (*result.values)[pair.first]=pair.second;
    }
    for(auto& property:customCandidates){
        auto& candidates=property.second;
        std::stable_sort(candidates.begin(),candidates.end(),[&](const auto& a,const auto& b){return wins(b.winner,a.winner)&&!wins(a.winner,b.winner);});
        if(candidates.empty()||ToLower(Trim(candidates.back().value))!=L"revert-layer")continue;
        auto chosen=candidates.end();
        while(chosen!=candidates.begin()){
            --chosen;
            if(ToLower(Trim(chosen->value))!=L"revert-layer")break;
            const auto layer=chosen->winner.layer;const bool inlineStyle=chosen->winner.inlineStyle;
            while(chosen!=candidates.begin()){
                const auto previous=chosen-1;
                if(previous->winner.layer!=layer||previous->winner.inlineStyle!=inlineStyle)break;
                --chosen;
            }
            if(chosen==candidates.begin()){chosen=candidates.end();break;}
        }
        const auto inherited=inheritedVariables.find(property.first);
        variables[property.first]=chosen==candidates.end()?(inherited==inheritedVariables.end()?std::wstring(1,L'\0'):inherited->second):chosen->value;
    }
    for(auto& pair:variables){
        const auto keyword=ToLower(Trim(pair.second));
        if(keyword==L"inherit"||keyword==L"unset"||keyword==L"revert"){
            const auto inherited=inheritedVariables.find(pair.first);
            pair.second=inherited==inheritedVariables.end()?std::wstring(1,L'\0'):inherited->second;
        }
    }
    static const ComputedStyle initialStyle=[](){
    ComputedStyle initialValues;
    auto initialNode=std::make_shared<Node>();initialNode->tag=L"span";SetDefault(initialNode,initialValues);
    for(const auto* property:{L"width",L"height",L"min-width",L"min-height",L"top",L"right",L"bottom",L"left",L"flex-basis"})(*initialValues.values)[property]=L"auto";
    for(const auto* property:{L"max-width",L"max-height",L"background-image",L"box-shadow",L"transform",L"text-decoration",L"text-decoration-line",L"content"})(*initialValues.values)[property]=L"none";
    (*initialValues.values)[L"background-color"]=L"transparent";(*initialValues.values)[L"opacity"]=L"1";
    (*initialValues.values)[L"line-height"]=L"normal";
    (*initialValues.values)[L"font-kerning"]=L"auto";
    (*initialValues.values)[L"caption-side"]=L"top";
    (*initialValues.values)[L"writing-mode"]=L"horizontal-tb";
    (*initialValues.values)[L"text-orientation"]=L"mixed";
    for(const auto* property:{L"row-gap",L"column-gap",L"gap",L"letter-spacing"})(*initialValues.values)[property]=L"normal";
    for(const auto* side:{L"top",L"right",L"bottom",L"left"}){
        (*initialValues.values)[L"margin-"+std::wstring(side)]=L"0px";
        (*initialValues.values)[L"padding-"+std::wstring(side)]=L"0px";
        (*initialValues.values)[L"border-"+std::wstring(side)+L"-width"]=L"medium";
        (*initialValues.values)[L"border-"+std::wstring(side)+L"-style"]=L"none";
        (*initialValues.values)[L"border-"+std::wstring(side)+L"-color"]=L"currentcolor";
    }
    return initialValues;
    }();
    const auto inheritedProperty=[](const std::wstring& name){
        static const auto names=CssSyntax::Words(L"border-collapse border-spacing caption-side color color-scheme font-family font-size font-style font-weight font-kerning letter-spacing word-spacing overflow-wrap word-break hyphens line-height list-style-image list-style-position list-style-type tab-size direction writing-mode text-orientation text-align white-space pointer-events visibility fill fill-opacity fill-rule stroke stroke-opacity stroke-width stroke-linecap stroke-linejoin stroke-miterlimit stroke-dasharray stroke-dashoffset");
        return std::find(names.begin(),names.end(),name)!=names.end();
    };
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
    bool collectLayerCandidates=true,axesResolved=false;
    auto setProperty = [&](const std::wstring& authoredName, const std::wstring& value,
                           const Winner& candidate) {
        if(axesResolved&&(authoredName==L"direction"||authoredName==L"writing-mode"))return;
        const auto name=LogicalProperty(authoredName==L"word-wrap"?L"overflow-wrap":authoredName,result.Get(L"direction",L"ltr"),result.Get(L"writing-mode",L"horizontal-tb"));
        if(needsLayerRevert&&collectLayerCandidates)propertyCandidates[name].push_back({candidate,value});
        const auto current = winners.find(name);
        if (current != winners.end() && !wins(candidate, current->second)) return;
        winners[name] = candidate;
        bool valid=true;
        auto resolved = ResolveVariables(value, variables,0,&valid);
        if(!valid)resolved=L"unset";
        const auto wideKeyword=ToLower(Trim(resolved));
        if(wideKeyword==L"initial"||(wideKeyword==L"unset"&&!inheritedProperty(name))){
            (*result.values)[name]=initialStyle.Get(name);
        }else if(wideKeyword==L"revert"){
            const auto original=baseValues.find(name);(*result.values)[name]=original==baseValues.end()?initialStyle.Get(name):original->second;
        }else if(wideKeyword==L"inherit"||wideKeyword==L"unset") {
            const auto inherited = parent ? parent->Get(authoredName==L"word-wrap"?L"overflow-wrap":authoredName) : L"";
            if (!inherited.empty()) (*result.values)[name] = inherited;
            else {
                const auto initial = baseValues.find(name);
                if (initial != baseValues.end()) (*result.values)[name] = initial->second;
                else (*result.values)[name] = L"";
            }
        } else (*result.values)[name] = std::move(resolved);
    };
    auto applyDeclaration = [&](const std::wstring& name, const std::wstring& authoredValue,
                                const Winner& candidate) {
        bool valid=true;auto value=ResolveVariables(authoredValue,variables,0,&valid);
        if(!valid)value=L"unset";
        const auto keyword=ToLower(Trim(value));
        const bool wide=keyword==L"inherit"||keyword==L"initial"||keyword==L"unset"||keyword==L"revert"||keyword==L"revert-layer";
        if(name==L"margin-inline"||name==L"margin-block"||name==L"padding-inline"||name==L"padding-block"||name==L"inset-inline"||name==L"inset-block"){
            const auto parts=wide?std::vector<std::wstring>{keyword}:SplitWhitespace(value);
            if(parts.empty()||parts.size()>2)return;
            if(!wide){
                const bool padding=name.rfind(L"padding-",0)==0;
                const bool unitless=node&&node->ownerDocument&&node->ownerDocument->QuirksMode();
                for(const auto& token:parts){float n=0;size_t used=0;
                    if(TryParseFloat(token,n,&used)&&((padding&&n<0)||(!unitless&&used==token.size()&&n!=0)))return;
                }
            }
            setProperty(name,value,candidate);
            setProperty(name+L"-start",parts[0],candidate);
            setProperty(name+L"-end",parts.size()==2?parts[1]:parts[0],candidate);
            return;
        }
        if(name==L"border-inline"||name==L"border-block"){
            setProperty(name,value,candidate);
            for(const auto* edge:{L"-start",L"-end"}){
                const auto side=name+edge;
                setProperty(side+L"-width",value,candidate);
                setProperty(side+L"-style",value,candidate);
                setProperty(side+L"-color",wide?keyword:BorderColorFromShorthand(value),candidate);
            }
            return;
        }
        for(const auto* axis:{L"inline",L"block"})for(const auto* suffix:{L"width",L"style",L"color"}){
            const auto base=L"border-"+std::wstring(axis);
            if(name!=base+L"-"+suffix)continue;
            const auto parts=wide?std::vector<std::wstring>{keyword}:SplitWhitespace(value);
            if(parts.empty()||parts.size()>2)return;
            setProperty(name,value,candidate);
            setProperty(base+L"-start-"+suffix,parts[0],candidate);
            setProperty(base+L"-end-"+suffix,parts.size()==2?parts[1]:parts[0],candidate);
            return;
        }
        if(name==L"border-inline-start"||name==L"border-inline-end"||name==L"border-block-start"||name==L"border-block-end"){
            setProperty(name,value,candidate);
            setProperty(name+L"-width",value,candidate);
            setProperty(name+L"-style",value,candidate);
            setProperty(name+L"-color",wide?keyword:BorderColorFromShorthand(value),candidate);
            return;
        }
        if(!wide&&(name==L"color"||name==L"background-color"||name==L"outline-color"||
           (name.rfind(L"border-",0)==0&&name.size()>6&&name.compare(name.size()-6,6,L"-color")==0))&&
           keyword!=L"currentcolor"&&StyleSheet::Color(value,0x01020304u)==0x01020304u){
            if(authoredValue.find(L"var(")==std::wstring::npos)return;
            value=L"unset";
        }
        if(name==L"all"&&wide){
            auto properties=*initialStyle.values;
            for(const auto& pair:baseValues)properties[pair.first]=L"";
            for(const auto* rule:matchedRules)for(const auto& item:rule->declarations)properties[item.name]=L"";
            for(const auto& pair:node->inlineStyle)properties[pair.first]=L"";
            for(const auto& pair:properties)if(pair.first.rfind(L"--",0)!=0&&pair.first!=L"direction"&&pair.first!=L"unicode-bidi"&&
                LogicalProperty(pair.first,result.Get(L"direction",L"ltr"),result.Get(L"writing-mode",L"horizontal-tb"))==pair.first)
                setProperty(pair.first,keyword,candidate);
            return;
        }
        if(wide){
            setProperty(name,keyword,candidate);
            if(name==L"background")for(const auto* property:{L"background-color",L"background-image",L"background-position",L"background-size",L"background-repeat"})setProperty(property,keyword,candidate);
            else if(name==L"font")for(const auto* property:{L"font-family",L"font-size",L"font-style",L"font-weight",L"line-height"})setProperty(property,keyword,candidate);
            else if(name==L"flex")for(const auto* property:{L"flex-grow",L"flex-shrink",L"flex-basis"})setProperty(property,keyword,candidate);
            else if(name==L"gap"){setProperty(L"row-gap",keyword,candidate);setProperty(L"column-gap",keyword,candidate);}
            else if(name==L"columns"){setProperty(L"column-width",keyword,candidate);setProperty(L"column-count",keyword,candidate);}
            else if(name==L"overflow"){setProperty(L"overflow-x",keyword,candidate);setProperty(L"overflow-y",keyword,candidate);}
            else if(name==L"margin"||name==L"padding"||name==L"inset"||name==L"border"||name==L"border-color"||name==L"border-style"||name==L"border-width"){
                for(const auto* side:{L"top",L"right",L"bottom",L"left"}){
                    if(name==L"inset")setProperty(side,keyword,candidate);
                    else if(name==L"border")for(const auto* suffix:{L"width",L"style",L"color"})setProperty(L"border-"+std::wstring(side)+L"-"+suffix,keyword,candidate);
                    else if(name.rfind(L"border-",0)==0)setProperty(L"border-"+std::wstring(side)+name.substr(6),keyword,candidate);
                    else setProperty(name+L"-"+side,keyword,candidate);
                }
            }
            return;
        }
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
        } else if (name == L"border-radius") {
            const auto resolved=ResolveVariables(value,variables);
            const auto slash=resolved.find(L'/');
            const auto horizontal=ExpandEdges(resolved.substr(0,slash));
            const auto vertical=slash==std::wstring::npos?horizontal:ExpandEdges(resolved.substr(slash+1));
            if(horizontal.size()==4&&vertical.size()==4){
                static const wchar_t* corners[]={L"top-left",L"top-right",L"bottom-right",L"bottom-left"};
                const auto keyword=ToLower(Trim(resolved));
                const bool wide=keyword==L"inherit"||keyword==L"initial"||keyword==L"unset"||
                    keyword==L"revert"||keyword==L"revert-layer";
                for(size_t i=0;i<4;++i)setProperty(L"border-"+std::wstring(corners[i])+L"-radius",
                    wide?resolved:horizontal[i]+L" "+vertical[i],candidate);
            }
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
        } else if (name == L"columns") {
            std::wstring count=L"auto",width=L"auto";bool hasCount=false,hasWidth=false,valid=true;
            const auto parts=SplitWhitespace(ResolveVariables(value,variables));
            if(parts.empty()||parts.size()>2)valid=false;
            for(const auto& part:parts){
                if(ToLower(part)==L"auto")continue;
                unsigned long long integer=0;size_t used=0;
                if(TryParseUnsignedInteger(part,integer,&used)&&used==part.size()&&integer>0){
                    if(hasCount)valid=false;count=part;hasCount=true;
                }else{
                    if(hasWidth||StyleSheet::Length(part,100,100,-1)<=0)valid=false;
                    width=part;hasWidth=true;
                }
            }
            if(valid){setProperty(L"column-count",count,candidate);setProperty(L"column-width",width,candidate);}
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
    const auto resolveLayerProperty=[&](const std::wstring& name){
        const auto found=propertyCandidates.find(name);if(found==propertyCandidates.end())return;
        auto& candidates=found->second;
        std::stable_sort(candidates.begin(),candidates.end(),[&](const auto& a,const auto& b){return wins(b.winner,a.winner)&&!wins(a.winner,b.winner);});
        if(candidates.empty()||ToLower(Trim(candidates.back().value))!=L"revert-layer")return;
        auto chosen=candidates.end();
        while(chosen!=candidates.begin()){
            --chosen;if(ToLower(Trim(chosen->value))!=L"revert-layer")break;
            const auto layer=chosen->winner.layer;const bool inlineStyle=chosen->winner.inlineStyle;
            while(chosen!=candidates.begin()){
                const auto previous=chosen-1;
                if(previous->winner.layer!=layer||previous->winner.inlineStyle!=inlineStyle)break;
                --chosen;
            }
            if(chosen==candidates.begin()){chosen=candidates.end();break;}
        }
        const auto value=chosen==candidates.end()?L"revert":chosen->value;
        const auto candidate=winners[name];winners.erase(name);
        collectLayerCandidates=false;setProperty(name,value,candidate);collectLayerCandidates=true;
    };
    // Resolve the axes before mapping any logical declaration. Their source
    // order relative to padding/margin does not alter the final writing mode.
    for(const auto* rulePointer:matchedRules){
        const auto& rule=*rulePointer;size_t ordinal=0;
        for(const auto& declaration:rule.declarations){
            const Winner candidate{declaration.important,rule.specificity,rule.order,rule.layer,false,++ordinal};
            if(declaration.name==L"direction"||declaration.name==L"writing-mode")
                applyDeclaration(declaration.name,declaration.value,candidate);
            else if(declaration.name==L"all"){
                const auto keyword=ToLower(Trim(ResolveVariables(declaration.value,variables)));
                if(keyword==L"initial"||keyword==L"inherit"||keyword==L"unset"||keyword==L"revert"||keyword==L"revert-layer")
                    applyDeclaration(L"writing-mode",keyword,candidate);
            }
        }
    }
    if(pseudo.empty())for(const auto& pair:node->inlineStyle){
        const auto order=node->inlineStyleOrder.find(pair.first);
        const Winner candidate{node->inlineStylePriority.count(pair.first)!=0,0,0,{},true,order==node->inlineStyleOrder.end()?0:order->second};
        if(pair.first==L"direction"||pair.first==L"writing-mode")applyDeclaration(pair.first,pair.second,candidate);
        else if(pair.first==L"all"){
            const auto keyword=ToLower(Trim(ResolveVariables(pair.second,variables)));
            if(keyword==L"initial"||keyword==L"inherit"||keyword==L"unset"||keyword==L"revert"||keyword==L"revert-layer")
                applyDeclaration(L"writing-mode",keyword,candidate);
        }
    }
    resolveLayerProperty(L"direction");resolveLayerProperty(L"writing-mode");axesResolved=true;
    for (const auto* rulePointer : matchedRules) {
        const auto& rule=*rulePointer;
        size_t ordinal=0;
        for (const auto& declaration : rule.declarations) {
            ++ordinal;
            if (declaration.name.rfind(L"--", 0) == 0) continue;
            const Winner candidate{declaration.important, rule.specificity, rule.order,rule.layer,false,ordinal};
            applyDeclaration(declaration.name, declaration.value, candidate);
        }
    }
    if(pseudo.empty())for (const auto& pair : node->inlineStyle) {
        if(pair.first.rfind(L"--",0)==0)continue;
        const auto order=node->inlineStyleOrder.find(pair.first);
        applyDeclaration(pair.first, pair.second,
                         {node->inlineStylePriority.count(pair.first)!=0, 0, 0,{},true,order==node->inlineStyleOrder.end()?0:order->second});
    }
    for(const auto& property:propertyCandidates)resolveLayerProperty(property.first);
    // HTML hidden-state inputs are non-rendered controls. Chromium enforces
    // this as a user-agent !important rule, so even a broad author rule such
    // as `input { display:block }` must not expose their submitted values.
    // Keep the rule in the common style cascade rather than teaching table or
    // form layout about one particular page.
    if(pseudo.empty()&&node&&node->tag==L"input"&&
       ToLower(Trim(node->Attribute(L"type")))==L"hidden")
        (*result.values)[L"display"]=L"none";
    if(pseudo.empty()&&node){
        const auto type=ToLower(Trim(node->Attribute(L"type")));
        float selectSize=0;TryParseFloat(node->Attribute(L"size"),selectSize);
        const bool clippedInput=node->tag==L"input"&&type!=L"checkbox"&&type!=L"radio"&&type!=L"range";
        const bool menuSelect=node->tag==L"select"&&!node->attributes.count(L"multiple")&&selectSize<=1;
        // Native text controls and menu selects have UA !important overflow
        // clipping. Author longhands/shorthands cannot expose their contents.
        if(clippedInput||menuSelect)for(const auto* property:{L"overflow",L"overflow-x",L"overflow-y"})
            (*result.values)[property]=L"clip";
        if(menuSelect&&!result.Is(L"appearance",L"none"))(*result.values)[L"line-height"]=L"normal";
    }
    // Font size computes once against the parent's computed CSS-pixel font.
    // The root's own rem unit uses the initial font, then descendants use the
    // root's resulting size. Device scale never participates in this step.
    const float parentFont=parent?Length(parent->Get(L"font-size",L"16px"),16,viewportWidth_,16,16):16;
    const bool rootElement=node&&node->tag==L"html"&&pseudo.empty();
    const float remBasis=rootElement?16:result.rootFontSize;
    auto fontValue=ToLower(Trim(result.Get(L"font-size",L"16px")));
    float fontSize=parentFont;
    static const FastMap<std::wstring,float> absoluteFonts={{L"xx-small",9.0f},{L"x-small",10.0f},{L"small",13.0f},
        {L"medium",16.0f},{L"large",18.0f},{L"x-large",24.0f},{L"xx-large",32.0f},{L"xxx-large",48.0f}};
    if(const auto keyword=absoluteFonts.find(fontValue);keyword!=absoluteFonts.end())fontSize=keyword->second;
    else if(fontValue==L"larger")fontSize=parentFont*1.2f;
    else if(fontValue==L"smaller")fontSize=parentFont/1.2f;
    else fontSize=Length(ResolveViewportUnits(ResolveRootFontUnits(fontValue,remBasis),viewportWidth_,viewportHeight_),
        parentFont,viewportWidth_,parentFont,parentFont);
    if(!std::isfinite(fontSize)||fontSize<0)fontSize=parentFont;
    (*result.values)[L"font-size"]=std::to_wstring(fontSize)+L"px";
    if(rootElement)result.rootFontSize=fontSize;
    if(result.rootFontSize!=16.0f)for(auto& property:*result.values){
        if(property.first.rfind(L"--",0)!=0&&property.first!=L"font-size")
            property.second=ResolveRootFontUnits(property.second,result.rootFontSize);
    }
    // Percentages and font-relative lengths compute against this element's
    // font, then descendants inherit the resulting length. Unitless numbers
    // stay numbers and are multiplied by each descendant's own font size.
    const auto lineHeight=ToLower(Trim(result.Get(L"line-height")));
    float number=0;size_t consumed=0;
    const bool mathLength=(lineHeight.rfind(L"calc(",0)==0||lineHeight.rfind(L"min(",0)==0||
        lineHeight.rfind(L"max(",0)==0||lineHeight.rfind(L"clamp(",0)==0)&&
        (lineHeight.find(L"px")!=std::wstring::npos||lineHeight.find(L"em")!=std::wstring::npos||lineHeight.find(L'%')!=std::wstring::npos);
    if((TryParseFloat(lineHeight,number,&consumed)&&consumed<lineHeight.size())||mathLength){
        const auto resolved=ResolveViewportUnits(lineHeight,viewportWidth_,viewportHeight_);
        (*result.values)[L"line-height"]=std::to_wstring(Length(resolved,fontSize,viewportWidth_,fontSize,fontSize))+L"px";
    }
    // Spacing lengths compute at their defining element. Descendants inherit
    // CSS-pixel lengths even when they change font size; normal letter spacing
    // remains a keyword so justification can distinguish it from explicit zero.
    for(const auto* property:{L"letter-spacing",L"word-spacing"}){
        const auto value=ToLower(Trim(result.Get(property,L"normal")));
        if(value==L"normal"||value.empty()){
            if(std::wstring_view(property)==L"word-spacing")(*result.values)[property]=L"0px";
            continue;
        }
        const float spacing=Length(ResolveViewportUnits(value,viewportWidth_,viewportHeight_),
            fontSize,viewportWidth_,0,fontSize);
        (*result.values)[property]=std::to_wstring(std::isfinite(spacing)?spacing:0)+L"px";
    }
    if(node&&node->type==NodeType::Element&&parent&&
       (parent->Is(L"display",L"flex")||parent->Is(L"display",L"inline-flex")||
        parent->Is(L"display",L"grid")||parent->Is(L"display",L"inline-grid"))&&
       !result.Is(L"position",L"absolute")&&!result.Is(L"position",L"fixed")){
        auto display=result.Get(L"display");
        if(display==L"inline"||display==L"inline-block")display=L"block";
        else if(display==L"inline-flex")display=L"flex";
        else if(display==L"inline-grid")display=L"grid";
        else if(display==L"inline-table")display=L"table";
        (*result.values)[L"display"]=display;
    }
    return result;
}

#include "CSSMath.inl"

#include "CSSColorFunctions.inl"

unsigned int StyleSheet::Color(const std::wstring& raw, unsigned int fallback) {
    auto value = ToLower(Trim(raw));
    if (value == L"transparent" || value == L"none") return 0;
    static const FastMap<std::wstring,unsigned int> named={
#include "CSSNamedColors.inl"
    };
    if(const auto found=named.find(value);found!=named.end())return found->second;
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
    return CssFunctionalColor(value,fallback);
}

} // namespace TWebFrame::Internal
