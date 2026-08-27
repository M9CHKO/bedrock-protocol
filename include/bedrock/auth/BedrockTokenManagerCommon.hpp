#pragma once

#include <bedrock/auth/JsPromise.hpp>
#include <bedrock/auth/JsRuntimeValue.hpp>
#include <bedrock/auth/XboxTokenManager.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bedrock {

struct BedrockTokenManagerObservers {
    using DebugMethod = std::function<void(
        const std::string& label,
        const std::vector<JsRuntimeValue>& arguments
    )>;

    DebugMethod debug;
};

struct BedrockTokenManagerDependencies {
    XboxTokenHttpClientPtr httpClient;
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue;
    std::function<double()> dateNowMilliseconds;
    BedrockTokenManagerObservers observers;
};

} // namespace bedrock
