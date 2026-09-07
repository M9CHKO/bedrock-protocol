#pragma once

#include <windows.h>
#include <winhttp.h>
#include <bedrock/auth/XboxTokenManager.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace cpe {
inline std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (!count) throw std::runtime_error("Invalid UTF-8");
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count);
    return result;
}
inline std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("Invalid HTTPS header encoding");
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}
struct HttpHandle {
    HINTERNET value;
    explicit HttpHandle(HINTERNET handle) : value(handle) {
        if (!value) throw std::runtime_error("HTTPS initialization failed: " + std::to_string(GetLastError()));
    }
    ~HttpHandle() { WinHttpCloseHandle(value); }
    HttpHandle(const HttpHandle&) = delete;
};
inline void httpCheck(BOOL success) {
    if (!success) throw std::runtime_error("HTTPS request failed: " + std::to_string(GetLastError()));
}

// Uses Windows certificate/proxy handling, never a shell, temporary token file,
// disabled certificate check or a managed callback on native authentication threads.
class WindowsTokenHttpClient final : public bedrock::IXboxTokenHttpClient {
    std::shared_ptr<bedrock::JsMicrotaskQueue> queue_;
public:
    explicit WindowsTokenHttpClient(std::shared_ptr<bedrock::JsMicrotaskQueue> queue)
        : queue_(std::move(queue)) {}
    bedrock::JsPromise<bedrock::XboxTokenHttpResponse> fetch(bedrock::XboxTokenHttpRequest input) override {
        return bedrock::JsPromise<bedrock::XboxTokenHttpResponse>::fromSynchronous(queue_,
            [input = std::move(input)] {
                auto url = wide(input.url);
                URL_COMPONENTS parts {};
                parts.dwStructSize = sizeof(parts);
                parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
                httpCheck(WinHttpCrackUrl(url.c_str(), 0, 0, &parts));
                if (parts.nScheme != INTERNET_SCHEME_HTTPS) throw std::runtime_error("Authentication requires HTTPS");
                std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
                std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
                if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
                if (path.empty()) path = L"/";
                HttpHandle session(WinHttpOpen(L"CPE-Relay-Windows/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
                httpCheck(WinHttpSetTimeouts(session.value, 15000, 15000, 20000, 20000));
                HttpHandle connection(WinHttpConnect(session.value, host.c_str(), parts.nPort, 0));
                auto method = wide(input.method);
                for (auto& letter : method) if (letter >= L'a' && letter <= L'z') letter -= L'a' - L'A';
                HttpHandle request(WinHttpOpenRequest(connection.value, method.c_str(), path.c_str(), nullptr,
                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
                DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
                httpCheck(WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)));
                std::wstring headers;
                for (const auto& [name, value] : input.headers) {
                    if (name.find_first_of("\r\n:") != std::string::npos || value.find_first_of("\r\n") != std::string::npos)
                        throw std::runtime_error("Invalid HTTPS header");
                    headers += wide(name) + L": " + wide(value) + L"\r\n";
                }
                if (input.body.size() > 8u * 1024u * 1024u) throw std::runtime_error("HTTPS body too large");
                httpCheck(WinHttpSendRequest(request.value, headers.c_str(), static_cast<DWORD>(headers.size()),
                    input.body.empty() ? nullptr : const_cast<char*>(input.body.data()),
                    static_cast<DWORD>(input.body.size()), static_cast<DWORD>(input.body.size()), 0));
                httpCheck(WinHttpReceiveResponse(request.value, nullptr));
                DWORD status = 0, size = sizeof(status);
                httpCheck(WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX));
                bedrock::XboxTokenHttpResponse result;
                result.status = static_cast<int>(status);
                auto query = [&](DWORD field) {
                    DWORD length = 0;
                    WinHttpQueryHeaders(request.value, field, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &length, WINHTTP_NO_HEADER_INDEX);
                    if (!length) return std::wstring();
                    if (length > 128 * 1024) throw std::runtime_error("HTTPS headers too large");
                    std::wstring text(length / sizeof(wchar_t), L'\0');
                    httpCheck(WinHttpQueryHeaders(request.value, field, WINHTTP_HEADER_NAME_BY_INDEX, text.data(), &length, WINHTTP_NO_HEADER_INDEX));
                    text.resize(length / sizeof(wchar_t));
                    while (!text.empty() && text.back() == L'\0') text.pop_back();
                    return text;
                };
                result.statusText = utf8(query(WINHTTP_QUERY_STATUS_TEXT));
                const auto raw = utf8(query(WINHTTP_QUERY_RAW_HEADERS_CRLF));
                for (std::size_t begin = 0; begin < raw.size();) {
                    const auto end = raw.find("\r\n", begin);
                    const auto line = raw.substr(begin, end == std::string::npos ? end : end - begin);
                    const auto colon = line.find(':');
                    if (colon != std::string::npos) {
                        auto value = line.substr(colon + 1);
                        value.erase(0, value.find_first_not_of(" \t"));
                        result.headers.emplace_back(line.substr(0, colon), std::move(value));
                    }
                    if (end == std::string::npos) break;
                    begin = end + 2;
                }
                char buffer[16384];
                DWORD read = 0;
                do {
                    httpCheck(WinHttpReadData(request.value, buffer, sizeof(buffer), &read));
                    if (result.bodyText.size() + read > 8u * 1024u * 1024u) throw std::runtime_error("HTTPS response too large");
                    result.bodyText.append(buffer, read);
                } while (read);
                return result;
            });
    }
};
} // namespace cpe
