package com.m9chko.bedrockrelay;

import android.app.Application;
import android.content.Context;
import android.content.SharedPreferences;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 28, application = Application.class)
public class InterfaceSettingsTest {
    @Test public void migrationKeepsFeaturesButNeverPausesMaps() {
        SharedPreferences p = RuntimeEnvironment.getApplication().getSharedPreferences("migration", Context.MODE_PRIVATE);
        p.edit().clear().putBoolean("no_render_maps", true).putBoolean("no_render_enabled", true)
            .putBoolean(RelayService.KEY_AUTO_CRAFT_STORE_BUTTON, true).putString(RelayService.KEY_HOST, "example.org").commit();
        InterfaceSettings.upgrade(p);
        assertFalse(p.getBoolean("no_render_maps", true));
        assertFalse(InterfaceSettings.visible(p, InterfaceSettings.MENU));
        assertFalse(InterfaceSettings.visible(p, InterfaceSettings.FILL));
        assertTrue(p.getBoolean("no_render_enabled", false));
        assertTrue(p.getBoolean(RelayService.KEY_AUTO_CRAFT_STORE_BUTTON, false));
        assertEquals("example.org", p.getString(RelayService.KEY_HOST, ""));
        p.edit().putBoolean(InterfaceSettings.FLOATING, true).putBoolean(InterfaceSettings.CRAFT, false).commit();
        InterfaceSettings.upgrade(p);
        assertTrue(InterfaceSettings.visible(p, InterfaceSettings.MENU));
        assertFalse(InterfaceSettings.visible(p, InterfaceSettings.CRAFT));
        assertTrue(InterfaceSettings.visible(p, InterfaceSettings.ZIP));
        assertTrue(InterfaceSettings.visible(p, InterfaceSettings.FILL));
        assertTrue(p.getBoolean(RelayService.KEY_AUTO_CRAFT_STORE_BUTTON, false));
    }
}
