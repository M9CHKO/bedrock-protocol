#include <bedrock/auth/MsalError.hpp>

#include <utility>

namespace bedrock {

std::string MsalAuthError::makeWhat(
    const std::string& errorCode,
    const std::string& errorMessage
) {
    return errorMessage.empty()
        ? errorCode
        : errorCode + ": " + errorMessage;
}

MsalAuthError::MsalAuthError(
    std::string errorCode,
    std::string errorMessage,
    std::string subError,
    std::string name
) : std::runtime_error(makeWhat(errorCode, errorMessage)),
    errorCode_(std::move(errorCode)),
    errorMessage_(std::move(errorMessage)),
    subError_(std::move(subError)),
    name_(std::move(name)) {
    // AuthError assigns these fields in this exact order. A subclass later
    // overwrites name without changing its insertion position.
    setEnumerable("errorCode", JsRuntimeValue::string(errorCode_));
    setEnumerable("errorMessage", JsRuntimeValue::string(errorMessage_));
    setEnumerable("subError", JsRuntimeValue::string(subError_));
    setEnumerable("name", JsRuntimeValue::string(name_));
}

void MsalAuthError::setCorrelationId(std::string correlationId) {
    correlationId_ = std::move(correlationId);
    setEnumerable(
        "correlationId",
        JsRuntimeValue::string(correlationId_)
    );
}

void MsalAuthError::setCorrelationId(JsRuntimeValue correlationId) {
    if (correlationId.isString()) {
        correlationId_ = correlationId.stringValue();
    } else if (correlationId.isUndefined()) {
        correlationId_ = "undefined";
    } else if (correlationId.isNull()) {
        correlationId_ = "null";
    } else if (correlationId.isBool()) {
        correlationId_ = correlationId.boolValue() ? "true" : "false";
    } else {
        correlationId_ = JsRuntimeJson::stringify(correlationId).value_or(
            "undefined"
        );
    }
    // AuthError.setCorrelationId assigns the raw JavaScript value. In
    // particular an undefined callback mutation creates an own property which
    // JSON.stringify subsequently omits.
    setEnumerable("correlationId", std::move(correlationId));
}

std::optional<std::string> MsalAuthError::jsonStringify() const {
    return JsRuntimeJson::stringify(enumerableProperties_);
}

void MsalAuthError::setEnumerable(
    std::string key,
    JsRuntimeValue value
) {
    enumerableProperties_.set(std::move(key), std::move(value));
}

MsalClientAuthError::MsalClientAuthError(
    std::string errorCode,
    std::string errorMessage
) : MsalAuthError(
        std::move(errorCode),
        std::move(errorMessage),
        {},
        "ClientAuthError"
    ) {}

MsalServerError::MsalServerError(
    std::string errorCode,
    std::string errorMessage,
    std::string subError,
    JsRuntimeValue errorNo,
    JsRuntimeValue status
) : MsalAuthError(
        std::move(errorCode),
        std::move(errorMessage),
        std::move(subError),
        "ServerError"
    ) {
    // ServerError assigns both properties even when the values are undefined;
    // JSON.stringify subsequently omits those undefined values.
    setEnumerable("errorNo", std::move(errorNo));
    setEnumerable("status", std::move(status));
}

std::optional<std::string> stringifyMsalException(
    std::exception_ptr error
) noexcept {
    if (!error) return std::string("{}");
    try {
        std::rethrow_exception(error);
    } catch (const MsalAuthError& msalError) {
        try {
            return msalError.jsonStringify();
        } catch (...) {
            return std::nullopt;
        }
    } catch (...) {
        // Native Error instances have no enumerable own JSON properties.
        return std::string("{}");
    }
}

} // namespace bedrock
