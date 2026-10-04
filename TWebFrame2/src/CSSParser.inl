bool StyleSheet::Parse(const std::wstring& source,std::wstring* error) {
    ++version_;
    rules_.clear();ruleIndex_.clear();universalRuleIndexes_.clear();keyframes_.clear();shadowStyles_.clear();
    selectorAttributes_.clear();nthChildSubjects_.clear();hasUniversalNthChild_=false;
    rootVariables_.clear();pseudoRules_.clear();hoverRuleIndexes_.clear();
    hoverRequiresBroadInvalidation_=false;mutationRequiresBroadInvalidation_=false;
    usesNthChild_=false;usesViewportFontSize_=false;
    const auto css=CssSyntax::Comments(source);
    int order=0,anonymousLayer=0;
    FastMap<std::wstring,std::vector<std::wstring>> layers;
    struct Context {std::wstring parents,layerName;std::vector<int> layer;std::vector<std::wstring> media;};
    auto enterLayer=[&](Context context,std::wstring name){
        if(name.empty())name=L"@anonymous"+std::to_wstring(++anonymousLayer);
        for(const auto& component:Split(name,L'.')){
            auto& siblings=layers[context.layerName];
            const auto found=std::find(siblings.begin(),siblings.end(),component);
            const int index=static_cast<int>(found==siblings.end()?siblings.size():found-siblings.begin());
            if(found==siblings.end())siblings.push_back(component);
            context.layer.push_back(index);
            if(!context.layerName.empty())context.layerName+=L'.';context.layerName+=component;
        }
        return context;
    };
    auto nestedSelector=[](const std::wstring& parent,const std::wstring& child){
        if(parent.empty())return Trim(child);
        const auto reference=L":is("+parent+L")";
        std::wstring selector;bool replaced=false;wchar_t quote=0;int attribute=0;
        for(size_t i=0;i<child.size();++i){const auto c=child[i];
            if(c==L'\\'){const auto end=CssSyntax::EscapeEnd(child,i);selector+=child.substr(i,end-i);i=end-1;continue;}
            if(quote){selector+=c;if(c==quote)quote=0;continue;}
            if(c==L'\''||c==L'"'){quote=c;selector+=c;continue;}
            if(c==L'[')++attribute;if(c==L']')--attribute;
            if(c==L'&'&&!attribute){selector+=reference;replaced=true;}else selector+=c;
        }
        return replaced?selector:reference+L" "+child;
    };
    auto addRules=[&](const Context& context,const std::vector<CssDeclaration>& declarations){
        if(context.parents.empty()||declarations.empty())return;
        for(const auto& selector:Split(context.parents,L',')){
            CssRule rule;rule.selector=selector;rule.declarations=declarations;
            rule.specificity=Specificity(selector);rule.order=order++;rule.mediaQueries=context.media;rule.layer=context.layer;
            size_t pseudo=std::wstring::npos,separator=0;
            for(size_t start=0;;){
                const auto colon=CssSyntax::Find(selector,L":",start);if(colon==std::wstring::npos)break;
                if(colon+1<selector.size()&&selector[colon+1]==L':'){pseudo=colon;separator=2;break;}
                const auto suffix=ToLower(Trim(selector.substr(colon+1)));
                if(suffix==L"before"||suffix==L"after"||suffix==L"first-line"||suffix==L"first-letter"){pseudo=colon;separator=1;break;}
                start=colon+1;
            }
            if(pseudo!=std::wstring::npos)rule.pseudo=ToLower(Trim(selector.substr(pseudo+separator)));
            rule.selectorParts=Document::CompileSelector(pseudo==std::wstring::npos?selector:selector.substr(0,pseudo));
            CollectSelectorAttributes(selector,selectorAttributes_);
            if(selector.find(L":lang(")!=std::wstring::npos)selectorAttributes_[L"lang"]=true;
            for(const auto& part:rule.selectorParts){
                if(part.find(L"nth-")!=std::wstring::npos){
                    usesNthChild_=true;const auto key=SelectorSubjectKey({part});
                    if(key.empty())hasUniversalNthChild_=true;else nthChildSubjects_[key]=true;
                }
            }
            rule.hasCustomDeclarations=std::any_of(declarations.begin(),declarations.end(),[](const auto& item){return item.name.rfind(L"--",0)==0;});
            const auto subject=SelectorSubjectKey(rule.selectorParts);
            rules_.push_back(std::move(rule));const auto index=rules_.size()-1;
            const bool broad=selector.find(L'+')!=std::wstring::npos||selector.find(L'~')!=std::wstring::npos||selector.find(L":has(")!=std::wstring::npos;
            mutationRequiresBroadInvalidation_=mutationRequiresBroadInvalidation_||broad;
            if(selector.find(L":hover")!=std::wstring::npos){hoverRuleIndexes_.push_back(index);hoverRequiresBroadInvalidation_=hoverRequiresBroadInvalidation_||broad;}
            if(subject.empty())universalRuleIndexes_.push_back(index);else ruleIndex_[subject].push_back(index);
            if(!rules_.back().pseudo.empty()&&std::find(pseudoRules_.begin(),pseudoRules_.end(),rules_.back().pseudo)==pseudoRules_.end())pseudoRules_.push_back(rules_.back().pseudo);
            if(context.media.empty()&&Trim(selector)==L":root")for(const auto& declaration:declarations)
                if(declaration.name.rfind(L"--",0)==0)rootVariables_[declaration.name]=declaration.value;
        }
    };
    std::function<bool(const std::wstring&,Context,unsigned)> parse;
    parse=[&](const std::wstring& text,Context context,unsigned depth){
        if(depth>128){if(error)*error=L"CSS block nesting exceeds 128 levels";return false;}
        size_t position=0;std::vector<CssDeclaration> declarations;
        auto flush=[&](){addRules(context,declarations);declarations.clear();};
        while(position<text.size()){
            while(position<text.size()&&(CssSyntax::Space(text[position])||text[position]==L';'||text[position]==L'}'))++position;
            if(position>=text.size())break;
            if(context.parents.empty()&&(text.compare(position,4,L"<!--")==0||text.compare(position,3,L"-->")==0)){position+=text[position]==L'<'?4:3;continue;}
            const bool at=text[position]==L'@';
            auto terminator=CssSyntax::Find(text,L";{",position);
            if(!at&&!context.parents.empty()){
                // Custom properties may contain arbitrary balanced {} values.
                const auto colon=CssSyntax::Find(text,L":",position);
                const auto rawName=colon==std::wstring::npos?L"":Trim(text.substr(position,colon-position));
                if(rawName.rfind(L"--",0)==0)terminator=CssSyntax::Find(text,L";",position);
                if(terminator==std::wstring::npos||text[terminator]==L';'){
                    CssSyntax::Declaration item;
                    if(CssSyntax::ParseDeclaration(std::wstring_view(text).substr(position,terminator==std::wstring::npos?text.size()-position:terminator-position),item)){
                        if(item.name==L"font-size"){
                            const auto value=ToLower(item.value);usesViewportFontSize_=usesViewportFontSize_||value.find(L"vw")!=std::wstring::npos||value.find(L"vh")!=std::wstring::npos||value.find(L"vmin")!=std::wstring::npos||value.find(L"vmax")!=std::wstring::npos;
                        }
                        declarations.push_back({item.name,item.value,item.important});
                    }
                    position=terminator==std::wstring::npos?text.size():terminator+1;continue;
                }
            }
            flush();
            const auto prelude=Trim(text.substr(position,terminator==std::wstring::npos?text.size()-position:terminator-position));
            const auto nameEnd=at?CssSyntax::IdentifierEnd(prelude,1):0;
            const auto name=at?ToLower(CssSyntax::Decode(std::wstring_view(prelude).substr(1,nameEnd-1))):L"";
            const auto argument=at?Trim(prelude.substr(nameEnd)):L"";
            if(terminator==std::wstring::npos||text[terminator]==L';'){
                if(at&&name==L"layer")for(const auto& layer:Split(argument,L','))enterLayer(context,layer);
                position=terminator==std::wstring::npos?text.size():terminator+1;continue;
            }
            const auto end=CssSyntax::Close(text,terminator);
            const auto body=text.substr(terminator+1,end-terminator-1);
            position=end<text.size()?end+1:text.size(); // EOF implicitly closes blocks.
            if(at){
                if(name==L"media"){
                    auto nested=context;nested.media.push_back(argument);if(!parse(body,nested,depth+1))return false;
                }else if(name==L"layer"){
                    if(!parse(body,enterLayer(context,argument),depth+1))return false;
                }else if(name==L"supports"){
                    if(Supports(argument)&&!parse(body,context,depth+1))return false;
                }else if((name==L"keyframes"||name==L"-webkit-keyframes")&&context.parents.empty()){
                    CssKeyframes animation;auto animationName=argument;
                    if(animationName.size()>1&&(animationName.front()==L'\''||animationName.front()==L'"')&&animationName.back()==animationName.front())animationName=animationName.substr(1,animationName.size()-2);
                    animation.name=CssSyntax::Decode(animationName);animation.mediaQueries=context.media;
                    size_t framePosition=0;
                    while(framePosition<body.size()){
                        const auto brace=CssSyntax::Find(body,L"{",framePosition);if(brace==std::wstring::npos)break;
                        const auto frameEnd=CssSyntax::Close(body,brace);std::vector<CssDeclaration> frameDeclarations;
                        for(const auto& item:CssSyntax::Declarations(std::wstring_view(body).substr(brace+1,frameEnd-brace-1)))
                            if(!item.important)frameDeclarations.push_back({item.name,item.value,false});
                        for(auto offsetText:Split(body.substr(framePosition,brace-framePosition),L',')){
                            offsetText=ToLower(offsetText);float offset=0;size_t used=0;
                            if(offsetText==L"from")offset=0;else if(offsetText==L"to")offset=1;
                            else if(TryParseFloat(offsetText,offset,&used)&&offsetText.substr(used)==L"%"&&offset>=0&&offset<=100)offset/=100;
                            else continue;
                            animation.frames.push_back({offset,frameDeclarations});
                        }
                        framePosition=frameEnd<body.size()?frameEnd+1:body.size();
                    }
                    std::stable_sort(animation.frames.begin(),animation.frames.end(),[](const auto& a,const auto& b){return a.offset<b.offset;});
                    if(!animation.name.empty()&&!animation.frames.empty())keyframes_.push_back(std::move(animation));
                }
            }else{
                Context nested=context;std::wstring selectors;
                for(const auto& child:Split(prelude,L',')){
                    if(!selectors.empty())selectors+=L", ";selectors+=nestedSelector(context.parents,child);
                }
                nested.parents=selectors;if(!parse(body,nested,depth+1))return false;
            }
        }
        flush();return true;
    };
    const bool success=parse(css,{},0);
    if(success&&error)error->clear();return success;
}
