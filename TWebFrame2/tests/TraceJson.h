#pragma once
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace ParityTrace {
inline std::wstring Quote(const std::wstring& value) {
    std::wstring out=L"\"";
    constexpr wchar_t hex[]=L"0123456789abcdef";
    for(wchar_t c:value){
        if(c==L'"'||c==L'\\'){out+=L'\\';out+=c;}
        else if(c<32){out+=L"\\u00";out+=hex[(c>>4)&15];out+=hex[c&15];}
        else out+=c;
    }
    return out+L'"';
}
inline void Space(const std::wstring& text,size_t& at){while(at<text.size()&&iswspace(text[at]))++at;}
inline bool String(const std::wstring& text,size_t& at,std::wstring& out) {
    if(at>=text.size()||text[at++]!=L'"')return false;
    out.clear();
    while(at<text.size()){
        wchar_t c=text[at++];
        if(c==L'"')return true;
        if(c==L'\\'){
            if(at>=text.size())return false;
            c=text[at++];
            if(c==L'u'){
                if(at+4>text.size())return false;
                unsigned value=0;
                for(int i=0;i<4;++i){const auto digit=towlower(text[at++]);
                    if(digit>=L'0'&&digit<=L'9')value=value*16+digit-L'0';
                    else if(digit>=L'a'&&digit<=L'f')value=value*16+digit-L'a'+10;
                    else return false;
                }
                c=static_cast<wchar_t>(value);
            }else if(c==L'n')c=L'\n';else if(c==L'r')c=L'\r';else if(c==L't')c=L'\t';
            else if(c==L'b')c=L'\b';else if(c==L'f')c=L'\f';
            else if(c!=L'"'&&c!=L'\\'&&c!=L'/')return false;
        }
        out+=c;
    }
    return false;
}
inline bool Skip(const std::wstring& text,size_t& at) {
    Space(text,at);if(at>=text.size())return false;
    if(text[at]==L'"'){std::wstring ignored;return String(text,at,ignored);}
    if(text[at]==L'{'||text[at]==L'['){
        const wchar_t close=text[at++]==L'{'?L'}':L']';
        while(at<text.size()){
            Space(text,at);if(at>=text.size())return false;
            if(text[at]==close){++at;return true;}
            if(text[at]==L','||text[at]==L':'){++at;continue;}
            if(!Skip(text,at))return false;
        }
        return false;
    }
    const size_t begin=at;
    while(at<text.size()&&!iswspace(text[at])&&text[at]!=L','&&text[at]!=L']'&&text[at]!=L'}')++at;
    return at>begin;
}
inline std::wstring RawField(const std::wstring& json,const std::wstring& key) {
    size_t at=0;Space(json,at);if(at>=json.size()||json[at++]!=L'{')return L"";
    while(at<json.size()){
        Space(json,at);if(at>=json.size()||json[at]==L'}')return L"";
        std::wstring name;if(!String(json,at,name))return L"";
        Space(json,at);if(at>=json.size()||json[at++]!=L':')return L"";
        Space(json,at);const size_t begin=at;if(!Skip(json,at))return L"";
        if(name==key)return json.substr(begin,at-begin);
        Space(json,at);if(at>=json.size()||json[at++]!=L',')return L"";
    }
    return L"";
}
inline std::wstring Field(const std::wstring& json,const std::wstring& key) {
    auto raw=RawField(json,key);size_t at=0;std::wstring value;
    return String(raw,at,value)?value:L"";
}
inline long Integer(const std::wstring& json,const std::wstring& key) {
    const auto raw=RawField(json,key);return wcstol(raw.c_str(),nullptr,10);
}
inline std::wstring SafeName(std::wstring value){
    for(auto& c:value)if(!((c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||c==L'-'||c==L'_'))c=L'_';
    return value.empty()?L"main":value.substr(0,120);
}
inline std::string Utf8(const std::wstring& value) {
    const int size=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    std::string bytes(static_cast<size_t>(size),'\0');
    WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),bytes.data(),size,nullptr,nullptr);
    return bytes;
}
inline void WriteText(const std::filesystem::path& path,const std::wstring& text){
    const auto bytes=Utf8(text);std::ofstream file(path,std::ios::binary);file.write(bytes.data(),bytes.size());
}
inline std::wstring ReadText(const std::filesystem::path& path){
    std::ifstream file(path,std::ios::binary);const std::string bytes((std::istreambuf_iterator<char>(file)),{});
    const int size=MultiByteToWideChar(CP_UTF8,0,bytes.data(),static_cast<int>(bytes.size()),nullptr,0);
    std::wstring text(static_cast<size_t>(size),L'\0');
    MultiByteToWideChar(CP_UTF8,0,bytes.data(),static_cast<int>(bytes.size()),text.data(),size);return text;
}
}
