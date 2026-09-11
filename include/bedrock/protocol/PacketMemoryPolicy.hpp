#pragma once

#include <cstddef>
#include <limits>
#include <string_view>

namespace bedrock {

// Shared limits for packet consumers that observe traffic but do not need a
// lossless, editable ProtoDef tree. Explicit decodePacket()/decodePacketStrict()
// calls remain lossless; event, inspector, bot and relay diagnostics use this
// policy so a map pixel array or deeply nested item NBT cannot multiply a
// single wire packet into hundreds of megabytes of short-lived allocations.
struct PacketMemoryPolicy final {
    static constexpr std::size_t StructuredItemPacketBytes = 16u * 1024u;
    static constexpr std::size_t MaximumTrackedInventoryPacketBytes =
        256u * 1024u;
    static constexpr std::size_t MaximumObservedFields = 4096u;

    static constexpr bool carriesExpandableItemNbt(
        std::string_view packetName
    ) noexcept {
        return packetName == "inventory_content" ||
            packetName == "inventory_slot" ||
            packetName == "mob_equipment" ||
            packetName == "mob_armor_equipment" ||
            packetName == "add_item_entity" ||
            packetName == "item_stack_response" ||
            packetName == "creative_content" ||
            packetName == "item_registry" ||
            packetName == "start_game" ||
            packetName == "crafting_data";
    }

    static constexpr bool shouldKeepItemNbtOpaque(
        std::string_view packetName,
        std::size_t payloadBytes
    ) noexcept {
        return payloadBytes > StructuredItemPacketBytes &&
            carriesExpandableItemNbt(packetName);
    }

    static constexpr bool hasPotentiallyHugeFieldArray(
        std::string_view packetName
    ) noexcept {
        return packetName == "clientbound_map_item_data" ||
            packetName == "item_registry" ||
            packetName == "creative_content" ||
            packetName == "start_game" ||
            packetName == "crafting_data" ||
            packetName == "biome_definition_list";
    }

    static constexpr bool shouldOmitStructuredBlobs(
        std::string_view,
        std::size_t payloadBytes
    ) noexcept {
        return payloadBytes > StructuredItemPacketBytes;
    }

    static constexpr std::size_t observedFieldLimit(
        std::string_view packetName,
        std::size_t payloadBytes
    ) noexcept {
        if (hasPotentiallyHugeFieldArray(packetName) ||
            shouldKeepItemNbtOpaque(packetName, payloadBytes)) {
            return MaximumObservedFields;
        }
        return std::numeric_limits<std::size_t>::max();
    }

    static constexpr bool shouldDecodeRelayParamsLazily(
        std::string_view packetName,
        std::size_t payloadBytes
    ) noexcept {
        return hasPotentiallyHugeFieldArray(packetName) ||
            shouldKeepItemNbtOpaque(packetName, payloadBytes) ||
            payloadBytes > MaximumTrackedInventoryPacketBytes;
    }
};

} // namespace bedrock
