package com.m9chko.bedrockrelay;

import java.util.Locale;

/** Shared speed mapping for both menus; no Android or packet dependencies. */
final class ShulkerDepositSettings {
    static final int MIN_INTERVAL_MS = 30;
    static final int MAX_INTERVAL_MS = 3000;
    static final int DEFAULT_INTERVAL_MS = 1000;
    static final int STEP_MS = 10;
    static final int MAX_PROGRESS = (MAX_INTERVAL_MS - MIN_INTERVAL_MS) / STEP_MS;

    private ShulkerDepositSettings() {}

    static int clampInterval(int value) {
        return Math.max(MIN_INTERVAL_MS, Math.min(MAX_INTERVAL_MS, value));
    }

    // Sliding to the right makes transfers faster, not slower.
    static int intervalForProgress(int progress) {
        int bounded = Math.max(0, Math.min(MAX_PROGRESS, progress));
        return MAX_INTERVAL_MS - bounded * STEP_MS;
    }

    static int progressForInterval(int interval) {
        return (MAX_INTERVAL_MS - clampInterval(interval)) / STEP_MS;
    }

    static String label(int interval) {
        int bounded = clampInterval(interval);
        return String.format(Locale.forLanguageTag("ru"),
            "Пауза: %d мс · до %.1f переноса/с", bounded, 1000.0 / bounded);
    }
}
