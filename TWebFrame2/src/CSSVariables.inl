namespace {
class CssVariableResolver {
    const FastMap<std::wstring,std::wstring>& variables_;
    FastMap<std::wstring,int> state_;
    FastMap<std::wstring,bool> cyclic_;
    FastMap<std::wstring,bool> resolving_;
    std::vector<std::wstring> stack_;
    std::vector<std::wstring> resolvingStack_;
    static std::vector<std::wstring> References(const std::wstring& text){
        std::vector<std::wstring> result;wchar_t quote=0;
        for(size_t i=0;i<text.size();++i){
            const auto c=text[i];
            if(c==L'\\'){i=CssSyntax::EscapeEnd(text,i)-1;continue;}
            if(quote){if(c==quote)quote=0;continue;}
            if(c==L'\''||c==L'"'){quote=c;continue;}
            if(text.compare(i,4,L"var(")==0&&(i==0||!CssSyntax::Name(text[i-1]))){
                const auto close=CssSyntax::Close(text,i+3);
                const auto comma=CssSyntax::Find(std::wstring_view(text).substr(i+4,close-i-4),L",");
                result.push_back(CssSyntax::Decode(Trim(text.substr(i+4,comma==std::wstring::npos?close-i-4:comma))));
                // WebView2 substitutes the selected fallback; an unused fallback
                // does not introduce a cycle in the active dependency graph.
                i=close;
            }
        }
        return result;
    }
    void Visit(const std::wstring& name,unsigned depth){
        if(depth>128){cyclic_[name]=true;return;}
        if(state_[name]==2)return;
        if(state_[name]==1){
            const auto start=std::find(stack_.begin(),stack_.end(),name);
            for(auto it=start;it!=stack_.end();++it)cyclic_[*it]=true;
            return;
        }
        state_[name]=1;stack_.push_back(name);
        const auto found=variables_.find(name);
        if(found!=variables_.end())for(const auto& reference:References(found->second))
            if(variables_.count(reference))Visit(reference,depth+1);
        stack_.pop_back();state_[name]=2;
    }
public:
    explicit CssVariableResolver(const FastMap<std::wstring,std::wstring>& values):variables_(values){
        for(const auto& item:values)Visit(item.first,0);
    }
    bool ResolveProperty(const std::wstring& name,std::wstring& output){
        if(resolvingStack_.size()>=128)return false;
        const auto found=variables_.find(name);
        if(found==variables_.end()||cyclic_[name]||found->second==std::wstring(1,L'\0'))return false;
        if(ToLower(Trim(found->second))==L"initial")return false;
        if(resolving_[name]){
            const auto start=std::find(resolvingStack_.begin(),resolvingStack_.end(),name);
            for(auto it=start;it!=resolvingStack_.end();++it)cyclic_[*it]=true;
            return false;
        }
        resolving_[name]=true;resolvingStack_.push_back(name);
        const bool success=Resolve(found->second,output);
        resolvingStack_.pop_back();resolving_[name]=false;
        return success&&!cyclic_[name];
    }
    bool Resolve(const std::wstring& text,std::wstring& output,unsigned depth=0){
        if(depth>128)return false;
        wchar_t quote=0;
        for(size_t i=0;i<text.size();){
            const auto c=text[i];
            if(c==L'\\'){const auto end=CssSyntax::EscapeEnd(text,i);output+=text.substr(i,end-i);i=end;continue;}
            if(quote){output+=c;if(c==quote)quote=0;++i;continue;}
            if(c==L'\''||c==L'"'){quote=c;output+=c;++i;continue;}
            if(text.compare(i,4,L"var(")!=0||(i>0&&CssSyntax::Name(text[i-1]))){output+=c;++i;continue;}
            const auto close=CssSyntax::Close(text,i+3);if(close>=text.size())return false;
            const auto expression=text.substr(i+4,close-i-4);
            const auto comma=CssSyntax::Find(expression,L",");
            const auto rawName=Trim(expression.substr(0,comma));
            const auto name=CssSyntax::Decode(rawName);
            if(name.rfind(L"--",0)!=0||CssSyntax::IdentifierEnd(rawName,0)!=rawName.size())return false;
            std::wstring replacement;bool valid=false;
            const auto found=variables_.find(name);
            if(found!=variables_.end()&&!cyclic_[name]&&found->second!=std::wstring(1,L'\0'))
                valid=ResolveProperty(name,replacement);
            if(!valid){
                replacement.clear();
                if(comma==std::wstring::npos||!Resolve(Trim(expression.substr(comma+1)),replacement,depth+1))return false;
            }
            output+=replacement;i=close+1;
        }
        return true;
    }
};
}

std::wstring StyleSheet::ResolveVariables(const std::wstring& input,
                                         const FastMap<std::wstring,std::wstring>& vars,
                                         int,bool* valid) const {
    if(input.find(L"var(")==std::wstring::npos){if(valid)*valid=true;return input;}
    CssVariableResolver resolver(vars);std::wstring result;
    const bool success=resolver.Resolve(input,result);
    if(valid)*valid=success;
    return success?result:std::wstring(1,L'\0');
}
