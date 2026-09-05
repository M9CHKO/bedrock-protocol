package com.m9chko.bedrockrelay;

/** Allocation-free camera/projection helpers shared with unit tests. */
final class ProjectionMath {
    private static final double MaximumDynamicFovBoostDegrees = 8.0;

    private ProjectionMath() {}

    // The twelve AABB edges. Clip edges before perspective division: dropping
    // corners behind the eye makes nearby boxes shrink or disappear abruptly.
    private static final int[] BOX_EDGES = {
        0,1, 2,3, 4,5, 6,7, 0,2, 1,3, 4,6, 5,7, 0,4, 1,5, 2,6, 3,7
    };

    static boolean projectBox(double dx, double dy, double dz,
        double halfWidth, double height, double sinYaw, double cosYaw,
        double sinPitch, double cosPitch, double focal, double near,
        double viewportWidth, double viewportHeight, double[] corners, double[] bounds) {
        if (!Double.isFinite(dx + dy + dz + halfWidth + height + focal) ||
            halfWidth <= 0 || height <= 0 || near <= 0 || focal <= 0) return false;
        bounds[0] = bounds[1] = Double.POSITIVE_INFINITY;
        bounds[2] = bounds[3] = Double.NEGATIVE_INFINITY;
        for (int i = 0; i < 8; ++i) {
            double x = dx + ((i & 4) == 0 ? -halfWidth : halfWidth);
            double y = dy + ((i & 2) == 0 ? 0 : height);
            double z = dz + ((i & 1) == 0 ? -halfWidth : halfWidth);
            corners[i * 3] = viewX(x, z, sinYaw, cosYaw);
            corners[i * 3 + 1] = viewY(x, y, z, sinYaw, cosYaw, sinPitch, cosPitch);
            corners[i * 3 + 2] = depth(x, y, z, sinYaw, cosYaw, sinPitch, cosPitch);
            if (corners[i * 3 + 2] >= near)
                includeProjection(corners[i * 3], corners[i * 3 + 1], corners[i * 3 + 2],
                    focal, viewportWidth, viewportHeight, bounds);
        }
        for (int edge = 0; edge < BOX_EDGES.length; edge += 2) {
            int a = BOX_EDGES[edge] * 3, b = BOX_EDGES[edge + 1] * 3;
            double za = corners[a + 2], zb = corners[b + 2];
            if ((za < near) == (zb < near)) continue;
            double t = (near - za) / (zb - za);
            includeProjection(corners[a] + (corners[b] - corners[a]) * t,
                corners[a + 1] + (corners[b + 1] - corners[a + 1]) * t,
                near, focal, viewportWidth, viewportHeight, bounds);
        }
        return Double.isFinite(bounds[0]) && Double.isFinite(bounds[1]) &&
            bounds[2] >= 0 && bounds[3] >= 0 &&
            bounds[0] <= viewportWidth && bounds[1] <= viewportHeight;
    }

    private static void includeProjection(double x, double y, double z,
        double focal, double width, double height, double[] bounds) {
        double sx = width * 0.5 + x * focal / z;
        double sy = height * 0.5 - y * focal / z;
        if (!Double.isFinite(sx) || !Double.isFinite(sy)) return;
        bounds[0] = Math.min(bounds[0], sx);
        bounds[1] = Math.min(bounds[1], sy);
        bounds[2] = Math.max(bounds[2], sx);
        bounds[3] = Math.max(bounds[3], sy);
    }

    /**
     * Bedrock changes the rendered FOV while the player moves quickly. The
     * exact render matrix is not sent over the protocol, so camera speed is
     * the best packet-only signal available for reproducing that change.
     */
    static double dynamicVerticalFov(
        double configuredFovDegrees,
        double cameraPositionSpeed
    ) {
        double speedBlend = smoothStep(3.5, 8.0, cameraPositionSpeed);
        return MotionSmoother.clamp(
            configuredFovDegrees +
                MaximumDynamicFovBoostDegrees * speedBlend,
            1.0,
            179.0
        );
    }

    /** Continuous 0..1 camera-motion profile; no tiny-motion mode switch. */
    static double cameraMotionStrength(
        double cameraPositionSpeed,
        double cameraAngleSpeedDegrees
    ) {
        return Math.max(
            smoothStep(0.15, 14.0, cameraPositionSpeed),
            smoothStep(2.0, 260.0, cameraAngleSpeedDegrees)
        );
    }

    /** Peaks at medium motion and returns to zero at rest/full-speed turns. */
    static double mediumMotionSoftness(double motionStrength) {
        double motion = MotionSmoother.clamp(motionStrength, 0.0, 1.0);
        return 4.0 * motion * (1.0 - motion);
    }

    static double focalPixels(
        double viewportHeight,
        double verticalFovDegrees
    ) {
        double height = Math.max(1.0, viewportHeight);
        double fov = MotionSmoother.clamp(verticalFovDegrees, 1.0, 179.0);
        return height * 0.5 / Math.tan(Math.toRadians(fov * 0.5));
    }

    static double viewX(
        double dx,
        double dz,
        double sinYaw,
        double cosYaw
    ) {
        // Bedrock yaw 0 faces +Z; screen-right is the negative horizontal
        // basis because positive yaw turns towards -X.
        return -(dx * cosYaw + dz * sinYaw);
    }

    static double viewY(
        double dx,
        double dy,
        double dz,
        double sinYaw,
        double cosYaw,
        double sinPitch,
        double cosPitch
    ) {
        return dx * (-sinYaw * sinPitch) +
            dy * cosPitch +
            dz * (cosYaw * sinPitch);
    }

    static double depth(
        double dx,
        double dy,
        double dz,
        double sinYaw,
        double cosYaw,
        double sinPitch,
        double cosPitch
    ) {
        return dx * (-sinYaw * cosPitch) +
            dy * (-sinPitch) +
            dz * (cosYaw * cosPitch);
    }

    static double smoothStep(double edge0, double edge1, double value) {
        if (!(edge1 > edge0)) return value >= edge1 ? 1.0 : 0.0;
        double phase = MotionSmoother.clamp(
            (value - edge0) / (edge1 - edge0),
            0.0,
            1.0
        );
        return phase * phase * (3.0 - 2.0 * phase);
    }
}
