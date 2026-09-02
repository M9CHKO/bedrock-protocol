package com.m9chko.bedrockrelay;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.LinkedHashSet;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;

/** Pure block-name matching used to classify schematic cells against world data. */
public final class SchematicBlockMatcher {
    private static final int FNV1A_OFFSET_BASIS = 0x811c9dc5;
    private static final int FNV1A_PRIME = 0x01000193;

    private static final int AIR_HASH = canonicalNameHash("air");
    private static final int CAVE_AIR_HASH = canonicalNameHash("cave_air");
    private static final int VOID_AIR_HASH = canonicalNameHash("void_air");
    private static final int STRUCTURE_VOID_HASH = canonicalNameHash("structure_void");

    private SchematicBlockMatcher() {}

    public enum Status {
        UNKNOWN,
        MISSING,
        CORRECT,
        WRONG
    }

    /** Canonical namespace-free base name, with state properties removed. */
    public static String canonicalBlockName(String blockState) {
        return TexturePackPaths.normalizedBlockName(blockState);
    }

    /**
     * Stable FNV-1a 32-bit hash of {@link #canonicalBlockName(String)}.
     * Zero is reserved for a null, blank, or otherwise empty name.
     */
    public static int canonicalNameHash(String blockState) {
        String canonicalName = canonicalBlockName(blockState);
        if (canonicalName.isEmpty()) return 0;

        int hash = FNV1A_OFFSET_BASIS;
        for (byte value : canonicalName.getBytes(StandardCharsets.UTF_8)) {
            hash ^= value & 0xff;
            hash *= FNV1A_PRIME;
        }
        return hash;
    }

    /** Canonical Bedrock name plus sorted state properties. */
    public static String canonicalStateSignature(String blockState) {
        String canonicalName = canonicalBlockName(blockState);
        if (canonicalName.isEmpty()) return "";
        String value = blockState == null ? "" : blockState.trim();
        int propertyStart = value.indexOf('[');
        int propertyEnd = value.lastIndexOf(']');
        if (propertyStart < 0 || propertyEnd <= propertyStart + 1) {
            return canonicalName;
        }

        Map<String, String> properties = new TreeMap<>();
        String body = value.substring(propertyStart + 1, propertyEnd);
        for (String entry : body.split(",")) {
            int separator = entry.indexOf('=');
            if (separator <= 0) continue;
            String name = entry.substring(0, separator)
                .trim()
                .toLowerCase(Locale.ROOT);
            String propertyValue = entry.substring(separator + 1)
                .trim()
                .toLowerCase(Locale.ROOT);
            if (name.isEmpty() || propertyValue.isEmpty()) continue;
            if (propertyValue.equals("true")) propertyValue = "1";
            else if (propertyValue.equals("false")) propertyValue = "0";
            properties.put(name, propertyValue);
        }
        if (properties.isEmpty()) return canonicalName;

        StringBuilder result = new StringBuilder(canonicalName).append('[');
        boolean first = true;
        for (Map.Entry<String, String> property : properties.entrySet()) {
            if (!first) result.append(',');
            first = false;
            result.append(property.getKey())
                .append('=')
                .append(property.getValue());
        }
        return result.append(']').toString();
    }

    public static int canonicalStateHash(String blockState) {
        return fnv1a32(canonicalStateSignature(blockState));
    }

    /** Builds the accepted canonical-name set for one expected schematic block. */
    public static ExpectedBlock expected(String expectedState, Iterable<String> aliases) {
        Set<Integer> hashes = new LinkedHashSet<>();
        addHash(hashes, expectedState);
        if (aliases != null) {
            for (String alias : aliases) addHash(hashes, alias);
        }
        if (hashes.isEmpty()) {
            throw new IllegalArgumentException("expected block has no usable name");
        }
        int[] values = new int[hashes.size()];
        int index = 0;
        for (int hash : hashes) values[index++] = hash;
        String signature = canonicalStateSignature(expectedState);
        return new ExpectedBlock(
            canonicalBlockName(expectedState),
            values,
            fnv1a32(signature),
            signature.indexOf('[') >= 0
        );
    }

    public static ExpectedBlock expected(String expectedState, String... aliases) {
        return expected(expectedState, aliases == null ? null : Arrays.asList(aliases));
    }

    /** Classifies a decoded block-state name. Null/empty means unavailable data. */
    public static Status match(ExpectedBlock expected, String actualState) {
        if (actualState == null || canonicalBlockName(actualState).isEmpty()) {
            return Status.UNKNOWN;
        }
        return match(expected, true, canonicalNameHash(actualState));
    }

    /**
     * Classifies a pre-hashed world block. {@code actualKnown} distinguishes an
     * unloaded/unresolved cell from a known cell whose block is air.
     */
    public static Status match(
        ExpectedBlock expected,
        boolean actualKnown,
        int actualCanonicalNameHash
    ) {
        if (expected == null) throw new NullPointerException("expected");
        if (!actualKnown || actualCanonicalNameHash == 0) return Status.UNKNOWN;
        if (isEmptyWorldBlock(actualCanonicalNameHash)) return Status.MISSING;
        return expected.matchesHash(actualCanonicalNameHash)
            ? Status.CORRECT
            : Status.WRONG;
    }

    /**
     * Matches a native world state. Exact property comparison is enabled for
     * Bedrock palettes whose property names and values share native semantics.
     */
    public static Status match(
        ExpectedBlock expected,
        boolean actualKnown,
        int actualCanonicalNameHash,
        int actualCanonicalStateHash,
        boolean requireExactProperties
    ) {
        Status base = match(expected, actualKnown, actualCanonicalNameHash);
        if (base != Status.CORRECT || !requireExactProperties ||
            !expected.hasStateProperties) {
            return base;
        }
        return actualCanonicalStateHash != 0 &&
            actualCanonicalStateHash == expected.exactStateHash
                ? Status.CORRECT
                : Status.WRONG;
    }

    private static boolean isEmptyWorldBlock(int hash) {
        return hash == AIR_HASH || hash == CAVE_AIR_HASH ||
            hash == VOID_AIR_HASH || hash == STRUCTURE_VOID_HASH;
    }

    private static void addHash(Set<Integer> hashes, String blockState) {
        int hash = canonicalNameHash(blockState);
        if (hash != 0) hashes.add(hash);
    }

    private static int fnv1a32(String value) {
        if (value == null || value.isEmpty()) return 0;
        int hash = FNV1A_OFFSET_BASIS;
        for (byte next : value.getBytes(StandardCharsets.UTF_8)) {
            hash ^= next & 0xff;
            hash *= FNV1A_PRIME;
        }
        return hash;
    }

    /** Precomputed canonical hashes for one schematic block and all its aliases. */
    public static final class ExpectedBlock {
        private final String canonicalName;
        private final int[] candidateHashes;
        private final int exactStateHash;
        private final boolean hasStateProperties;

        private ExpectedBlock(
            String canonicalName,
            int[] candidateHashes,
            int exactStateHash,
            boolean hasStateProperties
        ) {
            this.canonicalName = canonicalName;
            this.candidateHashes = candidateHashes;
            this.exactStateHash = exactStateHash;
            this.hasStateProperties = hasStateProperties;
        }

        public String canonicalName() {
            return canonicalName;
        }

        public int candidateCount() {
            return candidateHashes.length;
        }

        public int[] candidateHashes() {
            return candidateHashes.clone();
        }

        public boolean matchesHash(int actualCanonicalNameHash) {
            for (int candidate : candidateHashes) {
                if (candidate == actualCanonicalNameHash) return true;
            }
            return false;
        }

        public int exactStateHash() {
            return exactStateHash;
        }

        public boolean hasStateProperties() {
            return hasStateProperties;
        }
    }
}
