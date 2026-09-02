package com.m9chko.bedrockrelay;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/** Packet-only hostile-mob risk and incoming-damage estimator. */
final class ThreatAnalyzer {
    static final class DefenseState {
        final boolean healthKnown;
        final double health;
        final double maximumHealth;
        final boolean hungerKnown;
        final double hunger;
        final double saturation;
        final boolean absorptionKnown;
        final double absorption;
        final int resistanceLevel;
        final int armorPoints;
        final int armorToughness;
        final int enchantedArmorPieces;

        DefenseState(
            boolean healthKnown,
            double health,
            double maximumHealth,
            boolean hungerKnown,
            double hunger,
            double saturation,
            boolean absorptionKnown,
            double absorption,
            int resistanceLevel,
            int armorPoints,
            int armorToughness,
            int enchantedArmorPieces
        ) {
            this.healthKnown = healthKnown;
            this.health = health;
            this.maximumHealth = maximumHealth;
            this.hungerKnown = hungerKnown;
            this.hunger = hunger;
            this.saturation = saturation;
            this.absorptionKnown = absorptionKnown;
            this.absorption = absorption;
            this.resistanceLevel = resistanceLevel;
            this.armorPoints = armorPoints;
            this.armorToughness = armorToughness;
            this.enchantedArmorPieces = enchantedArmorPieces;
        }

        static DefenseState unknown() {
            return new DefenseState(
                false, 20, 20,
                false, 20, 5,
                false, 0,
                0, 0, 0, 0
            );
        }

        static DefenseState from(JSONObject state) {
            int armor = 0;
            int toughness = 0;
            int enchanted = 0;
            JSONArray equipment = state.optJSONArray("equipment");
            if (equipment != null) {
                for (int index = 0; index < equipment.length(); ++index) {
                    JSONObject item = equipment.optJSONObject(index);
                    if (item == null || !item.optBoolean("present", false)) {
                        continue;
                    }
                    String slot = item.optString("slot", "");
                    if (!isArmorSlot(slot)) continue;
                    String name = normalize(item.optString("name", ""));
                    armor += armorPoints(name, slot);
                    toughness += armorToughness(name);
                    if (item.optBoolean("enchanted", false)) ++enchanted;
                }
            }
            return new DefenseState(
                state.optBoolean("playerHealthKnown", false),
                Math.max(0.0, state.optDouble("playerHealth", 20.0)),
                Math.max(1.0, state.optDouble("playerMaximumHealth", 20.0)),
                state.optBoolean("playerHungerKnown", false),
                clamp(state.optDouble("playerHunger", 20.0), 0.0, 20.0),
                Math.max(0.0, state.optDouble("playerSaturation", 5.0)),
                state.optBoolean("playerAbsorptionKnown", false),
                Math.max(0.0, state.optDouble("playerAbsorption", 0.0)),
                Math.max(0, state.optInt("playerResistanceLevel", 0)),
                Math.min(20, armor),
                Math.min(12, toughness),
                Math.min(4, enchanted)
            );
        }

        private static boolean isArmorSlot(String slot) {
            return "helmet".equals(slot) || "chestplate".equals(slot) ||
                "leggings".equals(slot) || "boots".equals(slot);
        }

        private static int armorPoints(String name, String slot) {
            int material;
            if (name.contains("netherite") || name.contains("diamond")) {
                material = 5;
            } else if (name.contains("iron")) {
                material = 4;
            } else if (name.contains("chain")) {
                material = 3;
            } else if (name.contains("gold")) {
                material = 2;
            } else if (name.contains("turtle")) {
                return "helmet".equals(slot) ? 2 : 0;
            } else if (name.contains("leather")) {
                material = 1;
            } else {
                return 0;
            }
            if ("helmet".equals(slot)) {
                return material >= 5 ? 3 : material >= 2 ? 2 : 1;
            }
            if ("chestplate".equals(slot)) {
                if (material >= 5) return 8;
                if (material == 4) return 6;
                if (material >= 2) return 5;
                return 3;
            }
            if ("leggings".equals(slot)) {
                if (material >= 5) return 6;
                if (material == 4) return 5;
                if (material == 3) return 4;
                if (material == 2) return 3;
                return 2;
            }
            if ("boots".equals(slot)) {
                return material >= 4 ? 3 - (material == 4 ? 1 : 0) : 1;
            }
            return 0;
        }

        private static int armorToughness(String name) {
            if (name.contains("netherite")) return 3;
            if (name.contains("diamond")) return 2;
            return 0;
        }
    }

    static final class Result {
        final Set<String> dangerousEntityIds;
        final Threat primary;

        Result(Set<String> dangerousEntityIds, Threat primary) {
            this.dangerousEntityIds = dangerousEntityIds.isEmpty()
                ? Collections.emptySet()
                : Collections.unmodifiableSet(dangerousEntityIds);
            this.primary = primary;
        }

        static Result none() {
            return new Result(Collections.emptySet(), null);
        }
    }

    static final class Threat {
        final String entityId;
        final String mobName;
        final double distance;
        final double closingSpeed;
        final double damageMinimum;
        final double damageMaximum;
        final int score;
        final boolean imminent;
        final boolean conditionalAggression;
        final DefenseState defense;

        Threat(
            String entityId,
            String mobName,
            double distance,
            double closingSpeed,
            double damageMinimum,
            double damageMaximum,
            int score,
            boolean imminent,
            boolean conditionalAggression,
            DefenseState defense
        ) {
            this.entityId = entityId;
            this.mobName = mobName;
            this.distance = distance;
            this.closingSpeed = closingSpeed;
            this.damageMinimum = damageMinimum;
            this.damageMaximum = damageMaximum;
            this.score = score;
            this.imminent = imminent;
            this.conditionalAggression = conditionalAggression;
            this.defense = defense;
        }

        String fingerprint() {
            return entityId + ':' + Math.round(distance * 4.0) + ':' +
                Math.round(damageMinimum * 4.0) + ':' +
                Math.round(damageMaximum * 4.0) + ':' + score + ':' + imminent;
        }
    }

    private static final class MobSpec {
        final String displayName;
        final double damageMinimum;
        final double damageMaximum;
        final double attackReach;
        final boolean ranged;
        final boolean conditionalAggression;

        MobSpec(
            String displayName,
            double damageMinimum,
            double damageMaximum,
            double attackReach,
            boolean ranged,
            boolean conditionalAggression
        ) {
            this.displayName = displayName;
            this.damageMinimum = damageMinimum;
            this.damageMaximum = damageMaximum;
            this.attackReach = attackReach;
            this.ranged = ranged;
            this.conditionalAggression = conditionalAggression;
        }
    }

    private static final class MotionTrack {
        double distance;
        double closingSpeed;
        long updatedAtNanos;
        long seenGeneration;
    }

    private static final Map<String, MobSpec> HOSTILES = hostileMobs();
    private final Map<String, MotionTrack> motion = new HashMap<>();
    private long generation;

    synchronized Result analyze(
        EntityOutlineOverlayController.Frame frame,
        DefenseState defense,
        int configuredTriggerDistance
    ) {
        if (frame == null || !frame.camera.known) return Result.none();
        final long now = System.nanoTime();
        final double triggerDistance = Math.max(
            3.0,
            Math.min(32.0, configuredTriggerDistance)
        );
        ++generation;
        Set<String> dangerous = new HashSet<>();
        Threat primary = null;

        for (EntityOutlineOverlayController.EntitySample entity : frame.entities) {
            if (entity.player || entity.item) continue;
            MobSpec spec = findSpec(entity.type, entity.label);
            if (spec == null) continue;

            double dx = entity.x - frame.camera.x;
            double dy = entity.y + entity.height * 0.5 - frame.camera.y;
            double dz = entity.z - frame.camera.z;
            double distance = Math.sqrt(dx * dx + dy * dy + dz * dz);
            if (!Double.isFinite(distance)) continue;

            MotionTrack track = motion.get(entity.id);
            if (track == null) {
                track = new MotionTrack();
                track.distance = distance;
                track.updatedAtNanos = now;
                motion.put(entity.id, track);
            } else {
                double elapsed = (now - track.updatedAtNanos) / 1_000_000_000.0;
                if (elapsed >= 0.02 && elapsed <= 1.0) {
                    double measured = clamp(
                        (track.distance - distance) / elapsed,
                        -15.0,
                        15.0
                    );
                    track.closingSpeed = track.closingSpeed * 0.68 +
                        measured * 0.32;
                } else if (elapsed > 1.0) {
                    track.closingSpeed = 0.0;
                }
                track.distance = distance;
                track.updatedAtNanos = now;
            }
            track.seenGeneration = generation;

            if (distance > triggerDistance) continue;
            boolean veryClose = distance <= spec.attackReach + 1.25;
            boolean approaching = track.closingSpeed >= 0.28;
            boolean rangedReady = spec.ranged &&
                distance <= Math.min(triggerDistance, 18.0);
            if (spec.conditionalAggression && !veryClose && !approaching) {
                // Spiders, endermen and piglins are not always hostile. With
                // no target metadata we avoid a confident warning unless they
                // are close or clearly closing on the player.
                continue;
            }

            int score = (int) Math.round(
                28.0 + (1.0 - distance / triggerDistance) * 42.0 +
                    Math.max(0.0, track.closingSpeed) * 9.0 +
                    (veryClose ? 28.0 : 0.0) +
                    (rangedReady ? 10.0 : 0.0)
            );
            if (defense.healthKnown &&
                defense.health <= defense.maximumHealth * 0.35) {
                score += 10;
            }
            if (defense.hungerKnown && defense.hunger <= 6.0) score += 5;
            score = Math.max(0, Math.min(100, score));

            double low = damageAfterDefense(
                spec.damageMinimum,
                defense,
                true
            );
            double high = damageAfterDefense(
                spec.damageMaximum,
                defense,
                false
            );
            Threat threat = new Threat(
                entity.id,
                spec.displayName,
                distance,
                track.closingSpeed,
                Math.min(low, high),
                Math.max(low, high),
                score,
                veryClose || score >= 82,
                spec.conditionalAggression,
                defense
            );
            dangerous.add(entity.id);
            if (primary == null || threat.score > primary.score ||
                (threat.score == primary.score &&
                    threat.distance < primary.distance)) {
                primary = threat;
            }
        }

        Iterator<Map.Entry<String, MotionTrack>> iterator =
            motion.entrySet().iterator();
        while (iterator.hasNext()) {
            if (iterator.next().getValue().seenGeneration != generation) {
                iterator.remove();
            }
        }
        return new Result(dangerous, primary);
    }

    synchronized void reset() {
        motion.clear();
        generation = 0;
    }

    private static double damageAfterDefense(
        double incoming,
        DefenseState defense,
        boolean optimisticEnchantments
    ) {
        double armorEffect = Math.min(
            20.0,
            Math.max(
                defense.armorPoints / 5.0,
                defense.armorPoints - incoming /
                    (2.0 + defense.armorToughness / 4.0)
            )
        );
        double result = incoming * (1.0 - armorEffect / 25.0);
        if (defense.enchantedArmorPieces > 0) {
            double enchantReduction = defense.enchantedArmorPieces *
                (optimisticEnchantments ? 0.05 : 0.015);
            result *= 1.0 - Math.min(0.32, enchantReduction);
        }
        if (defense.resistanceLevel > 0) {
            result *= 1.0 - Math.min(
                0.8,
                defense.resistanceLevel * 0.20
            );
        }
        return Math.max(0.0, result);
    }

    private static MobSpec findSpec(String type, String label) {
        String normalizedType = normalize(type);
        MobSpec direct = HOSTILES.get(normalizedType);
        if (direct != null) return direct;
        String normalizedLabel = normalize(label);
        direct = HOSTILES.get(normalizedLabel);
        if (direct != null) return direct;
        for (Map.Entry<String, MobSpec> entry : HOSTILES.entrySet()) {
            if (normalizedType.endsWith(entry.getKey()) ||
                normalizedLabel.endsWith(entry.getKey())) {
                return entry.getValue();
            }
        }
        return null;
    }

    private static Map<String, MobSpec> hostileMobs() {
        Map<String, MobSpec> result = new HashMap<>();
        add(result, new MobSpec("Зомби", 2.5, 7.5, 2.2, false, false),
            "zombie", "zombie_villager", "zombie_villager_v2");
        add(result, new MobSpec("Утопленник", 3, 12, 2.4, false, false),
            "drowned");
        add(result, new MobSpec("Кадавр", 2.5, 7.5, 2.2, false, false),
            "husk");
        add(result, new MobSpec("Скелет", 1.5, 9, 2.0, true, false),
            "skeleton", "stray", "bogged");
        add(result, new MobSpec("Крипер", 12, 43, 4.5, false, false),
            "creeper");
        add(result, new MobSpec("Паук", 2, 6, 2.8, false, true),
            "spider");
        add(result, new MobSpec("Пещерный паук", 2, 7, 2.8, false, false),
            "cave_spider");
        add(result, new MobSpec("Эндермен", 4.5, 10.5, 3.0, false, true),
            "enderman");
        add(result, new MobSpec("Ведьма", 3, 12, 2.0, true, false),
            "witch");
        add(result, new MobSpec("Разбойник", 2, 10, 2.0, true, false),
            "pillager");
        add(result, new MobSpec("Поборник", 7.5, 19.5, 2.7, false, false),
            "vindicator");
        add(result, new MobSpec("Вызыватель", 4, 12, 2.0, true, false),
            "evocation_illager", "evoker");
        add(result, new MobSpec("Разоритель", 7, 18, 3.4, false, false),
            "ravager");
        add(result, new MobSpec("Досаждатель", 5.5, 13.5, 2.2, false, false),
            "vex");
        add(result, new MobSpec("Страж", 4, 12, 2.2, true, false),
            "guardian", "elder_guardian");
        add(result, new MobSpec("Шалкер", 4, 8, 2.0, true, false),
            "shulker");
        add(result, new MobSpec("Фантом", 4, 9, 3.0, false, false),
            "phantom");
        add(result, new MobSpec("Ифрит", 4, 10, 2.0, true, false),
            "blaze");
        add(result, new MobSpec("Гаст", 6, 17, 2.0, true, false),
            "ghast");
        add(result, new MobSpec("Скелет-иссушитель", 5, 12, 2.5, false, false),
            "wither_skeleton");
        add(result, new MobSpec("Иссушитель", 8, 20, 3.0, true, false),
            "wither");
        add(result, new MobSpec("Пиглин", 2.5, 9, 2.5, false, true),
            "piglin", "zombie_pigman", "zombified_piglin");
        add(result, new MobSpec("Брутальный пиглин", 7.5, 19.5, 2.7, false, false),
            "piglin_brute");
        add(result, new MobSpec("Хоглин", 3, 15, 3.2, false, false),
            "hoglin", "zoglin");
        add(result, new MobSpec("Слизень", 1, 8, 2.6, false, false),
            "slime", "magma_cube");
        add(result, new MobSpec("Чешуйница", 1, 4, 2.0, false, false),
            "silverfish", "endermite");
        add(result, new MobSpec("Хранитель", 16, 45, 3.5, true, false),
            "warden");
        add(result, new MobSpec("Дракон Края", 6, 15, 5.0, false, false),
            "ender_dragon");
        add(result, new MobSpec("Вихрь", 1, 6, 2.0, true, false),
            "breeze");
        return Collections.unmodifiableMap(result);
    }

    private static void add(
        Map<String, MobSpec> target,
        MobSpec spec,
        String... aliases
    ) {
        for (String alias : aliases) target.put(alias, spec);
    }

    private static String normalize(String value) {
        if (value == null) return "";
        String normalized = value.toLowerCase(Locale.ROOT)
            .replace("minecraft:", "")
            .replace(' ', '_')
            .replace('-', '_');
        int separator = normalized.lastIndexOf(':');
        return separator >= 0 ? normalized.substring(separator + 1) : normalized;
    }

    private static double clamp(double value, double minimum, double maximum) {
        return Math.max(minimum, Math.min(maximum, value));
    }
}
