package com.m9chko.bedrockrelay.schematic;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.zip.GZIPOutputStream;

public final class SchematicImporterTest {
    @Test
    public void spongeVarIntsDecodeAcrossByteBoundaries() throws Exception {
        byte[] encoded = {
            0,
            1,
            127,
            (byte) 0x80, 1,
            (byte) 0xac, 2
        };
        assertArrayEquals(
            new int[] { 0, 1, 127, 128, 300 },
            SchematicImporter.decodeVarInts(encoded, 5)
        );
    }

    @Test
    public void litematicPaletteBitsCrossLongBoundary() throws Exception {
        int bits = 5;
        int[] expected = new int[25];
        long[] packed = new long[2];
        for (int index = 0; index < expected.length; ++index) {
            expected[index] = (index * 7) & 31;
            putPacked(packed, index, bits, expected[index]);
        }
        for (int index = 0; index < expected.length; ++index) {
            assertEquals(
                expected[index],
                SchematicImporter.unpackLitematicPaletteId(
                    packed,
                    index,
                    bits
                )
            );
        }
    }

    @Test
    public void nbtReaderSupportsBothEndiannesses() throws Exception {
        byte[] big = minimalIntCompound(false, 0x12345678);
        byte[] little = minimalIntCompound(true, 0x12345678);
        Map<String, NbtReader.Tag> bigRoot = NbtReader.read(
            big,
            NbtReader.Endian.BIG
        ).compound();
        Map<String, NbtReader.Tag> littleRoot = NbtReader.read(
            little,
            NbtReader.Endian.LITTLE
        ).compound();
        assertEquals(0x12345678, bigRoot.get("Value").number().intValue());
        assertEquals(0x12345678, littleRoot.get("Value").number().intValue());
    }

    @Test
    public void importsGzipLegacySchematic() throws Exception {
        byte[] nbt = legacySchematicNbt();
        ByteArrayOutputStream compressed = new ByteArrayOutputStream();
        try (GZIPOutputStream gzip = new GZIPOutputStream(compressed)) {
            gzip.write(nbt);
        }
        SchematicModel model = new SchematicImporter().importStream(
            new ByteArrayInputStream(compressed.toByteArray()),
            "tiny.schematic"
        );
        assertEquals("MCEdit .schematic", model.format());
        assertEquals(2, model.sizeX());
        assertEquals(1, model.sizeY());
        assertEquals(1, model.sizeZ());
        assertEquals(1, model.nonAirBlocks());
        assertTrue(SchematicModel.isAirState(
            model.paletteState(model.paletteIndexAtLinear(0))
        ));
        assertEquals(
            "legacy:1:0",
            model.paletteState(model.paletteIndexAtLinear(1))
        );
    }

    @Test
    public void solidVolumeKeepsOnlyBoundaryBlocksForRendering() {
        List<String> palette = Arrays.asList("minecraft:air", "minecraft:stone");
        int[] blocks = new int[27];
        Arrays.fill(blocks, 1);
        SchematicModel model = new SchematicModel(
            "cube.nbt",
            "test",
            3,
            3,
            3,
            palette,
            blocks
        );
        assertEquals(27, model.nonAirBlocks());
        assertEquals(26, model.boundaryBlockCount());
    }

    private static void putPacked(
        long[] packed,
        int index,
        int bits,
        int value
    ) {
        long bitOffset = (long) index * bits;
        int start = (int) (bitOffset >>> 6);
        int shift = (int) (bitOffset & 63L);
        packed[start] |= ((long) value) << shift;
        if (shift + bits > 64) {
            packed[start + 1] |= ((long) value) >>> (64 - shift);
        }
    }

    private static byte[] minimalIntCompound(boolean little, int value)
        throws Exception {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        output.write(10);
        writeShort(output, 0, little);
        output.write(3);
        writeString(output, "Value", little);
        writeInt(output, value, little);
        output.write(0);
        return output.toByteArray();
    }

    private static byte[] legacySchematicNbt() throws Exception {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        DataOutputStream data = new DataOutputStream(output);
        data.writeByte(10);
        data.writeUTF("Schematic");
        writeShortTag(data, "Width", 2);
        writeShortTag(data, "Height", 1);
        writeShortTag(data, "Length", 1);
        writeByteArrayTag(data, "Blocks", new byte[] { 0, 1 });
        writeByteArrayTag(data, "Data", new byte[] { 0, 0 });
        data.writeByte(0);
        data.flush();
        return output.toByteArray();
    }

    private static void writeShortTag(
        DataOutputStream output,
        String name,
        int value
    ) throws Exception {
        output.writeByte(2);
        output.writeUTF(name);
        output.writeShort(value);
    }

    private static void writeByteArrayTag(
        DataOutputStream output,
        String name,
        byte[] value
    ) throws Exception {
        output.writeByte(7);
        output.writeUTF(name);
        output.writeInt(value.length);
        output.write(value);
    }

    private static void writeString(
        ByteArrayOutputStream output,
        String value,
        boolean little
    ) throws Exception {
        byte[] bytes = value.getBytes(java.nio.charset.StandardCharsets.UTF_8);
        writeShort(output, bytes.length, little);
        output.write(bytes);
    }

    private static void writeShort(
        ByteArrayOutputStream output,
        int value,
        boolean little
    ) {
        if (little) {
            output.write(value & 0xff);
            output.write((value >>> 8) & 0xff);
        } else {
            output.write((value >>> 8) & 0xff);
            output.write(value & 0xff);
        }
    }

    private static void writeInt(
        ByteArrayOutputStream output,
        int value,
        boolean little
    ) {
        if (little) {
            output.write(value & 0xff);
            output.write((value >>> 8) & 0xff);
            output.write((value >>> 16) & 0xff);
            output.write((value >>> 24) & 0xff);
        } else {
            output.write((value >>> 24) & 0xff);
            output.write((value >>> 16) & 0xff);
            output.write((value >>> 8) & 0xff);
            output.write(value & 0xff);
        }
    }
}
