#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace bedrock {

struct ItemDurabilityDefinition {
    std::string_view name;
    int32_t maximum;
};

struct ItemDurabilitySnapshot {
    int32_t damage;
    int32_t maximum;
    int32_t remaining;
    int32_t percent;
};

// This compact modern profile is derived from the vendored minecraft-data
// Bedrock 1.21.100 item registry. It is the same maxDurability source exposed
// by prismarine-registry and intentionally contains only damageable items.
inline constexpr std::array<ItemDurabilityDefinition, 68>
    ItemDurabilityDefinitions {{
        {"bow", 384},
        {"brush", 64},
        {"carrot_on_a_stick", 25},
        {"chainmail_boots", 195},
        {"chainmail_chestplate", 240},
        {"chainmail_helmet", 165},
        {"chainmail_leggings", 225},
        {"crossbow", 465},
        {"diamond_axe", 1561},
        {"diamond_boots", 429},
        {"diamond_chestplate", 528},
        {"diamond_helmet", 363},
        {"diamond_hoe", 1561},
        {"diamond_leggings", 495},
        {"diamond_pickaxe", 1561},
        {"diamond_shovel", 1561},
        {"diamond_sword", 1561},
        {"elytra", 432},
        {"fishing_rod", 64},
        {"flint_and_steel", 64},
        {"golden_axe", 32},
        {"golden_boots", 91},
        {"golden_chestplate", 112},
        {"golden_helmet", 77},
        {"golden_hoe", 32},
        {"golden_leggings", 105},
        {"golden_pickaxe", 32},
        {"golden_shovel", 32},
        {"golden_sword", 32},
        {"iron_axe", 250},
        {"iron_boots", 195},
        {"iron_chestplate", 240},
        {"iron_helmet", 165},
        {"iron_hoe", 250},
        {"iron_leggings", 225},
        {"iron_pickaxe", 250},
        {"iron_shovel", 250},
        {"iron_sword", 250},
        {"leather_boots", 65},
        {"leather_chestplate", 80},
        {"leather_helmet", 55},
        {"leather_leggings", 75},
        {"mace", 500},
        {"netherite_axe", 2031},
        {"netherite_boots", 481},
        {"netherite_chestplate", 592},
        {"netherite_helmet", 407},
        {"netherite_hoe", 2031},
        {"netherite_leggings", 555},
        {"netherite_pickaxe", 2031},
        {"netherite_shovel", 2031},
        {"netherite_sword", 2031},
        {"shears", 238},
        {"shield", 336},
        {"stone_axe", 131},
        {"stone_hoe", 131},
        {"stone_pickaxe", 131},
        {"stone_shovel", 131},
        {"stone_sword", 131},
        {"trident", 250},
        {"turtle_helmet", 275},
        {"warped_fungus_on_a_stick", 100},
        {"wolf_armor", 64},
        {"wooden_axe", 59},
        {"wooden_hoe", 59},
        {"wooden_pickaxe", 59},
        {"wooden_shovel", 59},
        {"wooden_sword", 59}
    }};

constexpr std::string_view normalizeItemRegistryName(
    std::string_view name
) noexcept {
    constexpr std::string_view MinecraftNamespace = "minecraft:";
    return name.starts_with(MinecraftNamespace)
        ? name.substr(MinecraftNamespace.size())
        : name;
}

struct ItemDurabilityVersion {
    int32_t major = 0;
    int32_t minor = 0;
    int32_t patch = 0;
};

constexpr ItemDurabilityVersion parseItemDurabilityVersion(
    std::string_view version
) noexcept {
    ItemDurabilityVersion out;
    std::array<int32_t*, 3> components {&out.major, &out.minor, &out.patch};
    std::size_t offset = 0;
    for (auto* component : components) {
        while (offset < version.size() &&
            (version[offset] < '0' || version[offset] > '9')) {
            ++offset;
        }
        while (offset < version.size() &&
            version[offset] >= '0' && version[offset] <= '9') {
            *component = *component * 10 + (version[offset] - '0');
            ++offset;
        }
    }
    return out;
}

constexpr bool itemDurabilityVersionAtLeast(
    std::string_view version,
    ItemDurabilityVersion minimum
) noexcept {
    const auto parsed = parseItemDurabilityVersion(
        version.empty() ? std::string_view("1.21.100") : version
    );
    if (parsed.major != minimum.major) return parsed.major > minimum.major;
    if (parsed.minor != minimum.minor) return parsed.minor > minimum.minor;
    return parsed.patch >= minimum.patch;
}

constexpr std::optional<int32_t> maximumItemDurability(
    std::string_view version,
    std::string_view registryName
) noexcept {
    const auto normalized = normalizeItemRegistryName(registryName);
    // minecraft-data has only four distinct durability profiles across every
    // version exposed by the Android app. Four names account for all profile
    // differences; the remaining 64 values are stable from 1.16.201 onward.
    if (normalized == "crossbow" && !itemDurabilityVersionAtLeast(
            version,
            {1, 19, 0}
        )) {
        return 326;
    }
    if (normalized == "brush" && !itemDurabilityVersionAtLeast(
            version,
            {1, 20, 0}
        )) {
        return std::nullopt;
    }
    if (normalized == "wolf_armor" && !itemDurabilityVersionAtLeast(
            version,
            {1, 20, 80}
        )) {
        return std::nullopt;
    }
    if (normalized == "mace" &&
        !itemDurabilityVersionAtLeast(version, {1, 21, 0})) {
        return std::nullopt;
    }
    for (const auto& definition : ItemDurabilityDefinitions) {
        if (definition.name == normalized) return definition.maximum;
    }
    return std::nullopt;
}

constexpr std::optional<int32_t> maximumItemDurability(
    std::string_view registryName
) noexcept {
    return maximumItemDurability("1.21.100", registryName);
}

constexpr std::optional<ItemDurabilitySnapshot> calculateItemDurability(
    std::string_view version,
    std::string_view registryName,
    std::optional<int32_t> damage
) noexcept {
    const auto maximum = maximumItemDurability(version, registryName);
    if (!maximum.has_value() || !damage.has_value()) return std::nullopt;

    const auto clampedDamage = *damage < 0
        ? 0
        : (*damage > *maximum ? *maximum : *damage);
    const auto remaining = *maximum - clampedDamage;
    const auto percent = static_cast<int32_t>(
        (static_cast<int64_t>(remaining) * 100 + *maximum / 2) / *maximum
    );
    return ItemDurabilitySnapshot {
        clampedDamage,
        *maximum,
        remaining,
        percent
    };
}

constexpr std::optional<ItemDurabilitySnapshot> calculateItemDurability(
    std::string_view registryName,
    std::optional<int32_t> damage
) noexcept {
    return calculateItemDurability("1.21.100", registryName, damage);
}

static_assert(maximumItemDurability("minecraft:diamond_sword") == 1561);
static_assert(maximumItemDurability("wolf_armor") == 64);
static_assert(maximumItemDurability("1.18.30", "crossbow") == 326);
static_assert(maximumItemDurability("1.19.1", "crossbow") == 465);
static_assert(!maximumItemDurability("1.19.80", "brush").has_value());
static_assert(maximumItemDurability("1.20.0", "brush") == 64);
static_assert(!maximumItemDurability("1.20.71", "wolf_armor").has_value());
static_assert(maximumItemDurability("1.20.80", "wolf_armor") == 64);
static_assert(!maximumItemDurability("1.20.80", "mace").has_value());
static_assert(!maximumItemDurability("minecraft:apple").has_value());
static_assert(calculateItemDurability("mace", 125)->percent == 75);

} // namespace bedrock
