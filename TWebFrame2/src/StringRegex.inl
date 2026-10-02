    Value StringRegexOperation(const std::wstring& input,const std::wstring& operation,const std::vector<Value>& args){
        const auto value=args.empty()?Value::Undefined():Deref(args[0]);
        const bool isRegex=value.object&&value.object->kind==ObjectKind::RegExp;
        const auto pattern=isRegex?String(value.object->props[L"$pattern"]):value.type==Value::Type::Undefined?L"":String(value);
        const auto flags=isRegex?String(value.object->props[L"$flags"]):operation==L"matchAll"?L"g":L"";
        const bool unicode=flags.find(L'u')!=std::wstring::npos;
        const auto expression=Regex(pattern,flags);RuntimeRegex::Match match;
        if(operation==L"search"){
            if(!expression->Search(input,0,flags.find(L'y')!=std::wstring::npos,match))return Value::Number(-1);
            RegexMatchValue(match,input);return Value::Number(static_cast<double>(match.index));
        }
        if(operation==L"match"&&flags.find(L'g')==std::wstring::npos)
            return isRegex?ExecuteRegex(value.object,input):expression->Search(input,0,false,match)?RegexMatchValue(match,input):Value::Null();
        if(operation==L"matchAll"&&isRegex&&flags.find(L'g')==std::wstring::npos)
            return Value::Thrown(ErrorValue(L"TypeError",L"matchAll requires a global RegExp"));
        const size_t limit=operation==L"split"&&args.size()>1&&Deref(args[1]).type!=Value::Type::Undefined?
            Uint32(args[1]):(std::numeric_limits<uint32_t>::max)();
        std::vector<Value> results;std::wstring output;
        if(!limit)return ArrayValue(results);
        size_t start=0,cursor=0;
        if(operation==L"matchAll"&&isRegex){const auto last=Number(GetProperty(value,L"lastIndex"));
            if(last>0)start=static_cast<size_t>(std::min(last,static_cast<double>(input.size())+1));}
        if(operation==L"match"&&isRegex)value.object->props[L"lastIndex"]=Value::Number(0);
        while(start<=input.size()&&expression->Search(input,start,operation!=L"split"&&flags.find(L'y')!=std::wstring::npos,match)){
            if(operation==L"split"){
                if(match.end==cursor){start=RuntimeRegex::Advance(input,match.end,unicode);continue;}
                if(match.index==input.size())break;
                results.push_back(Value::String(input.substr(cursor,match.index-cursor)));
                for(size_t i=1;i<match.captures.size()&&results.size()<limit;++i)
                    results.push_back(match.captures[i]?Value::String(*match.captures[i]):Value::Undefined());
                cursor=match.end;if(results.size()>=limit)break;
            }else if(operation==L"match"){
                RegexMatchValue(match,input);results.push_back(Value::String(*match.captures[0]));
            }else if(operation==L"matchAll")results.push_back(RegexMatchValue(match,input,flags.find(L'd')!=std::wstring::npos));
            else{
                output+=input.substr(cursor,match.index-cursor);
                const auto result=RegexMatchValue(match,input),replacement=args.size()>1?Deref(args[1]):Value::Undefined();
                if(IsCallable(replacement)){
                    auto arguments=result.object->items;arguments.push_back(Value::Number(static_cast<double>(match.index)));
                    arguments.push_back(Value::String(input));
                    if(!match.names.empty())arguments.push_back(result.object->props[L"groups"]);
                    output+=String(Call(replacement,Value::Undefined(),arguments));
                }else{
                    const auto text=String(replacement);
                    for(size_t i=0;i<text.size();++i){
                        if(text[i]!=L'$'||i+1==text.size()){output+=text[i];continue;}
                        const auto token=text[i+1];
                        if(token==L'$'){output+=L'$';++i;}
                        else if(token==L'&'){output+=*match.captures[0];++i;}
                        else if(token==L'`'){output+=input.substr(0,match.index);++i;}
                        else if(token==L'\''){output+=input.substr(match.end);++i;}
                        else if(token==L'<'&&!match.names.empty()){
                            const auto end=text.find(L'>',i+2);
                            if(end==std::wstring::npos){output+=L'$';continue;}
                            const auto name=text.substr(i+2,end-i-2);
                            for(const auto& group:match.names)if(group.first==name&&match.captures[group.second])output+=*match.captures[group.second];
                            i=end;
                        }else if(token>=L'0'&&token<=L'9'){
                            size_t capture=token-L'0',used=1;
                            if(i+2<text.size()&&text[i+2]>=L'0'&&text[i+2]<=L'9'){
                                const size_t two=capture*10+text[i+2]-L'0';if(two>0&&two<match.captures.size()){capture=two;used=2;}}
                            if(capture>0&&capture<match.captures.size()){if(match.captures[capture])output+=*match.captures[capture];i+=used;}
                            else output+=L'$';
                        }else output+=L'$';
                    }
                }
                cursor=match.end;if(flags.find(L'g')==std::wstring::npos)break;
            }
            start=match.end==match.index?RuntimeRegex::Advance(input,match.end,unicode):match.end;
            if(results.size()>1000000)throw JavaScriptException{ErrorValue(L"RangeError",L"Too many regular expression results")};
        }
        if(operation==L"replace"){output+=input.substr(cursor);if(isRegex&&flags.find(L'g')!=std::wstring::npos)value.object->props[L"lastIndex"]=Value::Number(0);return Value::String(output);}
        if(operation==L"split"){
            if(input.empty()&&expression->Search(input,0,false,match))return ArrayValue({});
            if(results.size()<limit)results.push_back(Value::String(input.substr(cursor)));
        }
        auto array=ArrayValue(results);
        if(operation==L"matchAll")return Call(GetProperty(array,L"values"),array,{});
        return operation==L"match"&&results.empty()?Value::Null():array;
    }
