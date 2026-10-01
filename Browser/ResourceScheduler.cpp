#include "ResourceScheduler.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

class WorkQueue {
public:
    using Task = std::function<void()>;

    explicit WorkQueue(std::size_t count) {
        count = std::max<std::size_t>(1, count);
        workers_.reserve(count);
        for (std::size_t index = 0; index < count; ++index)
            workers_.emplace_back([this] { Run(); });
    }

    ~WorkQueue() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
    }

    void Submit(ResourcePriority priority, Task task) {
        if (!task) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return;
            queues_[static_cast<std::size_t>(priority)].push_back(std::move(task));
        }
        ready_.notify_one();
    }

    std::size_t Size() const noexcept { return workers_.size(); }

private:
    void Run() {
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] {
                    return stopping_ || !queues_[0].empty() ||
                           !queues_[1].empty() || !queues_[2].empty();
                });
                if (stopping_ && queues_[0].empty() && queues_[1].empty() &&
                    queues_[2].empty()) return;
                for (auto& queue : queues_) if (!queue.empty()) {
                    task = std::move(queue.front());
                    queue.pop_front();
                    break;
                }
            }
            // A host callback must not be able to terminate a worker and leave
            // the scheduler permanently below its configured concurrency.
            try { if (task) task(); } catch (...) {}
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::array<std::deque<Task>, 3> queues_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

std::size_t DefaultWorkerCount() {
    const auto logical = std::max(4u, std::thread::hardware_concurrency());
    return std::max<std::size_t>(4, std::min<std::size_t>(16, logical));
}

TWebFrame::NetworkRequest GetRequest(const std::wstring& url,const std::wstring& referer,bool navigation=false){
    TWebFrame::NetworkRequest request;request.url=url;request.referrer=referer;
    request.origin=TWebFrame::BrowserContext::Origin(referer);
    request.siteForCookies=referer;request.topLevelNavigation=navigation;
    request.mode=TWebFrame::NetworkRequest::Mode::Navigation;
    request.credentials=TWebFrame::NetworkRequest::Credentials::Include;return request;
}
std::wstring RequestKey(const TWebFrame::NetworkRequest& request,std::uint64_t revision) {
    // Include the initiating origin, ancestor site policy and cookie generation
    // so a child frame cannot reuse a response fetched with different cookies.
    auto key=request.url+L"|"+TWebFrame::BrowserContext::Origin(request.referrer)+L"|"+
        (request.siteForCookies.empty()?L"":request.siteForCookies==L"null"?L"null":TWebFrame::BrowserContext::Site(request.siteForCookies))+
        L"|"+std::to_wstring(revision)+(request.topLevelNavigation?L"|navigation":L"|resource");
    key+=L"|"+request.method+L"|"+std::to_wstring(static_cast<unsigned>(request.mode))+L"|"+std::to_wstring(static_cast<unsigned>(request.credentials));
    for(const auto& header:request.headers)key+=L"|"+header.first+L":"+header.second;
    key+=request.includeCredentials?L"|include":L"|default";
    return key;
}
std::wstring Trim(const std::wstring& value){
    const auto first=value.find_first_not_of(L" \t"),last=value.find_last_not_of(L" \t");
    return first==std::wstring::npos?L"":value.substr(first,last-first+1);
}
std::wstring HeaderValue(const std::wstring& headers,const std::wstring& name){
    std::wstring value;
    for(size_t start=0;start<headers.size();){
        const auto end=headers.find(L"\r\n",start);const auto line=headers.substr(start,end-start);
        const auto colon=line.find(L':');
        if(colon!=std::wstring::npos&&Trim(line.substr(0,colon))==name){
            if(!value.empty())value+=L",";value+=Trim(line.substr(colon+1));
        }
        if(end==std::wstring::npos)break;start=end+2;
    }
    return value;
}
bool Seconds(const std::wstring& input,unsigned long long& seconds){
    auto text=Trim(input);
    if(text.size()>1&&text.front()==L'"'&&text.back()==L'"')text=text.substr(1,text.size()-2);
    if(text.empty()||text.find_first_not_of(L"0123456789")!=std::wstring::npos)return false;
    seconds=0;
    for(const auto digit:text)seconds=std::min<unsigned long long>(86400,seconds*10+digit-L'0');
    return true;
}

} // namespace

struct ResourceScheduler::Impl {
    struct CacheEntry {
        Response response;
        std::size_t bytes = 0;
        std::chrono::steady_clock::time_point stored;
        std::chrono::seconds lifetime{0};
        std::list<std::wstring>::iterator lru;
    };
    struct Pending {
        struct Subscriber {
            Callback callback;
            CancellationToken cancellation;
        };
        std::vector<Subscriber> subscribers;
    };

    explicit Impl(std::size_t workers, std::size_t limit)
        : cacheLimit(limit), queue(workers ? workers : DefaultWorkerCount()) {}

    Response CachedLocked(const std::wstring& key) {
        auto found = cache.find(key);
        if (found == cache.end()) return {};
        if (std::chrono::steady_clock::now() - found->second.stored >= found->second.lifetime) {
            cacheBytes -= found->second.bytes;
            lru.erase(found->second.lru);
            cache.erase(found);
            return {};
        }
        lru.splice(lru.begin(), lru, found->second.lru);
        return found->second.response;
    }

    Response Cached(const std::wstring& key) {
        std::lock_guard<std::mutex> lock(mutex);
        return CachedLocked(key);
    }

    void Remember(const std::wstring& key, const Response& response) {
        if(!response||!response->Ok())return;
        auto headers=response->headers;std::transform(headers.begin(),headers.end(),headers.begin(),towlower);
        // Conservatively cache explicit freshness only. Vary responses require
        // full request-header matching and are fetched again until that exists.
        if(headers.find(L"no-store")!=std::wstring::npos||headers.find(L"no-cache")!=std::wstring::npos||
           headers.find(L"vary:")!=std::wstring::npos)return;
        const auto control=HeaderValue(headers,L"cache-control");bool foundAge=false;
        unsigned long long seconds=0;
        for(size_t start=0;start<control.size();){
            const auto end=control.find(L',',start);const auto item=Trim(control.substr(start,end-start));
            const auto equal=item.find(L'=');
            if(equal!=std::wstring::npos&&Trim(item.substr(0,equal))==L"max-age"){
                if(foundAge||!Seconds(item.substr(equal+1),seconds))return;foundAge=true;
            }
            if(end==std::wstring::npos)break;start=end+1;
        }
        if(!foundAge||!seconds)return;
        unsigned long long age=0;const auto ageHeader=HeaderValue(headers,L"age");
        if(!ageHeader.empty()&&!Seconds(ageHeader,age))return;
        // Date can make a response older than its Age field indicates.
        SYSTEMTIME date{};const auto dateHeader=HeaderValue(headers,L"date");
        if(!dateHeader.empty()){
            FILETIME timestamp{};if(!WinHttpTimeToSystemTime(dateHeader.c_str(),&date)||!SystemTimeToFileTime(&date,&timestamp))return;
            ULARGE_INTEGER ticks{};ticks.LowPart=timestamp.dwLowDateTime;ticks.HighPart=timestamp.dwHighDateTime;
            const auto sent=static_cast<long long>(ticks.QuadPart/10000000ull)-11644473600ll;
            const auto now=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            if(now>sent)age=std::max<unsigned long long>(age,static_cast<unsigned long long>(now-sent));
        }
        if(age>=seconds)return;
        const auto lifetime=std::chrono::seconds(seconds-age);
        const std::size_t bytes = response ? response->body.size() : 0;
        auto existing = cache.find(key);
        if (existing != cache.end()) {
            cacheBytes -= existing->second.bytes;
            lru.erase(existing->second.lru);
            cache.erase(existing);
        }
        lru.push_front(key);
        cache.emplace(key, CacheEntry{response, bytes,
            std::chrono::steady_clock::now(),lifetime, lru.begin()});
        cacheBytes += bytes;
        while ((!lru.empty() && cacheBytes > cacheLimit) || cache.size() > 512) {
            const auto oldest = std::prev(lru.end());
            const auto found = cache.find(*oldest);
            if (found != cache.end()) {
                cacheBytes -= found->second.bytes;
                cache.erase(found);
            }
            lru.erase(oldest);
        }
    }

    std::mutex mutex;
    std::unordered_map<std::wstring, CacheEntry> cache;
    std::unordered_map<std::wstring, Pending> pending;
    std::list<std::wstring> lru;
    std::size_t cacheBytes = 0;
    std::size_t cacheLimit = 0;
    static std::wstring ProfileDirectory(){
        wchar_t directory[32768]{};const auto count=GetEnvironmentVariableW(L"LOCALAPPDATA",directory,32768);
        return count&&count<32768?std::wstring(directory)+L"\\TWebFrameBrowser\\Profile":L"";
    }
    std::shared_ptr<TWebFrame::BrowserContext> context=std::make_shared<TWebFrame::BrowserContext>(ProfileDirectory());
    HttpClient client{context};
    // Declared last so its destructor joins every worker before the mutex,
    // pending subscribers, cache, and HttpClient session are destroyed.
    WorkQueue queue;
};

ResourceScheduler::ResourceScheduler(std::size_t workerCount, std::size_t cacheBytes)
    : impl_(std::make_unique<Impl>(workerCount, cacheBytes)) {}

ResourceScheduler::~ResourceScheduler() = default;

void ResourceScheduler::Fetch(const std::wstring& url, const std::wstring& referer,
                              ResourcePriority priority, Callback callback,
                              CancellationToken cancellation,bool navigation) {
    Fetch(GetRequest(url,referer,navigation),priority,std::move(callback),std::move(cancellation));
}
void ResourceScheduler::Fetch(const TWebFrame::NetworkRequest& request,ResourcePriority priority,
                              Callback callback,CancellationToken cancellation) {
    if (!callback) callback = [](Response) {};
    if (cancellation && cancellation->load(std::memory_order_relaxed)) return;
    auto key = RequestKey(request,impl_->context->CookieRevision());
    if(request.method!=L"GET"){static std::atomic<std::uint64_t> sequence{0};key+=L"|request-"+std::to_wstring(++sequence);}

    Response cached;
    bool start = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        // Cache lookup and in-flight registration share one critical section;
        // otherwise a completion between the two could trigger a duplicate GET.
        cached = impl_->CachedLocked(key);
        if (!cached) {
            const auto inserted = impl_->pending.try_emplace(key);
            start = inserted.second;
            inserted.first->second.subscribers.push_back(
                {std::move(callback), std::move(cancellation)});
        }
    }
    if (cached) {
        if (!cancellation || !cancellation->load(std::memory_order_relaxed))
            callback(std::move(cached));
        return;
    }
    if (!start) return;

    impl_->queue.Submit(priority, [this, key,request] {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            const auto pending = impl_->pending.find(key);
            if (pending == impl_->pending.end()) return;
            const bool wanted = std::any_of(pending->second.subscribers.begin(),
                pending->second.subscribers.end(), [](const auto& subscriber) {
                    return !subscriber.cancellation ||
                        !subscriber.cancellation->load(std::memory_order_relaxed);
                });
            if (!wanted) { impl_->pending.erase(pending); return; }
        }
        auto response = std::make_shared<HttpResponse>(impl_->client.Request(request));
        std::vector<Impl::Pending::Subscriber> subscribers;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if(!request.topLevelNavigation&&request.method==L"GET")impl_->Remember(key, response);
            const auto found = impl_->pending.find(key);
            if (found != impl_->pending.end()) {
                subscribers = std::move(found->second.subscribers);
                impl_->pending.erase(found);
            }
        }
        for (auto& subscriber : subscribers)
            if (subscriber.callback && (!subscriber.cancellation ||
                !subscriber.cancellation->load(std::memory_order_relaxed))) {
                try { subscriber.callback(response); } catch (...) {}
            }
    });
}

void ResourceScheduler::Prefetch(const std::wstring& url, const std::wstring& referer,
                                 ResourcePriority priority,
                                 CancellationToken cancellation) {
    Fetch(url, referer, priority, [](Response) {}, std::move(cancellation));
}

ResourceScheduler::Response ResourceScheduler::TryGet(const std::wstring& url,
                                                       const std::wstring& referer) {
    return impl_->Cached(RequestKey(GetRequest(url,referer),impl_->context->CookieRevision()));
}

ResourceScheduler::Response ResourceScheduler::Get(const std::wstring& url,
                                                    const std::wstring& referer) {
    return Get(GetRequest(url,referer));
}
ResourceScheduler::Response ResourceScheduler::Get(const TWebFrame::NetworkRequest& request) {
    if (auto cached = impl_->Cached(RequestKey(request,impl_->context->CookieRevision()))) return cached;
    auto promise = std::make_shared<std::promise<Response>>();
    auto future = promise->get_future();
    Fetch(request, ResourcePriority::High,
          [promise](Response response) { promise->set_value(std::move(response)); });
    return future.get();
}

std::size_t ResourceScheduler::WorkerCount() const noexcept {
    return impl_->queue.Size();
}
std::shared_ptr<TWebFrame::BrowserContext> ResourceScheduler::Context() const {return impl_->context;}
