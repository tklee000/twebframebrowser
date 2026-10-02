#pragma once
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace RegressionIO {
inline std::wstring Quote(const std::wstring& value){
    std::wstring out=L"\"";constexpr wchar_t hex[]=L"0123456789abcdef";
    for(wchar_t c:value){if(c==L'"'||c==L'\\'){out+=L'\\';out+=c;}
        else if(c<32){out+=L"\\u00";out+=hex[(c>>4)&15];out+=hex[c&15];}else out+=c;}
    return out+L'"';
}
inline std::string Utf8(const std::wstring& value){
    const int size=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    std::string bytes(static_cast<size_t>(size),'\0');
    WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),bytes.data(),size,nullptr,nullptr);return bytes;
}
inline void WriteText(const std::filesystem::path& path,const std::wstring& value){
    const auto bytes=Utf8(value);std::ofstream output(path,std::ios::binary);output.write(bytes.data(),bytes.size());
}
inline std::wstring ReadText(const std::filesystem::path& path){
    std::ifstream input(path,std::ios::binary);const std::string bytes((std::istreambuf_iterator<char>(input)),{});
    const int size=MultiByteToWideChar(CP_UTF8,0,bytes.data(),static_cast<int>(bytes.size()),nullptr,0);
    std::wstring value(static_cast<size_t>(size),L'\0');
    MultiByteToWideChar(CP_UTF8,0,bytes.data(),static_cast<int>(bytes.size()),value.data(),size);return value;
}
}
