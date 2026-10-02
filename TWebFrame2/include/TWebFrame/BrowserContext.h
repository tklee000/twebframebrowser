#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace TWebFrame {
namespace Internal { struct OriginFileSystem; }
struct NetworkRequest {
    enum class Mode { Cors, NoCors, SameOrigin, Navigation };
    enum class Credentials { Omit, SameOrigin, Include };
    std::wstring url, method=L"GET", origin, referrer, siteForCookies;
    std::vector<std::pair<std::wstring,std::wstring>> headers;
    std::vector<unsigned char> body;
    unsigned timeout=0;
    bool includeCredentials=false, topLevelNavigation=false;
    Credentials credentials=Credentials::SameOrigin;
    Mode mode=Mode::Cors;
};
struct NetworkResponse {
    unsigned status=0;
    std::wstring statusText,url,headers,contentType,body,error;
    std::vector<unsigned char> bytes;
    double requestStartOffsetMs=-1,responseStartOffsetMs=-1;
    size_t decodedBodySize=0,encodedBodySize=0;
    bool timingAllowed=false,opaque=false;
};
struct StorageChange {
    std::wstring key,oldValue,newValue,url;
    bool clear=false,hadOld=false,hasNew=false;
    std::uint64_t source=0;
};
class StorageArea {
public:
    ~StorageArea();
    size_t Length() const;
    bool Key(size_t index,std::wstring& key) const;
    bool Get(const std::wstring& key,std::wstring& value) const;
    // False means the write exceeds the 5 MiB UTF-16 quota.
    bool Set(const std::wstring& key,const std::wstring& value,const std::wstring& url,std::uint64_t source);
    void Remove(const std::wstring& key,const std::wstring& url,std::uint64_t source);
    void Clear(const std::wstring& url,std::uint64_t source);
    std::uint64_t Subscribe(std::function<void(const StorageChange&)> listener);
    void Unsubscribe(std::uint64_t listener);
private:
    friend class BrowserContext;
    struct Impl;
    explicit StorageArea(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};
// A profile owns one HTTP session, cookie jar and origin storage partition.
// Views in a tab share its session-storage namespace with descendant frames.
class BrowserContext {
public:
    explicit BrowserContext(const std::wstring& profileDirectory=L"");
    ~BrowserContext();
    BrowserContext(const BrowserContext&)=delete;
    BrowserContext& operator=(const BrowserContext&)=delete;
    NetworkResponse Request(const NetworkRequest& request);
    // Optional diagnostic observer. Called on the requesting thread after the
    // response, outside profile locks; it must not throw or change the response.
    using NetworkObserver=std::function<void(const NetworkRequest&,const NetworkResponse&)>;
    void SetNetworkObserver(NetworkObserver observer);
    std::wstring DocumentCookie(const std::wstring& url,const std::wstring& siteForCookies=L"");
    void SetDocumentCookie(const std::wstring& url,const std::wstring& cookie,const std::wstring& siteForCookies=L"");
    bool CookiesEnabled() const;
    std::uint64_t CookieRevision() const;
    std::uint64_t NewBrowsingContext();
    void ReleaseBrowsingContext(std::uint64_t session);
    std::shared_ptr<StorageArea> Storage(const std::wstring& origin,std::uint64_t session=0);
    std::shared_ptr<Internal::OriginFileSystem> OriginFiles(const std::wstring& origin);
    static std::wstring Origin(const std::wstring& url);
    static std::wstring Site(const std::wstring& url);
    static const wchar_t* UserAgent();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
