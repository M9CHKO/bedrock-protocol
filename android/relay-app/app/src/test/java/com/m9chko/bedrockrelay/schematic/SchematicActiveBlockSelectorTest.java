package com.m9chko.bedrockrelay.schematic;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import com.m9chko.bedrockrelay.SchematicPlacementTransform;

import java.util.Arrays;
import java.util.Collections;

import org.junit.Test;

public final class SchematicActiveBlockSelectorTest {
    @Test
    public void boundedSelectionFollowsCameraChunkInsteadOfModelBoundary() {
        SchematicModel model = solidModel(64, 1, 1);
        SchematicPlacementTransform transform =
            new SchematicPlacementTransform(0, 0, 0, 64, 0, false);

        int[] firstChunk = SchematicActiveBlockSelector.select(
            model, transform, 0, 0, 0, 32, -1, 16
        );
        int[] thirdChunk = SchematicActiveBlockSelector.select(
            model, transform, 2, 0, 0, 32, -1, 16
        );

        assertArrayEquals(range(0, 16), firstChunk);
        assertArrayEquals(range(32, 48), thirdChunk);
    }

    @Test
    public void rotationAndMirrorStillSelectTheCameraSection() {
        SchematicModel model = solidModel(32, 1, 1);
        for (int rotation = 0; rotation < 4; ++rotation) {
            for (boolean mirrored : new boolean[] {false, true}) {
                SchematicPlacementTransform transform =
                    new SchematicPlacementTransform(
                        100,
                        20,
                        -50,
                        model.sizeX(),
                        rotation,
                        mirrored
                    );
                SchematicPlacementTransform.BlockPosition target =
                    transform.worldBlock(20, 0, 0);
                int[] selected = SchematicActiveBlockSelector.select(
                    model,
                    transform,
                    Math.floorDiv(target.x(), 16),
                    Math.floorDiv(target.y(), 16),
                    Math.floorDiv(target.z(), 16),
                    32,
                    -1,
                    32
                );
                assertEquals(32, selected.length);
                assertTrue(
                    "target missing for rotation=" + rotation +
                        " mirrored=" + mirrored,
                    Arrays.stream(selected).anyMatch(index -> index == 20)
                );
            }
        }
    }

    @Test
    public void staleOrDistantSelectedLayerProducesEmptyWindow() {
        SchematicModel model = solidModel(1, 3, 1);
        SchematicPlacementTransform transform =
            new SchematicPlacementTransform(0, 1_000, 0, 1, 0, false);

        assertEquals(0, SchematicActiveBlockSelector.select(
            model, transform, 0, 0, 0, 32, 1, 8
        ).length);
        assertEquals(0, SchematicActiveBlockSelector.select(
            model, transform, 0, 0, 0, 32, 99, 8
        ).length);
    }

    private static SchematicModel solidModel(int x, int y, int z) {
        int[] blocks = new int[Math.multiplyExact(Math.multiplyExact(x, y), z)];
        return new SchematicModel(
            "solid",
            "Bedrock .mcstructure",
            x,
            y,
            z,
            Collections.singletonList("minecraft:stone"),
            blocks
        );
    }

    private static int[] range(int start, int end) {
        int[] values = new int[end - start];
        for (int index = 0; index < values.length; ++index) {
            values[index] = start + index;
        }
        return values;
    }
}
