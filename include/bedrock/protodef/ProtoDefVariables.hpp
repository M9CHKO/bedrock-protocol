#pragma once

#include <concepts>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace bedrock {

using ProtoDefVariableMap = std::unordered_map<std::string, std::string>;

class ProtoDefVariableStore {
public:
    void setVariable(std::string key, std::string value) {
        std::lock_guard<std::mutex> lock(mutex_);
        variables_[std::move(key)] = std::move(value);
    }

    void setVariable(std::string key, const char* value) {
        setVariable(std::move(key), value ? std::string(value) : std::string());
    }

    void setVariable(std::string key, bool value) {
        setVariable(std::move(key), value ? "true" : "false");
    }

    template<std::integral T>
        requires (!std::same_as<T, bool>)
    void setVariable(std::string key, T value) {
        if constexpr (std::signed_integral<T>) {
            setVariable(std::move(key), std::to_string(static_cast<long long>(value)));
        } else {
            setVariable(std::move(key), std::to_string(static_cast<unsigned long long>(value)));
        }
    }

    template<std::floating_point T>
    void setVariable(std::string key, T value) {
        std::ostringstream out;
        out << std::setprecision(17) << value;
        setVariable(std::move(key), out.str());
    }

    std::optional<std::string> variable(std::string_view key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = variables_.find(std::string(key));
        if (found == variables_.end()) return std::nullopt;
        return found->second;
    }

    ProtoDefVariableMap snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return variables_;
    }

private:
    mutable std::mutex mutex_;
    ProtoDefVariableMap variables_;
};

using ProtoDefVariableStorePtr = std::shared_ptr<ProtoDefVariableStore>;

inline ProtoDefVariableStorePtr makeProtoDefVariableStore() {
    return std::make_shared<ProtoDefVariableStore>();
}

} // namespace bedrock
