package com.m9chko.bedrockrelay;

import static org.junit.Assert.assertEquals;

import java.util.List;

import org.junit.Test;

public final class SchematicBlockMatcherTest {
    @Test
    public void canonicalizesNamespacePropertiesAndCase() {
        assertEquals("oak_log", SchematicBlockMatcher.canonicalBlockName(
            " Minecraft:Oak_Log[axis=y] "
        ));
        assertEquals(
            SchematicBlockMatcher.canonicalNameHash("minecraft:oak_log[axis=x]"),
            SchematicBlockMatcher.canonicalNameHash("OAK_LOG")
        );
    }

    @Test
    public void usesStableFnv1a32Hash() {
        assertEquals(
            0x83791397,
            SchematicBlockMatcher.canonicalNameHash("minecraft:oak_log")
        );
    }

    @Test
    public void canonicalizesBedrockStateProperties() {
        assertEquals(
            "oak_log[old_log_type=birch,pillar_axis=y,stripped_bit=0]",
            SchematicBlockMatcher.canonicalStateSignature(
                "minecraft:oak_log[stripped_bit=false,pillar_axis=Y," +
                    "old_log_type=birch]"
            )
        );
    }

    @Test
    public void aliasesAreCandidatesAndDuplicatesAreRemoved() {
        SchematicBlockMatcher.ExpectedBlock expected =
            SchematicBlockMatcher.expected(
                "minecraft:bricks",
                List.of("brick_block", "minecraft:brick_block", "bricks")
            );

        assertEquals(2, expected.candidateCount());
        assertEquals(SchematicBlockMatcher.Status.CORRECT,
            SchematicBlockMatcher.match(expected, "minecraft:brick_block"));
    }

    @Test
    public void distinguishesUnknownMissingCorrectAndWrong() {
        SchematicBlockMatcher.ExpectedBlock expected =
            SchematicBlockMatcher.expected("minecraft:oak_log", "log");

        assertEquals(SchematicBlockMatcher.Status.UNKNOWN,
            SchematicBlockMatcher.match(expected, null));
        assertEquals(SchematicBlockMatcher.Status.UNKNOWN,
            SchematicBlockMatcher.match(expected, false,
                SchematicBlockMatcher.canonicalNameHash("oak_log")));
        assertEquals(SchematicBlockMatcher.Status.MISSING,
            SchematicBlockMatcher.match(expected, "minecraft:air"));
        assertEquals(SchematicBlockMatcher.Status.MISSING,
            SchematicBlockMatcher.match(expected, "minecraft:structure_void"));
        assertEquals(SchematicBlockMatcher.Status.CORRECT,
            SchematicBlockMatcher.match(expected, "minecraft:oak_log[axis=z]"));
        assertEquals(SchematicBlockMatcher.Status.CORRECT,
            SchematicBlockMatcher.match(expected, "minecraft:log"));
        assertEquals(SchematicBlockMatcher.Status.WRONG,
            SchematicBlockMatcher.match(expected, "minecraft:stone"));
    }

    @Test
    public void exactBedrockPropertiesDistinguishOrientation() {
        SchematicBlockMatcher.ExpectedBlock expected =
            SchematicBlockMatcher.expected(
                "minecraft:oak_log[pillar_axis=y,stripped_bit=false]"
            );
        int base = SchematicBlockMatcher.canonicalNameHash("oak_log");
        int correctState = SchematicBlockMatcher.canonicalStateHash(
            "oak_log[stripped_bit=0,pillar_axis=y]"
        );
        int wrongState = SchematicBlockMatcher.canonicalStateHash(
            "oak_log[stripped_bit=0,pillar_axis=x]"
        );

        assertEquals(SchematicBlockMatcher.Status.CORRECT,
            SchematicBlockMatcher.match(
                expected, true, base, correctState, true
            ));
        assertEquals(SchematicBlockMatcher.Status.WRONG,
            SchematicBlockMatcher.match(
                expected, true, base, wrongState, true
            ));
        assertEquals(SchematicBlockMatcher.Status.CORRECT,
            SchematicBlockMatcher.match(
                expected, true, base, wrongState, false
            ));
    }
}
