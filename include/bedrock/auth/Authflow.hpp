#pragma once

#include <exception>
#include <future>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bedrock {

// Runtime shape returned by prismarine-auth's getXboxToken(). Keep the
// JavaScript property spelling (including XSTSToken) because prismarine-realms
// consumes those names directly when it formats the Bedrock authorization
// header. The dependency currently returns expiresOn as an ISO timestamp.
struct XboxToken {
    std::string userXUID;
    std::string userHash;
    std::string XSTSToken;
    std::string expiresOn;
};

using MinecraftBedrockTokenChains = std::vector<std::string>;
using MinecraftBedrockTokenFuture =
    std::future<MinecraftBedrockTokenChains>;
using XboxTokenFuture = std::future<XboxToken>;

inline constexpr const char* AuthflowDefaultXboxRelyingParty =
    "http://xboxlive.com";

// Small Promise helpers for language-adapted supplied authflow objects. They
// deliberately use std::promise rather than std::async so a ready/rejected JS
// Promise does not create an unrelated native worker.
template <typename T>
std::future<T> makeReadyAuthflowFuture(T value) {
    std::promise<T> promise;
    auto future = promise.get_future();
    promise.set_value(std::move(value));
    return future;
}

template <typename T>
std::future<T> makeRejectedAuthflowFuture(std::exception_ptr error) {
    std::promise<T> promise;
    auto future = promise.get_future();
    promise.set_exception(std::move(error));
    return future;
}

template <typename T>
std::future<T> makeRejectedAuthflowFuture(std::string message) {
    return makeRejectedAuthflowFuture<T>(
        std::make_exception_ptr(std::runtime_error(std::move(message)))
    );
}

// Language-adapted runtime object accepted by options.authflow. The method
// invocation itself remains synchronous, while its returned std::future is the
// native counterpart of the JavaScript Promise. This distinction matters:
// throwing from a supplied method is observed before Client.startQueue(), but
// a rejected future settles after that synchronous connect stack.
class Authflow {
public:
    using MinecraftBedrockTokenMethod = std::function<
        MinecraftBedrockTokenFuture(std::string publicKey)
    >;
    using XboxTokenMethod = std::function<
        XboxTokenFuture(std::string relyingParty, bool forceRefresh)
    >;

    Authflow() = default;

    explicit Authflow(
        MinecraftBedrockTokenMethod getMinecraftBedrockToken,
        XboxTokenMethod getXboxToken = {}
    ) : getMinecraftBedrockToken_(std::move(getMinecraftBedrockToken)),
        getXboxToken_(std::move(getXboxToken)) {}

    MinecraftBedrockTokenFuture getMinecraftBedrockToken(
        std::string publicKey
    ) {
        if (!getMinecraftBedrockToken_) {
            // Exact V8 error text produced by calling a missing method on the
            // supplied runtime object in auth.js.
            throw std::runtime_error(
                "authflow.getMinecraftBedrockToken is not a function"
            );
        }
        return getMinecraftBedrockToken_(std::move(publicKey));
    }

    XboxTokenFuture getXboxToken(
        std::string relyingParty = AuthflowDefaultXboxRelyingParty,
        bool forceRefresh = false
    ) {
        if (!getXboxToken_) {
            throw std::runtime_error(
                "authflow.getXboxToken is not a function"
            );
        }
        return getXboxToken_(std::move(relyingParty), forceRefresh);
    }

    void setMinecraftBedrockTokenMethod(
        MinecraftBedrockTokenMethod method
    ) {
        getMinecraftBedrockToken_ = std::move(method);
    }

    void setXboxTokenMethod(XboxTokenMethod method) {
        getXboxToken_ = std::move(method);
    }

    bool hasMinecraftBedrockTokenMethod() const noexcept {
        return static_cast<bool>(getMinecraftBedrockToken_);
    }

    bool hasXboxTokenMethod() const noexcept {
        return static_cast<bool>(getXboxToken_);
    }

private:
    MinecraftBedrockTokenMethod getMinecraftBedrockToken_;
    XboxTokenMethod getXboxToken_;
};

using AuthflowPtr = std::shared_ptr<Authflow>;

} // namespace bedrock
