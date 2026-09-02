package com.m9chko.bedrockrelay;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.util.Collections;

public final class ThreatAnalyzerTest {
    @Test
    public void hostileInsideConfiguredDistanceTriggers() {
        ThreatAnalyzer analyzer = new ThreatAnalyzer();
        ThreatAnalyzer.Result result = analyzer.analyze(
            frame("42", "minecraft:zombie", 4.0),
            ThreatAnalyzer.DefenseState.unknown(),
            10
        );

        assertNotNull(result.primary);
        assertEquals("42", result.primary.entityId);
        assertTrue(result.dangerousEntityIds.contains("42"));
        assertTrue(result.primary.distance < 5.0);
    }

    @Test
    public void userDistanceSuppressesFarHostile() {
        ThreatAnalyzer analyzer = new ThreatAnalyzer();
        ThreatAnalyzer.Result result = analyzer.analyze(
            frame("42", "minecraft:zombie", 8.0),
            ThreatAnalyzer.DefenseState.unknown(),
            5
        );

        assertNull(result.primary);
        assertTrue(result.dangerousEntityIds.isEmpty());
    }

    @Test
    public void passiveEntityDoesNotTrigger() {
        ThreatAnalyzer analyzer = new ThreatAnalyzer();
        ThreatAnalyzer.Result result = analyzer.analyze(
            frame("7", "minecraft:cow", 2.0),
            ThreatAnalyzer.DefenseState.unknown(),
            12
        );

        assertNull(result.primary);
    }

    @Test
    public void armorAndResistanceReduceEstimatedDamage() {
        ThreatAnalyzer unprotectedAnalyzer = new ThreatAnalyzer();
        ThreatAnalyzer protectedAnalyzer = new ThreatAnalyzer();
        ThreatAnalyzer.Result unprotected = unprotectedAnalyzer.analyze(
            frame("9", "minecraft:warden", 3.0),
            ThreatAnalyzer.DefenseState.unknown(),
            12
        );
        ThreatAnalyzer.DefenseState protectedPlayer =
            new ThreatAnalyzer.DefenseState(
                true, 20, 20,
                true, 20, 5,
                true, 4,
                2, 20, 12, 4
            );
        ThreatAnalyzer.Result protectedResult = protectedAnalyzer.analyze(
            frame("9", "minecraft:warden", 3.0),
            protectedPlayer,
            12
        );

        assertNotNull(unprotected.primary);
        assertNotNull(protectedResult.primary);
        assertTrue(
            protectedResult.primary.damageMaximum <
                unprotected.primary.damageMaximum
        );
    }

    private static EntityOutlineOverlayController.Frame frame(
        String id,
        String type,
        double x
    ) {
        EntityOutlineOverlayController.CameraSample camera =
            new EntityOutlineOverlayController.CameraSample(
                true,
                true,
                1,
                0,
                0.9,
                0,
                0,
                0,
                1,
                0
            );
        EntityOutlineOverlayController.EntitySample entity =
            new EntityOutlineOverlayController.EntitySample(
                id,
                type,
                type,
                false,
                false,
                x,
                0,
                0,
                0.8,
                1.8,
                1,
                0
            );
        return new EntityOutlineOverlayController.Frame(
            camera,
            Collections.singletonList(entity),
            1,
            0
        );
    }
}
