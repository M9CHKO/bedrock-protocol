package com.m9chko.bedrockrelay;

import org.junit.Test;
import static org.junit.Assert.*;

public class ShulkerDepositSettingsTest {
    @Test public void defaultAndRangeAreSafe() {
        assertEquals(1000, ShulkerDepositSettings.DEFAULT_INTERVAL_MS);
        assertEquals(30, ShulkerDepositSettings.clampInterval(Integer.MIN_VALUE));
        assertEquals(3000, ShulkerDepositSettings.clampInterval(Integer.MAX_VALUE));
        assertEquals(3000, ShulkerDepositSettings.intervalForProgress(Integer.MIN_VALUE));
        assertEquals(30, ShulkerDepositSettings.intervalForProgress(Integer.MAX_VALUE));
    }

    @Test public void slidingRightMeansFasterWithTenMillisecondSteps() {
        int previous = 3010;
        for (int progress = 0; progress <= ShulkerDepositSettings.MAX_PROGRESS; ++progress) {
            int interval = ShulkerDepositSettings.intervalForProgress(progress);
            assertEquals(previous - 10, interval);
            assertEquals(progress, ShulkerDepositSettings.progressForInterval(interval));
            previous = interval;
        }
        assertEquals(30, previous);
        assertEquals(1000, ShulkerDepositSettings.intervalForProgress(
            ShulkerDepositSettings.progressForInterval(1000)));
    }

    @Test public void labelShowsDelayAndUpperBoundNotGuaranteedSpeed() {
        assertEquals("Пауза: 30 мс · до 33,3 переноса/с", ShulkerDepositSettings.label(30));
        assertEquals("Пауза: 1000 мс · до 1,0 переноса/с", ShulkerDepositSettings.label(1000));
        assertEquals("Пауза: 3000 мс · до 0,3 переноса/с", ShulkerDepositSettings.label(3000));
    }
}
