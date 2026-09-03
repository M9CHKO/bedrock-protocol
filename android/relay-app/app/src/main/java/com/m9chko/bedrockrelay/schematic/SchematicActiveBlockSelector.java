package com.m9chko.bedrockrelay.schematic;

import com.m9chko.bedrockrelay.SchematicPlacementTransform;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;

/** Selects a stable, bounded set of schematic cells around the camera. */
public final class SchematicActiveBlockSelector {
    /** Keeps JNI snapshots and comparison state bounded on very large models. */
    public static final int MAX_ACTIVE_BLOCKS = 262_144;

    private SchematicActiveBlockSelector() {}

    /**
     * Returns non-air indices from the nearest 16x16x16 world sections.
     *
     * <p>The selection key is deliberately chunk based. Sub-block camera
     * movement cannot reshuffle the query, while crossing a chunk boundary
     * exposes the next nearby part of a large schematic. Cells outside the
     * returned set remain UNKNOWN in the global progress counters.
     */
    public static int[] select(
        SchematicModel model,
        SchematicPlacementTransform transform,
        int cameraChunkX,
        int cameraChunkY,
        int cameraChunkZ,
        int maximumDistance,
        int selectedLayer
    ) {
        return select(
            model,
            transform,
            cameraChunkX,
            cameraChunkY,
            cameraChunkZ,
            maximumDistance,
            selectedLayer,
            MAX_ACTIVE_BLOCKS
        );
    }

    static int[] select(
        SchematicModel model,
        SchematicPlacementTransform transform,
        int cameraChunkX,
        int cameraChunkY,
        int cameraChunkZ,
        int maximumDistance,
        int selectedLayer,
        int maximumActiveBlocks
    ) {
        if (model == null) throw new NullPointerException("model");
        if (transform == null) throw new NullPointerException("transform");
        if (maximumDistance < 0) {
            throw new IllegalArgumentException("maximumDistance is negative");
        }
        if (selectedLayer < -1) {
            throw new IllegalArgumentException("selectedLayer is out of bounds");
        }
        // A layer saved for a previously loaded taller model simply has no
        // active cells in the current model.
        if (selectedLayer >= model.sizeY()) return new int[0];
        if (maximumActiveBlocks < 0) {
            throw new IllegalArgumentException("maximumActiveBlocks is negative");
        }
        int capacity = Math.min(maximumActiveBlocks, model.nonAirBlocks());
        if (capacity == 0) return new int[0];

        long radiusValue = (maximumDistance + 15L) / 16L + 1L;
        if (radiusValue > 64L) {
            throw new IllegalArgumentException("active section radius is too large");
        }
        int sectionRadius = Math.max(1, (int) radiusValue);
        Integer selectedSectionY = null;
        Integer selectedDy = null;
        if (selectedLayer >= 0) {
            long worldY = (long) transform.anchorY() + selectedLayer;
            if (worldY < Integer.MIN_VALUE || worldY > Integer.MAX_VALUE) {
                return new int[0];
            }
            selectedSectionY = Math.floorDiv((int) worldY, 16);
            long dy = (long) selectedSectionY - cameraChunkY;
            if (Math.abs(dy) > sectionRadius) return new int[0];
            selectedDy = (int) dy;
        }

        List<SectionOffset> offsets = sectionOffsets(
            sectionRadius,
            selectedDy
        );
        int[] selected = new int[capacity];
        int count = 0;
        long cameraCenterWorldY = (long) cameraChunkY * 16L + 8L;
        long cameraCenterLocalY = cameraCenterWorldY - transform.anchorY();

        for (SectionOffset offset : offsets) {
            long sectionX = (long) cameraChunkX + offset.dx;
            long sectionY = (long) cameraChunkY + offset.dy;
            long sectionZ = (long) cameraChunkZ + offset.dz;
            long worldMinX = sectionX * 16L;
            long worldMinY = sectionY * 16L;
            long worldMinZ = sectionZ * 16L;
            long worldMaxX = worldMinX + 15L;
            long worldMaxY = worldMinY + 15L;
            long worldMaxZ = worldMinZ + 15L;
            if (!fitsInt(worldMinX) || !fitsInt(worldMaxX) ||
                !fitsInt(worldMinY) || !fitsInt(worldMaxY) ||
                !fitsInt(worldMinZ) || !fitsInt(worldMaxZ)) {
                continue;
            }

            SchematicPlacementTransform.BlockPosition corner00 =
                transform.localBlock(
                    (int) worldMinX,
                    transform.anchorY(),
                    (int) worldMinZ
                );
            SchematicPlacementTransform.BlockPosition corner10 =
                transform.localBlock(
                    (int) worldMaxX,
                    transform.anchorY(),
                    (int) worldMinZ
                );
            SchematicPlacementTransform.BlockPosition corner01 =
                transform.localBlock(
                    (int) worldMinX,
                    transform.anchorY(),
                    (int) worldMaxZ
                );
            SchematicPlacementTransform.BlockPosition corner11 =
                transform.localBlock(
                    (int) worldMaxX,
                    transform.anchorY(),
                    (int) worldMaxZ
                );
            int minX = clampMinimum(
                minimum(
                    corner00.x(),
                    corner10.x(),
                    corner01.x(),
                    corner11.x()
                ),
                model.sizeX()
            );
            int maxX = clampMaximum(
                maximum(
                    corner00.x(),
                    corner10.x(),
                    corner01.x(),
                    corner11.x()
                ),
                model.sizeX()
            );
            int minZ = clampMinimum(
                minimum(
                    corner00.z(),
                    corner10.z(),
                    corner01.z(),
                    corner11.z()
                ),
                model.sizeZ()
            );
            int maxZ = clampMaximum(
                maximum(
                    corner00.z(),
                    corner10.z(),
                    corner01.z(),
                    corner11.z()
                ),
                model.sizeZ()
            );
            if (minX > maxX || minZ > maxZ) continue;

            int minY;
            int maxY;
            if (selectedLayer >= 0) {
                minY = selectedLayer;
                maxY = selectedLayer;
            } else {
                long localMinY = worldMinY - transform.anchorY();
                long localMaxY = worldMaxY - transform.anchorY();
                minY = clampMinimum(localMinY, model.sizeY());
                maxY = clampMaximum(localMaxY, model.sizeY());
                if (minY > maxY) continue;
            }

            int pivotY = clamp(
                cameraCenterLocalY,
                minY,
                maxY
            );
            int maximumStep = Math.max(pivotY - minY, maxY - pivotY);
            for (int step = 0; step <= maximumStep; ++step) {
                int lowerY = pivotY - step;
                if (lowerY >= minY) {
                    count = appendLayer(
                        model,
                        selected,
                        count,
                        lowerY,
                        minX,
                        maxX,
                        minZ,
                        maxZ
                    );
                    if (count == selected.length) return selected;
                }
                int upperY = pivotY + step;
                if (step != 0 && upperY <= maxY) {
                    count = appendLayer(
                        model,
                        selected,
                        count,
                        upperY,
                        minX,
                        maxX,
                        minZ,
                        maxZ
                    );
                    if (count == selected.length) return selected;
                }
            }
        }
        return Arrays.copyOf(selected, count);
    }

    private static int appendLayer(
        SchematicModel model,
        int[] output,
        int count,
        int y,
        int minX,
        int maxX,
        int minZ,
        int maxZ
    ) {
        for (int z = minZ; z <= maxZ; ++z) {
            int rowStart = (y * model.sizeZ() + z) * model.sizeX() + minX;
            int rowEnd = rowStart + (maxX - minX) + 1;
            int index = model.nextNonAirBlockIndex(rowStart);
            while (index >= 0 && index < rowEnd) {
                output[count++] = index;
                if (count == output.length) return count;
                index = model.nextNonAirBlockIndex(index + 1);
            }
        }
        return count;
    }

    private static List<SectionOffset> sectionOffsets(
        int radius,
        Integer selectedDy
    ) {
        int verticalCount = selectedDy == null ? radius * 2 + 1 : 1;
        int width = radius * 2 + 1;
        List<SectionOffset> offsets = new ArrayList<>(
            width * width * verticalCount
        );
        int firstDy = selectedDy == null ? -radius : selectedDy;
        int lastDy = selectedDy == null ? radius : selectedDy;
        for (int dy = firstDy; dy <= lastDy; ++dy) {
            for (int dz = -radius; dz <= radius; ++dz) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    offsets.add(new SectionOffset(dx, dy, dz));
                }
            }
        }
        offsets.sort(Comparator
            .comparingInt(SectionOffset::distanceSquared)
            .thenComparingInt(value -> Math.abs(value.dy))
            .thenComparingInt(value -> Math.abs(value.dz))
            .thenComparingInt(value -> Math.abs(value.dx))
            .thenComparingInt(value -> value.dy)
            .thenComparingInt(value -> value.dz)
            .thenComparingInt(value -> value.dx));
        return offsets;
    }

    private static boolean fitsInt(long value) {
        return value >= Integer.MIN_VALUE && value <= Integer.MAX_VALUE;
    }

    private static int clampMinimum(long value, int size) {
        if (value <= 0L) return 0;
        if (value >= size) return size;
        return (int) value;
    }

    private static int clampMaximum(long value, int size) {
        if (value < 0L) return -1;
        if (value >= size) return size - 1;
        return (int) value;
    }

    private static int clamp(long value, int minimum, int maximum) {
        if (value <= minimum) return minimum;
        if (value >= maximum) return maximum;
        return (int) value;
    }

    private static int minimum(int a, int b, int c, int d) {
        return Math.min(Math.min(a, b), Math.min(c, d));
    }

    private static int maximum(int a, int b, int c, int d) {
        return Math.max(Math.max(a, b), Math.max(c, d));
    }

    private static final class SectionOffset {
        final int dx;
        final int dy;
        final int dz;

        SectionOffset(int dx, int dy, int dz) {
            this.dx = dx;
            this.dy = dy;
            this.dz = dz;
        }

        int distanceSquared() {
            return dx * dx + dy * dy + dz * dz;
        }
    }
}
