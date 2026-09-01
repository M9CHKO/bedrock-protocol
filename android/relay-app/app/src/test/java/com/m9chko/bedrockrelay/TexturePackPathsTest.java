package com.m9chko.bedrockrelay;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import java.util.List;

import org.junit.Test;

public final class TexturePackPathsTest {
    @Test
    public void findsItemTextureInsideVersionedFullArchive() {
        assertEquals(
            "diamond_helmet.png",
            TexturePackPaths.itemFileName(
                "bedrock-samples-1.21.100.6/resource_pack/textures/items/" +
                    "diamond_helmet.png"
            )
        );
    }

    @Test
    public void flattensOfficialNestedItemTextureFolders() {
        assertEquals(
            "spawn_egg_zombie.png",
            TexturePackPaths.itemFileName(
                "resource_pack/textures/items/spawn_eggs/spawn_egg_zombie.png"
            )
        );
    }

    @Test
    public void rejectsTraversalAndNonPngEntries() {
        assertNull(TexturePackPaths.itemFileName(
            "../resource_pack/textures/items/diamond_sword.png"
        ));
        assertNull(TexturePackPaths.itemFileName(
            "resource_pack/textures/items/diamond_sword.tga"
        ));
        assertNull(TexturePackPaths.itemFileName(
            "resource_pack/textures/blocks/diamond_block.png"
        ));
    }

    @Test
    public void recognizesOfficialGlintWithOptionalArchivePrefix() {
        assertTrue(TexturePackPaths.isEnchantmentGlint(
            "bedrock-samples-main/resource_pack/textures/misc/" +
                "enchanted_item_glint.png"
        ));
        assertFalse(TexturePackPaths.isEnchantmentGlint(
            "resource_pack/textures/misc/enchanted_actor_glint.png"
        ));
    }

    @Test
    public void mapsRegistryNamesToBedrockTextureNames() {
        assertEquals(
            List.of("diamond_helmet.png"),
            TexturePackPaths.textureCandidates("minecraft:diamond_helmet")
        );
        assertEquals(
            List.of("golden_helmet.png", "gold_helmet.png"),
            TexturePackPaths.textureCandidates("minecraft:golden_helmet")
        );
        assertEquals(
            List.of("wooden_sword.png", "wood_sword.png"),
            TexturePackPaths.textureCandidates("minecraft:wooden_sword")
        );
    }

    @Test
    public void addsAnimatedAndLegacyItemAliases() {
        assertEquals(
            List.of("bow.png", "bow_standby.png"),
            TexturePackPaths.textureCandidates("minecraft:bow")
        );
        assertEquals(
            List.of("zombie_spawn_egg.png", "spawn_egg_zombie.png"),
            TexturePackPaths.textureCandidates("minecraft:zombie_spawn_egg")
        );
        assertEquals(
            List.of("totem_of_undying.png", "totem.png"),
            TexturePackPaths.textureCandidates("minecraft:totem_of_undying")
        );
    }
}
