package com.m9chko.bedrockrelay.schematic;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.zip.GZIPOutputStream;

public final class SchematicFormatCompatibilityTest {
    @Test
    public void importsBedrockMcstructureLittleEndian() throws Exception {
        NbtOut out = new NbtOut(true);
        out.rootCompound(root -> {
            root.list("size", 3, 3, list -> list.ints(2, 1, 1));
            root.compound("structure", structure -> {
                structure.list("block_indices", 9, 2, layers -> {
                    layers.list(3, 2, layer -> layer.ints(1, 0));
                    layers.list(3, 2, layer -> layer.ints(-1, -1));
                });
                structure.compound("palette", palettes ->
                    palettes.compound("default", defaults ->
                        defaults.list("block_palette", 10, 2, palette -> {
                            palette.compound(entry -> paletteEntry(entry, "minecraft:air"));
                            palette.compound(entry -> paletteEntry(entry, "minecraft:stone"));
                        })
                    )
                );
            });
        });
        SchematicModel model = importBytes(out.bytes(), "tiny.mcstructure");
        assertEquals("Bedrock .mcstructure", model.format());
        assertEquals("minecraft:stone", stateAt(model, 0));
        assertTrue(SchematicModel.isAirState(stateAt(model, 1)));
    }

    @Test
    public void importsVanillaStructureNbt() throws Exception {
        NbtOut out = new NbtOut(false);
        out.rootCompound(root -> {
            root.list("size", 3, 3, list -> list.ints(2, 1, 1));
            root.list("palette", 10, 2, palette -> {
                palette.compound(entry -> paletteEntry(entry, "minecraft:air"));
                palette.compound(entry -> paletteEntry(entry, "minecraft:oak_planks"));
            });
            root.list("blocks", 10, 1, blocks -> blocks.compound(block -> {
                block.list("pos", 3, 3, list -> list.ints(1, 0, 0));
                block.integer("state", 1);
            }));
        });
        SchematicModel model = importBytes(gzip(out.bytes()), "tiny.nbt");
        assertEquals("Vanilla structure .nbt", model.format());
        assertTrue(SchematicModel.isAirState(stateAt(model, 0)));
        assertEquals("minecraft:oak_planks", stateAt(model, 1));
    }

    @Test
    public void importsSpongeV3WrappedSchematic() throws Exception {
        NbtOut out = new NbtOut(false);
        out.rootCompound(root -> root.compound("Schematic", schematic -> {
            schematic.integer("Version", 3);
            schematic.shortNumber("Width", 2);
            schematic.shortNumber("Height", 1);
            schematic.shortNumber("Length", 1);
            schematic.compound("Blocks", blocks -> {
                blocks.compound("Palette", palette -> {
                    palette.integer("minecraft:air", 0);
                    palette.integer("minecraft:glass", 1);
                });
                blocks.byteArray("Data", new byte[] { 0, 1 });
            });
        }));
        SchematicModel model = importBytes(gzip(out.bytes()), "tiny.schem");
        assertEquals("Sponge .schem", model.format());
        assertTrue(SchematicModel.isAirState(stateAt(model, 0)));
        assertEquals("minecraft:glass", stateAt(model, 1));
    }

    @Test
    public void importsLitematicNegativeRegionFromMinimumCorner() throws Exception {
        NbtOut out = new NbtOut(false);
        out.rootCompound(root -> {
            root.integer("Version", 6);
            root.compound("Regions", regions ->
                regions.compound("negative", region -> {
                    region.compound("Position", position -> vector(position, 5, 0, 0));
                    region.compound("Size", size -> vector(size, -2, 1, 1));
                    region.list("BlockStatePalette", 10, 2, palette -> {
                        palette.compound(entry -> paletteEntry(entry, "minecraft:air"));
                        palette.compound(entry -> paletteEntry(entry, "minecraft:gold_block"));
                    });
                    // Two 2-bit entries: container x=0 is gold, x=1 is air.
                    region.longArray("BlockStates", new long[] { 1L });
                })
            );
        });
        SchematicModel model = importBytes(gzip(out.bytes()), "tiny.litematic");
        assertEquals("Litematica .litematic", model.format());
        assertEquals("minecraft:gold_block", stateAt(model, 0));
        assertTrue(SchematicModel.isAirState(stateAt(model, 1)));
    }

    private static SchematicModel importBytes(byte[] bytes, String name)
        throws Exception {
        return new SchematicImporter().importStream(
            new ByteArrayInputStream(bytes),
            name
        );
    }

    private static String stateAt(SchematicModel model, int index) {
        return model.paletteState(model.paletteIndexAtLinear(index));
    }

    private static byte[] gzip(byte[] source) throws Exception {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        try (GZIPOutputStream gzip = new GZIPOutputStream(output)) {
            gzip.write(source);
        }
        return output.toByteArray();
    }

    private static void paletteEntry(NbtOut compound, String name)
        throws IOException {
        compound.string("Name", name);
        compound.compound("Properties", ignored -> {});
    }

    private static void vector(NbtOut compound, int x, int y, int z)
        throws IOException {
        compound.integer("x", x);
        compound.integer("y", y);
        compound.integer("z", z);
    }

    private interface Writer {
        void write(NbtOut output) throws IOException;
    }

    private static final class NbtOut {
        private final ByteArrayOutputStream output;
        private final boolean little;

        NbtOut(boolean little) {
            this(new ByteArrayOutputStream(), little);
        }

        NbtOut(ByteArrayOutputStream output, boolean little) {
            this.output = output;
            this.little = little;
        }

        byte[] bytes() {
            return output.toByteArray();
        }

        void rootCompound(Writer writer) throws IOException {
            output.write(10);
            rawString("");
            writer.write(this);
            output.write(0);
        }

        void compound(String name, Writer writer) throws IOException {
            named(10, name);
            writer.write(this);
            output.write(0);
        }

        void compound(Writer writer) throws IOException {
            writer.write(this);
            output.write(0);
        }

        void list(
            String name,
            int elementType,
            int count,
            Writer writer
        ) throws IOException {
            named(9, name);
            output.write(elementType);
            rawInt(count);
            writer.write(this);
        }

        void list(int elementType, int count, Writer writer) throws IOException {
            output.write(elementType);
            rawInt(count);
            writer.write(this);
        }

        void ints(int... values) {
            for (int value : values) rawInt(value);
        }

        void integer(String name, int value) throws IOException {
            named(3, name);
            rawInt(value);
        }

        void shortNumber(String name, int value) throws IOException {
            named(2, name);
            rawShort(value);
        }

        void string(String name, String value) throws IOException {
            named(8, name);
            rawString(value);
        }

        void byteArray(String name, byte[] value) throws IOException {
            named(7, name);
            rawInt(value.length);
            output.write(value);
        }

        void longArray(String name, long[] value) throws IOException {
            named(12, name);
            rawInt(value.length);
            for (long item : value) rawLong(item);
        }

        private void named(int type, String name) throws IOException {
            output.write(type);
            rawString(name);
        }

        private void rawString(String value) throws IOException {
            byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
            rawShort(bytes.length);
            output.write(bytes);
        }

        private void rawShort(int value) {
            if (little) {
                output.write(value & 0xff);
                output.write((value >>> 8) & 0xff);
            } else {
                output.write((value >>> 8) & 0xff);
                output.write(value & 0xff);
            }
        }

        private void rawInt(int value) {
            if (little) {
                for (int shift = 0; shift <= 24; shift += 8) {
                    output.write((value >>> shift) & 0xff);
                }
            } else {
                for (int shift = 24; shift >= 0; shift -= 8) {
                    output.write((value >>> shift) & 0xff);
                }
            }
        }

        private void rawLong(long value) {
            if (little) {
                for (int shift = 0; shift <= 56; shift += 8) {
                    output.write((int) ((value >>> shift) & 0xff));
                }
            } else {
                for (int shift = 56; shift >= 0; shift -= 8) {
                    output.write((int) ((value >>> shift) & 0xff));
                }
            }
        }
    }
}
