package com.m9chko.bedrockrelay;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public final class MotionSmootherTest {
    @Test
    public void springDoesNotSnapAndConverges() {
        MotionSmoother.Axis axis = new MotionSmoother.Axis();
        axis.reset(0.0, 0.0);
        axis.step(10.0, 0.0, 0.08, 1.0 / 60.0);

        assertTrue(axis.value() > 0.0);
        assertTrue(axis.value() < 10.0);
        double previous = axis.value();
        for (int frame = 0; frame < 240; ++frame) {
            axis.step(10.0, 0.0, 0.08, 1.0 / 60.0);
            assertTrue(axis.value() >= previous - 1.0e-9);
            previous = axis.value();
        }
        assertEquals(10.0, axis.value(), 1.0e-5);
        assertEquals(0.0, axis.velocity(), 1.0e-4);
    }

    @Test
    public void movingTargetKeepsFiniteContinuousFrames() {
        MotionSmoother.Axis axis = new MotionSmoother.Axis();
        axis.reset(0.0, 0.0);
        double previous = axis.value();
        for (int frame = 1; frame <= 180; ++frame) {
            double time = frame / 60.0;
            double target = time * 4.0;
            axis.step(target, 4.0, 0.07, 1.0 / 60.0);
            double frameDistance = axis.value() - previous;
            assertTrue(Double.isFinite(axis.value()));
            assertTrue(Double.isFinite(axis.velocity()));
            assertTrue(frameDistance >= -1.0e-9);
            assertTrue(frameDistance < 0.2);
            previous = axis.value();
        }
        assertEquals(4.0, axis.velocity(), 1.0e-3);
    }

    @Test
    public void yawUnwrapsAcrossMinusOneEighty() {
        assertEquals(181.0, MotionSmoother.unwrapAngle(179.0, -179.0), 0.0);
        assertEquals(-181.0, MotionSmoother.unwrapAngle(-179.0, 179.0), 0.0);
    }

    @Test
    public void velocityFilterIsFrameRateIndependent() {
        double oneStep = MotionSmoother.filterVelocity(0.0, 10.0, 0.05, 0.05);
        double twoSteps = MotionSmoother.filterVelocity(0.0, 10.0, 0.025, 0.05);
        twoSteps = MotionSmoother.filterVelocity(
            twoSteps,
            10.0,
            0.025,
            0.05
        );
        assertEquals(oneStep, twoSteps, 1.0e-12);
    }

    @Test
    public void stalePredictionReturnsContinuouslyToAuthoritativeSample() {
        long maximum = 120_000_000L;
        long decay = 180_000_000L;
        assertEquals(
            0.12,
            MotionSmoother.predictionSeconds(maximum, 0, maximum, decay),
            1.0e-12
        );
        assertEquals(
            1.0,
            MotionSmoother.predictionVelocityScale(
                maximum,
                0,
                maximum,
                decay
            ),
            1.0e-12
        );
        assertEquals(
            0.0,
            MotionSmoother.predictionSeconds(
                maximum + decay,
                0,
                maximum,
                decay
            ),
            0.0
        );
        assertEquals(
            0.0,
            MotionSmoother.predictionVelocityScale(
                maximum + decay,
                0,
                maximum,
                decay
            ),
            0.0
        );
    }
}
