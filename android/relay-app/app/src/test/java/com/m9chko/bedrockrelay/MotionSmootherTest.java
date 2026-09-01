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
    public void stalePredictionSlowsWithoutEverReversing() {
        long linear = 65_000_000L;
        long maximum = 130_000_000L;
        double previous = -1.0;
        for (long age = 0; age <= 2_000_000_000L; age += 1_000_000L) {
            double horizon = MotionSmoother.predictionSeconds(
                age,
                0,
                linear,
                maximum
            );
            double velocityScale = MotionSmoother.predictionVelocityScale(
                age,
                0,
                linear,
                maximum
            );
            assertTrue(horizon >= previous - 1.0e-12);
            assertTrue(horizon <= 0.13 + 1.0e-12);
            assertTrue(velocityScale >= 0.0);
            assertTrue(velocityScale <= 1.0);
            previous = horizon;
        }
        assertEquals(0.13, previous, 1.0e-10);
        assertTrue(MotionSmoother.predictionVelocityScale(
            2_000_000_000L,
            0,
            linear,
            maximum
        ) < 1.0e-10);
    }

    @Test
    public void screenBoxSuppressesAlternatingProjectionJitter() {
        MotionSmoother.ScreenBox box = new MotionSmoother.ScreenBox();
        long now = 1_000_000_000L;
        box.step(80.0, 80.0, 120.0, 180.0, now, 1080.0, 1920.0);
        double previous = box.centerX();

        for (int frame = 1; frame <= 240; ++frame) {
            now += 16_666_667L;
            double trend = 100.0 + frame * 2.0;
            double jitter;
            switch (frame & 3) {
                case 0:
                    jitter = 3.0;
                    break;
                case 2:
                    jitter = -3.0;
                    break;
                default:
                    jitter = 0.0;
                    break;
            }
            box.step(
                trend + jitter - 20.0,
                80.0,
                trend + jitter + 20.0,
                180.0,
                now,
                1080.0,
                1920.0
            );
            double current = box.centerX();
            assertTrue(Double.isFinite(current));
            assertTrue(current >= previous - 0.05);
            assertTrue(current - previous < 8.0);
            previous = current;
        }
        assertEquals(580.0, box.centerX(), 8.0);
        assertEquals(40.0, box.right() - box.left(), 0.05);
    }

    @Test
    public void screenBoxDoesNotSnapOnFastCameraTurn() {
        MotionSmoother.ScreenBox box = new MotionSmoother.ScreenBox();
        long now = 1_000_000_000L;
        box.step(80.0, 80.0, 120.0, 180.0, now, 1080.0, 1920.0);

        now += 16_666_667L;
        box.step(880.0, 80.0, 920.0, 180.0, now, 1080.0, 1920.0);
        assertTrue(box.centerX() > 100.0);
        assertTrue(box.centerX() < 700.0);
        double previous = box.centerX();

        for (int frame = 0; frame < 180; ++frame) {
            now += 16_666_667L;
            box.step(
                880.0,
                80.0,
                920.0,
                180.0,
                now,
                1080.0,
                1920.0
            );
            assertTrue(Double.isFinite(box.centerX()));
            assertTrue(box.centerX() >= previous - 1.0e-9);
            assertTrue(box.centerX() <= 900.0 + 1.0e-9);
            previous = box.centerX();
        }
        assertEquals(900.0, box.centerX(), 0.1);
    }

    @Test
    public void screenBoxCentreFollowsOneCoherentTwoDimensionalPath() {
        MotionSmoother.ScreenBox box = new MotionSmoother.ScreenBox();
        long now = 1_000_000_000L;
        box.step(80.0, 50.0, 120.0, 150.0, now, 1080.0, 1920.0);

        for (int frame = 0; frame < 20; ++frame) {
            now += 8_333_333L;
            box.step(
                880.0,
                450.0,
                920.0,
                550.0,
                now,
                1080.0,
                1920.0
            );
            double horizontal = box.centerX() - 100.0;
            double vertical = box.centerY() - 100.0;
            assertEquals(horizontal, vertical * 2.0, 1.0e-8);
            assertEquals(40.0, box.right() - box.left(), 1.0e-8);
            assertEquals(100.0, box.bottom() - box.top(), 1.0e-8);
        }
    }

    @Test
    public void stationaryScreenBoxRejectsSubpixelPacketFlicker() {
        MotionSmoother.ScreenBox box = new MotionSmoother.ScreenBox();
        long now = 1_000_000_000L;
        box.step(80.0, 80.0, 120.0, 180.0, now, 1080.0, 1920.0);
        double minimum = Double.POSITIVE_INFINITY;
        double maximum = Double.NEGATIVE_INFINITY;

        for (int frame = 1; frame <= 600; ++frame) {
            now += 16_666_667L;
            double jitter = (frame & 1) == 0 ? 3.0 : -3.0;
            box.step(
                80.0 + jitter,
                80.0,
                120.0 + jitter,
                180.0,
                now,
                1080.0,
                1920.0
            );
            if (frame > 120) {
                minimum = Math.min(minimum, box.centerX());
                maximum = Math.max(maximum, box.centerX());
            }
        }
        assertTrue(maximum - minimum < 0.5);
        assertEquals(100.0, box.centerX(), 0.5);
    }

    @Test
    public void screenBoxAbsorbsTwentyHertzCameraCorrectionsAt120Fps() {
        MotionSmoother.ScreenBox box = new MotionSmoother.ScreenBox();
        long now = 1_000_000_000L;
        box.step(80.0, 80.0, 120.0, 180.0, now, 1080.0, 1920.0);
        double previous = box.centerX();
        double maximumFrameDistance = 0.0;

        for (int frame = 1; frame <= 720; ++frame) {
            now += 8_333_333L;
            double continuousMotion = 100.0 + frame * 2.0;
            int packetPhase = frame % 6;
            double packetCorrection = ((frame / 6) & 1) == 0
                ? -8.0
                : 8.0;
            packetCorrection *= 1.0 - packetPhase / 6.0;
            double target = continuousMotion + packetCorrection;
            box.step(
                target - 20.0,
                80.0,
                target + 20.0,
                180.0,
                now,
                1080.0,
                1920.0
            );

            double frameDistance = box.centerX() - previous;
            if (frame > 120) {
                assertTrue(frameDistance >= -0.01);
                maximumFrameDistance = Math.max(
                    maximumFrameDistance,
                    frameDistance
                );
            }
            previous = box.centerX();
        }

        assertTrue(maximumFrameDistance < 4.5);
        assertEquals(1_540.0, box.centerX(), 18.0);
    }

    @Test
    public void screenBoxChangesDirectionOnceWithoutRinging() {
        MotionSmoother.ScreenBox box = new MotionSmoother.ScreenBox();
        long now = 1_000_000_000L;
        box.step(80.0, 80.0, 120.0, 180.0, now, 1080.0, 1920.0);

        double previous = box.centerX();
        int direction = 0;
        int directionChanges = 0;
        for (int frame = 1; frame <= 360; ++frame) {
            now += 8_333_333L;
            double target;
            if (frame <= 80) {
                target = 100.0 + frame * 5.0;
            } else if (frame <= 160) {
                target = 500.0 - (frame - 80) * 5.0;
            } else {
                target = 100.0;
            }
            box.step(
                target - 20.0,
                80.0,
                target + 20.0,
                180.0,
                now,
                1080.0,
                1920.0
            );

            double current = box.centerX();
            double delta = current - previous;
            int nextDirection = delta > 0.01 ? 1 : delta < -0.01 ? -1 : 0;
            if (nextDirection != 0) {
                if (direction != 0 && direction != nextDirection) {
                    ++directionChanges;
                }
                direction = nextDirection;
            }
            assertTrue(current >= 100.0 - 1.0e-9);
            assertTrue(current <= 500.0 + 1.0e-9);
            previous = current;
        }

        assertEquals(1, directionChanges);
        assertEquals(100.0, box.centerX(), 0.1);
    }
}
