#include <TWebFrame/BrowserContext.h>
#include "ScriptHttp.h"
#include "OriginFileSystem.h"
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib,"bcrypt.lib")

namespace TWebFrame {
namespace {
constexpr size_t storageQuota=5*1024*1024;
void WriteString(std::ostream& stream,const std::wstring& text){
    const auto size=static_cast<std::uint32_t>(text.size());stream.write(reinterpret_cast<const char*>(&size),sizeof(size));
    stream.write(reinterpret_cast<const char*>(text.data()),size*sizeof(wchar_t));
}
bool ReadString(std::istream& stream,std::wstring& text){
    std::uint32_t size=0;stream.read(reinterpret_cast<char*>(&size),sizeof(size));
    if(!stream||size>storageQuota/sizeof(wchar_t))return false;
    text.resize(size);stream.read(reinterpret_cast<char*>(text.data()),size*sizeof(wchar_t));return !!stream;
}
std::wstring StorageFileName(const std::wstring& origin){
    unsigned char hash[32]{};
    {
        BCRYPT_ALG_HANDLE algorithm=nullptr;
        if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return L"";
        const auto status=BCryptHash(algorithm,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(origin.data())),
            static_cast<ULONG>(origin.size()*sizeof(wchar_t)),hash,sizeof(hash));
        BCryptCloseAlgorithmProvider(algorithm,0);if(status<0)return L"";
    }
    constexpr wchar_t digits[]=L"0123456789abcdef";std::wstring result;
    for(const auto byte:hash){result+=digits[byte>>4];result+=digits[byte&15];}return result+L".storage";
}
}
struct StorageArea::Impl {
    mutable std::mutex mutex;
    std::vector<std::pair<std::wstring,std::wstring>> values;
    std::map<std::uint64_t,std::function<void(const StorageChange&)>> listeners;
    std::uint64_t nextListener=0;
    std::wstring origin;
    std::filesystem::path file;
    void Save(){
        if(file.empty())return;
        std::error_code error;std::filesystem::create_directories(file.parent_path(),error);if(error)return;
        auto temp=file;temp+=L".tmp";
        std::ofstream stream(temp,std::ios::binary|std::ios::trunc);if(!stream)return;
        stream.write("TWFST001",8);WriteString(stream,origin);
        const auto count=static_cast<std::uint32_t>(values.size());stream.write(reinterpret_cast<const char*>(&count),sizeof(count));
        for(const auto& value:values){WriteString(stream,value.first);WriteString(stream,value.second);}
        stream.close();if(stream)MoveFileExW(temp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    }
    void Load(){
        if(file.empty())return;std::ifstream stream(file,std::ios::binary);if(!stream)return;
        char magic[8]{};stream.read(magic,8);if(std::string(magic,8)!="TWFST001")return;
        std::wstring storedOrigin;if(!ReadString(stream,storedOrigin)||storedOrigin!=origin)return;
        std::uint32_t count=0;stream.read(reinterpret_cast<char*>(&count),sizeof(count));if(!stream||count>storageQuota/4)return;
        size_t bytes=0;std::vector<std::pair<std::wstring,std::wstring>> loaded;std::unordered_set<std::wstring> keys;
        for(std::uint32_t index=0;index<count;++index){
            std::wstring key,value;if(!ReadString(stream,key)||!ReadString(stream,value))return;
            bytes+=(key.size()+value.size())*sizeof(wchar_t);if(bytes>storageQuota)return;
            if(!keys.insert(key).second)return;
            loaded.emplace_back(std::move(key),std::move(value));
        }
        values=std::move(loaded);
    }
    std::vector<std::function<void(const StorageChange&)>> Listeners(){
        std::vector<std::function<void(const StorageChange&)>> result;for(const auto& listener:listeners)result.push_back(listener.second);return result;
    }
};
StorageArea::StorageArea(std::shared_ptr<Impl> impl):impl_(std::move(impl)){}
StorageArea::~StorageArea()=default;
size_t StorageArea::Length() const {std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->values.size();}
bool StorageArea::Key(size_t index,std::wstring& key) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);if(index>=impl_->values.size())return false;key=impl_->values[index].first;return true;
}
bool StorageArea::Get(const std::wstring& key,std::wstring& value) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);for(const auto& item:impl_->values)if(item.first==key){value=item.second;return true;}return false;
}
bool StorageArea::Set(const std::wstring& key,const std::wstring& value,const std::wstring& url,std::uint64_t source){
    StorageChange change;change.key=key;change.newValue=value;change.hasNew=true;change.url=url;change.source=source;
    std::vector<std::function<void(const StorageChange&)>> listeners;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);auto found=std::find_if(impl_->values.begin(),impl_->values.end(),[&](const auto& item){return item.first==key;});
        if(found!=impl_->values.end()){if(found->second==value)return true;change.oldValue=found->second;change.hadOld=true;}
        size_t bytes=(key.size()+value.size())*sizeof(wchar_t);
        for(const auto& item:impl_->values)if(item.first!=key)bytes+=(item.first.size()+item.second.size())*sizeof(wchar_t);
        if(bytes>storageQuota)return false;
        if(found==impl_->values.end())impl_->values.emplace_back(key,value);else found->second=value;
        impl_->Save();listeners=impl_->Listeners();
    }
    for(const auto& listener:listeners)listener(change);return true;
}
void StorageArea::Remove(const std::wstring& key,const std::wstring& url,std::uint64_t source){
    StorageChange change;change.key=key;change.url=url;change.source=source;change.hadOld=true;
    std::vector<std::function<void(const StorageChange&)>> listeners;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);auto found=std::find_if(impl_->values.begin(),impl_->values.end(),[&](const auto& item){return item.first==key;});
        if(found==impl_->values.end())return;change.oldValue=found->second;impl_->values.erase(found);impl_->Save();listeners=impl_->Listeners();
    }
    for(const auto& listener:listeners)listener(change);
}
void StorageArea::Clear(const std::wstring& url,std::uint64_t source){
    StorageChange change;change.clear=true;change.url=url;change.source=source;std::vector<std::function<void(const StorageChange&)>> listeners;
    {std::lock_guard<std::mutex> lock(impl_->mutex);if(impl_->values.empty())return;impl_->values.clear();impl_->Save();listeners=impl_->Listeners();}
    for(const auto& listener:listeners)listener(change);
}
std::uint64_t StorageArea::Subscribe(std::function<void(const StorageChange&)> listener){
    std::lock_guard<std::mutex> lock(impl_->mutex);const auto id=++impl_->nextListener;impl_->listeners.emplace(id,std::move(listener));return id;
}
void StorageArea::Unsubscribe(std::uint64_t listener){std::lock_guard<std::mutex> lock(impl_->mutex);impl_->listeners.erase(listener);}
struct BrowserContext::Impl {
    std::shared_ptr<Internal::CookieJar> cookies=std::make_shared<Internal::CookieJar>();
    Internal::ScriptHttpSession http{cookies};
    std::mutex mutex;
    std::unordered_map<std::wstring,std::shared_ptr<StorageArea>> storage;
    std::unordered_map<std::wstring,std::shared_ptr<Internal::OriginFileSystem>> originFiles;
    std::atomic<std::uint64_t> nextContext{0};
    std::wstring profileDirectory;
    BrowserContext::NetworkObserver networkObserver;
};
BrowserContext::BrowserContext(const std::wstring& profileDirectory):impl_(std::make_unique<Impl>()){impl_->profileDirectory=profileDirectory;}
BrowserContext::~BrowserContext()=default;
std::shared_ptr<Internal::OriginFileSystem> BrowserContext::OriginFiles(const std::wstring& origin){
    const auto canonical=Origin(origin);if(canonical==L"null")return {};
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto& files=impl_->originFiles[canonical];if(!files){files=std::make_shared<Internal::OriginFileSystem>();
        if(!impl_->profileDirectory.empty()){const auto filename=StorageFileName(canonical);
            if(!filename.empty()){files->file=std::filesystem::path(impl_->profileDirectory)/L"OriginFiles"/(filename+L".opfs");files->Load();}}}
    return files;
}
NetworkResponse BrowserContext::Request(const NetworkRequest& request){
    auto response=impl_->http.Send(request);
    NetworkObserver observer;
    {std::lock_guard<std::mutex> lock(impl_->mutex);observer=impl_->networkObserver;}
    if(observer)try{observer(request,response);}catch(...){/* Diagnostics cannot break transport. */}
    return response;
}
void BrowserContext::SetNetworkObserver(NetworkObserver observer){
    std::lock_guard<std::mutex> lock(impl_->mutex);impl_->networkObserver=std::move(observer);
}
std::wstring BrowserContext::DocumentCookie(const std::wstring& url,const std::wstring& siteForCookies){return impl_->cookies->Header(url,siteForCookies,false,L"GET",true);}
void BrowserContext::SetDocumentCookie(const std::wstring& url,const std::wstring& cookie,const std::wstring& siteForCookies){impl_->cookies->Set(url,cookie,true,siteForCookies);}
bool BrowserContext::CookiesEnabled() const {return true;}
std::uint64_t BrowserContext::CookieRevision() const {return impl_->cookies->Revision();}
std::uint64_t BrowserContext::NewBrowsingContext(){return ++impl_->nextContext;}
void BrowserContext::ReleaseBrowsingContext(std::uint64_t session){
    if(!session)return;const auto prefix=std::to_wstring(session)+L"|";std::lock_guard<std::mutex> lock(impl_->mutex);
    for(auto item=impl_->storage.begin();item!=impl_->storage.end();){
        if(item->first.rfind(prefix,0)==0)item=impl_->storage.erase(item);else ++item;
    }
}
std::shared_ptr<StorageArea> BrowserContext::Storage(const std::wstring& origin,std::uint64_t session){
    const auto canonical=Origin(origin);if(canonical==L"null")return {};
    const auto key=std::to_wstring(session)+L"|"+canonical;
    std::lock_guard<std::mutex> lock(impl_->mutex);const auto found=impl_->storage.find(key);if(found!=impl_->storage.end())return found->second;
    auto data=std::make_shared<StorageArea::Impl>();data->origin=canonical;
    if(!session&&!impl_->profileDirectory.empty()){
        const auto filename=StorageFileName(canonical);if(!filename.empty())data->file=std::filesystem::path(impl_->profileDirectory)/L"LocalStorage"/filename;
    }
    data->Load();auto area=std::shared_ptr<StorageArea>(new StorageArea(std::move(data)));impl_->storage.emplace(key,area);return area;
}
std::wstring BrowserContext::Origin(const std::wstring& url){const Internal::HttpUrl parsed(url);return parsed.valid?parsed.origin:L"null";}
std::wstring BrowserContext::Site(const std::wstring& url){return Internal::SchemefulSite(url);}
const wchar_t* BrowserContext::UserAgent(){return L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) TWebFrame/1.0";}
}
