package com.m9chko.bedrockrelay;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

public final class PacketCameraTrackerTest {
    @Test
    public void fixedFovDoesNotChangeWhileCameraMoves() {
        PacketCameraTracker tracker = new PacketCameraTracker();
        long now = 1_000_000_000L;
        long updatedAtMs = 1_000L;
        double x = 0.0;

        tracker.update(camera(100, x, updatedAtMs), now);
        assertFixedFov(tracker.frame(now, 60, false));

        for (long tick = 101; tick <= 108; ++tick) {
            now += 50_000_000L;
            updatedAtMs += 50L;
            x += 0.4;
            tracker.update(camera(tick, x, updatedAtMs), now);

            for (int frame = 0; frame < 3; ++frame) {
                assertFixedFov(tracker.frame(
                    now + frame * 16_000_000L,
                    60,
                    false
                ));
            }
        }
    }

    private static EntityOutlineOverlayController.CameraSample camera(
        long tick,
        double x,
        long updatedAtMs
    ) {
        return new EntityOutlineOverlayController.CameraSample(
            true,
            true,
            tick,
            x,
            65.62,
            0.0,
            0.0,
            0.0,
            updatedAtMs,
            0L
        );
    }

    private static void assertFixedFov(PacketCameraTracker.State state) {
        assertEquals(60.0, state.verticalFov, 0.0);
    }
}
