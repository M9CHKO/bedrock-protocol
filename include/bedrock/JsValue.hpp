#pragma once

#include <cstddef>
#include <cmath>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace bedrock {

// JavaScript object spread distinguishes a missing own-property from an own
// property whose value is `undefined` or `null`. std::optional collapses all
// three states, so public option values that participate in JS presence or
// truthiness checks use this representation instead.
enum class JsPropertyPresence {
    Missing,
    Undefined,
    Null,
    Value
};

struct JsUndefined final {};
inline constexpr JsUndefined jsUndefined {};

template <typename T>
class JsProperty {
public:
    JsProperty() = default;
    JsProperty(const JsProperty&) = default;
    JsProperty(JsProperty&&) noexcept = default;
    JsProperty& operator=(const JsProperty&) = default;
    JsProperty& operator=(JsProperty&&) noexcept = default;

    JsProperty(T value)
        : value_(std::move(value)), presence_(JsPropertyPresence::Value),
          provided_(true) {}

    template <typename U = T>
        requires std::is_same_v<U, std::string>
    JsProperty(const char* value)
        : value_(value ? value : ""),
          presence_(JsPropertyPresence::Value), provided_(true) {}

    JsProperty(std::nullptr_t)
        : presence_(JsPropertyPresence::Null), provided_(true) {}

    JsProperty(JsUndefined)
        : presence_(JsPropertyPresence::Undefined), provided_(true) {}

    JsProperty(std::optional<T> value) {
        if (value.has_value()) {
            value_ = std::move(*value);
            presence_ = JsPropertyPresence::Value;
            provided_ = true;
        }
    }

    JsProperty& operator=(T value) {
        value_ = std::move(value);
        presence_ = JsPropertyPresence::Value;
        provided_ = true;
        return *this;
    }

    template <typename U = T>
        requires std::is_same_v<U, std::string>
    JsProperty& operator=(const char* value) {
        value_ = value ? value : "";
        presence_ = JsPropertyPresence::Value;
        provided_ = true;
        return *this;
    }

    JsProperty& operator=(std::nullptr_t) noexcept {
        presence_ = JsPropertyPresence::Null;
        provided_ = true;
        return *this;
    }

    JsProperty& operator=(JsUndefined) noexcept {
        presence_ = JsPropertyPresence::Undefined;
        provided_ = true;
        return *this;
    }

    JsProperty& operator=(std::optional<T> value) {
        if (value.has_value()) {
            return *this = std::move(*value);
        }
        reset();
        return *this;
    }

    bool has_value() const noexcept {
        return presence_ == JsPropertyPresence::Value;
    }

    bool hasOwn() const noexcept {
        return presence_ != JsPropertyPresence::Missing;
    }

    bool provided() const noexcept {
        return provided_;
    }

    bool isUndefined() const noexcept {
        return presence_ == JsPropertyPresence::Undefined;
    }

    bool isNull() const noexcept {
        return presence_ == JsPropertyPresence::Null;
    }

    JsPropertyPresence presence() const noexcept {
        return presence_;
    }

    bool truthy() const noexcept {
        if (!has_value()) return false;
        if constexpr (std::is_same_v<T, bool>) {
            return value_;
        } else if constexpr (std::is_floating_point_v<T>) {
            return value_ != T{} && !std::isnan(value_);
        } else if constexpr (std::is_arithmetic_v<T>) {
            return value_ != T{};
        } else {
            return !value_.empty();
        }
    }

    T& value() {
        if (!has_value()) throw std::bad_optional_access();
        return value_;
    }

    const T& value() const {
        if (!has_value()) throw std::bad_optional_access();
        return value_;
    }

    T& operator*() { return value(); }
    const T& operator*() const { return value(); }

    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

    T value_or(T fallback) const {
        return has_value() ? value_ : std::move(fallback);
    }

    void reset() noexcept {
        presence_ = JsPropertyPresence::Missing;
        provided_ = false;
    }

    void setUndefined() noexcept {
        presence_ = JsPropertyPresence::Undefined;
        provided_ = true;
    }

    void setResolved(T value) {
        value_ = std::move(value);
        presence_ = JsPropertyPresence::Value;
    }

    std::optional<T> optionalValue() const {
        return has_value() ? std::optional<T>(value_) : std::nullopt;
    }

    friend bool operator==(
        const JsProperty& left,
        const std::optional<T>& right
    ) {
        return left.optionalValue() == right;
    }

    friend bool operator==(
        const std::optional<T>& left,
        const JsProperty& right
    ) {
        return left == right.optionalValue();
    }

    friend bool operator!=(
        const JsProperty& left,
        const std::optional<T>& right
    ) {
        return !(left == right);
    }

    friend bool operator!=(
        const std::optional<T>& left,
        const JsProperty& right
    ) {
        return !(left == right);
    }

private:
    T value_ {};
    JsPropertyPresence presence_ = JsPropertyPresence::Missing;
    bool provided_ = false;
};

} // namespace bedrock
