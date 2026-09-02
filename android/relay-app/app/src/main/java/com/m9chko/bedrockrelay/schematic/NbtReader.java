package com.m9chko.bedrockrelay.schematic;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Small bounded NBT reader supporting Java big-endian and Bedrock little-endian files. */
final class NbtReader {
    enum Endian { BIG, LITTLE }

    private static final int MAX_DEPTH = 64;
    private static final int MAX_COLLECTION_ELEMENTS = SchematicModel.MAX_BLOCKS * 4;
    private static final int MAX_STRING_BYTES = 1_048_576;

    private NbtReader() {}

    static Tag read(byte[] bytes, Endian endian) throws IOException {
        if (bytes == null || bytes.length == 0) {
            throw new IOException("NBT file is empty");
        }
        Input input = new Input(bytes, endian);
        byte type = input.readByte();
        if (type == 0) throw new IOException("NBT root cannot be TAG_End");
        input.readString();
        Tag root = input.readPayload(type, 0);
        if (root.type != 10) {
            throw new IOException("NBT root must be a compound");
        }
        return root;
    }

    static final class Tag {
        final byte type;
        final Object value;

        Tag(byte type, Object value) {
            this.type = type;
            this.value = value;
        }

        @SuppressWarnings("unchecked")
        Map<String, Tag> compound() throws IOException {
            if (type != 10) throw typeError("compound");
            return (Map<String, Tag>) value;
        }

        @SuppressWarnings("unchecked")
        List<Tag> list() throws IOException {
            if (type != 9) throw typeError("list");
            return ((ListValue) value).values;
        }

        byte listElementType() throws IOException {
            if (type != 9) throw typeError("list");
            return ((ListValue) value).elementType;
        }

        Number number() throws IOException {
            if (type < 1 || type > 6) throw typeError("number");
            return (Number) value;
        }

        String string() throws IOException {
            if (type != 8) throw typeError("string");
            return (String) value;
        }

        byte[] bytes() throws IOException {
            if (type != 7) throw typeError("byte array");
            return (byte[]) value;
        }

        int[] ints() throws IOException {
            if (type != 11) throw typeError("int array");
            return (int[]) value;
        }

        long[] longs() throws IOException {
            if (type != 12) throw typeError("long array");
            return (long[]) value;
        }

        private IOException typeError(String expected) {
            return new IOException(
                "Expected NBT " + expected + ", found tag type " + type
            );
        }
    }

    private static final class ListValue {
        final byte elementType;
        final List<Tag> values;

        ListValue(byte elementType, List<Tag> values) {
            this.elementType = elementType;
            this.values = Collections.unmodifiableList(values);
        }
    }

    private static final class Input {
        private final byte[] bytes;
        private final Endian endian;
        private int offset;
        private long collectionElements;

        Input(byte[] bytes, Endian endian) {
            this.bytes = bytes;
            this.endian = endian;
        }

        Tag readPayload(byte type, int depth) throws IOException {
            if (depth > MAX_DEPTH) throw new IOException("NBT nesting is too deep");
            switch (type) {
                case 1:
                    return new Tag(type, readByte());
                case 2:
                    return new Tag(type, readShort());
                case 3:
                    return new Tag(type, readInt());
                case 4:
                    return new Tag(type, readLong());
                case 5:
                    return new Tag(type, Float.intBitsToFloat(readInt()));
                case 6:
                    return new Tag(type, Double.longBitsToDouble(readLong()));
                case 7: {
                    int length = readLength("byte array");
                    count(length);
                    return new Tag(type, readBytes(length));
                }
                case 8:
                    return new Tag(type, readString());
                case 9: {
                    byte elementType = readByte();
                    int length = readLength("list");
                    if (elementType == 0 && length != 0) {
                        throw new IOException("Non-empty NBT list has TAG_End type");
                    }
                    if (elementType < 0 || elementType > 12) {
                        throw new IOException("Unknown NBT list element type");
                    }
                    count(length);
                    List<Tag> values = new ArrayList<>(Math.min(length, 65_536));
                    for (int index = 0; index < length; ++index) {
                        values.add(readPayload(elementType, depth + 1));
                    }
                    return new Tag(type, new ListValue(elementType, values));
                }
                case 10: {
                    Map<String, Tag> values = new LinkedHashMap<>();
                    while (true) {
                        byte childType = readByte();
                        if (childType == 0) break;
                        if (childType < 0 || childType > 12) {
                            throw new IOException("Unknown NBT tag type");
                        }
                        String name = readString();
                        values.put(name, readPayload(childType, depth + 1));
                        count(1);
                    }
                    return new Tag(type, Collections.unmodifiableMap(values));
                }
                case 11: {
                    int length = readLength("int array");
                    count(length);
                    int[] values = new int[length];
                    for (int index = 0; index < length; ++index) {
                        values[index] = readInt();
                    }
                    return new Tag(type, values);
                }
                case 12: {
                    int length = readLength("long array");
                    count(length);
                    long[] values = new long[length];
                    for (int index = 0; index < length; ++index) {
                        values[index] = readLong();
                    }
                    return new Tag(type, values);
                }
                default:
                    throw new IOException("Unsupported NBT tag type " + type);
            }
        }

        String readString() throws IOException {
            int length = readUnsignedShort();
            if (length > MAX_STRING_BYTES) {
                throw new IOException("NBT string is too long");
            }
            byte[] value = readBytes(length);
            return new String(value, StandardCharsets.UTF_8);
        }

        byte readByte() throws IOException {
            require(1);
            return bytes[offset++];
        }

        short readShort() throws IOException {
            return (short) readUnsignedShort();
        }

        int readUnsignedShort() throws IOException {
            require(2);
            int first = bytes[offset++] & 0xff;
            int second = bytes[offset++] & 0xff;
            return endian == Endian.BIG
                ? (first << 8) | second
                : first | (second << 8);
        }

        int readInt() throws IOException {
            require(4);
            int result;
            if (endian == Endian.BIG) {
                result = ((bytes[offset] & 0xff) << 24) |
                    ((bytes[offset + 1] & 0xff) << 16) |
                    ((bytes[offset + 2] & 0xff) << 8) |
                    (bytes[offset + 3] & 0xff);
            } else {
                result = (bytes[offset] & 0xff) |
                    ((bytes[offset + 1] & 0xff) << 8) |
                    ((bytes[offset + 2] & 0xff) << 16) |
                    ((bytes[offset + 3] & 0xff) << 24);
            }
            offset += 4;
            return result;
        }

        long readLong() throws IOException {
            require(8);
            long result = 0;
            if (endian == Endian.BIG) {
                for (int index = 0; index < 8; ++index) {
                    result = (result << 8) | (bytes[offset + index] & 0xffL);
                }
            } else {
                for (int index = 7; index >= 0; --index) {
                    result = (result << 8) | (bytes[offset + index] & 0xffL);
                }
            }
            offset += 8;
            return result;
        }

        byte[] readBytes(int length) throws IOException {
            require(length);
            byte[] value = new byte[length];
            System.arraycopy(bytes, offset, value, 0, length);
            offset += length;
            return value;
        }

        int readLength(String kind) throws IOException {
            int length = readInt();
            if (length < 0 || length > MAX_COLLECTION_ELEMENTS) {
                throw new IOException("Invalid NBT " + kind + " length");
            }
            return length;
        }

        void count(int amount) throws IOException {
            collectionElements += amount;
            if (collectionElements > MAX_COLLECTION_ELEMENTS) {
                throw new IOException("NBT file contains too many values");
            }
        }

        void require(int amount) throws IOException {
            if (amount < 0 || offset > bytes.length - amount) {
                throw new IOException("Unexpected end of NBT file");
            }
        }
    }
}
