package com.m9chko.bedrockrelay;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertThrows;

import org.junit.Test;

public final class SchematicPlacementTransformTest {
    @Test
    public void mapsBlockCentersForEveryRotationAndMirrorCombination() {
        int[][][] expected = {
            {{10, 66, 21}, {8, 66, 20}, {9, 66, 18}, {11, 66, 19}},
            {{12, 66, 21}, {8, 66, 22}, {7, 66, 18}, {11, 66, 17}}
        };

        for (int mirrorIndex = 0; mirrorIndex < 2; mirrorIndex++) {
            for (int rotation = 0; rotation < 4; rotation++) {
                SchematicPlacementTransform transform =
                    new SchematicPlacementTransform(
                        10, 64, 20, 3, rotation, mirrorIndex == 1
                    );
                SchematicPlacementTransform.BlockPosition actual =
                    transform.worldBlock(0, 2, 1);
                int[] wanted = expected[mirrorIndex][rotation];
                assertEquals("x, mirror=" + mirrorIndex + " rot=" + rotation,
                    wanted[0], actual.x());
                assertEquals("y, mirror=" + mirrorIndex + " rot=" + rotation,
                    wanted[1], actual.y());
                assertEquals("z, mirror=" + mirrorIndex + " rot=" + rotation,
                    wanted[2], actual.z());

                assertEquals(actual.x(),
                    (int) Math.floor(transform.worldX(0.5d, 1.5d)));
                assertEquals(actual.z(),
                    (int) Math.floor(transform.worldZ(0.5d, 1.5d)));
            }
        }
    }

    @Test
    public void preservesFloorSemanticsAtNegativeCoordinates() {
        SchematicPlacementTransform transform =
            new SchematicPlacementTransform(-10, -4, -20, 4, 1, true);

        assertEquals(
            new SchematicPlacementTransform.BlockPosition(-14, -3, -19),
            transform.worldBlock(2, 1, 3)
        );
    }

    @Test
    public void normalizesQuarterTurns() {
        SchematicPlacementTransform minusOne =
            new SchematicPlacementTransform(0, 0, 0, 2, -1, false);
        SchematicPlacementTransform three =
            new SchematicPlacementTransform(0, 0, 0, 2, 3, false);

        assertEquals(3, minusOne.rotationQuarterTurns());
        assertEquals(three.worldBlock(1, 0, 0), minusOne.worldBlock(1, 0, 0));
    }

    @Test
    public void roundsCollisionSurfaceUpToPlacementCell() {
        assertEquals(64, SchematicPlacementTransform.placementAnchorY(64.0d));
        assertEquals(65, SchematicPlacementTransform.placementAnchorY(64.125d));
        assertEquals(65, SchematicPlacementTransform.placementAnchorY(64.5d));
        assertEquals(-3, SchematicPlacementTransform.placementAnchorY(-3.5d));
    }

    @Test
    public void rejectsNonFiniteCollisionSurface() {
        assertThrows(IllegalArgumentException.class,
            () -> SchematicPlacementTransform.placementAnchorY(Double.NaN));
    }
}
