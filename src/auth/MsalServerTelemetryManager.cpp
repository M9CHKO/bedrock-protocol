#include <bedrock/auth/MsalServerTelemetryManager.hpp>

#include <bedrock/auth/MsalError.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace bedrock {
namespace {

constexpr std::string_view kCacheKeyPrefix = "server-telemetry";
constexpr std::string_view kUnknownError = "unknown_error";

std::vector<std::uint16_t> utf16CodeUnits(std::string_view input) {
    std::vector<std::uint16_t> result;
    for (std::size_t offset = 0; offset < input.size();) {
        const auto first = static_cast<std::uint8_t>(input[offset]);
        std::uint32_t codePoint = first;
        std::size_t width = 1;
        const auto continuation = [&](std::size_t index) {
            return index < input.size() &&
                (static_cast<std::uint8_t>(input[index]) & 0xc0U) == 0x80U;
        };

        if (first >= 0xc2U && first <= 0xdfU && continuation(offset + 1)) {
            codePoint = ((first & 0x1fU) << 6) |
                (static_cast<std::uint8_t>(input[offset + 1]) & 0x3fU);
            width = 2;
        } else if (first >= 0xe0U && first <= 0xefU &&
            continuation(offset + 1) && continuation(offset + 2)) {
            codePoint = ((first & 0x0fU) << 12) |
                ((static_cast<std::uint8_t>(input[offset + 1]) & 0x3fU)
                    << 6) |
                (static_cast<std::uint8_t>(input[offset + 2]) & 0x3fU);
            width = 3;
        } else if (first >= 0xf0U && first <= 0xf4U &&
            continuation(offset + 1) && continuation(offset + 2) &&
            continuation(offset + 3)) {
            codePoint = ((first & 0x07U) << 18) |
                ((static_cast<std::uint8_t>(input[offset + 1]) & 0x3fU)
                    << 12) |
                ((static_cast<std::uint8_t>(input[offset + 2]) & 0x3fU)
                    << 6) |
                (static_cast<std::uint8_t>(input[offset + 3]) & 0x3fU);
            width = 4;
        }

        if (codePoint <= 0xffffU) {
            result.push_back(static_cast<std::uint16_t>(codePoint));
        } else {
            codePoint -= 0x10000U;
            result.push_back(static_cast<std::uint16_t>(
                0xd800U + (codePoint >> 10)
            ));
            result.push_back(static_cast<std::uint16_t>(
                0xdc00U + (codePoint & 0x3ffU)
            ));
        }
        offset += width;
    }
    return result;
}

std::string wtf8CodeUnit(std::uint16_t unit) {
    std::string result;
    if (unit <= 0x7fU) {
        result.push_back(static_cast<char>(unit));
    } else if (unit <= 0x7ffU) {
        result.push_back(static_cast<char>(0xc0U | (unit >> 6)));
        result.push_back(static_cast<char>(0x80U | (unit & 0x3fU)));
    } else {
        result.push_back(static_cast<char>(0xe0U | (unit >> 12)));
        result.push_back(static_cast<char>(0x80U | ((unit >> 6) & 0x3fU)));
        result.push_back(static_cast<char>(0x80U | (unit & 0x3fU)));
    }
    return result;
}

std::string jsToString(const JsRuntimeValue& value);

std::string jsArrayJoin(
    const JsRuntimeValue& array,
    std::string_view separator,
    std::size_t begin = 0,
    std::optional<std::size_t> end = std::nullopt
) {
    if (!array.isArray()) {
        throw std::runtime_error("array.join is not a function");
    }
    const auto length = array.length();
    const auto stop = std::min(end.value_or(length), length);
    std::string result;
    for (std::size_t index = begin; index < stop; ++index) {
        if (index != begin) result.append(separator);
        const auto* item = array.get(index);
        if (!item || item->isUndefined() || item->isNull()) continue;
        result += jsToString(*item);
    }
    return result;
}

std::string jsToString(const JsRuntimeValue& value) {
    if (value.isUndefined()) return "undefined";
    if (value.isNull()) return "null";
    if (value.isBool()) return value.boolValue() ? "true" : "false";
    if (value.isString()) return value.stringValue();
    if (value.isNumber()) {
        const auto number = value.numberValue();
        if (std::isnan(number)) return "NaN";
        if (std::isinf(number)) return number < 0 ? "-Infinity" : "Infinity";
        return JsRuntimeJson::stringify(value).value_or("undefined");
    }
    if (value.isArray()) return jsArrayJoin(value, ",");
    if (value.isMap()) return "[object Map]";
    if (value.isFunction()) {
        const auto* name = value.get("name");
        return "function " +
            (name && name->isString() ? name->stringValue() : std::string()) +
            "() { [native code] }";
    }
    return "[object Object]";
}

const JsRuntimeValue& requireProperty(
    const JsRuntimeValue& object,
    std::string_view property
) {
    if (object.isNull() || object.isUndefined()) {
        throw std::runtime_error(
            std::string("Cannot read properties of ") +
            (object.isNull() ? "null" : "undefined") + " (reading '" +
            std::string(property) + "')"
        );
    }
    const auto* value = object.get(property);
    if (!value) {
        static const JsRuntimeValue undefined = JsRuntimeValue::undefined();
        return undefined;
    }
    return *value;
}

JsRuntimeValue& requireMutableProperty(
    JsRuntimeValue& object,
    std::string_view property
) {
    if (!object.isObject() && !object.isArray()) {
        throw std::runtime_error(
            "Cannot set properties of primitive telemetry entity"
        );
    }
    auto* value = object.get(property);
    if (!value) {
        object.set(std::string(property), JsRuntimeValue::undefined());
        value = object.get(property);
    }
    return *value;
}

JsRuntimeValue requireArrayProperty(
    const JsRuntimeValue& entity,
    std::string_view property
) {
    const auto& value = requireProperty(entity, property);
    if (!value.isArray()) {
        throw std::runtime_error(
            std::string("lastRequests.") + std::string(property) +
            ".slice is not a function"
        );
    }
    return value;
}

double jsLength(const JsRuntimeValue& value) {
    if (value.isString()) {
        return static_cast<double>(utf16CodeUnits(value.stringValue()).size());
    }
    if (value.isArray()) return static_cast<double>(value.length());
    if (const auto* length = value.get("length");
        length && length->isNumber()) {
        return length->numberValue();
    }
    return std::numeric_limits<double>::quiet_NaN();
}

double requiredJsLength(const JsRuntimeValue& value) {
    if (value.isNull() || value.isUndefined()) {
        throw std::runtime_error(
            std::string("Cannot read properties of ") +
            (value.isNull() ? "null" : "undefined") +
            " (reading 'length')"
        );
    }
    return jsLength(value);
}

JsRuntimeValue indexedValue(const JsRuntimeValue& value, std::size_t index) {
    if (value.isArray()) {
        const auto* item = value.get(index);
        return item ? *item : JsRuntimeValue::undefined();
    }
    if (value.isString()) {
        const auto units = utf16CodeUnits(value.stringValue());
        return index < units.size()
            ? JsRuntimeValue::string(wtf8CodeUnit(units[index]))
            : JsRuntimeValue::undefined();
    }
    const auto* item = value.get(std::to_string(index));
    if (item) return *item;
    return JsRuntimeValue::undefined();
}

JsRuntimeValue truthyOrEmpty(const JsRuntimeValue& value) {
    return value.truthy() ? value : JsRuntimeValue::string("");
}

JsRuntimeValue slicedArray(const JsRuntimeValue& array, std::size_t begin) {
    if (!array.isArray()) {
        throw std::runtime_error("array.slice is not a function");
    }
    auto result = JsRuntimeValue::array();
    if (begin >= array.length()) return result;
    result.set("length", JsRuntimeValue::number(
        static_cast<double>(array.length() - begin)
    ));
    for (std::size_t index = begin; index < array.length(); ++index) {
        const auto* item = array.get(index);
        if (item) result.set(index - begin, *item);
    }
    return result;
}

JsRuntimeValue shiftedArrayValue(JsRuntimeValue& array) {
    if (!array.isArray()) {
        throw std::runtime_error("array.shift is not a function");
    }
    const auto length = array.length();
    if (length == 0) return JsRuntimeValue::undefined();

    const auto* first = array.get(0);
    auto result = first ? *first : JsRuntimeValue::undefined();
    for (std::size_t index = 1; index < length; ++index) {
        const auto* item = array.get(index);
        if (item) {
            auto moved = *item;
            array.set(index - 1, std::move(moved));
        } else {
            array.arrayNode()->erase(index - 1);
        }
    }
    array.set("length", JsRuntimeValue::number(
        static_cast<double>(length - 1)
    ));
    return result;
}

JsRuntimeValue jsAddOne(const JsRuntimeValue& value) {
    if (value.isString() || value.isArray() || value.isObject() ||
        value.isDate() || value.isMap() || value.isFunction() ||
        value.isOpaque()) {
        return JsRuntimeValue::string(jsToString(value) + "1");
    }
    if (value.isUndefined()) {
        return JsRuntimeValue::number(
            std::numeric_limits<double>::quiet_NaN()
        );
    }
    if (value.isNull()) return JsRuntimeValue::number(1);
    if (value.isBool()) {
        return JsRuntimeValue::number(value.boolValue() ? 2 : 1);
    }
    return JsRuntimeValue::number(value.numberValue() + 1);
}

std::vector<std::string> splitComma(const std::string& value) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (true) {
        const auto separator = value.find(',', begin);
        if (separator == std::string::npos) {
            result.push_back(value.substr(begin));
            return result;
        }
        result.push_back(value.substr(begin, separator - begin));
        begin = separator + 1;
    }
}

std::string joinComma(const std::vector<std::string>& values) {
    std::string result;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index) result.push_back(',');
        result += values[index];
    }
    return result;
}

MsalServerTelemetryRuntimeRequest runtimeRequest(
    MsalServerTelemetryRequest request
) {
    return MsalServerTelemetryRuntimeRequest {
        .clientId = JsRuntimeValue::string(std::move(request.clientId)),
        .apiId = JsRuntimeValue::number(request.apiId),
        .correlationId = JsRuntimeValue::string(
            std::move(request.correlationId)
        ),
        .wrapperSKU = JsRuntimeValue::string(std::move(request.wrapperSKU)),
        .wrapperVer = JsRuntimeValue::string(std::move(request.wrapperVer))
    };
}

} // namespace

JsRuntimeValue MsalServerTelemetryMemoryCache::getServerTelemetry(
    std::string_view key
) {
    const auto found = std::find_if(
        entries_.begin(),
        entries_.end(),
        [key](const auto& entry) { return entry.key == key; }
    );
    if (found == entries_.end() || !found->value.truthy() ||
        !key.starts_with(kCacheKeyPrefix)) {
        return JsRuntimeValue::null();
    }
    if (!found->value.isObject() && !found->value.isArray()) {
        return JsRuntimeValue::null();
    }
    if (found->value.hasOwn("hasOwnProperty")) {
        throw std::runtime_error(
            "entity.hasOwnProperty is not a function"
        );
    }
    if (!found->value.hasOwn("failedRequests") ||
        !found->value.hasOwn("errors") ||
        !found->value.hasOwn("cacheHits")) {
        return JsRuntimeValue::null();
    }
    return found->value;
}

void MsalServerTelemetryMemoryCache::setServerTelemetry(
    std::string key,
    JsRuntimeValue entity,
    std::string correlationId
) {
    (void) correlationId;
    setRawItem(std::move(key), std::move(entity));
}

bool MsalServerTelemetryMemoryCache::removeItem(
    std::string_view key,
    std::string_view correlationId
) {
    (void) correlationId;
    const auto found = std::find_if(
        entries_.begin(),
        entries_.end(),
        [key](const auto& entry) { return entry.key == key; }
    );
    if (found == entries_.end() || !found->value.truthy()) return false;
    entries_.erase(found);
    return true;
}

void MsalServerTelemetryMemoryCache::setRawItem(
    std::string key,
    JsRuntimeValue value
) {
    const auto found = std::find_if(
        entries_.begin(),
        entries_.end(),
        [&key](const auto& entry) { return entry.key == key; }
    );
    if (found == entries_.end()) {
        entries_.push_back(Entry {
            .key = std::move(key),
            .value = std::move(value)
        });
    } else {
        found->value = std::move(value);
    }
}

JsRuntimeValue MsalServerTelemetryMemoryCache::rawItem(
    std::string_view key
) const {
    const auto found = std::find_if(
        entries_.begin(),
        entries_.end(),
        [key](const auto& entry) { return entry.key == key; }
    );
    return found == entries_.end()
        ? JsRuntimeValue::undefined()
        : found->value;
}

MsalServerTelemetryError MsalServerTelemetryError::unknown() {
    return {};
}

MsalServerTelemetryError MsalServerTelemetryError::error(
    std::string message,
    std::string name
) {
    MsalServerTelemetryError result;
    result.kind = Kind::Error;
    result.name = std::move(name);
    result.message = std::move(message);
    return result;
}

MsalServerTelemetryError MsalServerTelemetryError::authError(
    std::string errorCode,
    std::string errorMessage,
    std::string subError,
    std::string name
) {
    MsalServerTelemetryError result;
    result.kind = Kind::AuthError;
    result.name = std::move(name);
    result.errorCode = std::move(errorCode);
    result.message = std::move(errorMessage);
    result.subError = std::move(subError);
    return result;
}

std::string MsalServerTelemetryError::toString() const {
    if (toStringOverride) return *toStringOverride;
    if (kind == Kind::Unknown) return {};

    std::string renderedName = name;
    std::string renderedMessage = message;
    if (kind == Kind::AuthError) {
        renderedMessage = message.empty()
            ? errorCode
            : errorCode + ": " + message;
    }
    if (renderedName.empty()) return renderedMessage;
    if (renderedMessage.empty()) return renderedName;
    return renderedName + ": " + renderedMessage;
}

MsalServerTelemetryManager::MsalServerTelemetryManager(
    MsalServerTelemetryRequest request,
    MsalServerTelemetryCachePtr cache
) : MsalServerTelemetryManager(
        runtimeRequest(std::move(request)),
        std::move(cache)
    ) {}

MsalServerTelemetryManager::MsalServerTelemetryManager(
    MsalServerTelemetryRuntimeRequest request,
    MsalServerTelemetryCachePtr cache
) : cache_(std::move(cache)),
    apiId_(std::move(request.apiId)),
    correlationId_(std::move(request.correlationId)),
    wrapperSKU_(truthyOrEmpty(request.wrapperSKU)),
    wrapperVer_(truthyOrEmpty(request.wrapperVer)) {
    if (!cache_) cache_ = std::make_shared<MsalServerTelemetryMemoryCache>();
    telemetryCacheKey_ = std::string(kCacheKeyPrefix) + "-" +
        jsToString(request.clientId);
}

std::string
MsalServerTelemetryManager::generateCurrentRequestHeaderValue() {
    const std::string request = jsToString(apiId_) + "," +
        jsToString(cacheOutcome_);

    std::vector<JsRuntimeValue> platformFields {
        wrapperSKU_,
        wrapperVer_
    };
    const auto nativeBrokerErrorCode = getNativeBrokerErrorCodeValue();
    if (std::isfinite(jsLength(nativeBrokerErrorCode)) &&
        jsLength(nativeBrokerErrorCode) > 0) {
        platformFields.push_back(JsRuntimeValue::string(
            "broker_error=" + jsToString(nativeBrokerErrorCode)
        ));
    }
    std::string platform;
    for (std::size_t index = 0; index < platformFields.size(); ++index) {
        if (index) platform.push_back(',');
        platform += jsToString(platformFields[index]);
    }

    return "5|" + request + "," + getRegionDiscoveryFields() + "|" +
        platform;
}

std::string MsalServerTelemetryManager::generateLastRequestHeaderValue() {
    const auto lastRequests = getLastRequests();
    const auto maxErrors = maxErrorsToSend(lastRequests);
    const auto failedRequests = requireArrayProperty(
        lastRequests,
        "failedRequests"
    );
    const auto errors = requireArrayProperty(lastRequests, "errors");
    const auto errorCount = errors.length();

    const std::string failed = jsArrayJoin(
        failedRequests,
        ",",
        0,
        2 * maxErrors
    );
    const std::string errorValues = jsArrayJoin(
        errors,
        ",",
        0,
        maxErrors
    );
    const std::string overflow = maxErrors < errorCount ? "1" : "0";
    const auto& cacheHits = requireProperty(lastRequests, "cacheHits");

    return "5|" + jsToString(cacheHits) + "|" + failed + "|" +
        errorValues + "|" + std::to_string(errorCount) + "," + overflow;
}

void MsalServerTelemetryManager::cacheFailedRequest(
    const MsalServerTelemetryError& error
) {
    auto lastRequests = getLastRequests();
    auto failedRequests = requireArrayProperty(lastRequests, "failedRequests");
    auto errors = requireArrayProperty(lastRequests, "errors");

    if (errors.length() >= kMaxCachedErrors) {
        shiftedArrayValue(failedRequests);
        shiftedArrayValue(failedRequests);
        shiftedArrayValue(errors);
    }

    failedRequests.push(apiId_);
    failedRequests.push(correlationId_);

    const auto errorString = error.toString();
    if (error.kind != MsalServerTelemetryError::Kind::Unknown &&
        !errorString.empty()) {
        if (error.kind == MsalServerTelemetryError::Kind::AuthError) {
            if (!error.subError.empty()) {
                errors.push(JsRuntimeValue::string(error.subError));
            } else if (!error.errorCode.empty()) {
                errors.push(JsRuntimeValue::string(error.errorCode));
            } else {
                errors.push(JsRuntimeValue::string(errorString));
            }
        } else {
            errors.push(JsRuntimeValue::string(errorString));
        }
    } else {
        errors.push(JsRuntimeValue::string(kUnknownError));
    }

    writeLastRequests(lastRequests);
}

void MsalServerTelemetryManager::cacheFailedRequest(
    std::exception_ptr error
) {
    if (!error) {
        cacheUnknownFailure();
        return;
    }
    try {
        std::rethrow_exception(error);
    } catch (const MsalAuthError& caught) {
        cacheFailedRequest(MsalServerTelemetryError::authError(
            caught.errorCode(),
            caught.errorMessage(),
            caught.subError(),
            caught.jsName()
        ));
    } catch (const std::exception& caught) {
        cacheFailedRequest(MsalServerTelemetryError::error(caught.what()));
    } catch (...) {
        cacheUnknownFailure();
    }
}

void MsalServerTelemetryManager::cacheUnknownFailure() {
    cacheFailedRequest(MsalServerTelemetryError::unknown());
}

JsRuntimeValue MsalServerTelemetryManager::incrementCacheHits() {
    auto lastRequests = getLastRequests();
    auto& cacheHits = requireMutableProperty(lastRequests, "cacheHits");
    cacheHits = jsAddOne(cacheHits);
    const auto result = cacheHits;
    writeLastRequests(lastRequests);
    return result;
}

JsRuntimeValue MsalServerTelemetryManager::getLastRequests() {
    auto result = cache_->getServerTelemetry(telemetryCacheKey_);
    if (result.truthy()) return result;
    return JsRuntimeValue::object({
        {"failedRequests", JsRuntimeValue::array()},
        {"errors", JsRuntimeValue::array()},
        {"cacheHits", JsRuntimeValue::number(0)}
    });
}

void MsalServerTelemetryManager::clearTelemetryCache() {
    const auto lastRequests = getLastRequests();
    const auto numErrorsFlushed = maxErrorsToSend(lastRequests);
    const auto errors = requireArrayProperty(lastRequests, "errors");
    const auto errorCount = errors.length();
    if (numErrorsFlushed == errorCount) {
        cache_->removeItem(
            telemetryCacheKey_,
            jsToString(correlationId_)
        );
        return;
    }

    const auto failedRequests = requireArrayProperty(
        lastRequests,
        "failedRequests"
    );
    auto remaining = JsRuntimeValue::object({
        {
            "failedRequests",
            slicedArray(failedRequests, numErrorsFlushed * 2)
        },
        {"errors", slicedArray(errors, numErrorsFlushed)},
        {"cacheHits", JsRuntimeValue::number(0)}
    });
    writeLastRequests(std::move(remaining));
}

std::size_t MsalServerTelemetryManager::maxErrorsToSend(
    const JsRuntimeValue& serverTelemetryEntity
) {
    const auto& errors = requireProperty(serverTelemetryEntity, "errors");
    const auto errorLength = requiredJsLength(errors);
    if (!std::isfinite(errorLength) || errorLength <= 0) return 0;
    const auto& failedRequests = requireProperty(
        serverTelemetryEntity,
        "failedRequests"
    );

    std::size_t maxErrors = 0;
    double dataSize = 0;
    const auto count = static_cast<std::size_t>(errorLength);
    for (std::size_t index = 0; index < count; ++index) {
        const auto apiId = truthyOrEmpty(indexedValue(
            failedRequests,
            2 * index
        ));
        const auto correlationId = truthyOrEmpty(indexedValue(
            failedRequests,
            2 * index + 1
        ));
        const auto errorCode = truthyOrEmpty(indexedValue(errors, index));
        const auto errorCodeLength = jsLength(errorCode);
        if (!std::isfinite(errorCodeLength)) break;

        dataSize += static_cast<double>(
            utf16CodeUnits(jsToString(apiId)).size() +
            utf16CodeUnits(jsToString(correlationId)).size()
        ) + errorCodeLength + 3;
        if (dataSize < static_cast<double>(kMaxLastHeaderBytes)) {
            ++maxErrors;
        } else {
            break;
        }
    }
    return maxErrors;
}

std::string MsalServerTelemetryManager::getRegionDiscoveryFields() const {
    return jsToString(truthyOrEmpty(regionUsed_)) + "," +
        jsToString(truthyOrEmpty(regionSource_)) + "," +
        jsToString(truthyOrEmpty(regionOutcome_));
}

void MsalServerTelemetryManager::updateRegionDiscoveryMetadata(
    MsalRegionDiscoveryMetadata metadata
) {
    regionUsed_ = std::move(metadata.regionUsed);
    regionSource_ = std::move(metadata.regionSource);
    regionOutcome_ = std::move(metadata.regionOutcome);
}

void MsalServerTelemetryManager::setCacheOutcome(
    MsalServerTelemetryCacheOutcome outcome
) {
    switch (outcome) {
        case MsalServerTelemetryCacheOutcome::NotApplicable:
            setCacheOutcome("0");
            return;
        case MsalServerTelemetryCacheOutcome::ForceRefreshOrClaims:
            setCacheOutcome("1");
            return;
        case MsalServerTelemetryCacheOutcome::NoCachedAccessToken:
            setCacheOutcome("2");
            return;
        case MsalServerTelemetryCacheOutcome::CachedAccessTokenExpired:
            setCacheOutcome("3");
            return;
        case MsalServerTelemetryCacheOutcome::ProactivelyRefreshed:
            setCacheOutcome("4");
            return;
    }
}

void MsalServerTelemetryManager::setCacheOutcome(std::string value) {
    setCacheOutcome(JsRuntimeValue::string(std::move(value)));
}

void MsalServerTelemetryManager::setCacheOutcome(JsRuntimeValue value) {
    cacheOutcome_ = std::move(value);
}

void MsalServerTelemetryManager::setNativeBrokerErrorCode(
    std::string errorCode
) {
    auto lastRequests = getLastRequests();
    lastRequests.set(
        "nativeBrokerErrorCode",
        JsRuntimeValue::string(std::move(errorCode))
    );
    writeLastRequests(std::move(lastRequests));
}

JsRuntimeValue
MsalServerTelemetryManager::getNativeBrokerErrorCodeValue() {
    const auto lastRequests = getLastRequests();
    const auto* value = lastRequests.get("nativeBrokerErrorCode");
    return value ? *value : JsRuntimeValue::undefined();
}

std::optional<std::string>
MsalServerTelemetryManager::getNativeBrokerErrorCode() {
    const auto value = getNativeBrokerErrorCodeValue();
    return value.isString()
        ? std::optional<std::string>(value.stringValue())
        : std::nullopt;
}

void MsalServerTelemetryManager::clearNativeBrokerErrorCode() {
    auto lastRequests = getLastRequests();
    if (lastRequests.isObject()) {
        lastRequests.objectNode()->erase("nativeBrokerErrorCode");
    } else if (lastRequests.isArray()) {
        lastRequests.arrayNode()->erase("nativeBrokerErrorCode");
    }
    writeLastRequests(std::move(lastRequests));
}

std::string MsalServerTelemetryManager::makeExtraSkuString(
    const MsalExtraSkuParams& params
) {
    std::vector<std::string> skuArray;
    if (params.skus && !params.skus->empty()) {
        skuArray = splitComma(*params.skus);
        if (skuArray.size() < 4) return *params.skus;
    } else {
        skuArray.assign(4, "|");
    }

    const auto setSku = [&](std::size_t index,
                            const std::optional<std::string>& name,
                            const std::optional<std::string>& version) {
        if (index >= skuArray.size() || !name || name->empty() ||
            !version || version->empty()) {
            return;
        }
        skuArray[index] = *name + "|" + *version;
    };
    setSku(0, params.libraryName, params.libraryVersion);
    setSku(2, params.extensionName, params.extensionVersion);
    return joinComma(skuArray);
}

void MsalServerTelemetryManager::writeLastRequests(JsRuntimeValue entity) {
    cache_->setServerTelemetry(
        telemetryCacheKey_,
        std::move(entity),
        jsToString(correlationId_)
    );
}

} // namespace bedrock
