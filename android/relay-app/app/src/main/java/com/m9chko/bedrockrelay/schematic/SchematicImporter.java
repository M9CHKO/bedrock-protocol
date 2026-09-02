package com.m9chko.bedrockrelay.schematic;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;
import java.util.zip.GZIPInputStream;
import java.util.zip.InflaterInputStream;

/** Imports the common Java and Bedrock schematic formats into one block volume. */
public final class SchematicImporter {
    private static final int MAX_SOURCE_BYTES = 64 * 1024 * 1024;
    private static final int MAX_EXPANDED_BYTES = 192 * 1024 * 1024;

    public SchematicModel importStream(
        InputStream source,
        String sourceName
    ) throws IOException {
        if (source == null) throw new IOException("Не удалось открыть файл схемы");
        byte[] packed = readBounded(source, MAX_SOURCE_BYTES, "Файл схемы слишком большой");
        List<byte[]> candidates = unpackCandidates(packed);
        boolean bedrockFirst = extension(sourceName).equals("mcstructure");
        NbtReader.Endian[] endianOrder = bedrockFirst
            ? new NbtReader.Endian[] { NbtReader.Endian.LITTLE, NbtReader.Endian.BIG }
            : new NbtReader.Endian[] { NbtReader.Endian.BIG, NbtReader.Endian.LITTLE };

        IOException bestError = null;
        for (byte[] candidate : candidates) {
            for (NbtReader.Endian endian : endianOrder) {
                try {
                    NbtReader.Tag root = NbtReader.read(candidate, endian);
                    SchematicModel model = parseKnownFormat(root, sourceName);
                    if (model != null) return model;
                } catch (IOException | IllegalArgumentException error) {
                    if (bestError == null || isUseful(error)) {
                        bestError = error instanceof IOException
                            ? (IOException) error
                            : new IOException(error.getMessage(), error);
                    }
                }
            }
        }
        if (bestError != null && isUseful(bestError)) throw bestError;
        throw new IOException(
            "Формат схемы не распознан. Поддерживаются .mcstructure, .nbt, " +
                ".litematic, .schem и .schematic"
        );
    }

    private SchematicModel parseKnownFormat(
        NbtReader.Tag root,
        String sourceName
    ) throws IOException {
        Map<String, NbtReader.Tag> values = root.compound();
        if (isLitematic(values)) return parseLitematic(values, sourceName);
        if (isBedrockStructure(values)) {
            return parseBedrockStructure(values, sourceName);
        }
        if (isVanillaStructure(values)) {
            return parseVanillaStructure(values, sourceName);
        }

        Map<String, NbtReader.Tag> schematic = values;
        NbtReader.Tag wrapped = values.get("Schematic");
        if (wrapped != null && wrapped.type == 10) schematic = wrapped.compound();
        if (isSponge(schematic)) return parseSponge(schematic, sourceName);
        if (isLegacySchematic(schematic)) {
            return parseLegacySchematic(schematic, sourceName);
        }
        return null;
    }

    private SchematicModel parseBedrockStructure(
        Map<String, NbtReader.Tag> root,
        String sourceName
    ) throws IOException {
        int[] size = intVector(require(root, "size"), 3, "size");
        int sizeX = size[0];
        int sizeY = size[1];
        int sizeZ = size[2];
        int volume = (int) SchematicModel.checkedVolume(sizeX, sizeY, sizeZ);

        Map<String, NbtReader.Tag> structure = compound(root, "structure");
        Map<String, NbtReader.Tag> paletteContainer = compound(structure, "palette");
        NbtReader.Tag defaultPalette = paletteContainer.get("default");
        Map<String, NbtReader.Tag> paletteValues = defaultPalette != null &&
            defaultPalette.type == 10
                ? defaultPalette.compound()
                : paletteContainer;
        List<String> palette = readPaletteList(
            require(paletteValues, "block_palette")
        );
        int air = ensureAir(palette);

        List<NbtReader.Tag> layers = require(structure, "block_indices").list();
        if (layers.isEmpty()) throw new IOException("mcstructure has no block layer");
        int[] primary = integerSequence(layers.get(0), volume, "block_indices[0]");
        int[] secondary = layers.size() > 1
            ? integerSequence(layers.get(1), volume, "block_indices[1]")
            : null;
        int[] blocks = new int[volume];
        Arrays.fill(blocks, air);
        for (int x = 0; x < sizeX; ++x) {
            for (int y = 0; y < sizeY; ++y) {
                for (int z = 0; z < sizeZ; ++z) {
                    int sourceIndex = x * sizeY * sizeZ + y * sizeZ + z;
                    int paletteIndex = primary[sourceIndex];
                    if (paletteIndex < 0 && secondary != null) {
                        paletteIndex = secondary[sourceIndex];
                    }
                    if (paletteIndex < 0) paletteIndex = air;
                    validatePaletteIndex(paletteIndex, palette.size());
                    blocks[linear(x, y, z, sizeX, sizeZ)] = paletteIndex;
                }
            }
        }
        return new SchematicModel(
            sourceName,
            "Bedrock .mcstructure",
            sizeX,
            sizeY,
            sizeZ,
            palette,
            blocks
        );
    }

    private SchematicModel parseVanillaStructure(
        Map<String, NbtReader.Tag> root,
        String sourceName
    ) throws IOException {
        int[] size = intVector(require(root, "size"), 3, "size");
        int sizeX = size[0];
        int sizeY = size[1];
        int sizeZ = size[2];
        int volume = (int) SchematicModel.checkedVolume(sizeX, sizeY, sizeZ);
        List<String> palette = readPaletteList(require(root, "palette"));
        int air = ensureAir(palette);
        int[] blocks = new int[volume];
        Arrays.fill(blocks, air);
        for (NbtReader.Tag blockTag : require(root, "blocks").list()) {
            Map<String, NbtReader.Tag> block = blockTag.compound();
            int[] position = intVector(require(block, "pos"), 3, "block position");
            int state = integer(require(block, "state"), "block state");
            validatePaletteIndex(state, palette.size());
            int x = position[0];
            int y = position[1];
            int z = position[2];
            if (x < 0 || x >= sizeX || y < 0 || y >= sizeY ||
                z < 0 || z >= sizeZ) {
                throw new IOException("Vanilla NBT block position is out of bounds");
            }
            blocks[linear(x, y, z, sizeX, sizeZ)] = state;
        }
        return new SchematicModel(
            sourceName,
            "Vanilla structure .nbt",
            sizeX,
            sizeY,
            sizeZ,
            palette,
            blocks
        );
    }

    private SchematicModel parseSponge(
        Map<String, NbtReader.Tag> root,
        String sourceName
    ) throws IOException {
        int sizeX = integer(require(root, "Width"), "Width");
        int sizeY = integer(require(root, "Height"), "Height");
        int sizeZ = integer(require(root, "Length"), "Length");
        int volume = (int) SchematicModel.checkedVolume(sizeX, sizeY, sizeZ);

        Map<String, NbtReader.Tag> blockSection = root;
        NbtReader.Tag blocksTag = root.get("Blocks");
        if (blocksTag != null && blocksTag.type == 10) {
            blockSection = blocksTag.compound();
        }
        Map<String, NbtReader.Tag> encodedPalette =
            compound(blockSection, "Palette");
        int maximumId = -1;
        for (NbtReader.Tag value : encodedPalette.values()) {
            maximumId = Math.max(maximumId, integer(value, "palette id"));
        }
        if (maximumId < 0 || maximumId >= SchematicModel.MAX_PALETTE_SIZE) {
            throw new IOException("Invalid Sponge palette");
        }
        List<String> palette = new ArrayList<>(
            java.util.Collections.nCopies(maximumId + 1, "minecraft:air")
        );
        for (Map.Entry<String, NbtReader.Tag> entry : encodedPalette.entrySet()) {
            int id = integer(entry.getValue(), "palette id");
            if (id < 0 || id >= palette.size()) {
                throw new IOException("Invalid Sponge palette id");
            }
            palette.set(id, canonicalStateString(entry.getKey()));
        }

        NbtReader.Tag dataTag = blockSection.get("Data");
        if (dataTag == null) dataTag = root.get("BlockData");
        if (dataTag == null) dataTag = blockSection.get("BlockData");
        if (dataTag == null || dataTag.type != 7) {
            throw new IOException("Sponge schematic has no block data");
        }
        int[] blocks = decodeVarInts(dataTag.bytes(), volume);
        for (int value : blocks) validatePaletteIndex(value, palette.size());
        return new SchematicModel(
            sourceName,
            "Sponge .schem",
            sizeX,
            sizeY,
            sizeZ,
            palette,
            blocks
        );
    }

    private SchematicModel parseLegacySchematic(
        Map<String, NbtReader.Tag> root,
        String sourceName
    ) throws IOException {
        int sizeX = integer(require(root, "Width"), "Width");
        int sizeY = integer(require(root, "Height"), "Height");
        int sizeZ = integer(require(root, "Length"), "Length");
        int volume = (int) SchematicModel.checkedVolume(sizeX, sizeY, sizeZ);
        byte[] ids = require(root, "Blocks").bytes();
        byte[] metadata = require(root, "Data").bytes();
        if (ids.length != volume || metadata.length != volume) {
            throw new IOException("Legacy schematic block arrays have the wrong size");
        }
        byte[] additions = null;
        NbtReader.Tag additionsTag = root.get("AddBlocks");
        if (additionsTag != null && additionsTag.type == 7) {
            additions = additionsTag.bytes();
        }

        PaletteBuilder palette = new PaletteBuilder();
        int[] blocks = new int[volume];
        for (int index = 0; index < volume; ++index) {
            int id = ids[index] & 0xff;
            if (additions != null && (index >> 1) < additions.length) {
                int pair = additions[index >> 1] & 0xff;
                int high = (index & 1) == 0 ? pair & 0x0f : pair >>> 4;
                id |= high << 8;
            }
            int data = metadata[index] & 0x0f;
            String state = id == 0
                ? "minecraft:air"
                : "legacy:" + id + ":" + data;
            blocks[index] = palette.id(state);
        }
        return new SchematicModel(
            sourceName,
            "MCEdit .schematic",
            sizeX,
            sizeY,
            sizeZ,
            palette.values,
            blocks
        );
    }

    private SchematicModel parseLitematic(
        Map<String, NbtReader.Tag> root,
        String sourceName
    ) throws IOException {
        Map<String, NbtReader.Tag> regions = compound(root, "Regions");
        if (regions.isEmpty()) throw new IOException("Litematic has no regions");
        List<Region> parsed = new ArrayList<>();
        int minimumX = Integer.MAX_VALUE;
        int minimumY = Integer.MAX_VALUE;
        int minimumZ = Integer.MAX_VALUE;
        int maximumX = Integer.MIN_VALUE;
        int maximumY = Integer.MIN_VALUE;
        int maximumZ = Integer.MIN_VALUE;
        for (Map.Entry<String, NbtReader.Tag> entry : regions.entrySet()) {
            Map<String, NbtReader.Tag> value = entry.getValue().compound();
            int[] position = compoundVector(value, "Position");
            int[] signedSize = compoundVector(value, "Size");
            Region region = new Region(entry.getKey(), position, signedSize, value);
            parsed.add(region);
            minimumX = Math.min(minimumX, region.minimumX());
            minimumY = Math.min(minimumY, region.minimumY());
            minimumZ = Math.min(minimumZ, region.minimumZ());
            maximumX = Math.max(maximumX, region.maximumX());
            maximumY = Math.max(maximumY, region.maximumY());
            maximumZ = Math.max(maximumZ, region.maximumZ());
        }
        int sizeX = Math.addExact(Math.subtractExact(maximumX, minimumX), 1);
        int sizeY = Math.addExact(Math.subtractExact(maximumY, minimumY), 1);
        int sizeZ = Math.addExact(Math.subtractExact(maximumZ, minimumZ), 1);
        int volume = (int) SchematicModel.checkedVolume(sizeX, sizeY, sizeZ);
        PaletteBuilder globalPalette = new PaletteBuilder();
        int globalAir = globalPalette.id("minecraft:air");
        int[] blocks = new int[volume];
        Arrays.fill(blocks, globalAir);

        for (Region region : parsed) {
            List<String> localPalette = readPaletteList(
                require(region.values, "BlockStatePalette")
            );
            int[] remap = new int[localPalette.size()];
            for (int index = 0; index < localPalette.size(); ++index) {
                remap[index] = globalPalette.id(localPalette.get(index));
            }
            long[] packed = require(region.values, "BlockStates").longs();
            int localVolume = region.sizeX * region.sizeY * region.sizeZ;
            int bits = Math.max(2, ceilLog2(Math.max(1, localPalette.size())));
            for (int localIndex = 0; localIndex < localVolume; ++localIndex) {
                int paletteIndex = unpackLitematicPaletteId(packed, localIndex, bits);
                validatePaletteIndex(paletteIndex, localPalette.size());
                int localX = localIndex % region.sizeX;
                int localZ = (localIndex / region.sizeX) % region.sizeZ;
                int localY = localIndex / (region.sizeX * region.sizeZ);
                // Litematica stores each region container from its minimum
                // corner to its maximum corner, even when the signed Size
                // points away from Position in a negative direction.
                int worldX = region.minimumX() + localX;
                int worldY = region.minimumY() + localY;
                int worldZ = region.minimumZ() + localZ;
                int x = worldX - minimumX;
                int y = worldY - minimumY;
                int z = worldZ - minimumZ;
                blocks[linear(x, y, z, sizeX, sizeZ)] = remap[paletteIndex];
            }
        }
        return new SchematicModel(
            sourceName,
            "Litematica .litematic",
            sizeX,
            sizeY,
            sizeZ,
            globalPalette.values,
            blocks
        );
    }

    static int[] decodeVarInts(byte[] bytes, int expectedCount) throws IOException {
        int[] values = new int[expectedCount];
        int offset = 0;
        for (int index = 0; index < expectedCount; ++index) {
            int value = 0;
            int shift = 0;
            while (true) {
                if (offset >= bytes.length || shift >= 35) {
                    throw new IOException("Malformed Sponge block varint data");
                }
                int next = bytes[offset++] & 0xff;
                value |= (next & 0x7f) << shift;
                if ((next & 0x80) == 0) break;
                shift += 7;
            }
            values[index] = value;
        }
        return values;
    }

    static int unpackLitematicPaletteId(long[] packed, int index, int bits)
        throws IOException {
        if (bits <= 0 || bits > 32 || index < 0) {
            throw new IOException("Invalid Litematic palette packing");
        }
        long bitOffset = (long) index * bits;
        int startLong = (int) (bitOffset >>> 6);
        int shift = (int) (bitOffset & 63L);
        if (startLong < 0 || startLong >= packed.length) {
            throw new IOException("Litematic block state array is too short");
        }
        long value = packed[startLong] >>> shift;
        if (shift + bits > 64) {
            if (startLong + 1 >= packed.length) {
                throw new IOException("Litematic block state array is truncated");
            }
            value |= packed[startLong + 1] << (64 - shift);
        }
        long mask = (1L << bits) - 1L;
        return (int) (value & mask);
    }

    private static boolean isLitematic(Map<String, NbtReader.Tag> root) {
        return hasCompound(root, "Regions");
    }

    private static boolean isBedrockStructure(Map<String, NbtReader.Tag> root) {
        NbtReader.Tag structure = root.get("structure");
        if (structure == null || structure.type != 10) return false;
        try {
            return structure.compound().containsKey("block_indices");
        } catch (IOException ignored) {
            return false;
        }
    }

    private static boolean isVanillaStructure(Map<String, NbtReader.Tag> root) {
        return root.containsKey("size") && root.containsKey("palette") &&
            root.containsKey("blocks");
    }

    private static boolean isSponge(Map<String, NbtReader.Tag> root) {
        if (!root.containsKey("Width") || !root.containsKey("Height") ||
            !root.containsKey("Length")) return false;
        if (hasCompound(root, "Palette") && root.containsKey("BlockData")) {
            return true;
        }
        NbtReader.Tag blocks = root.get("Blocks");
        if (blocks == null || blocks.type != 10) return false;
        try {
            return blocks.compound().containsKey("Palette");
        } catch (IOException ignored) {
            return false;
        }
    }

    private static boolean isLegacySchematic(Map<String, NbtReader.Tag> root) {
        NbtReader.Tag blocks = root.get("Blocks");
        NbtReader.Tag data = root.get("Data");
        return blocks != null && blocks.type == 7 &&
            data != null && data.type == 7 &&
            root.containsKey("Width") && root.containsKey("Height") &&
            root.containsKey("Length");
    }

    private static boolean hasCompound(
        Map<String, NbtReader.Tag> values,
        String key
    ) {
        NbtReader.Tag value = values.get(key);
        return value != null && value.type == 10;
    }

    private static Map<String, NbtReader.Tag> compound(
        Map<String, NbtReader.Tag> values,
        String key
    ) throws IOException {
        return require(values, key).compound();
    }

    private static NbtReader.Tag require(
        Map<String, NbtReader.Tag> values,
        String key
    ) throws IOException {
        NbtReader.Tag value = values.get(key);
        if (value == null) throw new IOException("NBT field is missing: " + key);
        return value;
    }

    private static int integer(NbtReader.Tag value, String name) throws IOException {
        long result = value.number().longValue();
        if (result < Integer.MIN_VALUE || result > Integer.MAX_VALUE) {
            throw new IOException("NBT integer is out of range: " + name);
        }
        return (int) result;
    }

    private static int[] intVector(
        NbtReader.Tag value,
        int expected,
        String name
    ) throws IOException {
        if (value.type == 11) {
            int[] values = value.ints();
            if (values.length != expected) {
                throw new IOException("Invalid NBT vector: " + name);
            }
            return values.clone();
        }
        List<NbtReader.Tag> list = value.list();
        if (list.size() != expected) {
            throw new IOException("Invalid NBT vector: " + name);
        }
        int[] result = new int[expected];
        for (int index = 0; index < expected; ++index) {
            result[index] = integer(list.get(index), name);
        }
        return result;
    }

    private static int[] compoundVector(
        Map<String, NbtReader.Tag> values,
        String key
    ) throws IOException {
        Map<String, NbtReader.Tag> vector = compound(values, key);
        return new int[] {
            integer(require(vector, "x"), key + ".x"),
            integer(require(vector, "y"), key + ".y"),
            integer(require(vector, "z"), key + ".z")
        };
    }

    private static int[] integerSequence(
        NbtReader.Tag value,
        int expected,
        String name
    ) throws IOException {
        int[] result;
        if (value.type == 11) {
            result = value.ints().clone();
        } else {
            List<NbtReader.Tag> list = value.list();
            result = new int[list.size()];
            for (int index = 0; index < result.length; ++index) {
                result[index] = integer(list.get(index), name);
            }
        }
        if (result.length != expected) {
            throw new IOException(name + " has the wrong number of blocks");
        }
        return result;
    }

    private static List<String> readPaletteList(NbtReader.Tag tag)
        throws IOException {
        List<NbtReader.Tag> entries = tag.list();
        if (entries.isEmpty() || entries.size() > SchematicModel.MAX_PALETTE_SIZE) {
            throw new IOException("Invalid or empty block palette");
        }
        List<String> result = new ArrayList<>(entries.size());
        for (NbtReader.Tag entry : entries) {
            result.add(canonicalPaletteEntry(entry.compound()));
        }
        return result;
    }

    private static String canonicalPaletteEntry(
        Map<String, NbtReader.Tag> entry
    ) throws IOException {
        NbtReader.Tag nameTag = entry.get("Name");
        if (nameTag == null) nameTag = entry.get("name");
        if (nameTag == null) throw new IOException("Palette entry has no block name");
        String name = canonicalStateString(nameTag.string());
        NbtReader.Tag propertiesTag = entry.get("Properties");
        if (propertiesTag == null) propertiesTag = entry.get("states");
        if (propertiesTag == null || propertiesTag.type != 10) return name;
        Map<String, NbtReader.Tag> properties = propertiesTag.compound();
        if (properties.isEmpty()) return name;
        TreeMap<String, String> sorted = new TreeMap<>();
        for (Map.Entry<String, NbtReader.Tag> property : properties.entrySet()) {
            sorted.put(property.getKey(), scalarString(property.getValue()));
        }
        StringBuilder result = new StringBuilder(name).append('[');
        boolean first = true;
        for (Map.Entry<String, String> property : sorted.entrySet()) {
            if (!first) result.append(',');
            first = false;
            result.append(property.getKey()).append('=').append(property.getValue());
        }
        return result.append(']').toString();
    }

    private static String scalarString(NbtReader.Tag value) throws IOException {
        if (value.type == 8) return value.string();
        if (value.type >= 1 && value.type <= 6) {
            Number number = value.number();
            if (value.type == 1 && (number.intValue() == 0 || number.intValue() == 1)) {
                return number.intValue() == 0 ? "false" : "true";
            }
            return number.toString();
        }
        throw new IOException("Unsupported block-state property value");
    }

    private static String canonicalStateString(String value) {
        String state = value == null ? "minecraft:air" : value.trim();
        if (state.isEmpty()) return "minecraft:air";
        int propertyStart = state.indexOf('[');
        String name = propertyStart >= 0 ? state.substring(0, propertyStart) : state;
        if (!name.contains(":") && !name.startsWith("legacy:")) {
            name = "minecraft:" + name;
        }
        return propertyStart >= 0 ? name + state.substring(propertyStart) : name;
    }

    private static int ensureAir(List<String> palette) throws IOException {
        for (int index = 0; index < palette.size(); ++index) {
            if (SchematicModel.isAirState(palette.get(index))) return index;
        }
        if (palette.size() >= SchematicModel.MAX_PALETTE_SIZE) {
            throw new IOException("Block palette cannot add an air entry");
        }
        palette.add("minecraft:air");
        return palette.size() - 1;
    }

    private static void validatePaletteIndex(int value, int size)
        throws IOException {
        if (value < 0 || value >= size) {
            throw new IOException("Block references a missing palette entry");
        }
    }

    private static int linear(int x, int y, int z, int sizeX, int sizeZ) {
        return (y * sizeZ + z) * sizeX + x;
    }

    private static int ceilLog2(int value) {
        if (value <= 1) return 0;
        return 32 - Integer.numberOfLeadingZeros(value - 1);
    }

    private static List<byte[]> unpackCandidates(byte[] packed) throws IOException {
        List<byte[]> result = new ArrayList<>();
        if (packed.length >= 2 && (packed[0] & 0xff) == 0x1f &&
            (packed[1] & 0xff) == 0x8b) {
            result.add(readBounded(
                new GZIPInputStream(new ByteArrayInputStream(packed)),
                MAX_EXPANDED_BYTES,
                "Распакованная схема слишком большая"
            ));
        } else if (looksLikeZlib(packed)) {
            try {
                result.add(readBounded(
                    new InflaterInputStream(new ByteArrayInputStream(packed)),
                    MAX_EXPANDED_BYTES,
                    "Распакованная схема слишком большая"
                ));
            } catch (IOException ignored) {
                // Some raw NBT files happen to begin with a valid zlib header.
            }
        }
        result.add(packed);
        return result;
    }

    private static boolean looksLikeZlib(byte[] bytes) {
        if (bytes.length < 2) return false;
        int cmf = bytes[0] & 0xff;
        int flg = bytes[1] & 0xff;
        return (cmf & 0x0f) == 8 && ((cmf << 8) + flg) % 31 == 0;
    }

    private static byte[] readBounded(
        InputStream input,
        int maximum,
        String overflowMessage
    ) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[32 * 1024];
        int total = 0;
        while (true) {
            int read = input.read(buffer);
            if (read < 0) break;
            if (read == 0) continue;
            total += read;
            if (total > maximum) throw new IOException(overflowMessage);
            output.write(buffer, 0, read);
        }
        return output.toByteArray();
    }

    private static String extension(String name) {
        if (name == null) return "";
        String lower = name.toLowerCase(Locale.ROOT);
        int dot = lower.lastIndexOf('.');
        return dot < 0 ? "" : lower.substring(dot + 1);
    }

    private static boolean isUseful(Throwable error) {
        String message = error.getMessage();
        return message != null &&
            (message.contains("Schematic") || message.contains("schematic") ||
             message.contains("Litematic") || message.contains("Sponge") ||
             message.contains("mcstructure") || message.contains("palette") ||
             message.contains("block"));
    }

    private static final class PaletteBuilder {
        final List<String> values = new ArrayList<>();
        final Map<String, Integer> ids = new LinkedHashMap<>();

        int id(String state) throws IOException {
            String canonical = canonicalStateString(state);
            Integer existing = ids.get(canonical);
            if (existing != null) return existing;
            if (values.size() >= SchematicModel.MAX_PALETTE_SIZE) {
                throw new IOException("Schematic palette is too large");
            }
            int id = values.size();
            values.add(canonical);
            ids.put(canonical, id);
            return id;
        }
    }

    private static final class Region {
        final String name;
        final int[] position;
        final Map<String, NbtReader.Tag> values;
        final int signX;
        final int signY;
        final int signZ;
        final int sizeX;
        final int sizeY;
        final int sizeZ;

        Region(
            String name,
            int[] position,
            int[] signedSize,
            Map<String, NbtReader.Tag> values
        ) throws IOException {
            this.name = name;
            this.position = position;
            this.values = values;
            signX = signedSize[0] < 0 ? -1 : 1;
            signY = signedSize[1] < 0 ? -1 : 1;
            signZ = signedSize[2] < 0 ? -1 : 1;
            sizeX = safeAbs(signedSize[0]);
            sizeY = safeAbs(signedSize[1]);
            sizeZ = safeAbs(signedSize[2]);
            SchematicModel.checkedVolume(sizeX, sizeY, sizeZ);
        }

        int minimumX() { return Math.min(position[0], position[0] + signX * (sizeX - 1)); }
        int minimumY() { return Math.min(position[1], position[1] + signY * (sizeY - 1)); }
        int minimumZ() { return Math.min(position[2], position[2] + signZ * (sizeZ - 1)); }
        int maximumX() { return Math.max(position[0], position[0] + signX * (sizeX - 1)); }
        int maximumY() { return Math.max(position[1], position[1] + signY * (sizeY - 1)); }
        int maximumZ() { return Math.max(position[2], position[2] + signZ * (sizeZ - 1)); }

        private static int safeAbs(int value) throws IOException {
            if (value == 0 || value == Integer.MIN_VALUE) {
                throw new IOException("Litematic region has invalid dimensions");
            }
            return Math.abs(value);
        }
    }
}
