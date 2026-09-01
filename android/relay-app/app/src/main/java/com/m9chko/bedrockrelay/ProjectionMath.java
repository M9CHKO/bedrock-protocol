package com.m9chko.bedrockrelay;

/** Allocation-free camera/projection helpers shared with unit tests. */
final class ProjectionMath {
    private static final double MaximumDynamicFovBoostDegrees = 8.0;

    private ProjectionMath() {}

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
