package com.m9chko.bedrockrelay;

import org.junit.Test;
import static org.junit.Assert.*;

public class AutoCraftSettingsTest {
    @Test public void defaultsAndBoundsMatchNative() {
        assertEquals(1000,AutoCraftSettings.CRAFT.defaultMs);
        assertEquals(700,AutoCraftSettings.WINDOW.defaultMs);
        assertEquals(100,AutoCraftSettings.CRAFT.clamp(Integer.MIN_VALUE));
        assertEquals(5000,AutoCraftSettings.CRAFT.clamp(Integer.MAX_VALUE));
        assertEquals(300,AutoCraftSettings.WINDOW.clamp(Integer.MIN_VALUE));
        assertEquals(3000,AutoCraftSettings.WINDOW.clamp(Integer.MAX_VALUE));
    }
    @Test public void sliderMappingsRoundTripAndMoveRightToFaster() {
        for (AutoCraftSettings.Spec spec:AutoCraftSettings.ALL) {
            for (int p=0; p<=spec.maxProgress(); ++p) {
                assertEquals(p,spec.progress(spec.interval(p)));
                if (p>0) assertEquals(spec.step,spec.interval(p-1)-spec.interval(p));
            }
            assertEquals(spec.max,spec.interval(Integer.MIN_VALUE));
            assertEquals(spec.min,spec.interval(Integer.MAX_VALUE));
            assertEquals(spec.defaultMs,spec.interval(spec.progress(spec.defaultMs)));
        }
    }
}
