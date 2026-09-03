package com.m9chko.bedrockrelay;

import java.util.Objects;

/**
 * Immutable placement transform shared by schematic rendering and world matching.
 *
 * <p>The anchor is the integer world-grid corner corresponding to the unrotated
 * schematic origin. Mirroring is applied along the local X axis before rotation.
 */
public final class SchematicPlacementTransform {
    private final int anchorX;
    private final int anchorY;
    private final int anchorZ;
    private final int sizeX;
    private final int rotationQuarterTurns;
    private final boolean mirrored;

    public SchematicPlacementTransform(
        int anchorX,
        int anchorY,
        int anchorZ,
        int sizeX,
        int rotationQuarterTurns,
        boolean mirrored
    ) {
        if (sizeX <= 0) {
            throw new IllegalArgumentException("sizeX must be positive");
        }
        this.anchorX = anchorX;
        this.anchorY = anchorY;
        this.anchorZ = anchorZ;
        this.sizeX = sizeX;
        this.rotationQuarterTurns = Math.floorMod(rotationQuarterTurns, 4);
        this.mirrored = mirrored;
    }

    public int anchorX() {
        return anchorX;
    }

    public int anchorY() {
        return anchorY;
    }

    public int anchorZ() {
        return anchorZ;
    }

    public int sizeX() {
        return sizeX;
    }

    public int rotationQuarterTurns() {
        return rotationQuarterTurns;
    }

    public boolean mirrored() {
        return mirrored;
    }

    /** Maps a continuous schematic coordinate to its world X coordinate. */
    public double worldX(double localX, double localZ) {
        double x = mirrored ? sizeX - localX : localX;
        switch (rotationQuarterTurns) {
            case 0: return anchorX + x;
            case 1: return anchorX - localZ;
            case 2: return anchorX - x;
            case 3: return anchorX + localZ;
            default: throw new AssertionError("normalized rotation");
        }
    }

    /** Maps a continuous schematic coordinate to its world Y coordinate. */
    public double worldY(double localY) {
        return anchorY + localY;
    }

    /** Maps a continuous schematic coordinate to its world Z coordinate. */
    public double worldZ(double localX, double localZ) {
        double x = mirrored ? sizeX - localX : localX;
        switch (rotationQuarterTurns) {
            case 0: return anchorZ + localZ;
            case 1: return anchorZ + x;
            case 2: return anchorZ - localZ;
            case 3: return anchorZ - x;
            default: throw new AssertionError("normalized rotation");
        }
    }

    /**
     * Maps a local block cell to the integer world cell containing its center.
     * This deliberately uses the same continuous transform as rendering.
     */
    public BlockPosition worldBlock(int localX, int localY, int localZ) {
        double centerX = localX + 0.5d;
        double centerZ = localZ + 0.5d;
        return new BlockPosition(
            floorToInt(worldX(centerX, centerZ)),
            Math.addExact(anchorY, localY),
            floorToInt(worldZ(centerX, centerZ))
        );
    }

    /**
     * Inverts {@link #worldBlock(int, int, int)} for an integer world cell.
     * The transform is an axis-aligned quarter turn, so every world cell maps
     * to exactly one local cell even when the result lies outside the model.
     */
    public BlockPosition localBlock(int worldX, int worldY, int worldZ) {
        long dx = (long) worldX - anchorX;
        long dz = (long) worldZ - anchorZ;
        long effectiveX;
        long localZ;
        switch (rotationQuarterTurns) {
            case 0:
                effectiveX = dx;
                localZ = dz;
                break;
            case 1:
                effectiveX = dz;
                localZ = -dx - 1L;
                break;
            case 2:
                effectiveX = -dx - 1L;
                localZ = -dz - 1L;
                break;
            case 3:
                effectiveX = -dz - 1L;
                localZ = dx;
                break;
            default:
                throw new AssertionError("normalized rotation");
        }
        long localX = mirrored ? (long) sizeX - 1L - effectiveX : effectiveX;
        return new BlockPosition(
            checkedInt(localX, "local X"),
            checkedInt((long) worldY - anchorY, "local Y"),
            checkedInt(localZ, "local Z")
        );
    }

    /**
     * Converts a collision-surface height to the first integer block cell above
     * (or exactly on) that surface. For example, a slab top at 64.5 anchors at 65.
     */
    public static int placementAnchorY(double collisionSurfaceY) {
        if (!Double.isFinite(collisionSurfaceY)) {
            throw new IllegalArgumentException("collisionSurfaceY must be finite");
        }
        double rounded = Math.ceil(collisionSurfaceY);
        if (rounded < Integer.MIN_VALUE || rounded > Integer.MAX_VALUE) {
            throw new ArithmeticException("placement anchor is outside int range");
        }
        return (int) rounded;
    }

    private static int floorToInt(double value) {
        if (!Double.isFinite(value)) {
            throw new ArithmeticException("world coordinate is not finite");
        }
        double rounded = Math.floor(value);
        if (rounded < Integer.MIN_VALUE || rounded > Integer.MAX_VALUE) {
            throw new ArithmeticException("world coordinate is outside int range");
        }
        return (int) rounded;
    }

    private static int checkedInt(long value, String coordinate) {
        if (value < Integer.MIN_VALUE || value > Integer.MAX_VALUE) {
            throw new ArithmeticException(coordinate + " is outside int range");
        }
        return (int) value;
    }

    /** Immutable integer world block coordinate. */
    public static final class BlockPosition {
        private final int x;
        private final int y;
        private final int z;

        public BlockPosition(int x, int y, int z) {
            this.x = x;
            this.y = y;
            this.z = z;
        }

        public int x() {
            return x;
        }

        public int y() {
            return y;
        }

        public int z() {
            return z;
        }

        @Override
        public boolean equals(Object other) {
            if (this == other) return true;
            if (!(other instanceof BlockPosition)) return false;
            BlockPosition that = (BlockPosition) other;
            return x == that.x && y == that.y && z == that.z;
        }

        @Override
        public int hashCode() {
            return Objects.hash(x, y, z);
        }

        @Override
        public String toString() {
            return "BlockPosition{" + x + ", " + y + ", " + z + '}';
        }
    }
}
