package com.m9chko.bedrockrelay;

/** Shared, bounded timing settings for both Auto 2 menus and the service. */
final class AutoCraftSettings {
    static final Spec CRAFT = new Spec("auto2_craft_interval_ms", "Пауза между крафтами", 100, 5000, 50, 1000);
    static final Spec WINDOW = new Spec("auto2_window_pause_ms", "Пауза между окнами", 300, 3000, 100, 700);
    static final Spec[] ALL = {CRAFT, WINDOW};
    private AutoCraftSettings() {}

    static final class Spec {
        final String key, title;
        final int min, max, step, defaultMs;
        Spec(String key, String title, int min, int max, int step, int defaultMs) {
            this.key=key; this.title=title; this.min=min; this.max=max; this.step=step; this.defaultMs=defaultMs;
        }
        int clamp(int value) { return Math.max(min, Math.min(max, value)); }
        int maxProgress() { return (max-min)/step; }
        int interval(int progress) { return max-Math.max(0, Math.min(maxProgress(),progress))*step; }
        int progress(int interval) { return (max-clamp(interval))/step; }
        String label(int value) { return title + ": " + clamp(value) + " мс"; }
    }
}
