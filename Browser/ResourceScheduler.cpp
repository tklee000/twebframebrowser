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

std::wstring RequestKey(const std::wstring& url) {
    // Referer is request metadata rather than a cache identity. Keeping it in
    // the key prevented a CSS-discovered image from sharing the page prefetch.
    return url;
}

} // namespace

struct ResourceScheduler::Impl {
    struct CacheEntry {
        Response response;
        std::size_t bytes = 0;
        std::chrono::steady_clock::time_point stored;
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
        if (!found->second.response->Ok() &&
            std::chrono::steady_clock::now() - found->second.stored >
                std::chrono::seconds(5)) {
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
        const std::size_t bytes = response ? response->body.size() : 0;
        auto existing = cache.find(key);
        if (existing != cache.end()) {
            cacheBytes -= existing->second.bytes;
            lru.erase(existing->second.lru);
            cache.erase(existing);
        }
        lru.push_front(key);
        cache.emplace(key, CacheEntry{response, bytes,
            std::chrono::steady_clock::now(), lru.begin()});
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
    HttpClient client;
    // Declared last so its destructor joins every worker before the mutex,
    // pending subscribers, cache, and HttpClient session are destroyed.
    WorkQueue queue;
};

ResourceScheduler::ResourceScheduler(std::size_t workerCount, std::size_t cacheBytes)
    : impl_(std::make_unique<Impl>(workerCount, cacheBytes)) {}

ResourceScheduler::~ResourceScheduler() = default;

void ResourceScheduler::Fetch(const std::wstring& url, const std::wstring& referer,
                              ResourcePriority priority, Callback callback,
                              CancellationToken cancellation) {
    if (!callback) callback = [](Response) {};
    if (cancellation && cancellation->load(std::memory_order_relaxed)) return;
    const auto key = RequestKey(url);

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

    impl_->queue.Submit(priority, [this, key, url, referer] {
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
        auto response = std::make_shared<HttpResponse>(impl_->client.Get(url, referer));
        std::vector<Impl::Pending::Subscriber> subscribers;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->Remember(key, response);
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
                                                       const std::wstring&) {
    return impl_->Cached(RequestKey(url));
}

ResourceScheduler::Response ResourceScheduler::Get(const std::wstring& url,
                                                    const std::wstring& referer) {
    if (auto cached = TryGet(url, referer)) return cached;
    auto promise = std::make_shared<std::promise<Response>>();
    auto future = promise->get_future();
    Fetch(url, referer, ResourcePriority::High,
          [promise](Response response) { promise->set_value(std::move(response)); });
    return future.get();
}

std::size_t ResourceScheduler::WorkerCount() const noexcept {
    return impl_->queue.Size();
}
