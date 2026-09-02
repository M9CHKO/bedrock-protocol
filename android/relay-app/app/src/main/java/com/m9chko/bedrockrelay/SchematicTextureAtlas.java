package com.m9chko.bedrockrelay;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;

import java.io.File;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Small original pixel-art texture set embedded as code. No Mojang artwork is
 * copied into the APK; block names only select a diagnostic material style.
 */
final class SchematicTextureAtlas {
    private static final int SIDE = 16;
    private final Map<String, Bitmap> fallbackTextures = new HashMap<>();
    private final Map<String, Bitmap> officialTextures = new HashMap<>();
    private final Set<String> missingOfficial = new HashSet<>();
    private final Set<String> pendingOfficial = new HashSet<>();
    private final OfficialTexturePack officialPack;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final ExecutorService decoder = Executors.newSingleThreadExecutor(
        runnable -> {
            Thread thread = new Thread(runnable, "schematic-textures");
            thread.setDaemon(true);
            return thread;
        }
    );
    private final Runnable invalidation;
    private long loadedRevision = Long.MIN_VALUE;
    private long nextRevisionCheckAtMs;
    private long generation;
    private boolean closed;

    SchematicTextureAtlas(Context context, Runnable invalidation) {
        officialPack = new OfficialTexturePack(context);
        this.invalidation = invalidation;
    }

    Bitmap textureFor(String blockState) {
        return textureFor(blockState, "side");
    }

    Bitmap textureFor(String blockState, String face) {
        long now = SystemClock.uptimeMillis();
        if (loadedRevision == Long.MIN_VALUE || now >= nextRevisionCheckAtMs) {
            nextRevisionCheckAtMs = now + 1_000L;
            long revision = officialPack.revision();
            if (revision != loadedRevision) {
                clearTextures();
                loadedRevision = revision;
            }
        }
        String blockName = TexturePackPaths.normalizedBlockName(blockState);
        String officialKey = "official:" + blockName + ":" + face;
        Bitmap existing = officialTextures.get(officialKey);
        if (existing != null) return existing;
        if (!closed && !missingOfficial.contains(officialKey) &&
            pendingOfficial.add(officialKey)) {
            scheduleOfficialTexture(
                officialKey,
                blockState,
                face,
                generation
            );
        }
        String key = textureKey(blockState);
        existing = fallbackTextures.get(key);
        if (existing != null) return existing;
        Bitmap created = createTexture(key);
        fallbackTextures.put(key, created);
        return created;
    }

    int officialTextureCount() {
        return officialTextures.size();
    }

    int pendingTextureCount() {
        return pendingOfficial.size();
    }

    void close() {
        closed = true;
        ++generation;
        decoder.shutdownNow();
        clearTextures();
    }

    private void scheduleOfficialTexture(
        String key,
        String blockState,
        String face,
        long requestedGeneration
    ) {
        decoder.execute(() -> {
            Bitmap decoded = null;
            try {
                File source = officialPack.blockTextureFile(blockState, face);
                decoded = decodeOfficial(source);
            } catch (Throwable ignored) {
            }
            Bitmap result = decoded;
            mainHandler.post(() -> {
                if (closed || generation != requestedGeneration) {
                    if (result != null && !result.isRecycled()) result.recycle();
                    return;
                }
                pendingOfficial.remove(key);
                if (result == null) {
                    missingOfficial.add(key);
                    return;
                }
                Bitmap previous = officialTextures.put(key, result);
                if (previous != null && previous != result &&
                    !previous.isRecycled()) {
                    previous.recycle();
                }
                if (invalidation != null) invalidation.run();
            });
        });
    }

    private void clearTextures() {
        ++generation;
        for (Bitmap bitmap : fallbackTextures.values()) {
            if (bitmap != null && !bitmap.isRecycled()) bitmap.recycle();
        }
        for (Bitmap bitmap : officialTextures.values()) {
            if (bitmap != null && !bitmap.isRecycled()) bitmap.recycle();
        }
        fallbackTextures.clear();
        officialTextures.clear();
        missingOfficial.clear();
        pendingOfficial.clear();
    }

    static String textureKey(String state) {
        String value = state == null
            ? ""
            : state.toLowerCase(Locale.ROOT);
        int properties = value.indexOf('[');
        if (properties >= 0) value = value.substring(0, properties);
        if (value.contains("water") || value.contains("bubble_column")) {
            return "water";
        }
        if (value.contains("lava") || value.contains("magma")) return "lava";
        if (value.contains("grass_block") || value.contains("moss")) return "grass";
        if (value.contains("dirt") || value.contains("mud")) return "dirt";
        if (value.contains("sand") || value.contains("end_stone")) return "sand";
        if (value.contains("snow") || value.contains("quartz") ||
            value.contains("calcite")) return "snow";
        if (value.contains("glass") || value.contains("ice")) return "glass";
        if (value.contains("leaves") || value.contains("vine") ||
            value.contains("azalea")) return "leaves";
        if (value.contains("log") || value.contains("stem") ||
            value.contains("hyphae")) return "log";
        if (value.contains("planks") || value.contains("wood") ||
            value.contains("bookshelf") || value.contains("chest")) {
            return "planks";
        }
        if (value.contains("brick")) return "bricks";
        if (value.contains("deepslate") || value.contains("blackstone") ||
            value.contains("basalt")) return "dark_stone";
        if (value.contains("netherrack") || value.contains("nether_wart")) {
            return "nether";
        }
        if (value.contains("ore")) return "ore:" + oreColor(value);
        String dye = dyeColor(value);
        if (dye != null && (value.contains("wool") ||
            value.contains("concrete") || value.contains("terracotta"))) {
            return "color:" + dye;
        }
        if (value.contains("stone") || value.contains("cobble") ||
            value.contains("andesite") || value.contains("granite") ||
            value.contains("diorite") || value.startsWith("legacy:")) {
            return "stone";
        }
        return "generic:" + Integer.toHexString(value.hashCode());
    }

    private static Bitmap createTexture(String key) {
        int[] pixels = new int[SIDE * SIDE];
        int base = baseColor(key);
        int seed = key.hashCode() ^ 0x51f15e5d;
        for (int y = 0; y < SIDE; ++y) {
            for (int x = 0; x < SIDE; ++x) {
                seed = seed * 1664525 + 1013904223;
                int noise = ((seed >>> 24) & 0x1f) - 15;
                int color = shade(base, noise);
                if ("planks".equals(key)) {
                    int board = y / 4;
                    color = shade(base, board % 2 == 0 ? 10 : -6);
                    if ((y & 3) == 0) color = shade(base, -34);
                    if ((x + (board & 1) * 8) % 8 == 0) {
                        color = shade(color, -20);
                    }
                } else if ("bricks".equals(key)) {
                    int row = y / 4;
                    boolean mortar = y % 4 == 0 ||
                        (x + (row & 1) * 4) % 8 == 0;
                    color = mortar ? 0xffb8a99b : shade(base, noise / 2);
                } else if ("log".equals(key)) {
                    color = shade(base, ((x / 3) & 1) == 0 ? -12 : 9);
                    if (x == 3 || x == 10) color = shade(base, -35);
                } else if ("leaves".equals(key)) {
                    color = shade(base, noise * 2);
                    if (((x * 5 + y * 3 + seed) & 15) == 0) {
                        color = 0x44365f35;
                    }
                } else if ("grass".equals(key)) {
                    color = shade(base, noise * 2);
                    if ((x + y * 3) % 11 == 0) color = 0xff4e8e43;
                } else if ("water".equals(key)) {
                    color = shade(base, ((x + y * 2) % 7 == 0) ? 25 : noise);
                    if ((y % 5) == 0 && (x + y) % 3 != 0) {
                        color = 0xff5bb8ef;
                    }
                } else if ("lava".equals(key)) {
                    color = ((x * 3 + y * 5 + (seed >>> 28)) & 7) < 2
                        ? 0xffffd54a
                        : (((x + y) & 3) == 0 ? 0xffff6b22 : 0xffd94118);
                } else if ("glass".equals(key)) {
                    boolean frame = x == 0 || y == 0 || x == SIDE - 1 ||
                        y == SIDE - 1 || x == y || x + y == SIDE - 1;
                    color = frame ? 0xff9cebf2 : 0x448adce8;
                } else if (key.startsWith("ore:")) {
                    color = shade(0xff858b91, noise);
                    if (((x * 7 + y * 11 + seed) & 15) < 3) {
                        color = orePixelColor(key.substring(4));
                    }
                } else if (key.startsWith("color:")) {
                    color = shade(base, noise / 3);
                    if ((x + y * 2) % 13 == 0) color = shade(color, -16);
                }
                pixels[y * SIDE + x] = color;
            }
        }
        Bitmap bitmap = Bitmap.createBitmap(
            SIDE,
            SIDE,
            Bitmap.Config.ARGB_8888
        );
        bitmap.setPixels(pixels, 0, SIDE, 0, 0, SIDE, SIDE);
        return bitmap;
    }

    private static Bitmap decodeOfficial(File source) {
        if (source == null || !source.isFile()) return null;
        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inScaled = false;
        options.inPreferredConfig = Bitmap.Config.ARGB_8888;
        Bitmap decoded = BitmapFactory.decodeFile(source.getAbsolutePath(), options);
        if (decoded == null || decoded.getWidth() <= 0 || decoded.getHeight() <= 0) {
            if (decoded != null) decoded.recycle();
            return null;
        }
        int side = Math.min(decoded.getWidth(), decoded.getHeight());
        if (decoded.getWidth() == side && decoded.getHeight() == side) {
            return decoded;
        }
        Bitmap firstFrame = Bitmap.createBitmap(decoded, 0, 0, side, side);
        if (firstFrame != decoded) decoded.recycle();
        return firstFrame;
    }

    private static int baseColor(String key) {
        switch (key) {
            case "water": return 0xff398dd7;
            case "lava": return 0xfff05a20;
            case "grass": return 0xff63ad52;
            case "dirt": return 0xff8a5c3b;
            case "sand": return 0xffd8c688;
            case "snow": return 0xffe8f0f4;
            case "glass": return 0xff8edce6;
            case "leaves": return 0xff427d3d;
            case "log": return 0xff855a36;
            case "planks": return 0xffad7545;
            case "bricks": return 0xffa95d4d;
            case "dark_stone": return 0xff414750;
            case "nether": return 0xff7d3439;
            case "stone": return 0xff85898d;
            default:
                if (key.startsWith("color:")) {
                    return dyeRgb(key.substring(6));
                }
                int hash = key.hashCode();
                return Color.HSVToColor(new float[] {
                    Math.floorMod(hash, 360),
                    0.42f,
                    0.82f
                });
        }
    }

    private static String oreColor(String value) {
        if (value.contains("diamond")) return "diamond";
        if (value.contains("emerald")) return "emerald";
        if (value.contains("redstone")) return "redstone";
        if (value.contains("lapis")) return "lapis";
        if (value.contains("gold")) return "gold";
        if (value.contains("copper")) return "copper";
        if (value.contains("coal")) return "coal";
        return "iron";
    }

    private static int orePixelColor(String ore) {
        switch (ore) {
            case "diamond": return 0xff55dbe0;
            case "emerald": return 0xff37c86d;
            case "redstone": return 0xffd83b42;
            case "lapis": return 0xff376bc4;
            case "gold": return 0xffffcf45;
            case "copper": return 0xffd47c57;
            case "coal": return 0xff25282b;
            default: return 0xffd6c5aa;
        }
    }

    private static String dyeColor(String value) {
        String[] names = {
            "light_blue", "light_gray", "magenta", "orange", "yellow",
            "lime", "pink", "gray", "cyan", "purple", "blue", "brown",
            "green", "red", "black", "white"
        };
        for (String name : names) {
            if (value.contains(":" + name + "_") ||
                value.endsWith("_" + name)) return name;
        }
        return null;
    }

    private static int dyeRgb(String dye) {
        switch (dye) {
            case "orange": return 0xffe8892d;
            case "magenta": return 0xffb84bb7;
            case "light_blue": return 0xff55a9d6;
            case "yellow": return 0xffe7cf3b;
            case "lime": return 0xff74ba36;
            case "pink": return 0xffd889a7;
            case "gray": return 0xff555a5e;
            case "light_gray": return 0xffa9aaa5;
            case "cyan": return 0xff318a91;
            case "purple": return 0xff7f45a2;
            case "blue": return 0xff3f55a3;
            case "brown": return 0xff70462d;
            case "green": return 0xff526b2f;
            case "red": return 0xffa83d3d;
            case "black": return 0xff26272a;
            default: return 0xffe7e7df;
        }
    }

    private static int shade(int color, int amount) {
        int alpha = Color.alpha(color);
        int red = clampColor(Color.red(color) + amount);
        int green = clampColor(Color.green(color) + amount);
        int blue = clampColor(Color.blue(color) + amount);
        return Color.argb(alpha, red, green, blue);
    }

    private static int clampColor(int value) {
        return Math.max(0, Math.min(255, value));
    }
}
