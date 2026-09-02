package com.m9chko.bedrockrelay;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Imports HUD item and schematic block PNGs from a user-selected official ZIP.
 * Mojang files stay in app-private storage and are never bundled into the APK.
 */
final class OfficialTexturePack {
    static final String KEY_REVISION = "official_texture_pack_revision";
    private static final String KEY_SOURCE = "official_texture_pack_source";
    private static final String KEY_COUNT = "official_texture_pack_count";
    private static final String KEY_BLOCK_COUNT = "official_texture_pack_block_count";
    private static final String KEY_BYTES = "official_texture_pack_bytes";
    private static final String DIRECTORY = "official-item-textures";
    private static final String PENDING_DIRECTORY = DIRECTORY + ".pending";
    private static final String BACKUP_DIRECTORY = DIRECTORY + ".previous";
    private static final String GLINT_FILE = "_enchanted_item_glint.png";
    private static final String BLOCKS_DIRECTORY = "blocks";
    private static final String BLOCK_INDEX_FILE = "_block_texture_index.json";
    private static final int MAX_ARCHIVE_ENTRIES = 100_000;
    private static final int MAX_TEXTURES = 32_768;
    private static final int MAX_ENTRY_BYTES = 8 * 1024 * 1024;
    private static final long MAX_TOTAL_BYTES = 192L * 1024L * 1024L;
    private static final int MAX_IMAGE_SIDE = 1_024;
    private static final long STALE_PENDING_MILLIS = 6L * 60L * 60L * 1000L;
    private static final Object IMPORT_LOCK = new Object();
    private static final Object SWAP_LOCK = new Object();

    private final Context context;
    private final SharedPreferences preferences;
    private final File filesDirectory;
    private long loadedBlockIndexRevision = Long.MIN_VALUE;
    private Map<String, String> loadedBlockIndex = Collections.emptyMap();

    OfficialTexturePack(Context context) {
        this.context = context.getApplicationContext();
        this.preferences = this.context.getSharedPreferences(
            RelayService.PREFERENCES,
            Context.MODE_PRIVATE
        );
        this.filesDirectory = this.context.getFilesDir();
        recoverInterruptedSwap();
    }

    ImportResult importArchive(InputStream source, String sourceName)
        throws IOException {
        synchronized (IMPORT_LOCK) {
            return importArchiveLocked(source, sourceName);
        }
    }

    private ImportResult importArchiveLocked(InputStream source, String sourceName)
        throws IOException {
        if (source == null) throw new IOException("Не удалось открыть архив");
        File pending = directory(PENDING_DIRECTORY);
        deleteRecursively(pending);
        if (!pending.mkdirs() && !pending.isDirectory()) {
            throw new IOException("Не удалось создать внутреннюю папку текстур");
        }

        int itemTextures = 0;
        int blockTextures = 0;
        int entries = 0;
        long totalBytes = 0;
        boolean glintImported = false;
        byte[] blocksDefinition = null;
        byte[] terrainDefinition = null;
        byte[] buffer = new byte[16 * 1024];
        try (ZipInputStream zip = new ZipInputStream(
            new BufferedInputStream(source, 64 * 1024)
        )) {
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                if (++entries > MAX_ARCHIVE_ENTRIES) {
                    throw new IOException("В архиве слишком много файлов");
                }
                if (entry.isDirectory()) continue;
                String fileName = TexturePackPaths.itemFileName(entry.getName());
                String blockRelative = TexturePackPaths.blockRelativeFileName(
                    entry.getName()
                );
                boolean glint = TexturePackPaths.isEnchantmentGlint(
                    entry.getName()
                );
                boolean blocksJson = TexturePackPaths.isBlocksDefinition(
                    entry.getName()
                );
                boolean terrainJson = TexturePackPaths.isTerrainDefinition(
                    entry.getName()
                );
                if (fileName == null && blockRelative == null && !glint &&
                    !blocksJson && !terrainJson) continue;
                if (itemTextures + blockTextures >= MAX_TEXTURES && !glint &&
                    !blocksJson && !terrainJson) {
                    throw new IOException("В архиве слишком много текстур");
                }
                if (entry.getSize() > MAX_ENTRY_BYTES) {
                    throw new IOException("Слишком крупный файл текстуры в архиве");
                }

                byte[] png = readEntry(zip, buffer);
                if (blocksJson) {
                    blocksDefinition = png;
                    continue;
                }
                if (terrainJson) {
                    terrainDefinition = png;
                    continue;
                }
                if (blockRelative != null && entry.getName().toLowerCase(
                    Locale.ROOT
                ).endsWith(".tga")) {
                    png = convertTgaToPng(png);
                    if (png == null) continue;
                }
                if (!validPng(png)) continue;
                if (totalBytes + png.length > MAX_TOTAL_BYTES) {
                    throw new IOException("Текстуры превышают безопасный лимит 192 МиБ");
                }
                File output;
                if (blockRelative != null) {
                    output = new File(
                        new File(pending, BLOCKS_DIRECTORY),
                        blockRelative
                    );
                    File parent = output.getParentFile();
                    if (parent != null && !parent.isDirectory() &&
                        !parent.mkdirs() && !parent.isDirectory()) {
                        throw new IOException("Не удалось создать папку block PNG");
                    }
                } else {
                    String outputName = glint ? GLINT_FILE : fileName;
                    output = new File(pending, outputName);
                }
                if (output.exists() && !glint) continue;
                try (FileOutputStream stream = new FileOutputStream(output)) {
                    stream.write(png);
                }
                totalBytes += png.length;
                if (glint) glintImported = true;
                else if (blockRelative != null) ++blockTextures;
                else ++itemTextures;
            }
        } catch (Throwable error) {
            deleteRecursively(pending);
            if (error instanceof IOException) throw (IOException) error;
            throw new IOException("Архив повреждён или имеет неверный формат", error);
        }

        if (itemTextures == 0 && blockTextures == 0) {
            deleteRecursively(pending);
            throw new IOException(
                "В архиве нет item или block PNG из resource_pack. " +
                    "Выберите FULL-архив bedrock-samples."
            );
        }
        writeBlockIndex(pending, blocksDefinition, terrainDefinition);
        swapIn(pending);
        long revision = nextRevision();
        String safeSource = safeSourceName(sourceName);
        preferences.edit()
            .putString(KEY_SOURCE, safeSource)
            .putInt(KEY_COUNT, itemTextures)
            .putInt(KEY_BLOCK_COUNT, blockTextures)
            .putLong(KEY_BYTES, totalBytes)
            .putLong(KEY_REVISION, revision)
            .apply();
        return new ImportResult(
            safeSource,
            itemTextures,
            blockTextures,
            totalBytes,
            glintImported,
            revision
        );
    }

    Status status() {
        File current = directory(DIRECTORY);
        int itemCount = current.isDirectory()
            ? preferences.getInt(KEY_COUNT, 0)
            : 0;
        int blockCount = current.isDirectory()
            ? preferences.getInt(KEY_BLOCK_COUNT, 0)
            : 0;
        return new Status(
            itemCount > 0 || blockCount > 0,
            preferences.getString(KEY_SOURCE, ""),
            itemCount,
            blockCount,
            preferences.getLong(KEY_BYTES, 0),
            new File(current, GLINT_FILE).isFile(),
            preferences.getLong(KEY_REVISION, 0)
        );
    }

    long revision() {
        return preferences.getLong(KEY_REVISION, 0);
    }

    File textureFile(String registryName) {
        File current = directory(DIRECTORY);
        if (!current.isDirectory()) return null;
        List<String> candidates = TexturePackPaths.textureCandidates(registryName);
        for (String candidate : candidates) {
            File value = new File(current, candidate);
            if (value.isFile()) return value;
        }
        return null;
    }

    File blockTextureFile(String registryName) {
        return blockTextureFile(registryName, "side");
    }

    File blockTextureFile(String registryName, String face) {
        File current = directory(DIRECTORY);
        File blocks = new File(current, BLOCKS_DIRECTORY);
        if (!blocks.isDirectory()) return null;
        String blockName = TexturePackPaths.normalizedBlockName(registryName);
        if (blockName.isEmpty()) return null;
        Map<String, String> index = blockIndex();
        String mapped = index.get(blockName + "#" + face);
        if (mapped == null) mapped = index.get(blockName);
        File mappedFile = safeBlockFile(blocks, mapped);
        if (mappedFile != null && mappedFile.isFile()) return mappedFile;
        for (String candidate : TexturePackPaths.blockFileCandidates(registryName)) {
            File direct = new File(blocks, candidate);
            if (direct.isFile()) return direct;
            File nested = findByName(blocks, candidate, 0);
            if (nested != null) return nested;
        }
        return null;
    }

    File enchantmentGlintFile() {
        File value = new File(directory(DIRECTORY), GLINT_FILE);
        return value.isFile() ? value : null;
    }

    void clear() {
        synchronized (IMPORT_LOCK) {
            synchronized (SWAP_LOCK) {
                deleteRecursively(directory(PENDING_DIRECTORY));
                deleteRecursively(directory(BACKUP_DIRECTORY));
                deleteRecursively(directory(DIRECTORY));
                preferences.edit()
                    .remove(KEY_SOURCE)
                    .remove(KEY_COUNT)
                    .remove(KEY_BLOCK_COUNT)
                    .remove(KEY_BYTES)
                    .putLong(KEY_REVISION, nextRevision())
                    .apply();
                loadedBlockIndexRevision = Long.MIN_VALUE;
                loadedBlockIndex = Collections.emptyMap();
            }
        }
    }

    private void writeBlockIndex(
        File pending,
        byte[] blocksDefinition,
        byte[] terrainDefinition
    ) throws IOException {
        JSONObject output = new JSONObject();
        try {
            File blocksDirectory = new File(pending, BLOCKS_DIRECTORY);
            Map<String, String> terrain = buildTerrainIndex(
                terrainDefinition,
                blocksDirectory
            );
            for (Map.Entry<String, String> entry : terrain.entrySet()) {
                String normalized = TexturePackPaths.normalizedBlockName(
                    entry.getKey()
                );
                if (!normalized.isEmpty() && !output.has(normalized)) {
                    output.put(normalized, entry.getValue());
                }
            }
            if (blocksDefinition != null) {
                JSONObject blocks = new JSONObject(cleanJson(new String(
                    blocksDefinition,
                    StandardCharsets.UTF_8
                )));
                Iterator<String> names = blocks.keys();
                while (names.hasNext()) {
                    String registryName = names.next();
                    JSONObject definition = blocks.optJSONObject(registryName);
                    if (definition == null) continue;
                    Object textures = definition.opt("textures");
                    if (textures == null) {
                        textures = definition.opt("carried_textures");
                    }
                    String normalized = TexturePackPaths.normalizedBlockName(
                        registryName
                    );
                    String side = resolveTextureReference(
                        terrain,
                        blocksDirectory,
                        textureReferenceForFace(textures, "side")
                    );
                    String top = resolveTextureReference(
                        terrain,
                        blocksDirectory,
                        textureReferenceForFace(textures, "top")
                    );
                    String bottom = resolveTextureReference(
                        terrain,
                        blocksDirectory,
                        textureReferenceForFace(textures, "bottom")
                    );
                    putBlockFaces(output, normalized, side, top, bottom);
                    if ("grass".equals(normalized)) {
                        putBlockFaces(
                            output,
                            "grass_block",
                            side,
                            top,
                            bottom
                        );
                    } else if ("grass_path".equals(normalized)) {
                        putBlockFaces(
                            output,
                            "dirt_path",
                            side,
                            top,
                            bottom
                        );
                    }
                }
            }
        } catch (Throwable error) {
            DiagnosticsLog.appendError(
                context,
                "textures",
                "Block texture mapping was incomplete; filename fallback remains",
                error
            );
        }
        byte[] bytes = output.toString().getBytes(StandardCharsets.UTF_8);
        try (FileOutputStream stream = new FileOutputStream(
            new File(pending, BLOCK_INDEX_FILE)
        )) {
            stream.write(bytes);
        }
    }

    private static Map<String, String> buildTerrainIndex(
        byte[] definition,
        File blocksDirectory
    ) throws Exception {
        Map<String, String> result = new HashMap<>();
        if (definition == null) return result;
        JSONObject root = new JSONObject(cleanJson(new String(
            definition,
            StandardCharsets.UTF_8
        )));
        JSONObject textureData = root.optJSONObject("texture_data");
        if (textureData == null) return result;
        Iterator<String> names = textureData.keys();
        while (names.hasNext()) {
            String name = names.next();
            JSONObject definitionObject = textureData.optJSONObject(name);
            if (definitionObject == null) continue;
            String reference = firstTextureReference(
                definitionObject.opt("textures")
            );
            String relative = existingTextureRelative(
                blocksDirectory,
                reference
            );
            if (relative != null) result.put(name, relative);
        }
        return result;
    }

    private static String firstTextureReference(Object value) {
        if (value instanceof String) return ((String) value).trim();
        if (value instanceof JSONArray) {
            JSONArray values = (JSONArray) value;
            for (int index = 0; index < values.length(); ++index) {
                String found = firstTextureReference(values.opt(index));
                if (found != null && !found.isEmpty()) return found;
            }
            return null;
        }
        if (!(value instanceof JSONObject)) return null;
        JSONObject object = (JSONObject) value;
        String[] preferred = {
            "side", "all", "north", "south", "east", "west", "up", "top",
            "down", "bottom", "path", "textures"
        };
        for (String key : preferred) {
            String found = firstTextureReference(object.opt(key));
            if (found != null && !found.isEmpty()) return found;
        }
        Iterator<String> keys = object.keys();
        while (keys.hasNext()) {
            String found = firstTextureReference(object.opt(keys.next()));
            if (found != null && !found.isEmpty()) return found;
        }
        return null;
    }

    private static String textureReferenceForFace(Object textures, String face) {
        if (!(textures instanceof JSONObject)) {
            return firstTextureReference(textures);
        }
        JSONObject object = (JSONObject) textures;
        String[] keys;
        switch (face) {
            case "top":
                keys = new String[] {"up", "top", "all", "side", "north"};
                break;
            case "bottom":
                keys = new String[] {"down", "bottom", "all", "side", "north"};
                break;
            default:
                keys = new String[] {
                    "side", "all", "north", "south", "east", "west", "up"
                };
                break;
        }
        for (String key : keys) {
            String reference = firstTextureReference(object.opt(key));
            if (reference != null && !reference.isEmpty()) return reference;
        }
        return firstTextureReference(textures);
    }

    private static String resolveTextureReference(
        Map<String, String> terrain,
        File blocksDirectory,
        String reference
    ) {
        String relative = reference == null ? null : terrain.get(reference);
        return relative != null
            ? relative
            : existingTextureRelative(blocksDirectory, reference);
    }

    private static void putBlockFaces(
        JSONObject output,
        String blockName,
        String side,
        String top,
        String bottom
    ) throws Exception {
        if (blockName == null || blockName.isEmpty()) return;
        String fallback = side != null ? side : top != null ? top : bottom;
        if (fallback == null) return;
        output.put(blockName, fallback);
        output.put(blockName + "#side", side == null ? fallback : side);
        output.put(blockName + "#top", top == null ? fallback : top);
        output.put(blockName + "#bottom", bottom == null ? fallback : bottom);
    }

    static String cleanJson(String source) {
        if (source == null || source.isEmpty()) return "";
        StringBuilder output = new StringBuilder(source.length());
        boolean inString = false;
        boolean escaped = false;
        boolean lineComment = false;
        boolean blockComment = false;
        for (int index = 0; index < source.length(); ++index) {
            char value = source.charAt(index);
            char next = index + 1 < source.length()
                ? source.charAt(index + 1)
                : '\0';
            if (lineComment) {
                if (value == '\n' || value == '\r') {
                    lineComment = false;
                    output.append(value);
                }
                continue;
            }
            if (blockComment) {
                if (value == '*' && next == '/') {
                    blockComment = false;
                    ++index;
                } else if (value == '\n' || value == '\r') {
                    output.append(value);
                }
                continue;
            }
            if (inString) {
                output.append(value);
                if (escaped) {
                    escaped = false;
                } else if (value == '\\') {
                    escaped = true;
                } else if (value == '"') {
                    inString = false;
                }
                continue;
            }
            if (value == '"') {
                inString = true;
                output.append(value);
            } else if (value == '/' && next == '/') {
                lineComment = true;
                ++index;
            } else if (value == '/' && next == '*') {
                blockComment = true;
                ++index;
            } else {
                output.append(value);
            }
        }
        return output.toString();
    }

    private static String existingTextureRelative(
        File blocksDirectory,
        String reference
    ) {
        if (reference == null || reference.trim().isEmpty()) return null;
        String relative = reference.trim().toLowerCase(Locale.ROOT)
            .replace('\\', '/');
        while (relative.startsWith("/")) relative = relative.substring(1);
        if (relative.startsWith("textures/blocks/")) {
            relative = relative.substring("textures/blocks/".length());
        } else if (relative.startsWith("blocks/")) {
            relative = relative.substring("blocks/".length());
        } else if (relative.startsWith("textures/")) {
            return null;
        }
        if (!relative.endsWith(".png")) relative += ".png";
        String checked = TexturePackPaths.blockRelativeFileName(
            "resource_pack/textures/blocks/" + relative
        );
        if (checked == null) return null;
        File file = new File(blocksDirectory, checked);
        return file.isFile() ? checked : null;
    }

    private synchronized Map<String, String> blockIndex() {
        long currentRevision = revision();
        if (loadedBlockIndexRevision == currentRevision) {
            return loadedBlockIndex;
        }
        Map<String, String> result = new HashMap<>();
        File source = new File(directory(DIRECTORY), BLOCK_INDEX_FILE);
        if (source.isFile() && source.length() <= MAX_ENTRY_BYTES) {
            try (InputStream input = new BufferedInputStream(
                new FileInputStream(source)
            )) {
                ByteArrayOutputStream bytes = new ByteArrayOutputStream(
                    (int) Math.max(32L, source.length())
                );
                byte[] buffer = new byte[8 * 1024];
                int count;
                while ((count = input.read(buffer)) != -1) {
                    if (bytes.size() + count > MAX_ENTRY_BYTES) break;
                    bytes.write(buffer, 0, count);
                }
                JSONObject root = new JSONObject(bytes.toString(
                    StandardCharsets.UTF_8.name()
                ));
                Iterator<String> keys = root.keys();
                while (keys.hasNext()) {
                    String key = keys.next();
                    String value = root.optString(key, "");
                    if (!key.isEmpty() && !value.isEmpty()) {
                        result.put(key, value);
                    }
                }
            } catch (Throwable error) {
                DiagnosticsLog.appendError(
                    context,
                    "textures",
                    "Could not read block texture index",
                    error
                );
            }
        }
        loadedBlockIndexRevision = currentRevision;
        loadedBlockIndex = result;
        return loadedBlockIndex;
    }

    private static File safeBlockFile(File root, String relative) {
        if (relative == null || relative.isEmpty()) return null;
        String checked = TexturePackPaths.blockRelativeFileName(
            "resource_pack/textures/blocks/" + relative
        );
        return checked == null ? null : new File(root, checked);
    }

    private static File findByName(File directory, String name, int depth) {
        if (directory == null || depth > 4) return null;
        File[] children = directory.listFiles();
        if (children == null) return null;
        for (File child : children) {
            if (child.isFile() && name.equals(child.getName())) return child;
        }
        for (File child : children) {
            if (!child.isDirectory()) continue;
            File found = findByName(child, name, depth + 1);
            if (found != null) return found;
        }
        return null;
    }

    private byte[] readEntry(ZipInputStream zip, byte[] buffer) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream(16 * 1024);
        int count;
        while ((count = zip.read(buffer)) != -1) {
            if (output.size() + count > MAX_ENTRY_BYTES) {
                throw new IOException("Слишком крупная PNG-текстура в архиве");
            }
            output.write(buffer, 0, count);
        }
        return output.toByteArray();
    }

    private static byte[] convertTgaToPng(byte[] source) {
        if (source == null || source.length < 18) return null;
        int idLength = source[0] & 0xff;
        int colorMapType = source[1] & 0xff;
        int imageType = source[2] & 0xff;
        int width = littleUnsignedShort(source, 12);
        int height = littleUnsignedShort(source, 14);
        int depth = source[16] & 0xff;
        boolean grayscale = imageType == 3;
        boolean raw = imageType == 2 || grayscale;
        boolean rle = imageType == 10;
        int bytesPerPixel = grayscale ? 1 : depth / 8;
        if (colorMapType != 0 || (!raw && !rle) || width < 1 || height < 1 ||
            width > MAX_IMAGE_SIDE || height > MAX_IMAGE_SIDE ||
            (!grayscale && bytesPerPixel != 3 && bytesPerPixel != 4)) {
            return null;
        }
        int position = 18 + idLength;
        if (position < 18 || position > source.length) return null;
        int pixelCount = width * height;
        int[] pixels = new int[pixelCount];
        int sequence = 0;
        boolean topOrigin = (source[17] & 0x20) != 0;
        boolean rightOrigin = (source[17] & 0x10) != 0;
        try {
            if (rle) {
                while (sequence < pixelCount) {
                    int header = source[position++] & 0xff;
                    int count = (header & 0x7f) + 1;
                    if (sequence + count > pixelCount) return null;
                    if ((header & 0x80) != 0) {
                        int color = tgaPixel(
                            source,
                            position,
                            bytesPerPixel,
                            false
                        );
                        position += bytesPerPixel;
                        for (int index = 0; index < count; ++index) {
                            putTgaPixel(
                                pixels,
                                sequence++,
                                width,
                                height,
                                topOrigin,
                                rightOrigin,
                                color
                            );
                        }
                    } else {
                        for (int index = 0; index < count; ++index) {
                            int color = tgaPixel(
                                source,
                                position,
                                bytesPerPixel,
                                false
                            );
                            position += bytesPerPixel;
                            putTgaPixel(
                                pixels,
                                sequence++,
                                width,
                                height,
                                topOrigin,
                                rightOrigin,
                                color
                            );
                        }
                    }
                }
            } else {
                while (sequence < pixelCount) {
                    int color = tgaPixel(
                        source,
                        position,
                        bytesPerPixel,
                        grayscale
                    );
                    position += bytesPerPixel;
                    putTgaPixel(
                        pixels,
                        sequence++,
                        width,
                        height,
                        topOrigin,
                        rightOrigin,
                        color
                    );
                }
            }
        } catch (IndexOutOfBoundsException error) {
            return null;
        }
        Bitmap bitmap = Bitmap.createBitmap(
            pixels,
            width,
            height,
            Bitmap.Config.ARGB_8888
        );
        ByteArrayOutputStream png = new ByteArrayOutputStream(
            Math.min(MAX_ENTRY_BYTES, Math.max(1_024, source.length))
        );
        boolean compressed = bitmap.compress(Bitmap.CompressFormat.PNG, 100, png);
        bitmap.recycle();
        return compressed && png.size() <= MAX_ENTRY_BYTES
            ? png.toByteArray()
            : null;
    }

    private static int littleUnsignedShort(byte[] source, int offset) {
        return (source[offset] & 0xff) | ((source[offset + 1] & 0xff) << 8);
    }

    private static int tgaPixel(
        byte[] source,
        int offset,
        int bytesPerPixel,
        boolean grayscale
    ) {
        if (grayscale) {
            int value = source[offset] & 0xff;
            return 0xff000000 | (value << 16) | (value << 8) | value;
        }
        int blue = source[offset] & 0xff;
        int green = source[offset + 1] & 0xff;
        int red = source[offset + 2] & 0xff;
        int alpha = bytesPerPixel == 4 ? source[offset + 3] & 0xff : 0xff;
        return (alpha << 24) | (red << 16) | (green << 8) | blue;
    }

    private static void putTgaPixel(
        int[] pixels,
        int sequence,
        int width,
        int height,
        boolean topOrigin,
        boolean rightOrigin,
        int color
    ) {
        int sourceX = sequence % width;
        int sourceY = sequence / width;
        int x = rightOrigin ? width - 1 - sourceX : sourceX;
        int y = topOrigin ? sourceY : height - 1 - sourceY;
        pixels[y * width + x] = color;
    }

    private static boolean validPng(byte[] value) {
        if (value.length < 24 ||
            (value[0] & 0xff) != 0x89 || value[1] != 0x50 ||
            value[2] != 0x4e || value[3] != 0x47) {
            return false;
        }
        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inJustDecodeBounds = true;
        BitmapFactory.decodeByteArray(value, 0, value.length, options);
        return options.outWidth > 0 && options.outHeight > 0 &&
            options.outWidth <= MAX_IMAGE_SIDE &&
            options.outHeight <= MAX_IMAGE_SIDE;
    }

    private void swapIn(File pending) throws IOException {
        synchronized (SWAP_LOCK) {
            File current = directory(DIRECTORY);
            File previous = directory(BACKUP_DIRECTORY);
            deleteRecursively(previous);
            boolean movedCurrent = false;
            if (current.exists()) {
                movedCurrent = current.renameTo(previous);
                if (!movedCurrent) {
                    deleteRecursively(pending);
                    throw new IOException("Не удалось заменить прежний набор текстур");
                }
            }
            if (!pending.renameTo(current)) {
                if (movedCurrent) previous.renameTo(current);
                deleteRecursively(pending);
                throw new IOException("Не удалось активировать импортированные текстуры");
            }
            deleteRecursively(previous);
        }
    }

    private void recoverInterruptedSwap() {
        synchronized (SWAP_LOCK) {
            File current = directory(DIRECTORY);
            File previous = directory(BACKUP_DIRECTORY);
            if (!current.exists() && previous.isDirectory()) {
                previous.renameTo(current);
            }
            File pending = directory(PENDING_DIRECTORY);
            if (pending.exists() &&
                System.currentTimeMillis() - pending.lastModified() >
                    STALE_PENDING_MILLIS) {
                deleteRecursively(pending);
            }
            if (current.exists()) deleteRecursively(previous);
        }
    }

    private long nextRevision() {
        long previous = preferences.getLong(KEY_REVISION, 0);
        long now = System.currentTimeMillis();
        return Math.max(now, previous + 1);
    }

    private File directory(String name) {
        return new File(filesDirectory, name);
    }

    private static String safeSourceName(String value) {
        String result = value == null ? "bedrock-samples FULL" : value.trim();
        result = result.replaceAll("[\\p{Cntrl}]", "");
        if (result.isEmpty()) result = "bedrock-samples FULL";
        if (result.length() > 120) result = result.substring(0, 120);
        return result;
    }

    private static void deleteRecursively(File value) {
        if (value == null || !value.exists()) return;
        File[] children = value.listFiles();
        if (children != null) {
            for (File child : children) deleteRecursively(child);
        }
        value.delete();
    }

    static final class ImportResult {
        final String sourceName;
        final int textureCount;
        final int itemTextureCount;
        final int blockTextureCount;
        final long bytes;
        final boolean enchantmentGlint;
        final long revision;

        ImportResult(
            String sourceName,
            int itemTextureCount,
            int blockTextureCount,
            long bytes,
            boolean enchantmentGlint,
            long revision
        ) {
            this.sourceName = sourceName;
            this.itemTextureCount = itemTextureCount;
            this.blockTextureCount = blockTextureCount;
            this.textureCount = itemTextureCount + blockTextureCount;
            this.bytes = bytes;
            this.enchantmentGlint = enchantmentGlint;
            this.revision = revision;
        }
    }

    static final class Status {
        final boolean imported;
        final String sourceName;
        final int textureCount;
        final int itemTextureCount;
        final int blockTextureCount;
        final long bytes;
        final boolean enchantmentGlint;
        final long revision;

        Status(
            boolean imported,
            String sourceName,
            int itemTextureCount,
            int blockTextureCount,
            long bytes,
            boolean enchantmentGlint,
            long revision
        ) {
            this.imported = imported;
            this.sourceName = sourceName;
            this.itemTextureCount = itemTextureCount;
            this.blockTextureCount = blockTextureCount;
            this.textureCount = itemTextureCount + blockTextureCount;
            this.bytes = bytes;
            this.enchantmentGlint = enchantmentGlint;
            this.revision = revision;
        }

        String description() {
            if (!imported) {
                return "Официальные текстуры не импортированы; HUD и схемы " +
                    "используют встроенную графику.";
            }
            return String.format(
                Locale.getDefault(),
                "%s\nпредметы: %d · блоки: %d PNG · %.1f МиБ%s",
                sourceName,
                itemTextureCount,
                blockTextureCount,
                bytes / (1024f * 1024f),
                enchantmentGlint ? " · glint найден" : ""
            );
        }
    }
}
