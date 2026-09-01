package com.m9chko.bedrockrelay;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.BitmapFactory;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Imports only HUD item PNGs from a user-selected official bedrock-samples ZIP.
 * Mojang files stay in app-private storage and are never bundled into the APK.
 */
final class OfficialTexturePack {
    static final String KEY_REVISION = "official_texture_pack_revision";
    private static final String KEY_SOURCE = "official_texture_pack_source";
    private static final String KEY_COUNT = "official_texture_pack_count";
    private static final String KEY_BYTES = "official_texture_pack_bytes";
    private static final String DIRECTORY = "official-item-textures";
    private static final String PENDING_DIRECTORY = DIRECTORY + ".pending";
    private static final String BACKUP_DIRECTORY = DIRECTORY + ".previous";
    private static final String GLINT_FILE = "_enchanted_item_glint.png";
    private static final int MAX_ARCHIVE_ENTRIES = 100_000;
    private static final int MAX_TEXTURES = 8_192;
    private static final int MAX_ENTRY_BYTES = 4 * 1024 * 1024;
    private static final long MAX_TOTAL_BYTES = 64L * 1024L * 1024L;
    private static final int MAX_IMAGE_SIDE = 1_024;
    private static final long STALE_PENDING_MILLIS = 6L * 60L * 60L * 1000L;
    private static final Object IMPORT_LOCK = new Object();
    private static final Object SWAP_LOCK = new Object();

    private final Context context;
    private final SharedPreferences preferences;
    private final File filesDirectory;

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

        int textures = 0;
        int entries = 0;
        long totalBytes = 0;
        boolean glintImported = false;
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
                boolean glint = TexturePackPaths.isEnchantmentGlint(
                    entry.getName()
                );
                if (fileName == null && !glint) continue;
                if (textures >= MAX_TEXTURES && !glint) {
                    throw new IOException("В архиве слишком много item-текстур");
                }
                if (entry.getSize() > MAX_ENTRY_BYTES) {
                    throw new IOException("Слишком крупная PNG-текстура в архиве");
                }

                byte[] png = readEntry(zip, buffer);
                if (!validPng(png)) continue;
                if (totalBytes + png.length > MAX_TOTAL_BYTES) {
                    throw new IOException("Текстуры превышают безопасный лимит 64 МиБ");
                }
                String outputName = glint ? GLINT_FILE : fileName;
                File output = new File(pending, outputName);
                if (output.exists() && !glint) continue;
                try (FileOutputStream stream = new FileOutputStream(output)) {
                    stream.write(png);
                }
                totalBytes += png.length;
                if (glint) glintImported = true;
                else ++textures;
            }
        } catch (Throwable error) {
            deleteRecursively(pending);
            if (error instanceof IOException) throw (IOException) error;
            throw new IOException("Архив повреждён или имеет неверный формат", error);
        }

        if (textures == 0) {
            deleteRecursively(pending);
            throw new IOException(
                "В архиве нет resource_pack/textures/items/*.png. " +
                    "Выберите FULL-архив bedrock-samples."
            );
        }
        swapIn(pending);
        long revision = nextRevision();
        String safeSource = safeSourceName(sourceName);
        preferences.edit()
            .putString(KEY_SOURCE, safeSource)
            .putInt(KEY_COUNT, textures)
            .putLong(KEY_BYTES, totalBytes)
            .putLong(KEY_REVISION, revision)
            .apply();
        return new ImportResult(
            safeSource,
            textures,
            totalBytes,
            glintImported,
            revision
        );
    }

    Status status() {
        File current = directory(DIRECTORY);
        int count = current.isDirectory() ? preferences.getInt(KEY_COUNT, 0) : 0;
        return new Status(
            count > 0,
            preferences.getString(KEY_SOURCE, ""),
            count,
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
                    .remove(KEY_BYTES)
                    .putLong(KEY_REVISION, nextRevision())
                    .apply();
            }
        }
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
        final long bytes;
        final boolean enchantmentGlint;
        final long revision;

        ImportResult(
            String sourceName,
            int textureCount,
            long bytes,
            boolean enchantmentGlint,
            long revision
        ) {
            this.sourceName = sourceName;
            this.textureCount = textureCount;
            this.bytes = bytes;
            this.enchantmentGlint = enchantmentGlint;
            this.revision = revision;
        }
    }

    static final class Status {
        final boolean imported;
        final String sourceName;
        final int textureCount;
        final long bytes;
        final boolean enchantmentGlint;
        final long revision;

        Status(
            boolean imported,
            String sourceName,
            int textureCount,
            long bytes,
            boolean enchantmentGlint,
            long revision
        ) {
            this.imported = imported;
            this.sourceName = sourceName;
            this.textureCount = textureCount;
            this.bytes = bytes;
            this.enchantmentGlint = enchantmentGlint;
            this.revision = revision;
        }

        String description() {
            if (!imported) {
                return "Официальные текстуры не импортированы; HUD использует " +
                    "встроенные силуэты.";
            }
            return String.format(
                Locale.getDefault(),
                "%s\n%d PNG · %.1f МиБ%s",
                sourceName,
                textureCount,
                bytes / (1024f * 1024f),
                enchantmentGlint ? " · glint найден" : ""
            );
        }
    }
}
