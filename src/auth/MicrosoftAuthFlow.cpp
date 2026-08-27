#include <bedrock/auth/MicrosoftAuthFlow.hpp>

#include <bedrock/auth/BedrockAuthJwt.hpp>

#include <openssl/evp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace bedrock {

struct MicrosoftAuthFlowState {
    MicrosoftAuthFlowState(
        JsRuntimeValue usernameValue,
        JsRuntimeValue optionsValue,
        MicrosoftAuthFlowManagers managersValue,
        JsRuntimeValue doTitleAuthValue
    ) : username(std::move(usernameValue)),
        options(std::move(optionsValue)),
        doTitleAuth(std::move(doTitleAuthValue)),
        managers(std::move(managersValue)) {}

    JsRuntimeValue username;
    JsRuntimeValue options;
    JsRuntimeValue doTitleAuth;
    MicrosoftAuthFlowManagers managers;
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue;
    std::function<JsPromise<void>(double)> delay;
    std::function<JsPromise<JsRuntimeValue>(
        JsRuntimeValue,
        JsRuntimeValue,
        JsRuntimeValue
    )> replayAuth;
    MicrosoftAuthFlowObservers observers;
};

namespace {

using ValuePromise = JsPromise<JsRuntimeValue>;

std::string jsNumberToString(double value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
    return JsRuntimeJson::stringify(
        JsRuntimeValue::number(value)
    ).value_or("undefined");
}

std::string jsToString(const JsRuntimeValue& value);

std::string arrayToString(const JsRuntimeValue& value) {
    std::string result;
    for (std::size_t index = 0; index < value.length(); ++index) {
        if (index) result.push_back(',');
        const auto* item = value.get(index);
        if (!item || item->isUndefined() || item->isNull()) continue;
        result += jsToString(*item);
    }
    return result;
}

std::string jsToString(const JsRuntimeValue& value) {
    if (value.isUndefined()) return "undefined";
    if (value.isNull()) return "null";
    if (value.isBool()) return value.boolValue() ? "true" : "false";
    if (value.isNumber()) return jsNumberToString(value.numberValue());
    if (value.isString()) return value.stringValue();
    if (value.isArray()) return arrayToString(value);
    if (value.isDate()) {
        return value.dateIsValid() ? value.dateIsoString() : "Invalid Date";
    }
    if (value.isMap()) return "[object Map]";
    return "[object Object]";
}

JsRuntimeValue getProperty(
    const JsRuntimeValue& value,
    std::string_view name
) {
    if (value.isUndefined() || value.isNull()) {
        throw std::runtime_error(
            std::string("Cannot read properties of ") +
            (value.isNull() ? "null" : "undefined") + " (reading '" +
            std::string(name) + "')"
        );
    }
    const auto* found = value.get(name);
    return found ? *found : JsRuntimeValue::undefined();
}

JsRuntimeValue getIndex(const JsRuntimeValue& value, std::size_t index) {
    if (value.isUndefined() || value.isNull()) {
        throw std::runtime_error(
            std::string("Cannot read properties of ") +
            (value.isNull() ? "null" : "undefined") + " (reading '" +
            std::to_string(index) + "')"
        );
    }
    const auto* found = value.get(index);
    return found ? *found : JsRuntimeValue::undefined();
}

JsRuntimeValue spreadObject(const JsRuntimeValue& source) {
    auto result = JsRuntimeValue::object();
    if (source.isUndefined() || source.isNull()) return result;
    for (const auto& property : source.ownProperties()) {
        result.set(property.key, property.value);
    }
    return result;
}

bool nullish(const JsRuntimeValue& value) {
    return value.isUndefined() || value.isNull();
}

void debug(
    const std::shared_ptr<MicrosoftAuthFlowState>& state,
    const std::string& label,
    std::vector<JsRuntimeValue> arguments = {}
) {
    if (state->observers.debug) {
        state->observers.debug(label, arguments);
    }
}

void consoleInfo(
    const std::shared_ptr<MicrosoftAuthFlowState>& state,
    const std::string& message
) {
    if (state->observers.consoleInfo) {
        state->observers.consoleInfo(message);
    } else {
        std::cout << message << "\n";
    }
}

std::string exceptionMessage(std::exception_ptr error) {
    try {
        if (error) std::rethrow_exception(error);
    } catch (const std::exception& caught) {
        return caught.what();
    } catch (...) {
        return "non-standard exception";
    }
    return {};
}

bool isUriError(std::exception_ptr error) {
    try {
        if (error) std::rethrow_exception(error);
    } catch (const JsUriError&) {
        return true;
    } catch (...) {
    }
    return false;
}

JsPromise<void> defaultDelay(
    const std::shared_ptr<JsMicrotaskQueue>& queue,
    double milliseconds
) {
    return JsPromise<void>::create(
        queue,
        [milliseconds](
            JsPromise<void>::ResolveFunction resolve,
            JsPromise<void>::RejectFunction reject
        ) mutable {
            double effective = milliseconds;
            if (!std::isfinite(effective) || effective < 1.0 ||
                effective > 2147483647.0) {
                effective = 1.0;
            }
            effective = std::trunc(effective);
            try {
                std::thread timer([
                    effective,
                    resolve = std::move(resolve)
                ]() mutable {
                    std::this_thread::sleep_for(std::chrono::milliseconds(
                        static_cast<std::int64_t>(effective)
                    ));
                    resolve();
                });
                timer.detach();
            } catch (...) {
                reject(std::current_exception());
            }
        }
    );
}

std::shared_ptr<JsMicrotaskQueue> selectQueue(
    const MicrosoftAuthFlowManagers& managers
) {
    if (managers.live) return managers.live->microtaskQueue();
    if (managers.msa) return managers.msa->microtaskQueue();
    if (managers.xbox) return managers.xbox->microtaskQueue();
    if (managers.bedrock) return managers.bedrock->microtaskQueue();
    if (managers.bedrockServices) {
        return managers.bedrockServices->microtaskQueue();
    }
    if (managers.playfab) return managers.playfab->microtaskQueue();
    return JsMicrotaskQueue::create();
}

ValuePromise verifyMsa(
    const std::shared_ptr<MicrosoftAuthFlowState>& state
) {
    if (state->managers.live) return state->managers.live->verifyTokens();
    if (state->managers.msa) return state->managers.msa->verifyTokens();
    return ValuePromise::rejected(
        state->microtaskQueue,
        "this.msa.verifyTokens is not a function"
    );
}

ValuePromise msaAccessToken(
    const std::shared_ptr<MicrosoftAuthFlowState>& state
) {
    if (state->managers.live) return state->managers.live->getAccessToken();
    if (state->managers.msa) return state->managers.msa->getAccessToken();
    return ValuePromise::rejected(
        state->microtaskQueue,
        "this.msa.getAccessToken is not a function"
    );
}

ValuePromise msaDeviceCode(
    const std::shared_ptr<MicrosoftAuthFlowState>& state
) {
    const auto callback = [state](const JsRuntimeValue& response) {
        if (state->observers.codeCallback) {
            state->observers.codeCallback(response);
            return;
        }
        consoleInfo(
            state,
            "[msa] First time signing in. Please authenticate now:"
        );
        consoleInfo(state, jsToString(getProperty(response, "message")));
    };
    if (state->managers.live) {
        return state->managers.live->authDeviceCode(
            [callback](JsRuntimeValue response) {
                callback(response);
            }
        );
    }
    if (state->managers.msa) {
        return state->managers.msa->authDeviceCode(callback);
    }
    return ValuePromise::rejected(
        state->microtaskQueue,
        "this.msa.authDeviceCode is not a function"
    );
}

void forceMsaRefresh(
    const std::shared_ptr<MicrosoftAuthFlowState>& state
) {
    if (state->managers.live) {
        state->managers.live->forceRefresh = JsRuntimeValue::boolean(true);
    }
    if (state->managers.msa) {
        state->managers.msa->forceRefresh = JsRuntimeValue::boolean(true);
    }
}

ValuePromise getMsaTokenState(
    const std::shared_ptr<MicrosoftAuthFlowState>& state
) {
    return verifyMsa(state).then([state](const JsRuntimeValue& verified) {
        if (verified.truthy()) {
            debug(state, "[msa] Using existing tokens");
            return msaAccessToken(state).then([](
                const JsRuntimeValue& access
            ) {
                return getProperty(access, "token");
            });
        }

        debug(state, "[msa] No valid cached tokens, need to sign in");
        return msaDeviceCode(state).then([state](const JsRuntimeValue& result) {
            const auto account = getProperty(result, "account");
            if (account.truthy()) {
                consoleInfo(
                    state,
                    "[msa] Signed in as " + jsToString(getProperty(
                        account,
                        "username"
                    ))
                );
            } else {
                consoleInfo(state, "[msa] Signed in with Microsoft");
            }
            debug(state, "[msa] got auth result", {result});
            return getProperty(result, "accessToken");
        });
    });
}

using RetryMethod = std::function<ValuePromise()>;
using BeforeRetryMethod = std::function<void()>;

ValuePromise retry(
    const std::shared_ptr<MicrosoftAuthFlowState>& state,
    const std::shared_ptr<RetryMethod>& method,
    const std::shared_ptr<BeforeRetryMethod>& beforeRetry,
    int times
) {
    ValuePromise attempt;
    try {
        attempt = (*method)();
    } catch (...) {
        attempt = ValuePromise::rejected(
            state->microtaskQueue,
            std::current_exception()
        );
    }
    if (times <= 1) return attempt;
    return attempt.catchError([
        state,
        method,
        beforeRetry,
        times
    ](std::exception_ptr error) -> ValuePromise {
        if (isUriError(error)) std::rethrow_exception(error);
        debug(
            state,
            exceptionMessage(error),
            {}
        );
        return state->delay(2000.0).then([beforeRetry] {
            (*beforeRetry)();
        }).then([state, method, beforeRetry, times] {
            return retry(state, method, beforeRetry, times - 1);
        });
    });
}

ValuePromise resolved(
    const std::shared_ptr<MicrosoftAuthFlowState>& state,
    JsRuntimeValue value
) {
    return ValuePromise::resolved(
        state->microtaskQueue,
        std::move(value)
    );
}

ValuePromise tokenOrUserRequest(
    const std::shared_ptr<MicrosoftAuthFlowState>& state,
    const JsRuntimeValue& cachedUser,
    const JsRuntimeValue& msaToken,
    const JsRuntimeValue& flow
) {
    const auto token = getProperty(cachedUser, "token");
    if (!nullish(token)) return resolved(state, token);
    return state->managers.xbox->getUserToken(
        msaToken,
        JsRuntimeValue::boolean(
            flow.isString() && flow.stringValue() == "msal"
        )
    );
}

ValuePromise tokenOrDeviceRequest(
    const std::shared_ptr<MicrosoftAuthFlowState>& state,
    const JsRuntimeValue& cachedDevice,
    const JsRuntimeValue& options
) {
    const auto token = getProperty(cachedDevice, "token");
    if (!nullish(token)) return resolved(state, token);
    return state->managers.xbox->getDeviceToken(options);
}

ValuePromise tokenOrTitleRequest(
    const std::shared_ptr<MicrosoftAuthFlowState>& state,
    const JsRuntimeValue& cachedTitle,
    const JsRuntimeValue& msaToken,
    const JsRuntimeValue& deviceToken
) {
    const auto token = getProperty(cachedTitle, "token");
    if (!nullish(token)) return resolved(state, token);
    if (!state->doTitleAuth.truthy()) {
        return resolved(state, JsRuntimeValue::undefined());
    }
    return state->managers.xbox->getTitleToken(msaToken, deviceToken);
}

ValuePromise getXboxTokenState(
    const std::shared_ptr<MicrosoftAuthFlowState>& state,
    JsRuntimeValue relyingParty,
    JsRuntimeValue forceRefresh
) {
    if (!state->managers.xbox) {
        return ValuePromise::rejected(
            state->microtaskQueue,
            "this.xbl.getCachedTokens is not a function"
        );
    }
    if (relyingParty.isUndefined()) {
        const auto configured = getProperty(state->options, "relyingParty");
        relyingParty = configured.truthy()
            ? configured
            : JsRuntimeValue::string(MicrosoftAuthFlow::XboxRelyingParty);
    }
    auto options = spreadObject(state->options);
    options.set("relyingParty", relyingParty);
    return state->managers.xbox->getCachedTokens(relyingParty).then([
        state,
        options,
        forceRefresh
    ](const JsRuntimeValue& cached) {
        const auto xstsToken = getProperty(cached, "xstsToken");
        const auto userToken = getProperty(cached, "userToken");
        const auto deviceToken = getProperty(cached, "deviceToken");
        const auto titleToken = getProperty(cached, "titleToken");
        if (getProperty(xstsToken, "valid").truthy() &&
            !forceRefresh.truthy()) {
            debug(state, "[xbl] Using existing XSTS token");
            return resolved(state, getProperty(xstsToken, "data"));
        }

        const auto password = getProperty(options, "password");
        if (password.truthy()) {
            debug(
                state,
                "[xbl] password is present, trying to authenticate using "
                "xboxreplay/xboxlive-auth"
            );
            if (state->replayAuth) {
                return state->replayAuth(
                    state->username,
                    password,
                    options
                );
            }
            return state->managers.xbox->doReplayAuth(
                state->username,
                password,
                options
            );
        }

        debug(state, "[xbl] Need to obtain tokens");
        auto method = std::make_shared<RetryMethod>([
            state,
            options,
            userToken,
            deviceToken,
            titleToken
        ] {
            return getMsaTokenState(state).then([
                state,
                options,
                userToken,
                deviceToken,
                titleToken
            ](const JsRuntimeValue& msaToken) {
                const auto flow = getProperty(options, "flow");
                const bool sisu = flow.isString() &&
                    flow.stringValue() == "sisu";
                if (sisu && (
                        !getProperty(userToken, "valid").truthy() ||
                        !getProperty(deviceToken, "valid").truthy() ||
                        !getProperty(titleToken, "valid").truthy()
                    )) {
                    debug(
                        state,
                        "[xbl] Sisu flow selected, trying to authenticate "
                        "with authTitle ID " + jsToString(getProperty(
                            options,
                            "authTitle"
                        ))
                    );
                    return state->managers.xbox->getDeviceToken(options).then([
                        state,
                        options,
                        msaToken
                    ](const JsRuntimeValue& device) {
                        return state->managers.xbox->doSisuAuth(
                            msaToken,
                            device,
                            options
                        );
                    });
                }

                return tokenOrUserRequest(
                    state,
                    userToken,
                    msaToken,
                    flow
                ).then([
                    state,
                    options,
                    deviceToken,
                    titleToken,
                    msaToken
                ](const JsRuntimeValue& user) {
                    return tokenOrDeviceRequest(
                        state,
                        deviceToken,
                        options
                    ).then([
                        state,
                        options,
                        titleToken,
                        msaToken,
                        user
                    ](const JsRuntimeValue& device) {
                        return tokenOrTitleRequest(
                            state,
                            titleToken,
                            msaToken,
                            device
                        ).then([
                            state,
                            options,
                            user,
                            device
                        ](const JsRuntimeValue& title) {
                            return state->managers.xbox->getXSTSToken(
                                JsRuntimeValue::object({
                                    {"userToken", user},
                                    {"deviceToken", device},
                                    {"titleToken", title}
                                }),
                                options
                            );
                        });
                    });
                });
            });
        });
        auto beforeRetry = std::make_shared<BeforeRetryMethod>([state] {
            forceMsaRefresh(state);
        });
        return retry(state, method, beforeRetry, 2);
    });
}

std::vector<std::uint8_t> decodeBase64(std::string value) {
    std::replace(value.begin(), value.end(), '-', '+');
    std::replace(value.begin(), value.end(), '_', '/');
    while (value.size() % 4 != 0) value.push_back('=');
    if (value.empty()) return {};
    std::vector<std::uint8_t> bytes((value.size() / 4) * 3 + 3);
    const int decoded = EVP_DecodeBlock(
        bytes.data(),
        reinterpret_cast<const unsigned char*>(value.data()),
        static_cast<int>(value.size())
    );
    if (decoded < 0) throw std::runtime_error("Invalid Bedrock JWT payload");
    std::size_t padding = 0;
    if (!value.empty() && value.back() == '=') ++padding;
    if (value.size() > 1 && value[value.size() - 2] == '=') ++padding;
    bytes.resize(static_cast<std::size_t>(decoded) - padding);
    return bytes;
}

JsRuntimeValue decodeJwtPayload(const JsRuntimeValue& jwtValue) {
    const auto jwt = jsToString(jwtValue);
    const auto first = jwt.find('.');
    const auto second = first == std::string::npos
        ? std::string::npos
        : jwt.find('.', first + 1);
    const std::string segment = first == std::string::npos
        ? std::string()
        : jwt.substr(
            first + 1,
            second == std::string::npos
                ? std::string::npos
                : second - first - 1
        );
    const auto decoded = decodeBase64(segment);
    return JsRuntimeJson::parse(std::string(
        reinterpret_cast<const char*>(decoded.data()),
        decoded.size()
    ));
}

ValuePromise getMinecraftBedrockTokenState(
    const std::shared_ptr<MicrosoftAuthFlowState>& state,
    JsRuntimeValue publicKey
) {
    if (!state->managers.bedrock) {
        return ValuePromise::rejected(
            state->microtaskQueue,
            "this.mba.verifyTokens is not a function"
        );
    }
    return state->managers.bedrock->verifyTokens().then([
        state,
        publicKey
    ](const JsRuntimeValue&) {
        // Source condition is `await verifyTokens() && false`; the cache is
        // intentionally never used because the ECDH key is not cached.
        if (!publicKey.truthy()) {
            throw std::runtime_error(
                "Need to specifiy a ECDH x509 URL encoded public key"
            );
        }
        debug(state, "[mc] Need to obtain tokens");
        auto method = std::make_shared<RetryMethod>([state, publicKey] {
            return getXboxTokenState(
                state,
                JsRuntimeValue::string(
                    MicrosoftAuthFlow::BedrockXstsRelyingParty
                ),
                JsRuntimeValue::boolean(false)
            ).then([state, publicKey](const JsRuntimeValue& xsts) {
                debug(state, "[xbl] xsts data", {xsts});
                return state->managers.bedrock->getAccessToken(
                    publicKey,
                    xsts
                );
            }).then([state](const JsRuntimeValue& token) {
                const auto chain = getProperty(token, "chain");
                const auto body = decodeJwtPayload(getIndex(chain, 1));
                const auto extraData = getProperty(body, "extraData");
                if (!getProperty(extraData, "titleId").truthy() &&
                    state->doTitleAuth.truthy()) {
                    throw std::runtime_error("missing titleId in response");
                }
                return chain;
            });
        });
        auto beforeRetry = std::make_shared<BeforeRetryMethod>([state] {
            state->managers.xbox->forceRefresh =
                JsRuntimeValue::boolean(true);
        });
        return retry(state, method, beforeRetry, 2);
    });
}

} // namespace

MicrosoftAuthFlow::MicrosoftAuthFlow(
    JsRuntimeValue usernameValue,
    JsRuntimeValue optionsValue,
    MicrosoftAuthFlowManagers managers,
    JsRuntimeValue doTitleAuthValue,
    MicrosoftAuthFlowDependencies dependencies
) : state_(std::make_shared<MicrosoftAuthFlowState>(
        std::move(usernameValue),
        std::move(optionsValue),
        std::move(managers),
        std::move(doTitleAuthValue)
    )),
    username(state_->username),
    options(state_->options),
    doTitleAuth(state_->doTitleAuth) {
    state_->microtaskQueue = dependencies.microtaskQueue
        ? std::move(dependencies.microtaskQueue)
        : selectQueue(state_->managers);
    state_->replayAuth = std::move(dependencies.replayAuth);
    state_->observers = std::move(dependencies.observers);
    if (dependencies.delay) {
        state_->delay = std::move(dependencies.delay);
    } else {
        const auto queue = state_->microtaskQueue;
        state_->delay = [queue](double milliseconds) {
            return defaultDelay(queue, milliseconds);
        };
    }
}

ValuePromise MicrosoftAuthFlow::getMsaToken() {
    const auto state = state_;
    return ValuePromise::fromSynchronous(
        state->microtaskQueue,
        [state] { return getMsaTokenState(state); }
    );
}

ValuePromise MicrosoftAuthFlow::getPlayfabLogin() {
    const auto state = state_;
    return ValuePromise::fromSynchronous(
        state->microtaskQueue,
        [state] {
            if (!state->managers.playfab) {
                return ValuePromise::rejected(
                    state->microtaskQueue,
                    "this.pfb.getCachedAccessToken is not a function"
                );
            }
            // The source omits await here. `cache` is a Promise, so
            // `cache.valid` is undefined and even a valid pfb cache never
            // short-circuits this method.
            (void) state->managers.playfab->getCachedAccessToken();
            return getXboxTokenState(
                state,
                JsRuntimeValue::string(MicrosoftAuthFlow::PlayfabRelyingParty),
                JsRuntimeValue::boolean(false)
            ).then([state](const JsRuntimeValue& xsts) {
                return state->managers.playfab->getAccessToken(xsts);
            });
        }
    );
}

ValuePromise MicrosoftAuthFlow::getMinecraftBedrockServicesToken(
    JsRuntimeValue serviceOptions
) {
    const auto state = state_;
    return ValuePromise::fromSynchronous(
        state->microtaskQueue,
        [state, serviceOptions = std::move(serviceOptions)]() mutable {
            if (!state->managers.bedrockServices) {
                return ValuePromise::rejected(
                    state->microtaskQueue,
                    "this.mcs.getCachedAccessToken is not a function"
                );
            }
            // Preserve the source typo: `({ verison })` and `{ verison }`.
            const auto verison = getProperty(serviceOptions, "verison");
            return state->managers.bedrockServices->
                getCachedAccessToken().then([state, verison](
                    const JsRuntimeValue& cached
                ) {
                    if (getProperty(cached, "valid").truthy()) {
                        return resolved(state, getProperty(cached, "data"));
                    }
                    return ValuePromise::fromSynchronous(
                        state->microtaskQueue,
                        [state] {
                            // Same behavior as this.getPlayfabLogin(), but use
                            // state directly to avoid retaining the C++ facade.
                            (void) state->managers.playfab->
                                getCachedAccessToken();
                            return getXboxTokenState(
                                state,
                                JsRuntimeValue::string(
                                    MicrosoftAuthFlow::PlayfabRelyingParty
                                ),
                                JsRuntimeValue::boolean(false)
                            ).then([state](const JsRuntimeValue& xsts) {
                                return state->managers.playfab->
                                    getAccessToken(xsts);
                            });
                        }
                    ).then([state, verison](const JsRuntimeValue& playfab) {
                        return state->managers.bedrockServices->getAccessToken(
                            getProperty(playfab, "SessionTicket"),
                            JsRuntimeValue::object({
                                {"verison", verison}
                            })
                        );
                    });
                });
        }
    );
}

ValuePromise MicrosoftAuthFlow::getXboxToken(
    JsRuntimeValue relyingParty,
    JsRuntimeValue forceRefresh
) {
    const auto state = state_;
    return ValuePromise::fromSynchronous(
        state->microtaskQueue,
        [state,
         relyingParty = std::move(relyingParty),
         forceRefresh = std::move(forceRefresh)]() mutable {
            return getXboxTokenState(
                state,
                std::move(relyingParty),
                std::move(forceRefresh)
            );
        }
    );
}

ValuePromise MicrosoftAuthFlow::getMinecraftBedrockToken(
    JsRuntimeValue publicKey
) {
    const auto state = state_;
    return ValuePromise::fromSynchronous(
        state->microtaskQueue,
        [state, publicKey = std::move(publicKey)]() mutable {
            return getMinecraftBedrockTokenState(
                state,
                std::move(publicKey)
            );
        }
    );
}

std::shared_ptr<JsMicrotaskQueue>
MicrosoftAuthFlow::microtaskQueue() const noexcept {
    return state_->microtaskQueue;
}

} // namespace bedrock
