package com.m9chko.bedrockrelay;

import com.m9chko.bedrockrelay.schematic.SchematicModel;

import java.util.Arrays;
import java.util.Objects;

/**
 * Builds a bounded, deterministic list of client-only schematic markers.
 *
 * <p>The supplied block indices and states are parallel arrays. Indices must
 * identify distinct non-air cells in {@code model}. Callers may supply only a
 * subset of the model; cells omitted from the arrays are counted as unknown.
 * Explicit unknown cells may be displayed while their containing world chunk
 * is waiting to be confirmed as loaded.
 */
public final class SchematicDebugMarkerPlanner {
    public static final byte BLOCK_UNKNOWN = 0;
    public static final byte BLOCK_MISSING = 1;
    public static final byte BLOCK_CORRECT = 2;
    public static final byte BLOCK_WRONG = 3;

    public static final int MAX_DISPLAYED_MARKERS = 1_800;
    // Every retained marker may become a textured falling-block preview. The
    // native side applies a stable cell diff, so raising this to the existing
    // bounded marker limit no longer causes a full entity rebuild on camera
    // movement. This is enough to show ordinary imported builds completely
    // while preserving the hard 1,800-cell memory and packet safety cap.
    public static final int MAX_TEXTURED_MARKERS = MAX_DISPLAYED_MARKERS;
    private static final int RECORD_WIDTH = 4;

    private SchematicDebugMarkerPlanner() {}

    /**
     * Plans unknown, missing, and wrong-block markers nearest to the camera.
     *
     * @param selectedLayer local schematic Y, or {@code -1} for every layer
     * @param maximumDistance maximum camera-to-block-center distance in blocks
     * @param opacityPercent marker opacity in the inclusive range 0..100
     */
    public static Result plan(
        SchematicModel model,
        SchematicPlacementTransform transform,
        int[] blockIndices,
        byte[] blockStates,
        boolean cameraKnown,
        double cameraX,
        double cameraY,
        double cameraZ,
        int selectedLayer,
        int maximumDistance,
        int opacityPercent
    ) {
        return plan(
            model,
            transform,
            blockIndices,
            blockStates,
            cameraKnown,
            cameraX,
            cameraY,
            cameraZ,
            selectedLayer,
            maximumDistance,
            opacityPercent,
            true
        );
    }

    /**
     * Plans markers and optionally includes exact palette states for the
     * bounded set of textured unknown- and missing-block previews.
     */
    public static Result plan(
        SchematicModel model,
        SchematicPlacementTransform transform,
        int[] blockIndices,
        byte[] blockStates,
        boolean cameraKnown,
        double cameraX,
        double cameraY,
        double cameraZ,
        int selectedLayer,
        int maximumDistance,
        int opacityPercent,
        boolean includeTextureStates
    ) {
        Objects.requireNonNull(model, "model");
        Objects.requireNonNull(transform, "transform");
        Objects.requireNonNull(blockIndices, "blockIndices");
        Objects.requireNonNull(blockStates, "blockStates");
        if (blockIndices.length != blockStates.length) {
            throw new IllegalArgumentException(
                "Block indices and states must have equal lengths"
            );
        }
        if (selectedLayer < -1 || selectedLayer >= model.sizeY()) {
            throw new IllegalArgumentException("Selected layer is out of bounds");
        }
        if (maximumDistance < 0) {
            throw new IllegalArgumentException(
                "Maximum marker distance cannot be negative"
            );
        }
        if (opacityPercent < 0 || opacityPercent > 100) {
            throw new IllegalArgumentException(
                "Marker opacity must be between 0 and 100"
            );
        }
        if (cameraKnown && (!Double.isFinite(cameraX) ||
                !Double.isFinite(cameraY) ||
                !Double.isFinite(cameraZ))) {
            throw new IllegalArgumentException(
                "Known camera coordinates must be finite"
            );
        }

        int correct = 0;
        int missing = 0;
        int wrong = 0;
        double maximumDistanceSquared =
            (double) maximumDistance * maximumDistance;
        MarkerHeap markers = cameraKnown && opacityPercent > 0
            ? new MarkerHeap(MAX_DISPLAYED_MARKERS)
            : null;

        for (int listIndex = 0; listIndex < blockIndices.length; ++listIndex) {
            int linearIndex = blockIndices[listIndex];
            if (linearIndex < 0 || linearIndex >= model.volume()) {
                throw new IllegalArgumentException(
                    "Block index is outside the schematic volume"
                );
            }
            int localX = model.xFromIndex(linearIndex);
            int localY = model.yFromIndex(linearIndex);
            int localZ = model.zFromIndex(linearIndex);
            if (model.isAirAt(localX, localY, localZ)) {
                throw new IllegalArgumentException(
                    "Block index identifies an air schematic cell"
                );
            }

            byte status = blockStates[listIndex];
            switch (status) {
                case BLOCK_UNKNOWN:
                    break;
                case BLOCK_MISSING:
                    ++missing;
                    break;
                case BLOCK_CORRECT:
                    ++correct;
                    break;
                case BLOCK_WRONG:
                    ++wrong;
                    break;
                default:
                    throw new IllegalArgumentException(
                        "Unknown schematic block status: " + status
                    );
            }

            if (markers == null ||
                (status != BLOCK_UNKNOWN &&
                    status != BLOCK_MISSING &&
                    status != BLOCK_WRONG) ||
                (selectedLayer >= 0 && localY != selectedLayer)) {
                continue;
            }

            SchematicPlacementTransform.BlockPosition position =
                transform.worldBlock(localX, localY, localZ);
            double dx = position.x() + 0.5d - cameraX;
            double dy = position.y() + 0.5d - cameraY;
            double dz = position.z() + 0.5d - cameraZ;
            double distanceSquared = dx * dx + dy * dy + dz * dz;
            if (distanceSquared > maximumDistanceSquared) continue;
            markers.offer(
                position.x(),
                position.y(),
                position.z(),
                status,
                linearIndex,
                distanceSquared
            );
        }

        int known = Math.addExact(correct, Math.addExact(missing, wrong));
        int total = model.nonAirBlocks();
        if (known > total) {
            throw new IllegalArgumentException(
                "Known block states exceed the schematic non-air block count"
            );
        }
        SortedMarkers sorted = markers == null
            ? new SortedMarkers(new int[0], new String[0])
            : markers.sorted(model, transform, includeTextureStates);
        return new Result(
            sorted.records,
            sorted.expectedBlockStates,
            total,
            correct,
            missing,
            wrong,
            total - known,
            opacityPercent
        );
    }

    /** Immutable marker plan and progress counters. */
    public static final class Result {
        private final int[] records;
        private final String[] expectedBlockStates;
        private final int total;
        private final int correct;
        private final int missing;
        private final int wrong;
        private final int unknown;
        private final int opacityPercent;

        private Result(
            int[] records,
            String[] expectedBlockStates,
            int total,
            int correct,
            int missing,
            int wrong,
            int unknown,
            int opacityPercent
        ) {
            this.records = records;
            this.expectedBlockStates = expectedBlockStates;
            this.total = total;
            this.correct = correct;
            this.missing = missing;
            this.wrong = wrong;
            this.unknown = unknown;
            this.opacityPercent = opacityPercent;
        }

        /** Flat records in nearest-first order: {@code [x,y,z,status]}. */
        public int[] records() {
            return records.clone();
        }

        /**
         * Exact Bedrock palette state parallel to each marker record. Entries
         * without a textured preview are null. The enclosing marker plan is
         * already capped, so providing all unknown and missing states remains
         * bounded.
         */
        public String[] expectedBlockStates() {
            return expectedBlockStates.clone();
        }

        public int total() {
            return total;
        }

        public int correct() {
            return correct;
        }

        public int missing() {
            return missing;
        }

        public int wrong() {
            return wrong;
        }

        public int unknown() {
            return unknown;
        }

        public int displayed() {
            return records.length / RECORD_WIDTH;
        }

        public int opacityPercent() {
            return opacityPercent;
        }

        public float alpha() {
            return opacityPercent / 100.0f;
        }
    }

    /** Max-heap retaining only the nearest marker candidates. */
    private static final class MarkerHeap {
        private final int capacity;
        private final int[] x;
        private final int[] y;
        private final int[] z;
        private final byte[] status;
        private final int[] linearIndex;
        private final double[] distanceSquared;
        private int size;

        MarkerHeap(int capacity) {
            this.capacity = capacity;
            x = new int[capacity];
            y = new int[capacity];
            z = new int[capacity];
            status = new byte[capacity];
            linearIndex = new int[capacity];
            distanceSquared = new double[capacity];
        }

        void offer(
            int candidateX,
            int candidateY,
            int candidateZ,
            byte candidateStatus,
            int candidateLinearIndex,
            double candidateDistanceSquared
        ) {
            if (size < capacity) {
                int slot = size++;
                set(
                    slot,
                    candidateX,
                    candidateY,
                    candidateZ,
                    candidateStatus,
                    candidateLinearIndex,
                    candidateDistanceSquared
                );
                siftUp(slot);
                return;
            }
            if (compareCandidate(
                    candidateDistanceSquared,
                    candidateX,
                    candidateY,
                    candidateZ,
                    candidateStatus,
                    candidateLinearIndex,
                    0
                ) >= 0) {
                return;
            }
            set(
                0,
                candidateX,
                candidateY,
                candidateZ,
                candidateStatus,
                candidateLinearIndex,
                candidateDistanceSquared
            );
            siftDown(0);
        }

        SortedMarkers sorted(
            SchematicModel model,
            SchematicPlacementTransform transform,
            boolean includeTextureStates
        ) {
            Integer[] order = new Integer[size];
            for (int index = 0; index < size; ++index) order[index] = index;
            Arrays.sort(order, this::compareSlots);
            int[] result = new int[Math.multiplyExact(size, RECORD_WIDTH)];
            String[] expectedBlockStates = new String[size];
            String[] transformedPalette = includeTextureStates
                ? new String[model.paletteSize()]
                : null;
            int texturedMarkers = 0;
            for (int output = 0; output < size; ++output) {
                int slot = order[output];
                int offset = output * RECORD_WIDTH;
                result[offset] = x[slot];
                result[offset + 1] = y[slot];
                result[offset + 2] = z[slot];
                result[offset + 3] = status[slot];
                if (includeTextureStates &&
                    (status[slot] == BLOCK_UNKNOWN ||
                        status[slot] == BLOCK_MISSING) &&
                    texturedMarkers < MAX_TEXTURED_MARKERS) {
                    int paletteIndex = model.paletteIndexAtLinear(
                        linearIndex[slot]
                    );
                    String transformedState = transformedPalette[paletteIndex];
                    if (transformedState == null) {
                        transformedState = SchematicBlockStateTransform.transform(
                            model.paletteState(paletteIndex),
                            transform.rotationQuarterTurns(),
                            transform.mirrored()
                        );
                        transformedPalette[paletteIndex] = transformedState;
                    }
                    expectedBlockStates[output] = transformedState;
                    ++texturedMarkers;
                }
            }
            return new SortedMarkers(result, expectedBlockStates);
        }

        private void siftUp(int slot) {
            while (slot > 0) {
                int parent = (slot - 1) >>> 1;
                if (compareSlots(slot, parent) <= 0) return;
                swap(slot, parent);
                slot = parent;
            }
        }

        private void siftDown(int slot) {
            while (true) {
                int left = slot * 2 + 1;
                if (left >= size) return;
                int right = left + 1;
                int farther = right < size && compareSlots(right, left) > 0
                    ? right
                    : left;
                if (compareSlots(farther, slot) <= 0) return;
                swap(slot, farther);
                slot = farther;
            }
        }

        /** Ascending nearest-first comparison. */
        private int compareSlots(int left, int right) {
            int comparison = Double.compare(
                distanceSquared[left],
                distanceSquared[right]
            );
            if (comparison != 0) return comparison;
            comparison = Integer.compare(x[left], x[right]);
            if (comparison != 0) return comparison;
            comparison = Integer.compare(y[left], y[right]);
            if (comparison != 0) return comparison;
            comparison = Integer.compare(z[left], z[right]);
            if (comparison != 0) return comparison;
            comparison = Byte.compare(status[left], status[right]);
            if (comparison != 0) return comparison;
            return Integer.compare(linearIndex[left], linearIndex[right]);
        }

        private int compareCandidate(
            double candidateDistanceSquared,
            int candidateX,
            int candidateY,
            int candidateZ,
            byte candidateStatus,
            int candidateLinearIndex,
            int slot
        ) {
            int comparison = Double.compare(
                candidateDistanceSquared,
                distanceSquared[slot]
            );
            if (comparison != 0) return comparison;
            comparison = Integer.compare(candidateX, x[slot]);
            if (comparison != 0) return comparison;
            comparison = Integer.compare(candidateY, y[slot]);
            if (comparison != 0) return comparison;
            comparison = Integer.compare(candidateZ, z[slot]);
            if (comparison != 0) return comparison;
            comparison = Byte.compare(candidateStatus, status[slot]);
            if (comparison != 0) return comparison;
            return Integer.compare(candidateLinearIndex, linearIndex[slot]);
        }

        private void set(
            int slot,
            int valueX,
            int valueY,
            int valueZ,
            byte valueStatus,
            int valueLinearIndex,
            double valueDistanceSquared
        ) {
            x[slot] = valueX;
            y[slot] = valueY;
            z[slot] = valueZ;
            status[slot] = valueStatus;
            linearIndex[slot] = valueLinearIndex;
            distanceSquared[slot] = valueDistanceSquared;
        }

        private void swap(int left, int right) {
            int intValue = x[left];
            x[left] = x[right];
            x[right] = intValue;
            intValue = y[left];
            y[left] = y[right];
            y[right] = intValue;
            intValue = z[left];
            z[left] = z[right];
            z[right] = intValue;
            intValue = linearIndex[left];
            linearIndex[left] = linearIndex[right];
            linearIndex[right] = intValue;
            byte byteValue = status[left];
            status[left] = status[right];
            status[right] = byteValue;
            double doubleValue = distanceSquared[left];
            distanceSquared[left] = distanceSquared[right];
            distanceSquared[right] = doubleValue;
        }
    }

    private static final class SortedMarkers {
        final int[] records;
        final String[] expectedBlockStates;

        SortedMarkers(int[] records, String[] expectedBlockStates) {
            this.records = records;
            this.expectedBlockStates = expectedBlockStates;
        }
    }
}
