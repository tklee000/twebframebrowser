// Shared by stylesheet matching and DOM selector APIs (inside DOM.cpp).
bool MatchSimple(const std::shared_ptr<Node>& node, const std::wstring& source);
std::vector<std::wstring> SplitSelector(std::wstring selector);

bool MatchParts(const std::shared_ptr<Node>& node,const std::vector<std::wstring>& parts,
                int index,const std::shared_ptr<Node>& anchor={}) {
    if(!node||index<0)return false;
    const auto& subject=parts[static_cast<size_t>(index)];
    if(subject==L":scope"&&anchor){if(node!=anchor)return false;}
    else if(!MatchSimple(node,subject))return false;
    if(index==0)return true;
    auto combinator=parts[static_cast<size_t>(index-1)];
    const bool explicitCombinator=combinator==L">"||combinator==L"+"||combinator==L"~";
    const int previous=index-(explicitCombinator?2:1);
    if(previous<0)return false;
    if(combinator==L"+"||combinator==L"~"){
        const auto parent=node->parent.lock();if(!parent)return false;
        auto end=std::find(parent->children.begin(),parent->children.end(),node);
        while(end!=parent->children.begin()){
            const auto sibling=*--end;if(sibling->type!=NodeType::Element)continue;
            if(MatchParts(sibling,parts,previous,anchor))return true;
            if(combinator==L"+")return false;
        }
        return false;
    }
    for(auto parent=node->parent.lock();parent;parent=parent->parent.lock()){
        if(MatchParts(parent,parts,previous,anchor))return true;
        if(combinator==L">")return false;
    }
    return false;
}

bool MatchNth(int index,std::wstring expression) {
    expression=ToLower(expression);
    expression.erase(std::remove_if(expression.begin(),expression.end(),CssSyntax::Space),expression.end());
    if(index<=0||expression.empty())return false;
    if(expression==L"odd")return index%2==1;
    if(expression==L"even")return index%2==0;
    const auto integer=[](const std::wstring& text,int& number){size_t used=0;return TryParseInteger(text,number,&used)&&used==text.size();};
    const auto n=expression.find(L'n');int a=0,b=0;
    if(n==std::wstring::npos)return integer(expression,b)&&index==b;
    const auto coefficient=expression.substr(0,n);
    if(coefficient.empty()||coefficient==L"+")a=1;
    else if(coefficient==L"-")a=-1;
    else if(!integer(coefficient,a))return false;
    if(n+1<expression.size()&&!integer(expression.substr(n+1),b))return false;
    if(a==0)return index==b;
    const auto delta=static_cast<long long>(index)-b;
    return delta%a==0&&delta/a>=0;
}

bool MatchSimple(const std::shared_ptr<Node>& node, const std::wstring& source) {
    if(!node||node->type!=NodeType::Element)return false;
    // Bound recursive functional selectors, without affecting ordinary matching.
    static thread_local unsigned depth=0;
    if(depth>=128)return false;
    struct DepthScope {unsigned& value;explicit DepthScope(unsigned& v):value(v){++value;}~DepthScope(){--value;}} scope(depth);
    const auto selector=Trim(source);if(selector.empty())return false;
    if(selector.find_first_of(L":[\\")==std::wstring::npos)return MatchPlainSimple(node,selector);
    size_t i=0;
    if(selector[i]==L'*')++i;
    else if(CssSyntax::Name(selector[i])||selector[i]==L'\\'){
        const auto end=CssSyntax::IdentifierEnd(selector,i);
        if(node->tag!=ToLower(CssSyntax::Decode(std::wstring_view(selector).substr(i,end-i))))return false;
        i=end;
    }
    while(i<selector.size()){
        const auto kind=selector[i++];
        if(kind==L'#'||kind==L'.'){
            const auto end=CssSyntax::IdentifierEnd(selector,i);
            if(end==i)return false;
            const auto name=CssSyntax::Decode(std::wstring_view(selector).substr(i,end-i));i=end;
            if(kind==L'#'?node->Attribute(L"id")!=name:!node->HasClass(name))return false;
        }else if(kind==L'['){
            const auto close=CssSyntax::Close(selector,i-1);if(close>=selector.size())return false;
            const auto expression=Trim(selector.substr(i,close-i));i=close+1;
            const auto equals=CssSyntax::Find(expression,L"=");
            size_t op=equals;std::wstring operation=L"=";
            if(equals!=std::wstring::npos&&equals>0&&std::wstring(L"~|^$*").find(expression[equals-1])!=std::wstring::npos){op=equals-1;operation=expression.substr(op,2);}
            const auto rawKey=Trim(expression.substr(0,op));
            if(rawKey.empty()||CssSyntax::IdentifierEnd(rawKey,0)!=rawKey.size())return false;
            const auto key=ToLower(CssSyntax::Decode(rawKey));
            if(!node->attributes.count(key))return false;
            if(equals==std::wstring::npos)continue;
            auto words=CssSyntax::Words(expression.substr(equals+1));
            if(words.empty()||words.size()>2)return false;
            bool insensitive=false;
            if(words.size()==2){const auto flag=ToLower(words[1]);if(flag!=L"i"&&flag!=L"s")return false;insensitive=flag==L"i";}
            auto value=words[0];
            if(value.size()>=2&&(value[0]==L'\''||value[0]==L'"')){
                if(value.back()!=value.front())return false;value=value.substr(1,value.size()-2);
            }else if(CssSyntax::IdentifierEnd(value,0)!=value.size())return false;
            value=CssSyntax::Decode(value);auto actual=node->Attribute(key);
            if(insensitive){
                const auto asciiLower=[](std::wstring& text){for(auto& c:text)if(c>=L'A'&&c<=L'Z')c+=L'a'-L'A';};
                asciiLower(actual);asciiLower(value);
            }
            bool matches=false;
            if(operation==L"=")matches=actual==value;
            else if(operation==L"|=")matches=actual==value||actual.rfind(value+L"-",0)==0;
            else if(!value.empty()){
                if(operation==L"^=")matches=actual.rfind(value,0)==0;
                else if(operation==L"$=")matches=actual.size()>=value.size()&&actual.compare(actual.size()-value.size(),value.size(),value)==0;
                else if(operation==L"*=")matches=actual.find(value)!=std::wstring::npos;
                else if(operation==L"~="){const auto tokens=CssSyntax::Words(actual);matches=std::find(tokens.begin(),tokens.end(),value)!=tokens.end();}
            }
            if(!matches)return false;
        }else if(kind==L':'){
            const auto end=CssSyntax::IdentifierEnd(selector,i);
            const auto name=ToLower(CssSyntax::Decode(std::wstring_view(selector).substr(i,end-i)));i=end;
            const bool functional=i<selector.size()&&selector[i]==L'(';
            std::wstring arguments;
            if(functional){const auto close=CssSyntax::Close(selector,i);if(close>=selector.size())return false;arguments=selector.substr(i+1,close-i-1);i=close+1;}
            if(name==L"is"||name==L"where"||name==L"not"){
                if(!functional)return false;bool matched=false;
                for(const auto& item:CssSyntax::Split(arguments,L','))if(Document::MatchesSelector(node,item)){matched=true;break;}
                if(name==L"not"?matched:!matched)return false;
            }else if(name==L"has"){
                if(!functional)return false;bool matched=false;
                for(const auto& relative:CssSyntax::Split(arguments,L',')){
                    if(relative.find(L":has(")!=std::wstring::npos)return false;
                    auto parts=SplitSelector(L":scope "+relative);
                    auto root=node;
                    if(!relative.empty()&&(relative[0]==L'+'||relative[0]==L'~'))while(const auto parent=root->parent.lock())root=parent;
                    std::function<void(const std::shared_ptr<Node>&)> visit=[&](const auto& candidate){
                        if(matched)return;
                        if(candidate!=node&&MatchParts(candidate,parts,static_cast<int>(parts.size())-1,node)){matched=true;return;}
                        for(const auto& child:candidate->children)visit(child);
                    };
                    visit(root);if(matched)break;
                }
                if(!matched)return false;
            }else if(name==L"nth-child"||name==L"nth-last-child"||name==L"nth-of-type"||name==L"nth-last-of-type"||
                     name==L"first-child"||name==L"last-child"||name==L"only-child"||name==L"first-of-type"||name==L"last-of-type"||name==L"only-of-type"){
                const bool sameType=name.find(L"of-type")!=std::wstring::npos;
                const bool last=name.find(L"last")!=std::wstring::npos;
                const bool nth=name.rfind(L"nth-",0)==0;
                if(nth!=functional)return false;
                std::wstring filter;auto formula=arguments;
                const auto of=ToLower(arguments).find(L" of ");
                if(of!=std::wstring::npos){if(sameType)return false;formula=arguments.substr(0,of);filter=Trim(arguments.substr(of+4));if(filter.empty())return false;}
                const auto parent=node->parent.lock();std::vector<std::shared_ptr<Node>> siblings;
                if(parent){for(const auto& child:parent->children)
                    if(child->type==NodeType::Element&&(!sameType||child->tag==node->tag)&&(filter.empty()||Document::MatchesSelector(child,filter)))siblings.push_back(child);
                }else if(filter.empty()||Document::MatchesSelector(node,filter))siblings.push_back(node);
                const auto found=std::find(siblings.begin(),siblings.end(),node);if(found==siblings.end())return false;
                const int index=static_cast<int>(last?siblings.end()-found:found-siblings.begin()+1);
                if(nth?!MatchNth(index,formula):name.find(L"only")!=std::wstring::npos?siblings.size()!=1:index!=1)return false;
            }else if(name==L"lang"){
                if(!functional)return false;std::wstring language;
                for(auto ancestor=node;ancestor;ancestor=ancestor->parent.lock()){
                    if(ancestor->attributes.count(L"lang")){language=ToLower(ancestor->Attribute(L"lang"));break;}
                }
                bool matched=false;
                for(auto range:CssSyntax::Split(arguments,L',')){
                    if(range.size()>1&&(range.front()==L'\''||range.front()==L'"'))range=range.substr(1,range.size()-2);
                    range=ToLower(CssSyntax::Decode(range));
                    if(!language.empty()&&(range==L"*"||language==range||language.rfind(range+L"-",0)==0))matched=true;
                }
                if(!matched)return false;
            }else{
                if(functional)return false;
                const bool disableable=node->tag==L"button"||node->tag==L"fieldset"||node->tag==L"input"||node->tag==L"optgroup"||node->tag==L"option"||node->tag==L"select"||node->tag==L"textarea";
                const bool requireable=node->tag==L"input"||node->tag==L"select"||node->tag==L"textarea";
                bool state=false;
                if(name==L"root")state=node->tag==L"html";
                else if(name==L"checked")state=node->checked;
                else if(name==L"indeterminate")state=node->indeterminate;
                else if(name==L"disabled")state=disableable&&node->disabled;
                else if(name==L"enabled")state=disableable&&!node->disabled;
                else if(name==L"required")state=requireable&&node->attributes.count(L"required")!=0;
                else if(name==L"optional")state=requireable&&node->attributes.count(L"required")==0;
                else if(name==L"link"||name==L"any-link")state=(node->tag==L"a"||node->tag==L"area")&&node->attributes.count(L"href")!=0;
                else if(name==L"hover")state=node->hovered;
                else if(name==L"focus")state=node->focused;
                else if(name==L"focus-visible")state=node->focusVisible;
                else if(name==L"focus-within")state=node->focusWithin||node->focused;
                else if(name==L"empty")state=std::none_of(node->children.begin(),node->children.end(),[](const auto& child){return child->type==NodeType::Element||(child->type==NodeType::Text&&!child->text.empty());});
                else return false;
                if(!state)return false;
            }
        }else return false;
    }
    return true;
}
