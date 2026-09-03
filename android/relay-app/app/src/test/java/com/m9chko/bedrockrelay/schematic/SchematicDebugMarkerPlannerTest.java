package com.m9chko.bedrockrelay.schematic;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertThrows;

import com.m9chko.bedrockrelay.SchematicDebugMarkerPlanner;
import com.m9chko.bedrockrelay.SchematicPlacementTransform;

import java.util.Arrays;

import org.junit.Test;

public final class SchematicDebugMarkerPlannerTest {
    @Test
    public void countsWholeModelAndDisplaysOnlyLoadedMissingOrWrongCells() {
        SchematicModel model = model(
            2,
            2,
            2,
            1, 1,
            1, 0,
            1, 1,
            0, 1
        );
        int[] indices = {0, 1, 2, 4, 5};
        byte[] states = {
            SchematicDebugMarkerPlanner.BLOCK_MISSING,
            SchematicDebugMarkerPlanner.BLOCK_CORRECT,
            SchematicDebugMarkerPlanner.BLOCK_WRONG,
            SchematicDebugMarkerPlanner.BLOCK_UNKNOWN,
            SchematicDebugMarkerPlanner.BLOCK_MISSING
        };

        SchematicDebugMarkerPlanner.Result result =
            SchematicDebugMarkerPlanner.plan(
                model,
                new SchematicPlacementTransform(10, 20, 30, 2, 0, false),
                indices,
                states,
                true,
                10.5d,
                20.5d,
                30.5d,
                0,
                4,
                40
            );

        assertEquals(6, result.total());
        assertEquals(1, result.correct());
        assertEquals(2, result.missing());
        assertEquals(1, result.wrong());
        assertEquals(2, result.unknown());
        assertEquals(2, result.displayed());
        assertEquals(40, result.opacityPercent());
        assertEquals(0.4f, result.alpha(), 0.0001f);
        assertArrayEquals(new int[] {
            10, 20, 30, SchematicDebugMarkerPlanner.BLOCK_MISSING,
            10, 20, 31, SchematicDebugMarkerPlanner.BLOCK_WRONG
        }, result.records());
        assertArrayEquals(
            new String[] {"minecraft:stone", null},
            result.expectedBlockStates()
        );
    }

    @Test
    public void appliesPlacementTransformBeforeSortingAndRadiusFiltering() {
        SchematicModel model = model(3, 1, 1, 1, 1, 1);
        int[] indices = {2, 0, 1};
        byte[] states = {
            SchematicDebugMarkerPlanner.BLOCK_WRONG,
            SchematicDebugMarkerPlanner.BLOCK_MISSING,
            SchematicDebugMarkerPlanner.BLOCK_MISSING
        };

        SchematicDebugMarkerPlanner.Result result =
            SchematicDebugMarkerPlanner.plan(
                model,
                new SchematicPlacementTransform(-10, 7, -20, 3, 1, true),
                indices,
                states,
                true,
                -10.5d,
                7.5d,
                -19.5d,
                -1,
                2,
                85,
                false
            );

        assertArrayEquals(new int[] {
            -11, 7, -20, SchematicDebugMarkerPlanner.BLOCK_WRONG,
            -11, 7, -19, SchematicDebugMarkerPlanner.BLOCK_MISSING,
            -11, 7, -18, SchematicDebugMarkerPlanner.BLOCK_MISSING
        }, result.records());
        assertArrayEquals(new String[] {
            null, null, null
        }, result.expectedBlockStates());
    }

    @Test
    public void retainsNearestEighteenHundredWithoutDependingOnInputOrder() {
        int count = 2_001;
        int[] blocks = new int[count];
        Arrays.fill(blocks, 1);
        SchematicModel model = model(count, 1, 1, blocks);
        int[] indices = new int[count];
        byte[] states = new byte[count];
        Arrays.fill(states, SchematicDebugMarkerPlanner.BLOCK_MISSING);
        for (int index = 0; index < count; ++index) {
            indices[index] = count - index - 1;
        }

        SchematicDebugMarkerPlanner.Result result =
            SchematicDebugMarkerPlanner.plan(
                model,
                new SchematicPlacementTransform(0, 0, 0, count, 0, false),
                indices,
                states,
                true,
                0.5d,
                0.5d,
                0.5d,
                -1,
                3_000,
                50
            );

        assertEquals(SchematicDebugMarkerPlanner.MAX_DISPLAYED_MARKERS,
            result.displayed());
        int[] records = result.records();
        assertEquals(0, records[0]);
        assertEquals(1_799, records[records.length - 4]);
    }

    @Test
    public void suppressesRecordsWithoutCameraOrWithZeroOpacity() {
        SchematicModel model = model(1, 1, 1, 1);
        int[] indices = {0};
        byte[] states = {SchematicDebugMarkerPlanner.BLOCK_WRONG};
        SchematicPlacementTransform transform =
            new SchematicPlacementTransform(0, 0, 0, 1, 0, false);

        SchematicDebugMarkerPlanner.Result cameraUnknown =
            SchematicDebugMarkerPlanner.plan(
                model,
                transform,
                indices,
                states,
                false,
                Double.NaN,
                Double.NaN,
                Double.NaN,
                -1,
                32,
                50
            );
        SchematicDebugMarkerPlanner.Result transparent =
            SchematicDebugMarkerPlanner.plan(
                model,
                transform,
                indices,
                states,
                true,
                0.5d,
                0.5d,
                0.5d,
                -1,
                32,
                0
            );

        assertEquals(0, cameraUnknown.displayed());
        assertEquals(0, cameraUnknown.expectedBlockStates().length);
        assertEquals(1, cameraUnknown.wrong());
        assertEquals(0, transparent.displayed());
        assertEquals(1, transparent.wrong());
    }

    @Test
    public void rejectsMismatchedArraysAndAirIndices() {
        SchematicModel model = model(2, 1, 1, 1, 0);
        SchematicPlacementTransform transform =
            new SchematicPlacementTransform(0, 0, 0, 2, 0, false);

        assertThrows(IllegalArgumentException.class, () ->
            SchematicDebugMarkerPlanner.plan(
                model,
                transform,
                new int[] {0},
                new byte[0],
                true,
                0.5d,
                0.5d,
                0.5d,
                -1,
                32,
                50
            )
        );
        assertThrows(IllegalArgumentException.class, () ->
            SchematicDebugMarkerPlanner.plan(
                model,
                transform,
                new int[] {1},
                new byte[] {SchematicDebugMarkerPlanner.BLOCK_UNKNOWN},
                true,
                0.5d,
                0.5d,
                0.5d,
                -1,
                32,
                50
            )
        );
    }

    private static SchematicModel model(
        int sizeX,
        int sizeY,
        int sizeZ,
        int... blocks
    ) {
        return new SchematicModel(
            "planner-test",
            "Bedrock .mcstructure",
            sizeX,
            sizeY,
            sizeZ,
            Arrays.asList("minecraft:air", "minecraft:stone"),
            blocks
        );
    }
}
