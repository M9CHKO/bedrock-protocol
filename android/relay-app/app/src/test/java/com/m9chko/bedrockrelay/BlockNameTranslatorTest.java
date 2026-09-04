package com.m9chko.bedrockrelay;

import static org.junit.Assert.assertEquals;

import java.io.BufferedReader;
import java.io.StringReader;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

import org.junit.Test;

public final class BlockNameTranslatorTest {
    @Test
    public void addsLegacyBedrockTextureAliasesForJavaNames() {
        Set<String> aliases = new LinkedHashSet<>();
        BlockNameTranslator.addHeuristicAliases("oak_wall_sign", aliases);
        assertEquals(List.of("wall_sign"), List.copyOf(aliases));

        aliases.clear();
        BlockNameTranslator.addHeuristicAliases("bricks", aliases);
        assertEquals(List.of("brick_block"), List.copyOf(aliases));

        aliases.clear();
        BlockNameTranslator.addHeuristicAliases("dirt_path", aliases);
        assertEquals(List.of("grass_path"), List.copyOf(aliases));
    }

    @Test
    public void readsExactDirectionalJavaToBedrockStates() throws Exception {
        Map<String, String> states = BlockNameTranslator.readExactStates(
            new BufferedReader(new StringReader(
                "minecraft:quartz_stairs[facing=east,half=bottom," +
                    "shape=straight,waterlogged=false]\t" +
                    "minecraft:quartz_stairs[upside_down_bit=false," +
                    "weirdo_direction=0]\n" +
                "ignored-line\n"
            ))
        );

        assertEquals(
            "minecraft:quartz_stairs[upside_down_bit=false," +
                "weirdo_direction=0]",
            states.get(
                "minecraft:quartz_stairs[facing=east,half=bottom," +
                    "shape=straight,waterlogged=false]"
            )
        );
        assertEquals(1, states.size());
    }

    @Test
    public void canonicalizesPropertylessStateForExactLookup() {
        assertEquals(
            "minecraft:quartz_block[]",
            BlockNameTranslator.exactStateKey("quartz_block")
        );
        assertEquals(
            "minecraft:oak_stairs[facing=north]",
            BlockNameTranslator.exactStateKey(
                "minecraft:oak_stairs[facing=north]"
            )
        );
    }
}
