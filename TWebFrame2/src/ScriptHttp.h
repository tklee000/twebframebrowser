#pragma once

#include "ScriptRequest.h"
#include "CookieJar.h"
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <cwctype>
#include <cwchar>
#include <memory>
#include <mutex>
#include <climits>
#include <chrono>
#include <shlwapi.h>

#pragma comment(lib,"winhttp.lib")
#pragma comment(lib,"shlwapi.lib")

namespace TWebFrame::Internal {
// A browsing context and its frames share this session. The host's text-only
// resource callback remains available for local and archive-backed resources.
class ScriptHttpSession {
    struct Handle {
        HINTERNET value=nullptr;
        explicit Handle(HINTERNET handle):value(handle){}
        ~Handle(){if(value)WinHttpCloseHandle(value);}
        Handle(const Handle&)=delete;
        Handle& operator=(const Handle&)=delete;
    };
    Handle session{nullptr};
    std::once_flag initialization;
    std::shared_ptr<CookieJar> cookies_;
    static std::wstring Lower(std::wstring value){std::transform(value.begin(),value.end(),value.begin(),towlower);return value;}
    static std::wstring Origin(const std::wstring& url){
        return BrowserContext::Origin(url);
    }
    static std::wstring Header(HINTERNET request,DWORD query,const wchar_t* name=WINHTTP_HEADER_NAME_BY_INDEX){
        DWORD bytes=0;WinHttpQueryHeaders(request,query,name,nullptr,&bytes,WINHTTP_NO_HEADER_INDEX);
        if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER||bytes<sizeof(wchar_t))return L"";
        std::wstring value(bytes/sizeof(wchar_t),L'\0');
        if(!WinHttpQueryHeaders(request,query,name,value.data(),&bytes,WINHTTP_NO_HEADER_INDEX))return L"";
        value.resize(bytes/sizeof(wchar_t));while(!value.empty()&&value.back()==L'\0')value.pop_back();return value;
    }
    static bool Contains(const std::wstring& list,const std::wstring& value){
        size_t start=0;while(start<list.size()){
            const auto end=list.find(L',',start);auto token=Lower(list.substr(start,end-start));
            const auto first=token.find_first_not_of(L" \t"),last=token.find_last_not_of(L" \t");
            if(first!=std::wstring::npos&&token.substr(first,last-first+1)==Lower(value))return true;
            if(end==std::wstring::npos)break;start=end+1;
        }return false;
    }
    static std::wstring Decode(const std::vector<unsigned char>& body,const std::wstring& contentType){
        if(body.empty())return L"";
        const auto type=Lower(contentType);UINT encoding=CP_UTF8;
        if(type.find(L"euc-kr")!=std::wstring::npos||type.find(L"windows-949")!=std::wstring::npos)encoding=949;
        else if(type.find(L"iso-8859-1")!=std::wstring::npos)encoding=28591;
        const auto* bytes=reinterpret_cast<const char*>(body.data());const auto count=static_cast<int>(body.size());
        const auto length=MultiByteToWideChar(encoding,0,bytes,count,nullptr,0);
        std::wstring text(static_cast<size_t>(std::max(0,length)),L'\0');
        if(length)MultiByteToWideChar(encoding,0,bytes,count,text.data(),length);
        if(!text.empty()&&text.front()==0xfeff)text.erase(text.begin());return text;
    }
public:
    explicit ScriptHttpSession(std::shared_ptr<CookieJar> cookies=std::make_shared<CookieJar>()):cookies_(std::move(cookies)){}
    ScriptResponse Send(const ScriptRequest& data){
        ScriptResponse result;result.url=data.url;
        const auto started=std::chrono::steady_clock::now();
        const auto elapsed=[&]{return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();};
        std::call_once(initialization,[this]{session.value=WinHttpOpen(BrowserContext::UserAgent(),
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
            if(session.value){DWORD connections=6;WinHttpSetOption(session.value,WINHTTP_OPTION_MAX_CONNS_PER_SERVER,&connections,sizeof(connections));}
            if(session.value)WinHttpSetTimeouts(session.value,5000,5000,8000,8000);
        });
        if(!session.value)return result;
        URL_COMPONENTS parts{sizeof(parts)};parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=static_cast<DWORD>(-1);
        if(!WinHttpCrackUrl(data.url.c_str(),0,0,&parts)||
           (parts.nScheme!=INTERNET_SCHEME_HTTP&&parts.nScheme!=INTERNET_SCHEME_HTTPS))return result;
        const std::wstring host(parts.lpszHostName,parts.dwHostNameLength);
        std::wstring path=parts.dwUrlPathLength?std::wstring(parts.lpszUrlPath,parts.dwUrlPathLength):L"/";
        if(parts.dwExtraInfoLength)path.append(parts.lpszExtraInfo,parts.dwExtraInfoLength);
        const auto fragment=path.find(L'#');if(fragment!=std::wstring::npos)path.erase(fragment);
        const bool crossOrigin=data.origin!=Origin(data.url);
        const bool noCors=data.mode==ScriptRequest::Mode::NoCors;
        const bool navigation=data.mode==ScriptRequest::Mode::Navigation;
        const bool credentials=data.credentials!=ScriptRequest::Credentials::Omit&&
            (data.includeCredentials||data.credentials==ScriptRequest::Credentials::Include||!crossOrigin||navigation);
        if(crossOrigin&&data.mode==ScriptRequest::Mode::SameOrigin)return result;
        auto method=data.method;std::transform(method.begin(),method.end(),method.begin(),towupper);
        if(noCors&&method!=L"GET"&&method!=L"HEAD"&&method!=L"POST")return result;
        Handle connection(WinHttpConnect(session.value,host.c_str(),parts.nPort,0));if(!connection.value)return result;
        std::wstring headers=L"Accept: */*\r\nAccept-Language: ko-KR,ko;q=0.9,en;q=0.7\r\n";
        std::vector<std::wstring> unsafeHeaders;
        for(const auto& header:data.headers){
            const auto name=Lower(header.first);
            if(name.empty()||name.find_first_of(L"\r\n:")!=std::wstring::npos||header.second.find_first_of(L"\r\n")!=std::wstring::npos)return result;
            // These headers belong to the transport, never to page script.
            if(name==L"cookie"||name==L"cookie2"||name==L"host"||name==L"origin"||name==L"referer"||name==L"content-length"||
               name==L"connection"||name==L"user-agent"||name.rfind(L"sec-",0)==0||name.rfind(L"proxy-",0)==0)continue;
            const bool safe=CorsSafelistedRequestHeader(name,header.second);
            if(noCors&&!safe)continue;
            headers+=header.first+L": "+header.second+L"\r\n";
            if(!safe)unsafeHeaders.push_back(name);
        }
        if((crossOrigin&&!noCors&&!navigation)||(method!=L"GET"&&method!=L"HEAD"))headers+=L"Origin: "+data.origin+L"\r\n";
        const auto originAllowed=[&](HINTERNET request){
            const auto allowed=Header(request,WINHTTP_QUERY_CUSTOM,L"Access-Control-Allow-Origin");
            if(allowed!=data.origin&&!(allowed==L"*"&&!credentials))return false;
            return !credentials||Header(request,WINHTTP_QUERY_CUSTOM,L"Access-Control-Allow-Credentials")==L"true";
        };
        if(crossOrigin&&!noCors&&!navigation&&((method!=L"GET"&&method!=L"HEAD"&&method!=L"POST")||!unsafeHeaders.empty())){
            Handle preflight(WinHttpOpenRequest(connection.value,L"OPTIONS",path.c_str(),nullptr,WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES,parts.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0));
            if(!preflight.value)return result;DWORD disable=WINHTTP_DISABLE_COOKIES|WINHTTP_DISABLE_REDIRECTS;
            WinHttpSetOption(preflight.value,WINHTTP_OPTION_DISABLE_FEATURE,&disable,sizeof(disable));
            std::wstring preflightHeaders=L"Origin: "+data.origin+L"\r\nAccess-Control-Request-Method: "+method+L"\r\n";
            if(!unsafeHeaders.empty()){
                preflightHeaders+=L"Access-Control-Request-Headers: ";
                for(size_t index=0;index<unsafeHeaders.size();++index)preflightHeaders+=(index?L", ":L"")+unsafeHeaders[index];
                preflightHeaders+=L"\r\n";
            }
            if(!WinHttpSendRequest(preflight.value,preflightHeaders.c_str(),static_cast<DWORD>(-1),WINHTTP_NO_REQUEST_DATA,0,0,0)||
               !WinHttpReceiveResponse(preflight.value,nullptr)||!originAllowed(preflight.value))return result;
            DWORD status=0,bytes=sizeof(status);
            if(!WinHttpQueryHeaders(preflight.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,&status,&bytes,WINHTTP_NO_HEADER_INDEX)||status<200||status>=300)return result;
            if(method!=L"GET"&&method!=L"HEAD"&&method!=L"POST"&&
               !Contains(Header(preflight.value,WINHTTP_QUERY_CUSTOM,L"Access-Control-Allow-Methods"),method))return result;
            const auto allowed=Header(preflight.value,WINHTTP_QUERY_CUSTOM,L"Access-Control-Allow-Headers");
            for(const auto& name:unsafeHeaders)if(!Contains(allowed,name)&&!(allowed==L"*"&&!credentials&&name!=L"authorization"))return result;
        }
        const auto referrer=data.referrer.empty()?L"":Origin(data.referrer)==Origin(data.url)?data.referrer:
            (Origin(data.referrer).rfind(L"https://",0)==0&&parts.nScheme==INTERNET_SCHEME_HTTP?L"":Origin(data.referrer)+L"/");
        Handle request(WinHttpOpenRequest(connection.value,method.c_str(),path.c_str(),nullptr,
            referrer.empty()?WINHTTP_NO_REFERER:referrer.c_str(),WINHTTP_DEFAULT_ACCEPT_TYPES,
            parts.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0));
        if(!request.value)return result;
        // WinHTTP's private cookie handling must never compete with the profile jar.
        DWORD disable=WINHTTP_DISABLE_COOKIES|WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(request.value,WINHTTP_OPTION_DISABLE_FEATURE,&disable,sizeof(disable));
        if(credentials){const auto cookie=cookies_->Header(data.url,data.siteForCookies,data.topLevelNavigation,method);
            if(!cookie.empty())headers+=L"Cookie: "+cookie+L"\r\n";}
        const int timeout=data.timeout?static_cast<int>(std::min(data.timeout,static_cast<unsigned>(INT_MAX))):8000;
        WinHttpSetTimeouts(request.value,timeout,timeout,timeout,timeout);
        DWORD decompress=WINHTTP_DECOMPRESSION_FLAG_GZIP|WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
        WinHttpSetOption(request.value,WINHTTP_OPTION_DECOMPRESSION,&decompress,sizeof(decompress));
        const auto size=method==L"GET"||method==L"HEAD"?0:static_cast<DWORD>(data.body.size());
        result.requestStartOffsetMs=elapsed();
        if(!WinHttpSendRequest(request.value,headers.c_str(),static_cast<DWORD>(-1),
            size?const_cast<unsigned char*>(data.body.data()):WINHTTP_NO_REQUEST_DATA,size,size,0)||
            !WinHttpReceiveResponse(request.value,nullptr))return result;
        result.responseStartOffsetMs=elapsed();
        // Cookie processing precedes CORS filtering, including failed CORS responses.
        if(credentials){DWORD index=0;for(;;){DWORD length=0;DWORD probe=index;
            WinHttpQueryHeaders(request.value,WINHTTP_QUERY_SET_COOKIE,WINHTTP_HEADER_NAME_BY_INDEX,nullptr,&length,&probe);
            if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER)break;
            std::wstring field(length/sizeof(wchar_t),L'\0');
            if(!WinHttpQueryHeaders(request.value,WINHTTP_QUERY_SET_COOKIE,WINHTTP_HEADER_NAME_BY_INDEX,field.data(),&length,&index))break;
            field.resize(wcslen(field.c_str()));cookies_->Set(data.url,field,false,data.siteForCookies,data.topLevelNavigation);
        }}
        if(crossOrigin&&!noCors&&!navigation&&!originAllowed(request.value))return result;
        // WinHTTP's wire URL omits fragments. Navigation documents retain
        // their fragment for bootstrap data and history; fetch URLs exclude it.
        DWORD urlBytes=0;WinHttpQueryOption(request.value,WINHTTP_OPTION_URL,nullptr,&urlBytes);
        if(!navigation&&urlBytes>sizeof(wchar_t)){
            std::wstring finalUrl(urlBytes/sizeof(wchar_t),L'\0');
            if(WinHttpQueryOption(request.value,WINHTTP_OPTION_URL,finalUrl.data(),&urlBytes))result.url.assign(finalUrl.c_str());
        }
        const bool redirectedCrossOrigin=Origin(result.url)!=data.origin;
        if(redirectedCrossOrigin&&data.mode==ScriptRequest::Mode::SameOrigin)return ScriptResponse{};
        if(!crossOrigin&&redirectedCrossOrigin&&!noCors&&!navigation&&!originAllowed(request.value))return ScriptResponse{};
        DWORD status=0,statusBytes=sizeof(status);
        if(!WinHttpQueryHeaders(request.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,&status,&statusBytes,WINHTTP_NO_HEADER_INDEX))return result;
        result.status=status;result.statusText=Header(request.value,WINHTTP_QUERY_STATUS_TEXT);
        if(status==301||status==302||status==303||status==307||status==308){
            const auto destination=Header(request.value,WINHTTP_QUERY_LOCATION);
            if(!destination.empty()){
                // Resolve relative redirects without allowing credentials or cookies
                // from one request to be blindly forwarded to a different host.
                static thread_local unsigned redirects=0;
                if(redirects>=20)return ScriptResponse{};
                auto next=data;DWORD capacity=32768;std::wstring combined(capacity,L'\0');
                if(FAILED(UrlCombineW(data.url.c_str(),destination.c_str(),combined.data(),&capacity,0)))return ScriptResponse{};
                combined.resize(capacity);next.url=combined;
                if(destination.find(L'#')==std::wstring::npos){
                    const auto hash=data.url.find(L'#');
                    if(hash!=std::wstring::npos){const auto nextHash=next.url.find(L'#');if(nextHash!=std::wstring::npos)next.url.resize(nextHash);next.url+=data.url.substr(hash);}
                }
                if((status==303&&method!=L"HEAD")||((status==301||status==302)&&method==L"POST")){
                    next.method=L"GET";next.body.clear();next.headers.erase(std::remove_if(next.headers.begin(),next.headers.end(),[](const auto& header){
                        const auto name=Lower(header.first);return name==L"content-type"||name==L"content-encoding"||name==L"content-language"||name==L"content-location";
                    }),next.headers.end());
                }
                if(Origin(next.url)!=Origin(data.url))next.headers.erase(std::remove_if(next.headers.begin(),next.headers.end(),[](const auto& header){return Lower(header.first)==L"authorization";}),next.headers.end());
                struct RedirectScope {unsigned& depth;explicit RedirectScope(unsigned& value):depth(value){++depth;}~RedirectScope(){--depth;}} scope(redirects);
                return Send(next);
            }
        }
        result.opaque=noCors&&(crossOrigin||redirectedCrossOrigin);
        if(result.opaque){
            if(Lower(Header(request.value,WINHTTP_QUERY_CUSTOM,L"Cross-Origin-Resource-Policy"))==L"same-origin")return ScriptResponse{};
            // A successful no-cors transport resolves fetch, but no status,
            // headers, URL or body may be exposed to the page's JavaScript.
            return result;
        }
        result.contentType=Header(request.value,WINHTTP_QUERY_CONTENT_TYPE);
        const auto timingOrigin=Header(request.value,WINHTTP_QUERY_CUSTOM,L"Timing-Allow-Origin");
        result.timingAllowed=!crossOrigin||Contains(timingOrigin,data.origin)||Contains(timingOrigin,L"*");
        const auto encodedLength=Header(request.value,WINHTTP_QUERY_CONTENT_LENGTH);
        if(!encodedLength.empty()){
            wchar_t* end=nullptr;const auto length=std::wcstoull(encodedLength.c_str(),&end,10);
            if(end!=encodedLength.c_str()&&*end==0&&length<=24*1024*1024)result.encodedBodySize=static_cast<size_t>(length);
        }
        const auto raw=Header(request.value,WINHTTP_QUERY_RAW_HEADERS_CRLF);size_t start=raw.find(L"\r\n");
        while(start!=std::wstring::npos&&start+2<raw.size()){
            start+=2;const auto end=raw.find(L"\r\n",start);const auto line=raw.substr(start,end-start);
            const auto colon=line.find(L':');if(colon==std::wstring::npos)break;
            const auto name=Lower(line.substr(0,colon));
            const bool safe=name==L"cache-control"||name==L"content-language"||name==L"content-length"||name==L"content-type"||
                name==L"expires"||name==L"last-modified"||name==L"pragma";
            const auto exposed=Header(request.value,WINHTTP_QUERY_CUSTOM,L"Access-Control-Expose-Headers");
            if(name!=L"set-cookie"&&name!=L"set-cookie2"&&(!crossOrigin||navigation||safe||Contains(exposed,name)||
               (exposed==L"*"&&!credentials)))result.headers+=line+L"\r\n";
            start=end;
        }
        std::vector<unsigned char> bytes;unsigned char block[32768];
        for(;;){DWORD received=0;if(!WinHttpReadData(request.value,block,sizeof(block),&received))return ScriptResponse{};
            if(!received)break;if(bytes.size()+received>24*1024*1024)return ScriptResponse{};bytes.insert(bytes.end(),block,block+received);}
        result.decodedBodySize=bytes.size();result.body=Decode(bytes,result.contentType);result.bytes=std::move(bytes);return result;
    }
};
}
