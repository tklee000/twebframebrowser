#pragma once

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>

struct HttpResponse {
    DWORD status = 0;
    std::wstring url;
    std::wstring contentType;
    std::wstring error;
    std::vector<unsigned char> body;
    bool Ok() const { return status >= 200 && status < 300; }
};

// One WinHTTP session shares cookies across pages and their resources.
class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpResponse Get(const std::wstring& url, const std::wstring& referer = L"");
    static std::wstring DecodeText(const HttpResponse& response);

private:
    HINTERNET session_ = nullptr;
};
