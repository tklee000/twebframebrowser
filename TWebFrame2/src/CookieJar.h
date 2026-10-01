#pragma once

#include <TWebFrame/BrowserContext.h>
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <limits>
#include <mutex>
#include <unordered_set>

#pragma comment(lib,"normaliz.lib")

namespace TWebFrame::Internal {
inline std::wstring HttpLower(std::wstring text){
    std::transform(text.begin(),text.end(),text.begin(),towlower);return text;
}
inline std::wstring HttpTrim(const std::wstring& text){
    const auto start=text.find_first_not_of(L" \t"),end=text.find_last_not_of(L" \t");
    return start==std::wstring::npos?L"":text.substr(start,end-start+1);
}
struct HttpUrl {
    std::wstring host,path,origin,scheme;
    INTERNET_PORT port=0;
    bool valid=false,secure=false;
    explicit HttpUrl(const std::wstring& url){
        URL_COMPONENTS parts{sizeof(parts)};
        parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=static_cast<DWORD>(-1);
        if(!WinHttpCrackUrl(url.c_str(),0,0,&parts)||
           (parts.nScheme!=INTERNET_SCHEME_HTTP&&parts.nScheme!=INTERNET_SCHEME_HTTPS))return;
        host=HttpLower(std::wstring(parts.lpszHostName,parts.dwHostNameLength));
        if(host.size()>2&&host.front()==L'['&&host.back()==L']')host=host.substr(1,host.size()-2);
        if(host.empty())return;
        if(host.find(L':')==std::wstring::npos){
            const int count=IdnToAscii(0,host.c_str(),static_cast<int>(host.size()),nullptr,0);
            if(!count)return;
            std::wstring ascii(count,L'\0');IdnToAscii(0,host.c_str(),static_cast<int>(host.size()),ascii.data(),count);host=HttpLower(ascii);
        }
        secure=parts.nScheme==INTERNET_SCHEME_HTTPS;scheme=secure?L"https":L"http";port=parts.nPort;
        origin=scheme+L"://"+(host.find(L':')!=std::wstring::npos?L"["+host+L"]":host);
        if(port!=(secure?443:80))origin+=L":"+std::to_wstring(port);
        path=parts.dwUrlPathLength?std::wstring(parts.lpszUrlPath,parts.dwUrlPathLength):L"/";
        valid=true;
    }
};
inline bool DomainMatches(const std::wstring& host,const std::wstring& domain){
    return host==domain||(host.size()>domain.size()&&host[host.size()-domain.size()-1]==L'.'&&
        host.compare(host.size()-domain.size(),domain.size(),domain)==0&&
        host.find_first_not_of(L"0123456789.")!=std::wstring::npos&&host.find(L':')==std::wstring::npos);
}
inline std::wstring PublicSuffix(const std::wstring& host){
    static const std::unordered_set<std::wstring> rules={
#include "PublicSuffixRules.inc"
    };
    auto suffix=host.substr(host.rfind(L'.')==std::wstring::npos?0:host.rfind(L'.')+1);
    size_t start=0;
    for(;;){
        const auto candidate=host.substr(start);
        if(rules.count(L"!"+candidate))return candidate.substr(candidate.find(L'.')+1);
        if(rules.count(candidate)&&candidate.size()>suffix.size())suffix=candidate;
        const auto dot=host.find(L'.',start);
        if(dot==std::wstring::npos)break;
        if(rules.count(L"*."+host.substr(dot+1))&&candidate.size()>suffix.size())suffix=candidate;
        start=dot+1;
    }
    return suffix;
}
inline std::wstring SchemefulSite(const std::wstring& url){
    const HttpUrl parsed(url);if(!parsed.valid)return L"";
    auto domain=parsed.host;
    if(domain.find(L':')==std::wstring::npos&&domain.find_first_not_of(L"0123456789.")!=std::wstring::npos){
        const auto suffix=PublicSuffix(domain);
        if(domain.size()>suffix.size()){
            const auto end=domain.size()-suffix.size()-1;
            const auto dot=end?domain.rfind(L'.',end-1):std::wstring::npos;
            domain=domain.substr(dot==std::wstring::npos?0:dot+1);
        }
    }
    return parsed.scheme+L"://"+(domain.find(L':')!=std::wstring::npos?L"["+domain+L"]":domain);
}
class CookieJar {
    struct Cookie {
        enum class SameSite { Lax, Strict, None };
        std::wstring name,value,domain,path;
        bool hostOnly=true,secure=false,httpOnly=false;
        SameSite sameSite=SameSite::Lax;
        std::int64_t expires=std::numeric_limits<std::int64_t>::max();
        std::uint64_t created=0;
    };
    mutable std::mutex mutex_;
    std::vector<Cookie> cookies_;
    std::uint64_t serial_=0;
    std::atomic<std::uint64_t> revision_{0};
    static std::int64_t Now(){return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
    static bool PathMatches(const std::wstring& path,const std::wstring& prefix){
        return path==prefix||(path.size()>prefix.size()&&path.compare(0,prefix.size(),prefix)==0&&
            (prefix.back()==L'/'||path[prefix.size()]==L'/'));
    }
    static bool Invalid(const std::wstring& value){
        return std::any_of(value.begin(),value.end(),[](wchar_t c){return c<32||c==127||c>255;});
    }
    static bool CookieDate(const std::wstring& value,std::int64_t& result){
        const auto delimiter=[](wchar_t c){return c==9||(c>=32&&c<=47)||(c>=59&&c<=64)||(c>=91&&c<=96)||(c>=123&&c<=126);};
        SYSTEMTIME time{};bool foundTime=false,foundDay=false,foundMonth=false,foundYear=false;
        for(size_t start=0;start<value.size();){
            while(start<value.size()&&delimiter(value[start]))++start;if(start==value.size())break;
            size_t end=start;while(end<value.size()&&!delimiter(value[end]))++end;
            const auto token=HttpLower(value.substr(start,end-start));start=end;
            const auto number=[&](size_t& cursor,unsigned& output,unsigned maximum){
                const auto begin=cursor;output=0;
                while(cursor<token.size()&&token[cursor]>=L'0'&&token[cursor]<=L'9'){output=output*10+token[cursor++]-L'0';if(cursor-begin>maximum)return false;}
                return cursor>begin;
            };
            size_t cursor=0;unsigned hour=0,minute=0,second=0;
            if(!foundTime&&number(cursor,hour,2)&&cursor<token.size()&&token[cursor++]==L':'&&
               number(cursor,minute,2)&&cursor<token.size()&&token[cursor++]==L':'&&number(cursor,second,2)){
                time.wHour=static_cast<WORD>(hour);time.wMinute=static_cast<WORD>(minute);time.wSecond=static_cast<WORD>(second);foundTime=true;continue;
            }
            cursor=0;unsigned digits=0;
            if(!foundDay&&number(cursor,digits,2)){time.wDay=static_cast<WORD>(digits);foundDay=true;continue;}
            if(!foundMonth&&token.size()>=3){
                constexpr const wchar_t* months[]={L"jan",L"feb",L"mar",L"apr",L"may",L"jun",L"jul",L"aug",L"sep",L"oct",L"nov",L"dec"};
                for(unsigned month=0;month<12;++month)if(token.compare(0,3,months[month])==0){time.wMonth=static_cast<WORD>(month+1);foundMonth=true;break;}
                if(foundMonth)continue;
            }
            cursor=0;if(!foundYear&&number(cursor,digits,4)&&cursor>=2){time.wYear=static_cast<WORD>(digits);foundYear=true;}
        }
        if(time.wYear>=70&&time.wYear<=99)time.wYear+=1900;else if(time.wYear<=69)time.wYear+=2000;
        if(!foundTime||!foundDay||!foundMonth||!foundYear||time.wYear<1601||time.wDay<1||time.wDay>31||
           time.wHour>23||time.wMinute>59||time.wSecond>59)return false;
        FILETIME file{};if(!SystemTimeToFileTime(&time,&file))return false;
        ULARGE_INTEGER ticks{};ticks.LowPart=file.dwLowDateTime;ticks.HighPart=file.dwHighDateTime;
        result=static_cast<std::int64_t>(ticks.QuadPart/10000000ull)-11644473600ll;return true;
    }
    void Expire(std::int64_t now){
        const auto count=cookies_.size();cookies_.erase(std::remove_if(cookies_.begin(),cookies_.end(),
            [now](const Cookie& cookie){return cookie.expires<=now;}),cookies_.end());
        if(count!=cookies_.size())++revision_;
    }
public:
    std::uint64_t Revision(){std::lock_guard<std::mutex> lock(mutex_);Expire(Now());return revision_.load();}
    bool Set(const std::wstring& url,const std::wstring& field,bool fromScript=false,const std::wstring& site=L"",bool navigation=false){
        const HttpUrl parsed(url);if(!parsed.valid||field.size()>4096||Invalid(field))return false;
        const auto end=field.find(L';'),equal=field.find(L'=');
        if(equal==std::wstring::npos||equal>=end)return false;
        Cookie cookie;cookie.name=HttpTrim(field.substr(0,equal));cookie.value=HttpTrim(field.substr(equal+1,end-equal-1));
        if(cookie.name.empty()||cookie.name.find_first_of(LR"(()<>@,;:\"/[]?={} )")!=std::wstring::npos)return false;
        cookie.domain=parsed.host;
        const auto slash=parsed.path.rfind(L'/');cookie.path=slash==0||slash==std::wstring::npos?L"/":parsed.path.substr(0,slash);
        bool domainAttribute=false,pathAttribute=false,maxAge=false;
        const auto now=Now();
        for(auto start=end;start!=std::wstring::npos;){
            ++start;const auto next=field.find(L';',start);const auto item=HttpTrim(field.substr(start,next-start));
            const auto delimiter=item.find(L'=');const auto name=HttpLower(HttpTrim(item.substr(0,delimiter)));
            auto value=delimiter==std::wstring::npos?L"":HttpTrim(item.substr(delimiter+1));
            if(name==L"domain"&&!value.empty()){
                value=HttpLower(value);if(value.front()==L'.')value.erase(0,1);
                const HttpUrl domainUrl(parsed.scheme+L"://"+value+L"/");
                if(!domainUrl.valid||domainUrl.host!=value||!DomainMatches(parsed.host,value))return false;
                if(PublicSuffix(value)==value){if(value!=parsed.host)return false;}else{cookie.domain=value;cookie.hostOnly=false;}
                domainAttribute=true;
            }else if(name==L"path"&&!value.empty()&&value.front()==L'/'){cookie.path=value;pathAttribute=true;}
            else if(name==L"secure")cookie.secure=true;
            else if(name==L"httponly")cookie.httpOnly=true;
            else if(name==L"samesite"){
                value=HttpLower(value);if(value==L"strict")cookie.sameSite=Cookie::SameSite::Strict;
                else if(value==L"none")cookie.sameSite=Cookie::SameSite::None;
                else cookie.sameSite=Cookie::SameSite::Lax;
            }else if(name==L"max-age"&&!value.empty()){
                size_t digit=value.front()==L'-'?1:0;
                if(digit<value.size()&&value.find_first_not_of(L"0123456789",digit)==std::wstring::npos){
                    maxAge=true;
                    if(value.front()==L'-'||value.find_first_not_of(L'0')==std::wstring::npos)cookie.expires=0;
                    else{const auto seconds=_wcstoi64(value.c_str(),nullptr,10);cookie.expires=now+std::min<std::int64_t>(std::max<std::int64_t>(0,seconds),400ll*24*60*60);}
                }
            }else if(name==L"expires"&&!maxAge){
                std::int64_t expires=0;
                if(CookieDate(value,expires))cookie.expires=std::min(expires,now+400ll*24*60*60);
            }
            start=next;
        }
        if((fromScript&&cookie.httpOnly)||(cookie.secure&&!parsed.secure)||
           (cookie.sameSite==Cookie::SameSite::None&&!cookie.secure))return false;
        const auto prefix=HttpLower(cookie.name);
        if(prefix.rfind(L"__secure-",0)==0&&(!cookie.secure||!parsed.secure))return false;
        if(prefix.rfind(L"__host-",0)==0&&(!cookie.secure||!parsed.secure||domainAttribute||!pathAttribute||cookie.path!=L"/"))return false;
        if(!navigation&&!site.empty()&&SchemefulSite(site)!=SchemefulSite(url)&&cookie.sameSite!=Cookie::SameSite::None)return false;
        std::lock_guard<std::mutex> lock(mutex_);Expire(now);
        for(const auto& existing:cookies_){
            if(existing.name!=cookie.name)continue;
            if(fromScript&&existing.httpOnly&&existing.domain==cookie.domain&&existing.path==cookie.path)return false;
            if(!parsed.secure&&existing.secure&&(DomainMatches(cookie.domain,existing.domain)||DomainMatches(existing.domain,cookie.domain))&&PathMatches(cookie.path,existing.path))return false;
        }
        auto found=std::find_if(cookies_.begin(),cookies_.end(),[&](const Cookie& old){return old.name==cookie.name&&old.domain==cookie.domain&&old.path==cookie.path;});
        if(found!=cookies_.end()){cookie.created=found->created;cookies_.erase(found);}else cookie.created=++serial_;
        if(cookie.expires>now)cookies_.push_back(std::move(cookie));
        while(cookies_.size()>3000)cookies_.erase(cookies_.begin());
        ++revision_;return true;
    }
    std::wstring Header(const std::wstring& url,const std::wstring& site=L"",bool navigation=false,const std::wstring& method=L"GET",bool script=false){
        const HttpUrl parsed(url);if(!parsed.valid)return L"";
        const bool sameSite=site.empty()||SchemefulSite(site)==SchemefulSite(url);
        const bool safe=method==L"GET"||method==L"HEAD"||method==L"OPTIONS"||method==L"TRACE";
        std::lock_guard<std::mutex> lock(mutex_);Expire(Now());std::vector<const Cookie*> matches;
        for(const auto& cookie:cookies_){
            if((cookie.hostOnly?parsed.host!=cookie.domain:!DomainMatches(parsed.host,cookie.domain))||
               !PathMatches(parsed.path,cookie.path)||(cookie.secure&&!parsed.secure)||(script&&cookie.httpOnly))continue;
            if(!sameSite&&cookie.sameSite!=Cookie::SameSite::None&&
               (script||cookie.sameSite==Cookie::SameSite::Strict||!navigation||!safe))continue;
            matches.push_back(&cookie);
        }
        std::stable_sort(matches.begin(),matches.end(),[](const Cookie* a,const Cookie* b){return a->path.size()!=b->path.size()?a->path.size()>b->path.size():a->created<b->created;});
        std::wstring result;for(const auto* cookie:matches){if(!result.empty())result+=L"; ";result+=cookie->name+L"="+cookie->value;}return result;
    }
};
}
