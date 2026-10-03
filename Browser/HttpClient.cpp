#include "HttpClient.h"

#include <algorithm>
#include <cwctype>
#include <string>

#pragma comment(lib, "winhttp.lib")

namespace {
std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
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

HttpClient::HttpClient(std::shared_ptr<TWebFrame::BrowserContext> context):context_(std::move(context)) {}
HttpClient::~HttpClient()=default;

HttpResponse HttpClient::Get(const std::wstring& url,const std::wstring& referer,bool navigation) {
    TWebFrame::NetworkRequest request;request.url=url;request.referrer=referer;
    request.origin=TWebFrame::BrowserContext::Origin(referer);
    request.siteForCookies=referer;request.topLevelNavigation=navigation;
    request.mode=TWebFrame::NetworkRequest::Mode::Navigation;
    request.credentials=TWebFrame::NetworkRequest::Credentials::Include;
    return Request(request);
}
HttpResponse HttpClient::Request(const TWebFrame::NetworkRequest& request) {
    const auto result=context_->Request(request);
    HttpResponse response;response.status=result.status;response.url=result.url;
    response.contentType=result.contentType;response.headers=result.headers;response.body=result.bytes;
    // HTTP 4xx/5xx responses can carry complete navigation documents. Keep
    // transport errors separate from the server's HTTP status.
    response.error=result.error;
    if(!response.status&&response.error.empty())response.error=L"Network request failed";
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
