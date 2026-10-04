namespace {
bool CssSupportsShape(const std::wstring& input,unsigned depth=0){
    if(depth>128)return false;
    const auto text=Trim(input);const auto words=SplitWhitespace(text);if(words.empty())return false;
    if(words.size()>1){
        if(ToLower(words[0])==L"not")return words.size()==2&&CssSupportsShape(words[1],depth+1);
        if(words.size()%2==0)return false;
        const auto op=ToLower(words[1]);if(op!=L"and"&&op!=L"or")return false;
        for(size_t i=0;i<words.size();i+=2)if((i&&ToLower(words[i-1])!=op)||!CssSupportsShape(words[i],depth+1))return false;
        return true;
    }
    if(text.front()==L'('&&CssSyntax::Close(text,0)==text.size()-1){
        const auto inner=Trim(text.substr(1,text.size()-2));if(inner.empty())return false;
        if(inner.front()==L'('||inner.rfind(L"not ",0)==0)return CssSupportsShape(inner,depth+1);
        return true;
    }
    const auto end=CssSyntax::IdentifierEnd(text,0);
    return end>0&&end<text.size()&&text[end]==L'('&&CssSyntax::Close(text,end)==text.size()-1;
}
bool CssSupportedSelector(const std::wstring& selector,unsigned depth=0){
    if(depth>128)return false;
    const auto selectors=Split(selector,L',');if(selectors.empty())return false;
    for(const auto& item:selectors){
        const auto parts=Document::CompileSelector(item);if(parts.empty())return false;bool needsSubject=true;
        for(const auto& part:parts){
            if(part==L">"||part==L"+"||part==L"~"){if(needsSubject)return false;needsSubject=true;continue;}
            needsSubject=false;
            for(size_t i=0;i<part.size();){
                const auto c=part[i++];
                if(c==L'['){const auto close=CssSyntax::Close(part,i-1);if(close>=part.size())return false;i=close+1;continue;}
                if(c==L'*')continue;
                if(c==L':'&&i<part.size()&&part[i]==L':')++i;
                if(c==L'&'||c==L'|'||c==L'!'||CssSyntax::Space(c))return false;
                const auto start=(c==L':'||c==L'#'||c==L'.')?i:i-1;
                const auto end=CssSyntax::IdentifierEnd(part,start);if(end==start)return false;
                const auto name=ToLower(CssSyntax::Decode(std::wstring_view(part).substr(start,end-start)));i=end;
                if(c!=L':')continue;
                static const auto supported=CssSyntax::Words(L"root checked indeterminate disabled enabled required optional link any-link hover focus focus-visible focus-within empty first-child last-child only-child first-of-type last-of-type only-of-type before after first-line first-letter placeholder backdrop is where not has nth-child nth-last-child nth-of-type nth-last-of-type lang");
                if(std::find(supported.begin(),supported.end(),name)==supported.end())return false;
                if(i<part.size()&&part[i]==L'('){
                    const auto close=CssSyntax::Close(part,i);if(close>=part.size())return false;
                    auto arguments=part.substr(i+1,close-i-1);i=close+1;
                    if(name==L"is"||name==L"where"||name==L"not"||name==L"has"){
                        for(auto argument:Split(arguments,L',')){
                            if(name==L"has"&&!argument.empty()&&(argument[0]==L'>'||argument[0]==L'+'||argument[0]==L'~'))argument=Trim(argument.substr(1));
                            if(!CssSupportedSelector(argument,depth+1))return false;
                        }
                    }
                }
            }
        }
        if(needsSubject)return false;
    }
    return true;
}
bool CssCondition(const std::wstring& input,const std::function<bool(const std::wstring&)>& feature,unsigned depth=0){
    if(depth>128)return false;
    const auto text=Trim(input);const auto words=SplitWhitespace(text);
    if(words.empty())return false;
    if(ToLower(words[0])==L"not")return words.size()==2&&!CssCondition(words[1],feature,depth+1);
    if(words.size()>1){
        if(words.size()%2==0)return false;
        const auto operation=ToLower(words[1]);if(operation!=L"and"&&operation!=L"or")return false;
        bool result=operation==L"and";
        for(size_t i=0;i<words.size();i+=2){
            if(i&&ToLower(words[i-1])!=operation)return false;
            const bool value=CssCondition(words[i],feature,depth+1);
            result=operation==L"and"?result&&value:result||value;
        }
        return result;
    }
    if(text.front()==L'('&&CssSyntax::Close(text,0)==text.size()-1){
        const auto inner=Trim(text.substr(1,text.size()-2));
        if(CssSyntax::Find(inner,L":<>=")!=std::wstring::npos)return feature(inner);
        return CssCondition(inner,feature,depth+1);
    }
    return feature(text);
}
}

bool StyleSheet::Supports(const std::wstring& condition){
    const auto text=CssSyntax::Comments(condition);if(!CssSupportsShape(text))return false;
    return CssCondition(text,[](const std::wstring& feature){
        if(feature.rfind(L"selector(",0)==0&&CssSyntax::Close(feature,8)==feature.size()-1)
            return CssSupportedSelector(feature.substr(9,feature.size()-10));
        const auto colon=CssSyntax::Find(feature,L":");
        if(colon==std::wstring::npos)return false;
        return Supports(Trim(feature.substr(0,colon)),Trim(feature.substr(colon+1)));
    });
}

bool StyleSheet::Supports(const std::wstring& property,const std::wstring& input){
    auto name=ToLower(Trim(property));const auto value=ToLower(Trim(input));
    if(value.empty())return name.rfind(L"--",0)==0;
    if(name.rfind(L"--",0)==0)return true;
    const auto keyword=[&](std::wstring_view choices){
        for(const auto& item:CssSyntax::Words(choices))if(item==value)return true;return false;
    };
    const bool wide=keyword(L"initial inherit unset revert revert-layer");
    if(name==L"display")return wide||keyword(L"none block inline inline-block flex inline-flex grid inline-grid table inline-table table-row table-cell table-row-group table-header-group table-footer-group table-column table-column-group table-caption list-item contents");
    if(name==L"position")return wide||keyword(L"static relative absolute fixed sticky");
    if(name==L"box-sizing")return wide||keyword(L"content-box border-box");
    if(name==L"overflow"||name==L"overflow-x"||name==L"overflow-y")return wide||keyword(L"visible hidden clip scroll auto");
    if(name==L"visibility")return wide||keyword(L"visible hidden collapse");
    if(name==L"color"||name==L"background-color"||name==L"border-color"||name==L"outline-color"||name==L"fill"||name==L"stroke"||
       (name.rfind(L"border-",0)==0&&name.size()>6&&name.compare(name.size()-6,6,L"-color")==0))
        return wide||value==L"currentcolor"||Color(value,0x01020304u)!=0x01020304u;
    if(name==L"width"||name==L"height"||name==L"min-width"||name==L"max-width"||name==L"min-height"||name==L"max-height"||
       name==L"top"||name==L"right"||name==L"bottom"||name==L"left"||name==L"font-size"||name==L"gap"||name==L"row-gap"||name==L"column-gap"||
       name==L"margin"||name==L"padding"||name.rfind(L"margin-",0)==0||name.rfind(L"padding-",0)==0){
        if(wide)return true;
        const bool padding=name==L"padding"||name.rfind(L"padding-",0)==0;
        const bool margin=name==L"margin"||name.rfind(L"margin-",0)==0;
        const bool gap=name==L"gap"||name==L"row-gap"||name==L"column-gap";
        const bool size=name.find(L"width")!=std::wstring::npos||name.find(L"height")!=std::wstring::npos;
        const bool inset=name==L"top"||name==L"right"||name==L"bottom"||name==L"left";
        if(value==L"auto")return margin||inset||size;
        if(value==L"none")return name==L"max-width"||name==L"max-height";
        if(value==L"normal")return gap;
        if(keyword(L"min-content max-content fit-content"))return size;
        const float invalid=std::numeric_limits<float>::quiet_NaN();
        const auto tokens=SplitWhitespace(value);
        const size_t maximum=name==L"padding"||name==L"margin"?4:name==L"gap"?2:1;
        if(tokens.empty()||tokens.size()>maximum)return false;
        for(const auto& token:tokens){
            if(margin&&token==L"auto")continue;
            float literal=0;size_t used=0;
            if(TryParseFloat(token,literal,&used)&&used==token.size()&&literal!=0)return false;
            const auto number=Length(token,100,100,invalid);
            if(!std::isfinite(number)||((padding||gap||size||name==L"font-size")&&number<0))return false;
        }
        return true;
    }
    if(name==L"opacity"||name==L"flex-grow"||name==L"flex-shrink"||name==L"order"||name==L"z-index"){
        if(wide)return true;
        float number=0;size_t used=0;if(!TryParseFloat(value,number,&used)||used!=value.size())return false;
        if(name==L"flex-grow"||name==L"flex-shrink")return number>=0;
        if(name==L"order"||name==L"z-index")return std::floor(number)==number;
        return true;
    }
    return false;
}
