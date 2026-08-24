#pragma once

#include <bedrock/auth/JsRuntimeValue.hpp>

#include <exception>
#include <optional>
#include <stdexcept>
#include <string>

namespace bedrock {

// Native representation of @azure/msal-common AuthError. The ordinary C++
// what() string matches Error.message, while enumerableProperties() retains
// the own enumerable fields observed by JSON.stringify(error).
class MsalAuthError : public std::runtime_error {
public:
    MsalAuthError(
        std::string errorCode,
        std::string errorMessage = {},
        std::string subError = {},
        std::string name = "AuthError"
    );

    const std::string& errorCode() const noexcept { return errorCode_; }
    const std::string& errorMessage() const noexcept { return errorMessage_; }
    const std::string& subError() const noexcept { return subError_; }
    const std::string& jsName() const noexcept { return name_; }
    const std::string& correlationId() const noexcept {
        return correlationId_;
    }

    void setCorrelationId(std::string correlationId);
    void setCorrelationId(JsRuntimeValue correlationId);

    const JsRuntimeValue& enumerableProperties() const noexcept {
        return enumerableProperties_;
    }
    std::optional<std::string> jsonStringify() const;

protected:
    void setEnumerable(std::string key, JsRuntimeValue value);

private:
    static std::string makeWhat(
        const std::string& errorCode,
        const std::string& errorMessage
    );

    std::string errorCode_;
    std::string errorMessage_;
    std::string subError_;
    std::string name_;
    std::string correlationId_;
    JsRuntimeValue enumerableProperties_ = JsRuntimeValue::object();
};

class MsalClientAuthError final : public MsalAuthError {
public:
    MsalClientAuthError(
        std::string errorCode,
        std::string errorMessage
    );
};

class MsalServerError final : public MsalAuthError {
public:
    MsalServerError(
        std::string errorCode,
        std::string errorMessage,
        std::string subError = {},
        JsRuntimeValue errorNo = JsRuntimeValue::undefined(),
        JsRuntimeValue status = JsRuntimeValue::undefined()
    );
};

// JSON.stringify(error) counterpart used at the prismarine-auth catch/debug
// boundary. Plain Error-like native exceptions have no enumerable fields.
std::optional<std::string> stringifyMsalException(
    std::exception_ptr error
) noexcept;

} // namespace bedrock
