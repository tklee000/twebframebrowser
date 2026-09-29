#include "HttpClient.h"

#include <algorithm>
#include <cwctype>
#include <string>

#pragma comment(lib, "winhttp.lib")

namespace {
struct Handle {
    HINTERNET value = nullptr;
    ~Handle() { if (value) WinHttpCloseHandle(value); }
};

std::wstring LastErrorText(DWORD code) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
        reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::wstring result = message ? message : L"Windows 오류 " + std::to_wstring(code);
    if (message) LocalFree(message);
    return result;
}

std::wstring Header(HINTERNET request, DWORD flag) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, flag, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                        &bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) return L"";
    std::wstring result(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, flag, WINHTTP_HEADER_NAME_BY_INDEX,
                             result.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) return L"";
    result.resize(wcslen(result.c_str()));
    return result;
}

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}

std::wstring Origin(const std::wstring& url) {
    const auto scheme = url.find(L"://");
    if (scheme == std::wstring::npos) return L"";
    return Lower(url.substr(0, url.find_first_of(L"/?#", scheme + 3)));
}

std::wstring Decode(const std::vector<unsigned char>& bytes, UINT codePage, DWORD flags = 0) {
    if (bytes.empty()) return L"";
    int count = MultiByteToWideChar(codePage, flags,
        reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), nullptr, 0);
    if (count <= 0) return L"";
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(codePage, flags, reinterpret_cast<const char*>(bytes.data()),
                        static_cast<int>(bytes.size()), result.data(), count);
    return result;
}
} // namespace

HttpClient::HttpClient() {
    session_ = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        L"AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (session_) {
        WinHttpSetTimeouts(session_, 5000, 5000, 5000, 8000);
        DWORD perHostConnections = 6;
        WinHttpSetOption(session_, WINHTTP_OPTION_MAX_CONNS_PER_SERVER,
                         &perHostConnections, sizeof(perHostConnections));
    }
}

HttpClient::~HttpClient() {
    if (session_) WinHttpCloseHandle(session_);
}

HttpResponse HttpClient::Get(const std::wstring& url, const std::wstring& referer) {
    // Every call owns its connection and request handles.  The session handle
    // is intentionally shared so WinHTTP can reuse connections and cookies
    // while independent resources are transferred concurrently.
    HttpResponse response;
    response.url = url;
    if (!session_) { response.error = L"WinHTTP 세션을 시작할 수 없습니다."; return response; }
    URL_COMPONENTSW parts{sizeof(parts)};
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)) {
        response.error = L"HTTP 또는 HTTPS 주소가 필요합니다.";
        return response;
    }
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path = parts.dwUrlPathLength ?
        std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : L"/";
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    Handle connection{WinHttpConnect(session_, host.c_str(), parts.nPort, 0)};
    if (!connection.value) { response.error = LastErrorText(GetLastError()); return response; }
    std::wstring effectiveReferer;
    const auto targetOrigin = Origin(url), refererOrigin = Origin(referer);
    if (!refererOrigin.empty() && refererOrigin == targetOrigin) effectiveReferer = referer;
    else if (targetOrigin.rfind(L"https://", 0) == 0 &&
             refererOrigin.rfind(L"https://", 0) == 0) effectiveReferer = refererOrigin + L"/";
    Handle request{WinHttpOpenRequest(connection.value, L"GET", path.c_str(),
        nullptr, effectiveReferer.empty() ? WINHTTP_NO_REFERER : effectiveReferer.c_str(),
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)};
    if (!request.value) { response.error = LastErrorText(GetLastError()); return response; }
    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    WinHttpSetOption(request.value, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
    const wchar_t headers[] = L"Accept: text/html,application/xhtml+xml,image/avif,image/webp,image/*,*/*;q=0.8\r\n"
                              L"Accept-Language: ko-KR,ko;q=0.9,en;q=0.7\r\n";
    if (!WinHttpSendRequest(request.value, headers, static_cast<DWORD>(-1),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) {
        response.error = LastErrorText(GetLastError());
        return response;
    }
    DWORD statusSize = sizeof(response.status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &response.status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        response.error = LastErrorText(GetLastError());
        return response;
    }
    response.contentType = Header(request.value, WINHTTP_QUERY_CONTENT_TYPE);
    // WinHTTP follows ordinary redirects; record the destination for relative URLs.
    DWORD finalUrlBytes = 0;
    WinHttpQueryOption(request.value, WINHTTP_OPTION_URL, nullptr, &finalUrlBytes);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && finalUrlBytes > sizeof(wchar_t)) {
        std::wstring finalUrl(finalUrlBytes / sizeof(wchar_t), L'\0');
        if (WinHttpQueryOption(request.value, WINHTTP_OPTION_URL, finalUrl.data(), &finalUrlBytes))
            response.url.assign(finalUrl.c_str());
    }
    constexpr size_t maxResponse = 24 * 1024 * 1024;
    unsigned char block[32768];
    for (;;) {
        DWORD received = 0;
        if (!WinHttpReadData(request.value, block, sizeof(block), &received)) {
            response.error = LastErrorText(GetLastError());
            response.body.clear();
            return response;
        }
        if (!received) break;
        if (response.body.size() + received > maxResponse) {
            response.error = L"응답 크기가 24 MB 제한을 넘었습니다.";
            response.body.clear();
            return response;
        }
        response.body.insert(response.body.end(), block, block + received);
    }
    if (!response.Ok()) response.error = L"HTTP " + std::to_wstring(response.status);
    return response;
}

std::wstring HttpClient::DecodeText(const HttpResponse& response) {
    if (response.body.empty()) return L"";
    if (response.body.size() >= 3 && response.body[0] == 0xEF &&
        response.body[1] == 0xBB && response.body[2] == 0xBF) {
        std::vector<unsigned char> withoutBom(response.body.begin() + 3, response.body.end());
        return Decode(withoutBom, CP_UTF8);
    }
    std::wstring type = Lower(response.contentType);
    if (type.find(L"euc-kr") != std::wstring::npos ||
        type.find(L"ks_c_5601") != std::wstring::npos ||
        type.find(L"windows-949") != std::wstring::npos) return Decode(response.body, 949);
    if (type.find(L"iso-8859-1") != std::wstring::npos) return Decode(response.body, 28591);
    std::wstring utf8 = Decode(response.body, CP_UTF8, MB_ERR_INVALID_CHARS);
    return utf8.empty() ? Decode(response.body, 949) : utf8;
}
