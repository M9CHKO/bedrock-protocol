package com.m9chko.bedrockrelay;

import org.junit.Test;
import static org.junit.Assert.*;

public class ShulkerDepositOverlayControllerTest {
    @Test public void controlDoesNotDependOnContainerOrEnabledState() {
        // Visibility is deliberately independent of module on/off and the
        // Minecraft UI-blocked bit, unlike the other overlays.
        assertTrue(ShulkerDepositOverlayController.shouldShow(true, true));
        assertFalse(ShulkerDepositOverlayController.shouldShow(false, true));
        assertFalse(ShulkerDepositOverlayController.shouldShow(true, false));
        assertFalse(ShulkerDepositOverlayController.shouldShow(false, false));
    }
}
