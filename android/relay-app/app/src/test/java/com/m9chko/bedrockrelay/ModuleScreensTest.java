package com.m9chko.bedrockrelay;

import android.app.Application;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.android.controller.ActivityController;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 28, application = Application.class, qualifiers = "mdpi", shadows = MainActivityLayoutTest.NativeShadow.class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class ModuleScreensTest {
    private static TextView text(View view, String value) {
        if (view instanceof TextView && value.contentEquals(((TextView)view).getText())) return (TextView)view;
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup)view;
            for (int i=0; i<group.getChildCount(); ++i) {
                TextView found = text(group.getChildAt(i), value);
                if (found != null) return found;
            }
        }
        return null;
    }
    private static void render(View root, int width, int height, String name) throws Exception {
        root.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY));
        root.layout(0, 0, width, height);
        Bitmap bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        root.draw(new Canvas(bitmap));
        File directory = new File("build/module-previews");
        assertTrue(directory.isDirectory() || directory.mkdirs());
        try (FileOutputStream out = new FileOutputStream(new File(directory, name + ".png"))) {
            assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, out));
        }
        bitmap.recycle();
    }
    @Test public void separateModuleScreensAndSettingsRender() throws Exception {
        try (ActivityController<MainActivity> controller = Robolectric.buildActivity(MainActivity.class).create().start()) {
            MainActivity activity = controller.get();
            View root = ((ViewGroup)activity.findViewById(android.R.id.content)).getChildAt(0);
            render(root, 390, 844, "connection");
            text(root, "Модули").performClick();
            render(root, 390, 844, "catalog");
            String[] ids = {"map_streaming", "no_render", "outline", "minimap", "equipment", "threat", "platform", "zip", "fill", "totem", "armor", "deposit", "craft", "chunks", "library", "textures"};
            for (String id : ids) {
                View tile = root.findViewWithTag("module-" + id);
                assertNotNull("Catalog tile " + id, tile);
                tile.performClick();
                assertNull("Other modules detached", root.findViewWithTag("module-map_streaming"));
                assertNotNull(root.findViewWithTag("modules-back"));
                render(root, 390, 844, "module-" + id);
                root.findViewWithTag("modules-back").performClick();
            }
            root.findViewWithTag("main-tab-Настройки").performClick();
            assertTrue(root.findViewWithTag("main-tab-Настройки").isSelected());
            render(root, 390, 844, "settings");
            assertFalse(activity.getSharedPreferences(RelayService.PREFERENCES, Context.MODE_PRIVATE).getBoolean("no_render_maps", true));
            text(root, "Модули").performClick();
            root.findViewWithTag("module-map_streaming").performClick();
            render(root, 844, 390, "map-landscape");
            Bundle saved = new Bundle();
            controller.saveInstanceState(saved);
            assertEquals("map_streaming", saved.getString("selected_module"));
            try (ActivityController<MainActivity> recreated = Robolectric.buildActivity(MainActivity.class).create(saved).start()) {
                assertNotNull(recreated.get().findViewById(android.R.id.content).findViewWithTag("modules-back"));
            }
        }
    }
}
