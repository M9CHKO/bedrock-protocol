package com.m9chko.bedrockrelay;

import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;

/** Rotates and mirrors directional block-state properties with a schematic. */
final class SchematicBlockStateTransform {
    private SchematicBlockStateTransform() {}

    static String transform(
        String blockState,
        int rotationQuarterTurns,
        boolean mirrored
    ) {
        if (blockState == null || blockState.isEmpty()) return blockState;
        int rotation = Math.floorMod(rotationQuarterTurns, 4);
        if (rotation == 0 && !mirrored) return blockState;

        int propertyStart = blockState.indexOf('[');
        if (propertyStart < 0) return blockState;
        if (!blockState.endsWith("]") || propertyStart == 0) return blockState;
        String name = blockState.substring(0, propertyStart);
        String shortName = name.substring(name.indexOf(':') + 1)
            .toLowerCase(Locale.ROOT);
        String body = blockState.substring(propertyStart + 1, blockState.length() - 1);
        if (body.isEmpty()) return blockState;

        TreeMap<String, String> properties = new TreeMap<>();
        for (String entry : body.split(",", -1)) {
            int separator = entry.indexOf('=');
            if (separator <= 0 || separator + 1 >= entry.length()) {
                return blockState;
            }
            String key = entry.substring(0, separator);
            String value = entry.substring(separator + 1);
            String transformedKey = transformDirectionalKey(
                key,
                rotation,
                mirrored
            );
            String transformedValue = transformValue(
                shortName,
                key,
                value,
                rotation,
                mirrored
            );
            if (properties.put(transformedKey, transformedValue) != null) {
                // A malformed/custom state must never lose a property because
                // two keys happened to collapse after transformation.
                return blockState;
            }
        }

        StringBuilder result = new StringBuilder(name).append('[');
        boolean first = true;
        for (Map.Entry<String, String> property : properties.entrySet()) {
            if (!first) result.append(',');
            first = false;
            result.append(property.getKey()).append('=').append(property.getValue());
        }
        return result.append(']').toString();
    }

    private static String transformValue(
        String blockName,
        String key,
        String value,
        int rotation,
        boolean mirrored
    ) {
        switch (key) {
            case "facing":
            case "horizontal_facing":
            case "minecraft:facing_direction":
            case "torch_facing_direction":
                return transformCardinal(value, rotation, mirrored);
            case "minecraft:cardinal_direction":
                return transformBedrockCardinalDirection(
                    blockName,
                    value,
                    rotation,
                    mirrored
                );
            case "block_face":
            case "minecraft:block_face":
                return transformCardinal(value, rotation, mirrored);
            case "facing_direction":
                return transformFacingDirection(value, rotation, mirrored);
            case "weirdo_direction":
                return transformMappedDirection(
                    value,
                    new String[] {"east", "west", "south", "north"},
                    rotation,
                    mirrored
                );
            case "coral_direction":
                return transformMappedDirection(
                    value,
                    new String[] {"west", "east", "north", "south"},
                    rotation,
                    mirrored
                );
            case "direction":
                if ("chalkboard".equals(blockName)) {
                    return transformRotation(value, 16, rotation, mirrored);
                }
                return transformDirectionProperty(
                    blockName,
                    value,
                    rotation,
                    mirrored
                );
            case "ground_sign_direction":
                return transformRotation(value, 16, rotation, mirrored);
            case "rotation":
                return blockName.contains("sign")
                    ? transformRotation(value, 16, rotation, mirrored)
                    : value;
            case "rail_direction":
                return transformRailDirection(value, rotation, mirrored);
            case "shape":
                if (blockName.contains("rail")) {
                    return transformRailShape(value, rotation, mirrored);
                }
                return mirrored ? mirrorHandedValue(value) : value;
            case "axis":
            case "pillar_axis":
            case "portal_axis":
                return transformAxis(value, rotation);
            case "orientation":
                return transformCompoundDirections(value, rotation, mirrored);
            case "lever_direction":
                return transformLeverDirection(value, rotation, mirrored);
            case "multi_face_direction_bits":
                return transformFaceBits(value, rotation, mirrored, true);
            case "vine_direction_bits":
                return transformFaceBits(value, rotation, mirrored, false);
            case "coral_fan_direction":
                return transformBinaryAxis(value, rotation);
            case "huge_mushroom_bits":
                return transformHugeMushroomBits(value, rotation, mirrored);
            case "hinge":
                return mirrored ? mirrorHandedValue(value) : value;
            case "door_hinge_bit":
                return mirrored ? invertBoolean(value) : value;
            default:
                return value;
        }
    }

    private static String transformDirectionalKey(
        String key,
        int rotation,
        boolean mirrored
    ) {
        String[] tokens = key.split("_", -1);
        boolean changed = false;
        for (int index = 0; index < tokens.length; ++index) {
            String transformed = transformCardinal(tokens[index], rotation, mirrored);
            if (!transformed.equals(tokens[index])) {
                tokens[index] = transformed;
                changed = true;
            }
        }
        return changed ? String.join("_", tokens) : key;
    }

    private static String transformDirectionProperty(
        String blockName,
        String value,
        int rotation,
        boolean mirrored
    ) {
        String[] directions;
        if (blockName.endsWith("trapdoor") || "trapdoor".equals(blockName)) {
            directions = new String[] {"east", "west", "south", "north"};
        } else if ("bell".equals(blockName) ||
            "decorated_pot".equals(blockName)) {
            directions = new String[] {"north", "east", "south", "west"};
        } else {
            directions = new String[] {"south", "west", "north", "east"};
        }
        return transformMappedDirection(
            value,
            directions,
            rotation,
            mirrored
        );
    }

    private static String transformBedrockCardinalDirection(
        String blockName,
        String value,
        int rotation,
        boolean mirrored
    ) {
        if (!mirrored ||
            (!blockName.endsWith("_door") &&
                !"small_dripleaf_block".equals(blockName))) {
            return transformCardinal(value, rotation, mirrored);
        }

        // Bedrock's door and small-dripleaf cardinal strings are offset from
        // their physical model direction. Mirroring the physical X axis is
        // therefore encoded as north <-> south, not east <-> west. Rotation
        // remains the ordinary cardinal rotation once that mirror is applied.
        String mirroredValue;
        if ("north".equals(value)) mirroredValue = "south";
        else if ("south".equals(value)) mirroredValue = "north";
        else mirroredValue = value;
        return transformCardinal(mirroredValue, rotation, false);
    }

    private static String transformFacingDirection(
        String value,
        int rotation,
        boolean mirrored
    ) {
        Integer numeric = parseInteger(value);
        if (numeric == null || numeric < 2 || numeric > 5) return value;
        String[] directions = {"down", "up", "north", "south", "west", "east"};
        String transformed = transformCardinal(
            directions[numeric],
            rotation,
            mirrored
        );
        for (int index = 2; index < directions.length; ++index) {
            if (directions[index].equals(transformed)) return Integer.toString(index);
        }
        return value;
    }

    private static String transformMappedDirection(
        String value,
        String[] directions,
        int rotation,
        boolean mirrored
    ) {
        Integer numeric = parseInteger(value);
        if (numeric == null || numeric < 0 || numeric >= directions.length) {
            return value;
        }
        String transformed = transformCardinal(
            directions[numeric],
            rotation,
            mirrored
        );
        for (int index = 0; index < directions.length; ++index) {
            if (directions[index].equals(transformed)) return Integer.toString(index);
        }
        return value;
    }

    private static String transformRotation(
        String value,
        int steps,
        int rotation,
        boolean mirrored
    ) {
        Integer numeric = parseInteger(value);
        if (numeric == null || numeric < 0 || numeric >= steps) return value;
        int transformed = mirrored ? Math.floorMod(-numeric, steps) : numeric;
        transformed = Math.floorMod(
            transformed + rotation * (steps / 4),
            steps
        );
        return Integer.toString(transformed);
    }

    private static String transformRailDirection(
        String value,
        int rotation,
        boolean mirrored
    ) {
        String[] shapes = {
            "north_south",
            "east_west",
            "ascending_east",
            "ascending_west",
            "ascending_north",
            "ascending_south",
            "south_east",
            "south_west",
            "north_west",
            "north_east"
        };
        Integer numeric = parseInteger(value);
        if (numeric == null || numeric < 0 || numeric >= shapes.length) return value;
        String transformed = transformRailShape(
            shapes[numeric],
            rotation,
            mirrored
        );
        for (int index = 0; index < shapes.length; ++index) {
            if (sameRailShape(shapes[index], transformed)) {
                return Integer.toString(index);
            }
        }
        return value;
    }

    private static String transformRailShape(
        String value,
        int rotation,
        boolean mirrored
    ) {
        if ("north_south".equals(value) || "east_west".equals(value)) {
            String first = value.substring(0, value.indexOf('_'));
            String transformed = transformCardinal(first, rotation, mirrored);
            return ("north".equals(transformed) || "south".equals(transformed))
                ? "north_south"
                : "east_west";
        }
        if (value.startsWith("ascending_")) {
            return "ascending_" + transformCardinal(
                value.substring("ascending_".length()),
                rotation,
                mirrored
            );
        }
        int separator = value.indexOf('_');
        if (separator <= 0) return value;
        String first = transformCardinal(
            value.substring(0, separator),
            rotation,
            mirrored
        );
        String second = transformCardinal(
            value.substring(separator + 1),
            rotation,
            mirrored
        );
        return first + "_" + second;
    }

    private static boolean sameRailShape(String left, String right) {
        if (left.equals(right)) return true;
        int separator = right.indexOf('_');
        return separator > 0 &&
            left.equals(right.substring(separator + 1) + "_" +
                right.substring(0, separator));
    }

    private static String transformAxis(String value, int rotation) {
        if ((rotation & 1) == 0) return value;
        if ("x".equals(value)) return "z";
        if ("z".equals(value)) return "x";
        return value;
    }

    private static String transformLeverDirection(
        String value,
        int rotation,
        boolean mirrored
    ) {
        String cardinal = transformCardinal(value, rotation, mirrored);
        if (!cardinal.equals(value)) return cardinal;
        if ((rotation & 1) == 0) return value;
        if (value.endsWith("north_south")) {
            return value.substring(0, value.length() - "north_south".length()) +
                "east_west";
        }
        if (value.endsWith("east_west")) {
            return value.substring(0, value.length() - "east_west".length()) +
                "north_south";
        }
        return value;
    }

    private static String transformCompoundDirections(
        String value,
        int rotation,
        boolean mirrored
    ) {
        String[] tokens = value.split("_", -1);
        for (int index = 0; index < tokens.length; ++index) {
            tokens[index] = transformCardinal(tokens[index], rotation, mirrored);
        }
        return String.join("_", tokens);
    }

    private static String transformFaceBits(
        String value,
        int rotation,
        boolean mirrored,
        boolean sixFaces
    ) {
        Integer numeric = parseInteger(value);
        int maximum = sixFaces ? 63 : 15;
        if (numeric == null || numeric < 0 || numeric > maximum) return value;
        int result = sixFaces ? numeric & 3 : 0;
        String[] directions = {"south", "west", "north", "east"};
        int[] bits = sixFaces
            ? new int[] {4, 8, 16, 32}
            : new int[] {1, 2, 4, 8};
        for (int index = 0; index < directions.length; ++index) {
            if ((numeric & bits[index]) == 0) continue;
            String transformed = transformCardinal(
                directions[index],
                rotation,
                mirrored
            );
            for (int output = 0; output < directions.length; ++output) {
                if (directions[output].equals(transformed)) {
                    result |= bits[output];
                    break;
                }
            }
        }
        return Integer.toString(result);
    }

    private static String transformBinaryAxis(String value, int rotation) {
        Integer numeric = parseInteger(value);
        if (numeric == null || (numeric != 0 && numeric != 1)) return value;
        return (rotation & 1) == 0 ? value : Integer.toString(numeric ^ 1);
    }

    private static String transformHugeMushroomBits(
        String value,
        int rotation,
        boolean mirrored
    ) {
        Integer numeric = parseInteger(value);
        if (numeric == null || numeric < 1 || numeric > 9 || numeric == 5) {
            return value;
        }
        int index = numeric - 1;
        int x = index % 3 - 1;
        int z = index / 3 - 1;
        if (mirrored) x = -x;
        int transformedX;
        int transformedZ;
        switch (rotation) {
            case 0: transformedX = x; transformedZ = z; break;
            case 1: transformedX = -z; transformedZ = x; break;
            case 2: transformedX = -x; transformedZ = -z; break;
            case 3: transformedX = z; transformedZ = -x; break;
            default: throw new AssertionError("normalized rotation");
        }
        return Integer.toString(
            (transformedZ + 1) * 3 + transformedX + 2
        );
    }

    private static String mirrorHandedValue(String value) {
        if ("left".equals(value)) return "right";
        if ("right".equals(value)) return "left";
        if (value.endsWith("_left")) {
            return value.substring(0, value.length() - 5) + "_right";
        }
        if (value.endsWith("_right")) {
            return value.substring(0, value.length() - 6) + "_left";
        }
        return value;
    }

    private static String invertBoolean(String value) {
        if ("true".equals(value)) return "false";
        if ("false".equals(value)) return "true";
        if ("1".equals(value)) return "0";
        if ("0".equals(value)) return "1";
        return value;
    }

    private static String transformCardinal(
        String value,
        int rotation,
        boolean mirrored
    ) {
        int x;
        int z;
        switch (value) {
            case "north": x = 0; z = -1; break;
            case "south": x = 0; z = 1; break;
            case "west": x = -1; z = 0; break;
            case "east": x = 1; z = 0; break;
            default: return value;
        }
        if (mirrored) x = -x;
        int transformedX;
        int transformedZ;
        switch (rotation) {
            case 0: transformedX = x; transformedZ = z; break;
            case 1: transformedX = -z; transformedZ = x; break;
            case 2: transformedX = -x; transformedZ = -z; break;
            case 3: transformedX = z; transformedZ = -x; break;
            default: throw new AssertionError("normalized rotation");
        }
        if (transformedZ < 0) return "north";
        if (transformedZ > 0) return "south";
        return transformedX < 0 ? "west" : "east";
    }

    private static Integer parseInteger(String value) {
        try {
            return Integer.valueOf(value);
        } catch (NumberFormatException ignored) {
            return null;
        }
    }
}
