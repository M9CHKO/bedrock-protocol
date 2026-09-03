package com.m9chko.bedrockrelay.schematic;

import java.util.ArrayList;
import java.util.BitSet;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

/** Immutable, format-neutral block volume used by the schematic renderer. */
public final class SchematicModel {
    public static final int MAX_BLOCKS = 8 * 1024 * 1024;
    public static final int MAX_PALETTE_SIZE = 65_536;

    private final String sourceName;
    private final String format;
    private final int sizeX;
    private final int sizeY;
    private final int sizeZ;
    private final List<String> palette;
    private final int[] blocks;
    private final int[] boundaryBlocks;
    private final boolean[] airPalette;
    private final BitSet nonAirMask;
    private final int nonAirBlocks;

    SchematicModel(
        String sourceName,
        String format,
        int sizeX,
        int sizeY,
        int sizeZ,
        List<String> palette,
        int[] blocks
    ) {
        long volume = checkedVolume(sizeX, sizeY, sizeZ);
        if (blocks == null || blocks.length != volume) {
            throw new IllegalArgumentException(
                "Block array does not match schematic dimensions"
            );
        }
        if (palette == null || palette.isEmpty() ||
            palette.size() > MAX_PALETTE_SIZE) {
            throw new IllegalArgumentException("Invalid schematic palette");
        }

        this.sourceName = cleanName(sourceName);
        this.format = format == null || format.trim().isEmpty()
            ? "NBT"
            : format.trim();
        this.sizeX = sizeX;
        this.sizeY = sizeY;
        this.sizeZ = sizeZ;
        this.palette = Collections.unmodifiableList(
            new ArrayList<>(palette)
        );
        this.blocks = blocks.clone();

        int nonAir = 0;
        int boundaryCount = 0;
        BitSet occupied = new BitSet(this.blocks.length);
        boolean[] air = new boolean[this.palette.size()];
        for (int index = 0; index < air.length; ++index) {
            air[index] = isAirState(this.palette.get(index));
        }
        airPalette = air;
        for (int index = 0; index < this.blocks.length; ++index) {
            int paletteIndex = this.blocks[index];
            if (paletteIndex < 0 || paletteIndex >= this.palette.size()) {
                throw new IllegalArgumentException(
                    "Block palette index is out of bounds"
                );
            }
            if (air[paletteIndex]) continue;
            occupied.set(index);
            ++nonAir;
            if (isBoundary(index, air)) ++boundaryCount;
        }
        nonAirBlocks = nonAir;
        nonAirMask = occupied;
        boundaryBlocks = new int[boundaryCount];
        int output = 0;
        for (int index = 0; index < this.blocks.length; ++index) {
            int paletteIndex = this.blocks[index];
            if (!air[paletteIndex] && isBoundary(index, air)) {
                boundaryBlocks[output++] = index;
            }
        }
    }

    public String sourceName() {
        return sourceName;
    }

    public String format() {
        return format;
    }

    public int sizeX() {
        return sizeX;
    }

    public int sizeY() {
        return sizeY;
    }

    public int sizeZ() {
        return sizeZ;
    }

    public int volume() {
        return blocks.length;
    }

    public int nonAirBlocks() {
        return nonAirBlocks;
    }

    public int boundaryBlockCount() {
        return boundaryBlocks.length;
    }

    public int boundaryBlockIndexAt(int index) {
        return boundaryBlocks[index];
    }

    public int xFromIndex(int index) {
        return index % sizeX;
    }

    public int zFromIndex(int index) {
        return (index / sizeX) % sizeZ;
    }

    public int yFromIndex(int index) {
        return index / (sizeX * sizeZ);
    }

    public int paletteIndexAtLinear(int index) {
        return blocks[index];
    }

    /**
     * Returns the next non-air linear index at or after {@code fromIndex}, or
     * {@code -1}. The compact bit set lets radius-window queries skip long air
     * runs without allocating an integer index for every schematic block.
     */
    public int nextNonAirBlockIndex(int fromIndex) {
        if (fromIndex < 0) throw new IndexOutOfBoundsException("Negative index");
        return nonAirMask.nextSetBit(fromIndex);
    }

    public String paletteState(int index) {
        return palette.get(index);
    }

    /** Outside the model is air; used to omit fully covered render faces. */
    public boolean isAirAt(int x, int y, int z) {
        if (x < 0 || x >= sizeX || y < 0 || y >= sizeY ||
            z < 0 || z >= sizeZ) {
            return true;
        }
        int paletteIndex = blocks[(y * sizeZ + z) * sizeX + x];
        return paletteIndex < 0 || paletteIndex >= airPalette.length ||
            airPalette[paletteIndex];
    }

    public int paletteSize() {
        return palette.size();
    }

    public String description() {
        return String.format(
            Locale.getDefault(),
            "%s • %d×%d×%d • %,d блоков",
            format,
            sizeX,
            sizeY,
            sizeZ,
            nonAirBlocks
        );
    }

    List<String> paletteInternal() {
        return palette;
    }

    int[] blocksCopy() {
        return blocks.clone();
    }

    private boolean isBoundary(int index, boolean[] air) {
        int x = xFromIndex(index);
        int z = zFromIndex(index);
        int y = yFromIndex(index);
        return x == 0 || x + 1 == sizeX ||
            y == 0 || y + 1 == sizeY ||
            z == 0 || z + 1 == sizeZ ||
            isAir(index - 1, air) ||
            isAir(index + 1, air) ||
            isAir(index - sizeX, air) ||
            isAir(index + sizeX, air) ||
            isAir(index - sizeX * sizeZ, air) ||
            isAir(index + sizeX * sizeZ, air);
    }

    private boolean isAir(int index, boolean[] air) {
        int paletteIndex = blocks[index];
        return paletteIndex < 0 || paletteIndex >= air.length ||
            air[paletteIndex];
    }

    static long checkedVolume(int x, int y, int z) {
        if (x <= 0 || y <= 0 || z <= 0) {
            throw new IllegalArgumentException(
                "Schematic dimensions must be positive"
            );
        }
        long volume = (long) x * y * z;
        if (volume > MAX_BLOCKS) {
            throw new IllegalArgumentException(
                "Schematic is too large (maximum " + MAX_BLOCKS + " blocks)"
            );
        }
        return volume;
    }

    public static boolean isAirState(String state) {
        if (state == null) return true;
        String normalized = state.trim().toLowerCase(Locale.ROOT);
        int properties = normalized.indexOf('[');
        if (properties >= 0) normalized = normalized.substring(0, properties);
        return "air".equals(normalized) ||
            "minecraft:air".equals(normalized) ||
            "minecraft:cave_air".equals(normalized) ||
            "minecraft:void_air".equals(normalized) ||
            "minecraft:structure_void".equals(normalized);
    }

    private static String cleanName(String value) {
        String clean = value == null ? "Схема" : value.trim();
        if (clean.isEmpty()) clean = "Схема";
        return clean.length() <= 180 ? clean : clean.substring(0, 180);
    }
}
