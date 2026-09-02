package com.m9chko.bedrockrelay;

import static org.junit.Assert.assertEquals;

import java.util.LinkedHashSet;
import java.util.List;
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
}
