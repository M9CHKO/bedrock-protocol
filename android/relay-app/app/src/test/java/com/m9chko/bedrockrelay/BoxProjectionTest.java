package com.m9chko.bedrockrelay;

import org.junit.Test;
import static org.junit.Assert.*;

public class BoxProjectionTest {
    private final double[] corners = new double[24], bounds = new double[4];
    private boolean project(double z, double width) {
        return ProjectionMath.projectBox(0, -0.9, z, width, 1.8,
            0, 1, 0, 1, 300, 0.05, 800, 600, corners, bounds);
    }
    @Test public void visibleBoxHasSymmetricBounds() {
        assertTrue(project(4, 0.3));
        assertEquals(800, bounds[0] + bounds[2], 0.00001);
        assertEquals(600, bounds[1] + bounds[3], 0.00001);
    }
    @Test public void centerBehindCameraDoesNotHideVisibleEdges() {
        assertTrue(project(-0.1, 0.3));
        assertTrue(bounds[0] < 0 && bounds[2] > 800);
        for (double value : bounds) assertTrue(Double.isFinite(value));
    }
    @Test public void fullyBehindAndInvalidBoxesAreRejected() {
        assertFalse(project(-4, 0.3));
        assertFalse(project(Double.NaN, 0.3));
        assertFalse(project(4, -1));
    }
    @Test public void nearPlaneCrossingIsContinuous() {
        assertTrue(project(0.351, 0.3));
        double before = bounds[0];
        assertTrue(project(0.349, 0.3));
        assertTrue(Math.abs(bounds[0] - before) < 40);
    }
}
