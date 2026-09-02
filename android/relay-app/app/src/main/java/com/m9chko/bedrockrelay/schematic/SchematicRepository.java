package com.m9chko.bedrockrelay.schematic;

import android.content.Context;
import android.content.SharedPreferences;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.EOFException;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.UUID;

/** Private app storage for imported schematics and the currently selected entry. */
public final class SchematicRepository {
    private static final int MAGIC = 0x43505343; // CPSC
    private static final int VERSION = 1;
    private static final int MAX_STRING_BYTES = 1_048_576;
    private static final String PREFERENCES = "schematic_repository";
    private static final String ACTIVE_ID = "active_id";

    private final File directory;
    private final SharedPreferences preferences;
    private final SchematicImporter importer = new SchematicImporter();

    public SchematicRepository(Context context) {
        Context app = context.getApplicationContext();
        directory = new File(app.getFilesDir(), "schematics");
        preferences = app.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE);
    }

    public ImportResult importAndActivate(
        InputStream input,
        String sourceName
    ) throws IOException {
        SchematicModel model = importer.importStream(input, sourceName);
        ensureDirectory();
        String id = UUID.randomUUID().toString();
        File temporary = new File(directory, id + ".tmp");
        File target = fileFor(id);
        boolean committed = false;
        try {
            writeModel(temporary, model);
            if (!temporary.renameTo(target)) {
                throw new IOException("Не удалось сохранить импортированную схему");
            }
            committed = true;
            preferences.edit().putString(ACTIVE_ID, id).apply();
            return new ImportResult(id, model);
        } finally {
            if (!committed && temporary.exists()) {
                // This is an app-private incomplete temporary file.
                temporary.delete();
            }
        }
    }

    public SchematicModel loadActive() throws IOException {
        String id = activeId();
        if (id.isEmpty()) return null;
        File file = fileFor(id);
        if (!file.isFile()) {
            preferences.edit().remove(ACTIVE_ID).apply();
            return null;
        }
        return readModel(file);
    }

    public String activeId() {
        String value = preferences.getString(ACTIVE_ID, "");
        return value == null ? "" : value;
    }

    public boolean activate(String id) {
        if (!validId(id) || !fileFor(id).isFile()) return false;
        preferences.edit().putString(ACTIVE_ID, id).apply();
        return true;
    }

    public boolean delete(String id) {
        if (!validId(id)) return false;
        File target = fileFor(id);
        boolean removed = !target.exists() || target.delete();
        if (removed && id.equals(activeId())) {
            preferences.edit().remove(ACTIVE_ID).apply();
        }
        return removed;
    }

    public List<Entry> list() {
        List<Entry> result = new ArrayList<>();
        File[] files = directory.listFiles((parent, name) ->
            name.endsWith(".cps") && validId(name.substring(0, name.length() - 4))
        );
        if (files == null) return result;
        String active = activeId();
        for (File file : files) {
            String id = file.getName().substring(0, file.getName().length() - 4);
            try {
                Header header = readHeader(file);
                result.add(new Entry(
                    id,
                    header.sourceName,
                    header.format,
                    header.sizeX,
                    header.sizeY,
                    header.sizeZ,
                    header.nonAirBlocks,
                    file.lastModified(),
                    id.equals(active)
                ));
            } catch (IOException ignored) {
                // A broken entry remains isolated and can be removed manually.
            }
        }
        result.sort(Comparator.comparingLong((Entry value) -> value.modifiedAtMs)
            .reversed());
        return result;
    }

    private void ensureDirectory() throws IOException {
        if (!directory.isDirectory() && !directory.mkdirs() &&
            !directory.isDirectory()) {
            throw new IOException("Не удалось создать хранилище схем");
        }
    }

    private File fileFor(String id) {
        return new File(directory, id + ".cps");
    }

    private static boolean validId(String id) {
        if (id == null || id.length() != 36) return false;
        try {
            UUID.fromString(id);
            return true;
        } catch (IllegalArgumentException ignored) {
            return false;
        }
    }

    private static void writeModel(File target, SchematicModel model)
        throws IOException {
        try (DataOutputStream output = new DataOutputStream(
            new BufferedOutputStream(new FileOutputStream(target)))) {
            output.writeInt(MAGIC);
            output.writeInt(VERSION);
            writeString(output, model.sourceName());
            writeString(output, model.format());
            output.writeInt(model.sizeX());
            output.writeInt(model.sizeY());
            output.writeInt(model.sizeZ());
            output.writeInt(model.nonAirBlocks());
            output.writeInt(model.paletteSize());
            for (String state : model.paletteInternal()) {
                writeString(output, state);
            }
            int[] blocks = model.blocksCopy();
            output.writeInt(blocks.length);
            for (int block : blocks) output.writeInt(block);
        }
    }

    private static SchematicModel readModel(File source) throws IOException {
        try (DataInputStream input = new DataInputStream(
            new BufferedInputStream(new FileInputStream(source)))) {
            Header header = readHeader(input);
            int paletteSize = checkedCount(input.readInt(),
                SchematicModel.MAX_PALETTE_SIZE, "palette");
            List<String> palette = new ArrayList<>(paletteSize);
            for (int index = 0; index < paletteSize; ++index) {
                palette.add(readString(input));
            }
            int expected = (int) SchematicModel.checkedVolume(
                header.sizeX,
                header.sizeY,
                header.sizeZ
            );
            int blockCount = checkedCount(input.readInt(),
                SchematicModel.MAX_BLOCKS, "blocks");
            if (blockCount != expected) {
                throw new IOException("Stored schematic dimensions are inconsistent");
            }
            int[] blocks = new int[blockCount];
            for (int index = 0; index < blockCount; ++index) {
                blocks[index] = input.readInt();
            }
            return new SchematicModel(
                header.sourceName,
                header.format,
                header.sizeX,
                header.sizeY,
                header.sizeZ,
                palette,
                blocks
            );
        } catch (EOFException error) {
            throw new IOException("Файл сохранённой схемы повреждён", error);
        }
    }

    private static Header readHeader(File source) throws IOException {
        try (DataInputStream input = new DataInputStream(
            new BufferedInputStream(new FileInputStream(source)))) {
            return readHeader(input);
        }
    }

    private static Header readHeader(DataInputStream input) throws IOException {
        if (input.readInt() != MAGIC || input.readInt() != VERSION) {
            throw new IOException("Unsupported stored schematic");
        }
        String sourceName = readString(input);
        String format = readString(input);
        int sizeX = input.readInt();
        int sizeY = input.readInt();
        int sizeZ = input.readInt();
        int nonAir = input.readInt();
        SchematicModel.checkedVolume(sizeX, sizeY, sizeZ);
        if (nonAir < 0 || nonAir > (long) sizeX * sizeY * sizeZ) {
            throw new IOException("Invalid stored schematic block count");
        }
        return new Header(sourceName, format, sizeX, sizeY, sizeZ, nonAir);
    }

    private static int checkedCount(int value, int maximum, String kind)
        throws IOException {
        if (value < 1 || value > maximum) {
            throw new IOException("Invalid stored schematic " + kind + " count");
        }
        return value;
    }

    private static void writeString(DataOutputStream output, String value)
        throws IOException {
        byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
        if (bytes.length > MAX_STRING_BYTES) {
            throw new IOException("Stored schematic text is too long");
        }
        output.writeInt(bytes.length);
        output.write(bytes);
    }

    private static String readString(DataInputStream input) throws IOException {
        int length = input.readInt();
        if (length < 0 || length > MAX_STRING_BYTES) {
            throw new IOException("Invalid stored schematic text length");
        }
        byte[] bytes = new byte[length];
        input.readFully(bytes);
        return new String(bytes, StandardCharsets.UTF_8);
    }

    public static final class ImportResult {
        public final String id;
        public final SchematicModel model;

        ImportResult(String id, SchematicModel model) {
            this.id = id;
            this.model = model;
        }
    }

    public static final class Entry {
        public final String id;
        public final String sourceName;
        public final String format;
        public final int sizeX;
        public final int sizeY;
        public final int sizeZ;
        public final int nonAirBlocks;
        public final long modifiedAtMs;
        public final boolean active;

        Entry(
            String id,
            String sourceName,
            String format,
            int sizeX,
            int sizeY,
            int sizeZ,
            int nonAirBlocks,
            long modifiedAtMs,
            boolean active
        ) {
            this.id = id;
            this.sourceName = sourceName;
            this.format = format;
            this.sizeX = sizeX;
            this.sizeY = sizeY;
            this.sizeZ = sizeZ;
            this.nonAirBlocks = nonAirBlocks;
            this.modifiedAtMs = modifiedAtMs;
            this.active = active;
        }
    }

    private static final class Header {
        final String sourceName;
        final String format;
        final int sizeX;
        final int sizeY;
        final int sizeZ;
        final int nonAirBlocks;

        Header(
            String sourceName,
            String format,
            int sizeX,
            int sizeY,
            int sizeZ,
            int nonAirBlocks
        ) {
            this.sourceName = sourceName;
            this.format = format;
            this.sizeX = sizeX;
            this.sizeY = sizeY;
            this.sizeZ = sizeZ;
            this.nonAirBlocks = nonAirBlocks;
        }
    }
}
