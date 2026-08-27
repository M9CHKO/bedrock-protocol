#pragma once

#include <bedrock/auth/LiveTokenManager.hpp>
#include <bedrock/auth/MinecraftBedrockServicesManager.hpp>
#include <bedrock/auth/MinecraftBedrockTokenManager.hpp>
#include <bedrock/auth/MsaTokenManager.hpp>
#include <bedrock/auth/PlayfabTokenManager.hpp>
#include <bedrock/auth/XboxTokenManager.hpp>

#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace bedrock {

class JsUriError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct MicrosoftAuthFlowManagers {
    std::shared_ptr<LiveTokenManager> live;
    std::shared_ptr<MsaTokenManager> msa;
    std::shared_ptr<XboxTokenManager> xbox;
    std::shared_ptr<MinecraftBedrockTokenManager> bedrock;
    std::shared_ptr<MinecraftBedrockServicesTokenManager> bedrockServices;
    std::shared_ptr<PlayfabTokenManager> playfab;
};

struct MicrosoftAuthFlowObservers {
    using DebugMethod = std::function<void(
        const std::string& label,
        const std::vector<JsRuntimeValue>& arguments
    )>;

    DebugMethod debug;
    std::function<void(const std::string& message)> consoleInfo;
    std::function<void(const JsRuntimeValue& response)> codeCallback;
};

struct MicrosoftAuthFlowDependencies {
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue;
    std::function<JsPromise<void>(double milliseconds)> delay;

    // @xboxreplay/xboxlive-auth seam used only by the optional password
    // branch. A native adapter can be installed without coupling the normal
    // device-code/Sisu Bedrock flow to that package.
    std::function<JsPromise<JsRuntimeValue>(
        JsRuntimeValue email,
        JsRuntimeValue password,
        JsRuntimeValue options
    )> replayAuth;
    MicrosoftAuthFlowObservers observers;
};

struct MicrosoftAuthFlowState;

// Bedrock-only orchestration port of prismarine-auth's MicrosoftAuthFlow.js.
// Token-manager construction/cache selection remains a separate lifecycle
// concern; JavaTokenManager and getMinecraftJavaToken are deliberately absent.
class MicrosoftAuthFlow {
private:
    std::shared_ptr<MicrosoftAuthFlowState> state_;

public:
    inline static constexpr const char* XboxRelyingParty =
        "http://xboxlive.com";
    inline static constexpr const char* BedrockXstsRelyingParty =
        "https://multiplayer.minecraft.net/";
    inline static constexpr const char* PlayfabRelyingParty =
        "https://b980a380.minecraft.playfabapi.com/";

    MicrosoftAuthFlow(
        JsRuntimeValue usernameValue,
        JsRuntimeValue optionsValue,
        MicrosoftAuthFlowManagers managers,
        JsRuntimeValue doTitleAuthValue = JsRuntimeValue::undefined(),
        MicrosoftAuthFlowDependencies dependencies = {}
    );

    MicrosoftAuthFlow(const MicrosoftAuthFlow&) = delete;
    MicrosoftAuthFlow& operator=(const MicrosoftAuthFlow&) = delete;

    JsRuntimeValue& username;
    JsRuntimeValue& options;
    JsRuntimeValue& doTitleAuth;

    JsPromise<JsRuntimeValue> getMsaToken();
    JsPromise<JsRuntimeValue> getPlayfabLogin();
    JsPromise<JsRuntimeValue> getMinecraftBedrockServicesToken(
        JsRuntimeValue serviceOptions
    );
    JsPromise<JsRuntimeValue> getXboxToken(
        JsRuntimeValue relyingParty = JsRuntimeValue::undefined(),
        JsRuntimeValue forceRefresh = JsRuntimeValue::boolean(false)
    );
    JsPromise<JsRuntimeValue> getMinecraftBedrockToken(
        JsRuntimeValue publicKey
    );

    std::shared_ptr<JsMicrotaskQueue> microtaskQueue() const noexcept;
};

} // namespace bedrock
