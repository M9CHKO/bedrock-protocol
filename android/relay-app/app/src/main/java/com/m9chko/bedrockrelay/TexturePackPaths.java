package com.m9chko.bedrockrelay;

import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/** Pure path/name helpers shared by the importer and local unit tests. */
final class TexturePackPaths {
    private static final String ITEM_MARKER =
        "resource_pack/textures/items/";
    private static final String GLINT_PATH =
        "resource_pack/textures/misc/enchanted_item_glint.png";

    private TexturePackPaths() {}

    static String itemFileName(String archiveEntry) {
        String path = normalizedArchivePath(archiveEntry);
        if (path == null) return null;
        int marker = markerIndex(path, ITEM_MARKER);
        if (marker < 0) return null;
        String relative = path.substring(marker + ITEM_MARKER.length());
        if (relative.isEmpty() || relative.endsWith("/")) return null;
        int separator = relative.lastIndexOf('/');
        String fileName = separator >= 0
            ? relative.substring(separator + 1)
            : relative;
        return safePngName(fileName) ? fileName : null;
    }

    static boolean isEnchantmentGlint(String archiveEntry) {
        String path = normalizedArchivePath(archiveEntry);
        if (path == null) return false;
        int marker = markerIndex(path, GLINT_PATH);
        return marker >= 0 && marker + GLINT_PATH.length() == path.length();
    }

    static List<String> textureCandidates(String registryName) {
        Set<String> names = new LinkedHashSet<>();
        String value = registryName == null
            ? ""
            : registryName.toLowerCase(Locale.ROOT).trim();
        int namespace = value.indexOf(':');
        if (namespace >= 0) value = value.substring(namespace + 1);
        value = value.replaceAll("[^a-z0-9_]+", "_");
        value = trimUnderscores(value);
        if (value.isEmpty()) return new ArrayList<>();

        add(names, value);
        if (value.startsWith("golden_")) {
            add(names, "gold_" + value.substring("golden_".length()));
        }
        if (value.startsWith("wooden_")) {
            add(names, "wood_" + value.substring("wooden_".length()));
        }
        if (value.endsWith("_spawn_egg")) {
            add(names, "spawn_egg_" + value.substring(
                0,
                value.length() - "_spawn_egg".length()
            ));
        }

        switch (value) {
            case "bow": add(names, "bow_standby"); break;
            case "crossbow": add(names, "crossbow_standby"); break;
            case "fishing_rod": add(names, "fishing_rod_uncast"); break;
            case "filled_map": add(names, "map_filled"); break;
            case "map": add(names, "map_empty"); break;
            case "enchanted_book": add(names, "book_enchanted"); break;
            case "writable_book": add(names, "book_writable"); break;
            case "written_book": add(names, "book_written"); break;
            case "golden_apple": add(names, "apple_golden"); break;
            case "enchanted_golden_apple": add(names, "apple_golden"); break;
            case "dragon_breath": add(names, "dragons_breath"); break;
            case "totem_of_undying": add(names, "totem"); break;
            case "firework_rocket": add(names, "fireworks"); break;
            case "firework_star": add(names, "fireworks_charge"); break;
            case "clock": add(names, "clock_item"); break;
            case "compass": add(names, "compass_item"); break;
            case "recovery_compass": add(names, "recovery_compass_item"); break;
            case "cod": add(names, "fish_raw"); break;
            case "cooked_cod": add(names, "fish_cooked"); break;
            case "salmon": add(names, "fish_salmon_raw"); break;
            case "cooked_salmon": add(names, "fish_salmon_cooked"); break;
            case "tropical_fish": add(names, "fish_clownfish_raw"); break;
            case "pufferfish": add(names, "fish_pufferfish_raw"); break;
            default: break;
        }

        List<String> result = new ArrayList<>(names.size());
        for (String name : names) result.add(name + ".png");
        return result;
    }

    private static void add(Set<String> names, String value) {
        if (value != null && !value.isEmpty()) names.add(value);
    }

    private static int markerIndex(String path, String marker) {
        int from = 0;
        while (true) {
            int index = path.indexOf(marker, from);
            if (index < 0) return -1;
            if (index == 0 || path.charAt(index - 1) == '/') return index;
            from = index + 1;
        }
    }

    private static String normalizedArchivePath(String value) {
        if (value == null || value.isEmpty() || value.indexOf('\0') >= 0) {
            return null;
        }
        String path = value.replace('\\', '/').toLowerCase(Locale.ROOT);
        if (path.startsWith("/") || path.matches("^[a-z]:/.*")) return null;
        for (String part : path.split("/")) {
            if (part.equals("..")) return null;
        }
        return path;
    }

    private static boolean safePngName(String value) {
        return value.length() <= 120 &&
            value.matches("[a-z0-9][a-z0-9_.-]*\\.png");
    }

    private static String trimUnderscores(String value) {
        int start = 0;
        int end = value.length();
        while (start < end && value.charAt(start) == '_') ++start;
        while (end > start && value.charAt(end - 1) == '_') --end;
        return value.substring(start, end);
    }
}
