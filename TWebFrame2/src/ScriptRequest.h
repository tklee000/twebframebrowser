#pragma once

#include <functional>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <TWebFrame/BrowserContext.h>

namespace TWebFrame::Internal {
inline bool CorsSafelistedRequestHeader(const std::wstring& name,const std::wstring& value){
    if(value.size()>128)return false;
    const auto unsafeByte=[](wchar_t character){
        return character>255||character<=8||(character>=10&&character<=31)||character==127||
            std::wstring_view(L"\"():<>?@[\\]{}").find(character)!=std::wstring_view::npos;
    };
    if(name==L"accept"||name==L"content-type"){
        for(const auto character:value)if(unsafeByte(character))return false;
        if(name==L"accept")return true;
        auto mime=value.substr(0,value.find(L';'));
        const auto first=mime.find_first_not_of(L" \t"),last=mime.find_last_not_of(L" \t");
        if(first==std::wstring::npos)return false;mime=mime.substr(first,last-first+1);
        for(auto& character:mime)if(character>=L'A'&&character<=L'Z')character+=L'a'-L'A';
        return mime==L"application/x-www-form-urlencoded"||mime==L"multipart/form-data"||mime==L"text/plain";
    }
    if(name==L"accept-language"||name==L"content-language"){
        for(const auto character:value)
            if(!((character>=L'0'&&character<=L'9')||(character>=L'A'&&character<=L'Z')||
                 (character>=L'a'&&character<=L'z')||std::wstring_view(L" *,-.;=").find(character)!=std::wstring_view::npos))return false;
        return true;
    }
    if(name==L"range"&&value.rfind(L"bytes=",0)==0){
        const auto dash=value.find(L'-',6);
        if(dash==std::wstring::npos||dash==6)return false;
        for(size_t index=6;index<value.size();++index)
            if(index!=dash&&(value[index]<L'0'||value[index]>L'9'))return false;
        if(dash+1==value.size())return true;
        auto start=value.substr(6,dash-6),end=value.substr(dash+1);
        const auto normalize=[](std::wstring& number){const auto first=number.find_first_not_of(L'0');number.erase(0,first==std::wstring::npos?number.size()-1:first);};
        normalize(start);normalize(end);return start.size()<end.size()||(start.size()==end.size()&&start<=end);
    }
    return false;
}
using ScriptRequest=TWebFrame::NetworkRequest;
using ScriptResponse=TWebFrame::NetworkResponse;
using ScriptRequestLoader=std::function<ScriptResponse(const ScriptRequest&)>;
using AsyncScriptRequestLoader=std::function<void(const ScriptRequest&,std::function<void(ScriptResponse)>)>;
}
