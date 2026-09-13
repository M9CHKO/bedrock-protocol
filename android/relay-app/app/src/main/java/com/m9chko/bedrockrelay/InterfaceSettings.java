package com.m9chko.bedrockrelay;

import android.content.SharedPreferences;

/** UI visibility is independent of whether an automation is enabled. */
final class InterfaceSettings {
    static final String VERSION = "interface_version";
    static final String FLOATING = "floating_controls";
    static final String MENU = "floating_menu";
    static final String CRAFT = "floating_craft";
    static final String DEPOSIT = "floating_deposit";
    static final String ZIP = "floating_zip";
    static final String FILL = "floating_fill";
    private InterfaceSettings() {}

    static void upgrade(SharedPreferences preferences) {
        SharedPreferences.Editor edit = preferences.edit().putBoolean("no_render_maps", false);
        if (preferences.getInt(VERSION, 0) < 2) {
            edit.putBoolean(FLOATING, false).putInt(VERSION, 2);
        }
        edit.apply();
    }

    static boolean visible(SharedPreferences preferences, String module) {
        return preferences.getBoolean(FLOATING, false) && preferences.getBoolean(module, true);
    }
}
