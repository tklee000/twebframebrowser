#pragma once

#include <windows.h>
#include <memory>
#include <string>
#include <vector>

namespace TWebFrame::Internal {

// Windows supplies ICU as an OS component. Resolve the C ABI at runtime so
// older hosts can leave unsupported Intl services absent without a DLL link
// dependency or a second JavaScript engine.
class WindowsCollation {
    using Open=void* (__cdecl*)(const char*,int*);
    using Close=void (__cdecl*)(void*);
    using Attribute=void (__cdecl*)(void*,int,int,int*);
    using Compare=int (__cdecl*)(const void*,const char16_t*,int,const char16_t*,int);
    Open open_=nullptr;Close close_=nullptr;Attribute attribute_=nullptr;Compare compare_=nullptr;
    WindowsCollation(){
        const auto library=LoadLibraryExW(L"icu.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(library){
            open_=reinterpret_cast<Open>(GetProcAddress(library,"ucol_open"));
            close_=reinterpret_cast<Close>(GetProcAddress(library,"ucol_close"));
            attribute_=reinterpret_cast<Attribute>(GetProcAddress(library,"ucol_setAttribute"));
            compare_=reinterpret_cast<Compare>(GetProcAddress(library,"ucol_strcoll"));
        }
    }
public:
    static const WindowsCollation& Instance(){static const WindowsCollation instance;return instance;}
    bool Available()const{return open_&&close_&&attribute_&&compare_;}
    std::shared_ptr<void> Create(const std::string& locale,const std::wstring& sensitivity,
                                 const std::wstring& caseFirst,bool numeric,bool ignorePunctuation)const{
        if(!Available())return {};int status=0;
        auto result=std::shared_ptr<void>(open_(locale.c_str(),&status),[close=close_](void* value){if(value)close(value);});
        if(status>0||!result)return {};
        // UColAttribute and UColAttributeValue are fixed ICU C ABI enums.
        attribute_(result.get(),4,17,&status); // normalization mode, on
        attribute_(result.get(),5,sensitivity==L"base"||sensitivity==L"case"?0:sensitivity==L"accent"?1:2,&status);
        attribute_(result.get(),3,sensitivity==L"case"?17:16,&status); // case level
        attribute_(result.get(),2,caseFirst==L"upper"?25:caseFirst==L"lower"?24:16,&status);
        attribute_(result.get(),7,numeric?17:16,&status);
        attribute_(result.get(),1,ignorePunctuation?20:21,&status);
        return status<=0?result:std::shared_ptr<void>{};
    }
    int CompareStrings(const std::shared_ptr<void>& collator,const std::wstring& first,const std::wstring& second)const{
        return compare_(collator.get(),reinterpret_cast<const char16_t*>(first.data()),static_cast<int>(first.size()),
                        reinterpret_cast<const char16_t*>(second.data()),static_cast<int>(second.size()));
    }
};

// Relative-time patterns and number formatting come from the same ICU locale
// data as the OS. Keep the native formatter alive on the JavaScript instance.
class WindowsRelativeTime {
    using Open=void* (__cdecl*)(const char*,void*,int,int,int*);
    using Close=void (__cdecl*)(void*);
    using Format=int (__cdecl*)(const void*,double,int,char16_t*,int,int*);
    using NumberingOpen=void* (__cdecl*)(const char*,int*);
    using NumberingName=const char* (__cdecl*)(const void*);
    Open open_=nullptr;Close close_=nullptr;Format numeric_=nullptr,automatic_=nullptr;
    NumberingOpen numberingOpen_=nullptr;Close numberingClose_=nullptr;NumberingName numberingName_=nullptr;
    WindowsRelativeTime(){
        const auto library=LoadLibraryExW(L"icu.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(library){
            open_=reinterpret_cast<Open>(GetProcAddress(library,"ureldatefmt_open"));
            close_=reinterpret_cast<Close>(GetProcAddress(library,"ureldatefmt_close"));
            numeric_=reinterpret_cast<Format>(GetProcAddress(library,"ureldatefmt_formatNumeric"));
            automatic_=reinterpret_cast<Format>(GetProcAddress(library,"ureldatefmt_format"));
            numberingOpen_=reinterpret_cast<NumberingOpen>(GetProcAddress(library,"unumsys_open"));
            numberingClose_=reinterpret_cast<Close>(GetProcAddress(library,"unumsys_close"));
            numberingName_=reinterpret_cast<NumberingName>(GetProcAddress(library,"unumsys_getName"));
        }
    }
public:
    static const WindowsRelativeTime& Instance(){static const WindowsRelativeTime instance;return instance;}
    bool Available()const{return open_&&close_&&numeric_&&automatic_&&numberingOpen_&&numberingClose_&&numberingName_;}
    std::shared_ptr<void> Create(const std::string& locale,int style,std::string& numberingSystem)const{
        if(!Available())return {};int status=0;
        // UDISPCTX_CAPITALIZATION_NONE is the stable ICU C ABI value 0x100.
        auto formatter=std::shared_ptr<void>(open_(locale.c_str(),nullptr,style,0x100,&status),
            [close=close_](void* value){if(value)close(value);});
        if(status>0||!formatter)return {};
        status=0;auto numbering=std::shared_ptr<void>(numberingOpen_(locale.c_str(),&status),
            [close=numberingClose_](void* value){if(value)close(value);});
        if(status>0||!numbering)return {};
        const auto name=numberingName_(numbering.get());if(!name)return {};
        numberingSystem=name;return formatter;
    }
    bool FormatValue(const std::shared_ptr<void>& formatter,double value,int unit,bool numeric,std::wstring& text)const{
        if(!formatter)return false;const auto format=numeric?numeric_:automatic_;
        int status=0;char16_t buffer[256]{};
        auto length=format(formatter.get(),value,unit,buffer,256,&status);
        if(status<=0&&length>=0&&length<256){text.assign(reinterpret_cast<const wchar_t*>(buffer),length);return true;}
        if(status!=15||length<0||length>1048576)return false; // U_BUFFER_OVERFLOW_ERROR
        std::vector<char16_t> larger(static_cast<size_t>(length)+1);status=0;
        length=format(formatter.get(),value,unit,larger.data(),static_cast<int>(larger.size()),&status);
        if(status>0||length<0||static_cast<size_t>(length)>=larger.size())return false;
        text.assign(reinterpret_cast<const wchar_t*>(larger.data()),length);return true;
    }
};

} // namespace TWebFrame::Internal
