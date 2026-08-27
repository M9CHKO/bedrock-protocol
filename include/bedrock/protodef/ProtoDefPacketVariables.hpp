#pragma once

#include <bedrock/protodef/ProtoDefField.hpp>
#include <bedrock/protodef/ProtoDefValue.hpp>
#include <bedrock/protodef/ProtoDefVariables.hpp>

#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bedrock::detail {

inline bool packetCarriesItemPalette(const std::string& packetName) {
    return packetName == "item_registry" || packetName == "start_game";
}

inline std::optional<std::string> variableKey(const ProtoDefValue& value) {
    switch (value.kind) {
        case ProtoDefValue::Kind::Int:
            return std::to_string(value.intValue);
        case ProtoDefValue::Kind::UInt:
            return std::to_string(value.uintValue);
        case ProtoDefValue::Kind::Double:
            return std::to_string(static_cast<int64_t>(value.doubleValue));
        case ProtoDefValue::Kind::String:
            return value.stringValue;
        default:
            return std::nullopt;
    }
}

inline bool isTruthyVariableKey(const std::string& value) {
    if (value.empty()) return false;
    try {
        return std::stoll(value) != 0;
    } catch (const std::exception&) {
        return true;
    }
}

inline void updateShieldItemId(
    const ProtoDefVariableStorePtr& variables,
    const std::string& name,
    const std::string& runtimeId
) {
    if (variables && name == "minecraft:shield" && isTruthyVariableKey(runtimeId)) {
        variables->setVariable("ShieldItemID", runtimeId);
    }
}

inline void updateItemPaletteFromValue(
    const std::string& packetName,
    const ProtoDefValue& packet,
    const ProtoDefVariableStorePtr& variables
) {
    if (!variables || !packetCarriesItemPalette(packetName) ||
        packet.kind != ProtoDefValue::Kind::Object) {
        return;
    }

    const auto* itemstates = packet.get("itemstates");
    if (!itemstates || itemstates->kind != ProtoDefValue::Kind::Array) return;

    for (const auto& state : itemstates->arrayValue) {
        if (state.kind != ProtoDefValue::Kind::Object) continue;
        const auto* name = state.get("name");
        const auto* runtimeId = state.get("runtime_id");
        if (!name || name->kind != ProtoDefValue::Kind::String || !runtimeId) continue;
        const auto key = variableKey(*runtimeId);
        if (!key.has_value()) continue;
        updateShieldItemId(variables, name->stringValue, *key);
        if (name->stringValue == "minecraft:shield" && isTruthyVariableKey(*key)) return;
    }
}

struct DecodedItemState {
    std::optional<std::string> name;
    std::optional<std::string> runtimeId;
};

inline void updateItemPaletteFromFields(
    const std::string& packetName,
    const std::vector<ProtoDefField>& fields,
    const ProtoDefVariableStorePtr& variables
) {
    if (!variables || !packetCarriesItemPalette(packetName)) return;

    std::unordered_map<std::size_t, DecodedItemState> states;
    constexpr std::string_view prefix = "itemstates[";
    for (const auto& field : fields) {
        if (field.path.rfind(prefix, 0) != 0) continue;
        const auto close = field.path.find(']', prefix.size());
        if (close == std::string::npos) continue;

        std::size_t index = 0;
        try {
            index = static_cast<std::size_t>(std::stoull(
                field.path.substr(prefix.size(), close - prefix.size())
            ));
        } catch (const std::exception&) {
            continue;
        }

        const auto suffix = field.path.substr(close + 1);
        if (suffix == ".name") {
            states[index].name = field.value;
        } else if (suffix == ".runtime_id") {
            states[index].runtimeId = field.value;
        }
    }

    for (const auto& [index, state] : states) {
        (void) index;
        if (!state.name.has_value() || !state.runtimeId.has_value()) continue;
        updateShieldItemId(variables, *state.name, *state.runtimeId);
        if (*state.name == "minecraft:shield" && isTruthyVariableKey(*state.runtimeId)) return;
    }
}

} // namespace bedrock::detail
