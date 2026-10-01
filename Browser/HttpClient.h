#pragma once

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>
#include <memory>
#include <TWebFrame/BrowserContext.h>

struct HttpResponse {
    DWORD status = 0;
    std::wstring url;
    std::wstring contentType;
    std::wstring error;
    std::vector<unsigned char> body;
    std::wstring headers;
    bool Ok() const { return status >= 200 && status < 300; }
};

// BrowserContext owns the shared HTTP transport and explicit profile cookie jar.
class HttpClient {
public:
    explicit HttpClient(std::shared_ptr<TWebFrame::BrowserContext> context=std::make_shared<TWebFrame::BrowserContext>());
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpResponse Get(const std::wstring& url, const std::wstring& referer = L"",bool navigation=false);
    HttpResponse Request(const TWebFrame::NetworkRequest& request);
    static std::wstring DecodeText(const HttpResponse& response);
    std::shared_ptr<TWebFrame::BrowserContext> Context() const {return context_;}

private:
    std::shared_ptr<TWebFrame::BrowserContext> context_;
};
