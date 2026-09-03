package com.m9chko.bedrockrelay;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertSame;

import org.junit.Test;

public final class SchematicBlockStateTransformTest {
    @Test
    public void identityPreservesExactPaletteString() {
        String state =
            "minecraft:oak_stairs[upside_down_bit=false,weirdo_direction=3]";

        assertSame(state, SchematicBlockStateTransform.transform(state, 0, false));
    }

    @Test
    public void rotatesModernAndLegacyFacingProperties() {
        assertTransform(
            "minecraft:furnace[minecraft:cardinal_direction=north]",
            1,
            false,
            "minecraft:furnace[minecraft:cardinal_direction=east]"
        );
        assertTransform(
            "minecraft:dispenser[facing_direction=2,triggered_bit=false]",
            1,
            false,
            "minecraft:dispenser[facing_direction=5,triggered_bit=false]"
        );
        assertTransform(
            "minecraft:observer[minecraft:facing_direction=west]",
            3,
            false,
            "minecraft:observer[minecraft:facing_direction=south]"
        );
        assertTransform(
            "minecraft:torch[torch_facing_direction=north]",
            1,
            false,
            "minecraft:torch[torch_facing_direction=east]"
        );
    }

    @Test
    public void usesPerPropertyBedrockDirectionEncodings() {
        assertTransform(
            "minecraft:oak_stairs[upside_down_bit=false,weirdo_direction=3]",
            1,
            false,
            "minecraft:oak_stairs[upside_down_bit=false,weirdo_direction=0]"
        );
        assertTransform(
            "minecraft:oak_trapdoor[direction=3,open_bit=false]",
            1,
            false,
            "minecraft:oak_trapdoor[direction=0,open_bit=false]"
        );
        assertTransform(
            "minecraft:loom[direction=2]",
            1,
            false,
            "minecraft:loom[direction=3]"
        );
        assertTransform(
            "minecraft:decorated_pot[direction=0]",
            1,
            false,
            "minecraft:decorated_pot[direction=1]"
        );
        assertTransform(
            "minecraft:bell[direction=1]",
            0,
            true,
            "minecraft:bell[direction=3]"
        );
        assertTransform(
            "minecraft:brain_coral_wall_fan[coral_direction=2]",
            1,
            false,
            "minecraft:brain_coral_wall_fan[coral_direction=1]"
        );
    }

    @Test
    public void mirrorsHandedBlocksAndDirectionalKeys() {
        assertTransform(
            "minecraft:wooden_door[door_hinge_bit=true,minecraft:cardinal_direction=east]",
            0,
            true,
            "minecraft:wooden_door[door_hinge_bit=false,minecraft:cardinal_direction=east]"
        );
        assertTransform(
            "minecraft:wooden_door[door_hinge_bit=false,minecraft:cardinal_direction=north]",
            0,
            true,
            "minecraft:wooden_door[door_hinge_bit=true,minecraft:cardinal_direction=south]"
        );
        assertTransform(
            "minecraft:small_dripleaf_block[minecraft:cardinal_direction=west]",
            0,
            true,
            "minecraft:small_dripleaf_block[minecraft:cardinal_direction=west]"
        );
        assertTransform(
            "minecraft:oak_stairs[shape=inner_left,weirdo_direction=0]",
            0,
            true,
            "minecraft:oak_stairs[shape=inner_right,weirdo_direction=1]"
        );
        assertTransform(
            "minecraft:cobblestone_wall[wall_connection_type_east=tall,wall_connection_type_north=short]",
            1,
            false,
            "minecraft:cobblestone_wall[wall_connection_type_east=short,wall_connection_type_south=tall]"
        );
    }

    @Test
    public void rotatesAxesSignsRailsAndFaceMasks() {
        assertTransform(
            "minecraft:oak_log[pillar_axis=x]",
            1,
            false,
            "minecraft:oak_log[pillar_axis=z]"
        );
        assertTransform(
            "minecraft:standing_sign[ground_sign_direction=0]",
            1,
            false,
            "minecraft:standing_sign[ground_sign_direction=4]"
        );
        assertTransform(
            "minecraft:chalkboard[direction=4]",
            0,
            true,
            "minecraft:chalkboard[direction=12]"
        );
        assertTransform(
            "minecraft:rail[rail_direction=6]",
            1,
            false,
            "minecraft:rail[rail_direction=7]"
        );
        assertTransform(
            "minecraft:glow_lichen[multi_face_direction_bits=48]",
            1,
            false,
            "minecraft:glow_lichen[multi_face_direction_bits=36]"
        );
        assertTransform(
            "minecraft:vine[vine_direction_bits=3]",
            1,
            false,
            "minecraft:vine[vine_direction_bits=6]"
        );
        assertTransform(
            "minecraft:large_amethyst_bud[minecraft:block_face=east]",
            0,
            true,
            "minecraft:large_amethyst_bud[minecraft:block_face=west]"
        );
        assertTransform(
            "minecraft:red_mushroom_block[huge_mushroom_bits=1]",
            1,
            false,
            "minecraft:red_mushroom_block[huge_mushroom_bits=3]"
        );
    }

    @Test
    public void rotatesCompoundOrientationAndJavaStates() {
        assertTransform(
            "minecraft:crafter[orientation=north_up]",
            1,
            false,
            "minecraft:crafter[orientation=east_up]"
        );
        assertTransform(
            "minecraft:oak_sign[rotation=4]",
            0,
            true,
            "minecraft:oak_sign[rotation=12]"
        );
        assertTransform(
            "minecraft:powered_rail[shape=ascending_east]",
            1,
            false,
            "minecraft:powered_rail[shape=ascending_south]"
        );
        assertTransform(
            "minecraft:oak_door[facing=east,hinge=left]",
            0,
            true,
            "minecraft:oak_door[facing=west,hinge=right]"
        );
    }

    private static void assertTransform(
        String source,
        int rotation,
        boolean mirrored,
        String expected
    ) {
        assertEquals(
            expected,
            SchematicBlockStateTransform.transform(source, rotation, mirrored)
        );
    }
}
