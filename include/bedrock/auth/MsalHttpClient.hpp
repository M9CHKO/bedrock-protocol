#pragma once

#include <bedrock/auth/JsPromise.hpp>
#include <bedrock/auth/JsRuntimeValue.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bedrock {

struct MsalHttpRequest {
    std::string method = "POST";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    // Optional Axios/follow-redirects behavior used by the legacy XboxReplay
    // password flow. Ordinary MSAL requests retain the zero/no-follow default.
    int maxRedirects = 0;
    bool decompress = false;

    const std::string* header(std::string_view name) const noexcept;
};

struct MsalHttpResponse {
    int status = 0;
    // Final effective URL after redirects. Empty for injected transports that
    // do not need to model response.request.res.responseUrl.
    std::string url;

    // Scalar convenience view used by the native PCA. Header names are
    // lower-case and duplicate scalar fields have Node's IncomingMessage
    // joining semantics. Array-valued fields (currently set-cookie) are
    // flattened here; headersObject is the authoritative JavaScript shape.
    std::vector<std::pair<std::string, std::string>> headers;
    JsRuntimeValue headersObject = JsRuntimeValue::object();
    std::string bodyText;
    JsRuntimeValue body = JsRuntimeValue::undefined();

    const std::string* header(std::string_view lowerCaseName) const noexcept;
};

class IMsalHttpClient {
public:
    virtual ~IMsalHttpClient() = default;
    virtual JsPromise<MsalHttpResponse> send(MsalHttpRequest request) = 0;
};

using MsalHttpClientPtr = std::shared_ptr<IMsalHttpClient>;

// Default transport used by the native MSAL subset. It delegates only the
// byte transport to curl; all authority, polling, response, cache, and error
// logic remains in C++ and is independently injectable in tests.
class CurlMsalHttpClient final : public IMsalHttpClient {
public:
    explicit CurlMsalHttpClient(
        std::shared_ptr<JsMicrotaskQueue> microtaskQueue
    );

    JsPromise<MsalHttpResponse> send(MsalHttpRequest request) override;

private:
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue_;
};

} // namespace bedrock
