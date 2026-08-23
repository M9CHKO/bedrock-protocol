#pragma once

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace bedrock {

// JavaScript advertisement fields can change type after fromString(): protocol
// remains a string, while the five numeric fields become a number, null, or NaN.
// Keep that observable state without making ordinary C++ assignments cumbersome.
class ServerAdvertisementScalar {
public:
    enum class Type {
        Undefined,
        Null,
        String,
        Number,
        Boolean
    };

    ServerAdvertisementScalar() noexcept = default;
    ServerAdvertisementScalar(std::nullopt_t) noexcept;
    ServerAdvertisementScalar(std::nullptr_t) noexcept;
    ServerAdvertisementScalar(const char* value);
    ServerAdvertisementScalar(std::string value);
    ServerAdvertisementScalar(std::string_view value);
    ServerAdvertisementScalar(bool value) noexcept;
    ServerAdvertisementScalar(float value) noexcept;
    ServerAdvertisementScalar(double value) noexcept;

    template <typename Integer,
              std::enable_if_t<
                  std::is_integral_v<std::remove_reference_t<Integer>> &&
                  !std::is_same_v<std::remove_cv_t<std::remove_reference_t<Integer>>, bool>,
                  int> = 0>
    ServerAdvertisementScalar(Integer value) noexcept
        : value_(static_cast<double>(value)) {}

    ServerAdvertisementScalar& operator=(std::nullopt_t) noexcept;
    ServerAdvertisementScalar& operator=(std::nullptr_t) noexcept;
    ServerAdvertisementScalar& operator=(const char* value);
    ServerAdvertisementScalar& operator=(std::string value);
    ServerAdvertisementScalar& operator=(std::string_view value);
    ServerAdvertisementScalar& operator=(bool value) noexcept;
    ServerAdvertisementScalar& operator=(float value) noexcept;
    ServerAdvertisementScalar& operator=(double value) noexcept;

    template <typename Integer,
              std::enable_if_t<
                  std::is_integral_v<std::remove_reference_t<Integer>> &&
                  !std::is_same_v<std::remove_cv_t<std::remove_reference_t<Integer>>, bool>,
                  int> = 0>
    ServerAdvertisementScalar& operator=(Integer value) noexcept {
        value_ = static_cast<double>(value);
        return *this;
    }

    static ServerAdvertisementScalar undefined() noexcept;
    static ServerAdvertisementScalar null() noexcept;
    static ServerAdvertisementScalar nan() noexcept;

    Type type() const noexcept;
    bool isUndefined() const noexcept;
    bool isNull() const noexcept;
    bool isString() const noexcept;
    bool isNumber() const noexcept;
    bool isBoolean() const noexcept;
    bool isNaN() const noexcept;
    bool isTruthy() const noexcept;

    const std::string* stringValue() const noexcept;
    std::optional<double> numberValue() const noexcept;
    std::optional<std::int64_t> toInteger() const noexcept;

    // Equivalent to JavaScript String(value). ServerAdvertisement::toString()
    // uses Array.join semantics internally, where null/undefined become empty.
    std::string toString() const;

    friend bool operator==(const ServerAdvertisementScalar& lhs,
                           const ServerAdvertisementScalar& rhs) noexcept;
    friend bool operator!=(const ServerAdvertisementScalar& lhs,
                           const ServerAdvertisementScalar& rhs) noexcept {
        return !(lhs == rhs);
    }

    friend bool operator==(const ServerAdvertisementScalar& lhs,
                           std::string_view rhs) noexcept;
    friend bool operator==(const ServerAdvertisementScalar& lhs,
                           const char* rhs) noexcept;
    friend bool operator==(const ServerAdvertisementScalar& lhs,
                           double rhs) noexcept;

    template <typename Integer,
              std::enable_if_t<
                  std::is_integral_v<std::remove_reference_t<Integer>> &&
                  !std::is_same_v<std::remove_cv_t<std::remove_reference_t<Integer>>, bool>,
                  int> = 0>
    friend bool operator==(const ServerAdvertisementScalar& lhs,
                           Integer rhs) noexcept {
        const auto* value = std::get_if<double>(&lhs.value_);
        return value && *value == static_cast<double>(rhs);
    }

    friend std::ostream& operator<<(std::ostream& out,
                                    const ServerAdvertisementScalar& value);

private:
    struct Undefined final {
        friend bool operator==(Undefined, Undefined) noexcept = default;
    };
    struct Null final {
        friend bool operator==(Null, Null) noexcept = default;
    };

    using Storage = std::variant<Undefined, Null, std::string, double, bool>;
    Storage value_ = Undefined{};

    std::string toJoinString() const;

    friend class ServerAdvertisement;
};

using ServerAdvertisementObject =
    std::unordered_map<std::string, ServerAdvertisementScalar>;

class ServerAdvertisement {
public:
    inline static constexpr std::string_view CURRENT_VERSION = "1.26.0";

    // Public field names mirror src/server/advertisement.js. `header` only
    // records parsed input; just like the JavaScript implementation, output
    // always starts with the literal "MCPE".
    ServerAdvertisementScalar header;
    ServerAdvertisementScalar motd = "Bedrock Protocol Server";
    ServerAdvertisementScalar name;
    ServerAdvertisementScalar protocol;
    ServerAdvertisementScalar version;
    ServerAdvertisementScalar playersOnline = 0;
    ServerAdvertisementScalar playersMax = 5;
    ServerAdvertisementScalar serverId;
    ServerAdvertisementScalar levelName = "bedrock-protocol";
    ServerAdvertisementScalar gamemode = "Creative";
    ServerAdvertisementScalar gamemodeId = 1;
    ServerAdvertisementScalar portV4;
    ServerAdvertisementScalar portV6;

    explicit ServerAdvertisement(
        ServerAdvertisementObject obj = {},
        ServerAdvertisementScalar port = ServerAdvertisementScalar::undefined(),
        std::string minecraftVersion = std::string(CURRENT_VERSION)
    );

    ServerAdvertisement& fromString(std::string_view str);

    // JavaScript accepts and ignores extra arguments to toString. Keeping an
    // optional scalar here lets toBuffer(version) follow that call shape.
    std::string toString(
        const ServerAdvertisementScalar& ignoredVersion =
            ServerAdvertisementScalar::undefined()
    ) const;

    std::vector<std::uint8_t> toBuffer(
        const ServerAdvertisementScalar& version =
            ServerAdvertisementScalar::undefined()
    ) const;

private:
    void assignObject(const ServerAdvertisementObject& obj);
};

// Module-level exports from advertisement.js.
std::vector<std::uint8_t> getServerName();

template <typename Client>
std::vector<std::uint8_t> getServerName(const Client&) {
    return getServerName();
}

ServerAdvertisement fromServerName(std::string_view string);

} // namespace bedrock
