package com.m9chko.bedrockrelay;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public final class ProjectionMathTest {
    @Test
    public void cardinalCameraDirectionsProjectToScreenCentre() {
        assertForwardProjectsToCentre(0.0, 0.0);
        assertForwardProjectsToCentre(90.0, 0.0);
        assertForwardProjectsToCentre(-90.0, 0.0);
        assertForwardProjectsToCentre(180.0, 0.0);
        assertForwardProjectsToCentre(35.0, 55.0);
        assertForwardProjectsToCentre(-120.0, -70.0);
    }

    @Test
    public void verticalFovUsesViewportHeight() {
        assertEquals(
            540.0,
            ProjectionMath.focalPixels(1_080.0, 90.0),
            1.0e-9
        );
        assertTrue(
            ProjectionMath.focalPixels(1_080.0, 60.0) >
                ProjectionMath.focalPixels(1_080.0, 90.0)
        );
    }

    @Test
    public void dynamicFovIsBoundedAndOnlyChangesWithMovement() {
        assertEquals(
            70.0,
            ProjectionMath.dynamicVerticalFov(70.0, 0.0),
            1.0e-9
        );
        double sprintFov = ProjectionMath.dynamicVerticalFov(70.0, 5.8);
        assertTrue(sprintFov > 70.0);
        assertTrue(sprintFov < 78.0);
        assertEquals(
            78.0,
            ProjectionMath.dynamicVerticalFov(70.0, 30.0),
            1.0e-9
        );
    }

    @Test
    public void motionProfileHasNoBooleanDiscontinuity() {
        double previous = ProjectionMath.cameraMotionStrength(0.0, 0.0);
        for (int speed = 1; speed <= 260; ++speed) {
            double current = ProjectionMath.cameraMotionStrength(0.0, speed);
            assertTrue(current >= previous);
            assertTrue(current - previous < 0.02);
            previous = current;
        }
        assertEquals(0.0, ProjectionMath.mediumMotionSoftness(0.0), 0.0);
        assertEquals(1.0, ProjectionMath.mediumMotionSoftness(0.5), 0.0);
        assertEquals(0.0, ProjectionMath.mediumMotionSoftness(1.0), 0.0);
    }

    private static void assertForwardProjectsToCentre(
        double yawDegrees,
        double pitchDegrees
    ) {
        double yaw = Math.toRadians(yawDegrees);
        double pitch = Math.toRadians(pitchDegrees);
        double sinYaw = Math.sin(yaw);
        double cosYaw = Math.cos(yaw);
        double sinPitch = Math.sin(pitch);
        double cosPitch = Math.cos(pitch);
        double distance = 25.0;
        double dx = -sinYaw * cosPitch * distance;
        double dy = -sinPitch * distance;
        double dz = cosYaw * cosPitch * distance;

        assertEquals(
            0.0,
            ProjectionMath.viewX(dx, dz, sinYaw, cosYaw),
            1.0e-9
        );
        assertEquals(
            0.0,
            ProjectionMath.viewY(
                dx,
                dy,
                dz,
                sinYaw,
                cosYaw,
                sinPitch,
                cosPitch
            ),
            1.0e-9
        );
        assertEquals(
            distance,
            ProjectionMath.depth(
                dx,
                dy,
                dz,
                sinYaw,
                cosYaw,
                sinPitch,
                cosPitch
            ),
            1.0e-9
        );
    }
}
