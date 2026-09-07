package com.m9chko.bedrockrelay;

import android.content.Context;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;

/** App-private library shared by the UI and the native .nbt commands. */
final class NbtTransferRepository {
    private static final long MAX_FILE_BYTES = 8L * 1024L * 1024L + 80L;
    private static final byte[] QAZAQ_MAGIC =
        "QZNBTF02".getBytes(StandardCharsets.US_ASCII);

    private final File directory;

    NbtTransferRepository(Context context) {
        directory = new File(context.getApplicationContext().getFilesDir(), "NBT");
    }

    Entry importFile(InputStream source, String sourceName) throws IOException {
        if (source == null) throw new IOException("NBT-файл не открылся");
        String safeSourceName = sourceName == null ? "imported.qznbt" : sourceName;
        String lower = safeSourceName.toLowerCase(Locale.ROOT).trim();
        boolean desktop = lower.endsWith(".qznbt");
        boolean relayJson = lower.endsWith(".cpenbt.json");
        if (!desktop && !relayJson) {
            throw new IOException("Поддерживаются .qznbt и .cpenbt.json");
        }
        String slot = sanitizeSlot(stripExtension(safeSourceName));
        if (!directory.exists() && !directory.mkdirs()) {
            throw new IOException("Не удалось создать папку NBT");
        }
        String extension = desktop ? ".qznbt" : ".cpenbt.json";
        File target = new File(directory, slot + extension);
        File temporary = new File(directory, slot + extension + ".tmp");
        long total = 0L;
        try (FileOutputStream output = new FileOutputStream(temporary, false)) {
            byte[] buffer = new byte[32 * 1024];
            for (int read; (read = source.read(buffer)) >= 0;) {
                if (read == 0) continue;
                total += read;
                if (total > MAX_FILE_BYTES) {
                    throw new IOException("NBT превышает безопасный лимит 8 МиБ");
                }
                output.write(buffer, 0, read);
            }
            output.getFD().sync();
        } catch (Throwable error) {
            //noinspection ResultOfMethodCallIgnored
            temporary.delete();
            if (error instanceof IOException) throw (IOException) error;
            throw new IOException("Не удалось сохранить NBT", error);
        }
        if (total == 0L) {
            //noinspection ResultOfMethodCallIgnored
            temporary.delete();
            throw new IOException("NBT-файл пуст");
        }
        if (desktop && !hasQazaqMagic(temporary)) {
            //noinspection ResultOfMethodCallIgnored
            temporary.delete();
            throw new IOException("Файл .qznbt не имеет формата QZNBTF02");
        }
        try {
            Files.move(
                temporary.toPath(),
                target.toPath(),
                StandardCopyOption.REPLACE_EXISTING,
                StandardCopyOption.ATOMIC_MOVE
            );
        } catch (IOException atomicMoveError) {
            Files.move(
                temporary.toPath(),
                target.toPath(),
                StandardCopyOption.REPLACE_EXISTING
            );
        }
        return new Entry(
            slot,
            target.getName(),
            target.length(),
            target.lastModified(),
            desktop
        );
    }

    List<Entry> list() {
        List<Entry> result = new ArrayList<>();
        File[] files = directory.listFiles();
        if (files == null) return result;
        for (File file : files) {
            if (!file.isFile()) continue;
            String lower = file.getName().toLowerCase(Locale.ROOT);
            boolean desktop = lower.endsWith(".qznbt");
            boolean relayJson = lower.endsWith(".cpenbt.json");
            if (!desktop && !relayJson) continue;
            result.add(new Entry(
                sanitizeSlot(stripExtension(file.getName())),
                file.getName(),
                file.length(),
                file.lastModified(),
                desktop
            ));
        }
        result.sort(Comparator
            .comparingLong((Entry value) -> value.modifiedAtMs)
            .reversed()
            .thenComparing(value -> value.fileName));
        return result;
    }

    private static boolean hasQazaqMagic(File file) throws IOException {
        byte[] header = new byte[QAZAQ_MAGIC.length];
        try (FileInputStream input = new FileInputStream(file)) {
            if (input.read(header) != header.length) return false;
        }
        for (int index = 0; index < header.length; ++index) {
            if (header[index] != QAZAQ_MAGIC[index]) return false;
        }
        return true;
    }

    private static String stripExtension(String value) {
        String result = value == null ? "" : value.trim();
        String lower = result.toLowerCase(Locale.ROOT);
        if (lower.endsWith(".cpenbt.json")) {
            return result.substring(0, result.length() - ".cpenbt.json".length());
        }
        if (lower.endsWith(".qznbt")) {
            return result.substring(0, result.length() - ".qznbt".length());
        }
        return result;
    }

    private static String sanitizeSlot(String value) {
        StringBuilder result = new StringBuilder();
        for (int index = 0; value != null && index < value.length(); ++index) {
            char character = value.charAt(index);
            boolean valid = character >= 'a' && character <= 'z' ||
                character >= 'A' && character <= 'Z' ||
                character >= '0' && character <= '9' ||
                character == '_' || character == '-';
            result.append(valid ? character : '_');
            if (result.length() == 32) break;
        }
        while (result.length() > 0 && result.charAt(result.length() - 1) == '_') {
            result.deleteCharAt(result.length() - 1);
        }
        return result.length() == 0 ? "imported" : result.toString();
    }

    static final class Entry {
        final String slot;
        final String fileName;
        final long sizeBytes;
        final long modifiedAtMs;
        final boolean desktopFormat;

        Entry(
            String slot,
            String fileName,
            long sizeBytes,
            long modifiedAtMs,
            boolean desktopFormat
        ) {
            this.slot = slot;
            this.fileName = fileName;
            this.sizeBytes = sizeBytes;
            this.modifiedAtMs = modifiedAtMs;
            this.desktopFormat = desktopFormat;
        }
    }
}
