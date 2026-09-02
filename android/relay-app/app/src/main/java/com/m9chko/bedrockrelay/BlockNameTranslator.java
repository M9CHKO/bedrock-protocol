package com.m9chko.bedrockrelay;

import android.content.Context;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Iterator;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.HashMap;
import java.util.Set;

/** Java schematic block names translated to Bedrock resource-pack names. */
final class BlockNameTranslator {
    private static final String ASSET =
        "minecraft-data/1.21.100/block-name-j2b.json";

    private final Context context;
    private volatile Map<String, List<String>> aliases;

    BlockNameTranslator(Context context) {
        this.context = context.getApplicationContext();
    }

    List<String> bedrockCandidates(String state) {
        String javaName = TexturePackPaths.normalizedBlockName(state);
        if (javaName.isEmpty()) return Collections.emptyList();
        Set<String> result = new LinkedHashSet<>();
        result.add(javaName);
        List<String> mapped = loadAliases().get(javaName);
        if (mapped != null) result.addAll(mapped);
        addHeuristicAliases(javaName, result);
        return new ArrayList<>(result);
    }

    private Map<String, List<String>> loadAliases() {
        Map<String, List<String>> current = aliases;
        if (current != null) return current;
        synchronized (this) {
            if (aliases != null) return aliases;
            Map<String, List<String>> loaded = new HashMap<>();
            try (InputStream input = context.getAssets().open(ASSET)) {
                ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                byte[] buffer = new byte[16 * 1024];
                int count;
                while ((count = input.read(buffer)) >= 0) {
                    if (count > 0) bytes.write(buffer, 0, count);
                }
                JSONObject root = new JSONObject(bytes.toString(
                    StandardCharsets.UTF_8.name()
                ));
                Iterator<String> names = root.keys();
                while (names.hasNext()) {
                    String name = names.next();
                    JSONArray values = root.optJSONArray(name);
                    if (values == null) continue;
                    List<String> translated = new ArrayList<>(values.length());
                    for (int index = 0; index < values.length(); ++index) {
                        String value = values.optString(index, "")
                            .toLowerCase(Locale.ROOT)
                            .trim();
                        if (!value.isEmpty() && !translated.contains(value)) {
                            translated.add(value);
                        }
                    }
                    if (!translated.isEmpty()) loaded.put(name, translated);
                }
            } catch (Throwable error) {
                DiagnosticsLog.appendError(
                    context,
                    "schematics",
                    "Java-to-Bedrock block-name table could not be loaded",
                    error
                );
            }
            aliases = loaded;
            return loaded;
        }
    }

    static void addHeuristicAliases(String name, Set<String> output) {
        if (name == null || output == null) return;
        switch (name) {
            case "bricks": output.add("brick_block"); break;
            case "grass": output.add("grass_block"); break;
            case "short_grass": output.add("tallgrass"); break;
            case "dirt_path": output.add("grass_path"); break;
            case "oak_sign": output.add("standing_sign"); break;
            case "oak_wall_sign": output.add("wall_sign"); break;
            case "oak_fence_gate": output.add("fence_gate"); break;
            case "redstone_lamp": output.add("redstone_lamp_off"); break;
            case "lit_redstone_lamp": output.add("redstone_lamp_on"); break;
            case "jack_o_lantern": output.add("lit_pumpkin"); break;
            case "nether_portal": output.add("portal"); break;
            default: break;
        }
        if (name.endsWith("_wall_banner")) output.add("wall_banner");
        else if (name.endsWith("_banner")) output.add("standing_banner");
        if (name.endsWith("_wall_sign")) output.add("wall_sign");
        else if (name.endsWith("_sign")) output.add("standing_sign");
        if (name.endsWith("_fence_gate")) output.add("fence_gate");
    }
}
