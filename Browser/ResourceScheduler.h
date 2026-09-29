#pragma once

#include "HttpClient.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

enum class ResourcePriority { High, Normal, Low };

// Bounded browser-wide request scheduler. It keeps blocking WinHTTP calls off
// the UI thread, coalesces identical in-flight requests, and shares a bounded
// in-memory response cache across tabs.
class ResourceScheduler final {
public:
    using Response = std::shared_ptr<const HttpResponse>;
    using Callback = std::function<void(Response)>;
    using CancellationToken = std::shared_ptr<std::atomic_bool>;

    explicit ResourceScheduler(std::size_t workerCount = 0,
                               std::size_t cacheBytes = 128u * 1024u * 1024u);
    ~ResourceScheduler();
    ResourceScheduler(const ResourceScheduler&) = delete;
    ResourceScheduler& operator=(const ResourceScheduler&) = delete;

    void Fetch(const std::wstring& url, const std::wstring& referer,
               ResourcePriority priority, Callback callback,
               CancellationToken cancellation = {});
    void Prefetch(const std::wstring& url, const std::wstring& referer,
                  ResourcePriority priority = ResourcePriority::Low,
                  CancellationToken cancellation = {});

    // Compatibility path for TWebFrame's synchronous resource callbacks.
    // Prefetched resources return immediately; an unexpected dynamic request
    // waits on a network worker rather than performing I/O in this caller.
    Response Get(const std::wstring& url, const std::wstring& referer = L"");
    Response TryGet(const std::wstring& url, const std::wstring& referer = L"");

    std::size_t WorkerCount() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
