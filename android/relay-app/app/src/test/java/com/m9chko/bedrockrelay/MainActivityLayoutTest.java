package com.m9chko.bedrockrelay;

import android.app.Application;
import android.content.Context;
import android.graphics.Rect;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.android.controller.ActivityController;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.Implementation;
import org.robolectric.annotation.Implements;
import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 28, application = Application.class, shadows = MainActivityLayoutTest.NativeShadow.class)
public class MainActivityLayoutTest {
    @Implements(value = NativeBridge.class, isInAndroidSdk = false)
    public static class NativeShadow {
        @Implementation protected static void __staticInitializer__() {}
        @Implementation protected static String supportedVersions() { return "[\"1.21.100\"]"; }
        @Implementation protected static String snapshot() { return "{}"; }
    }

    private TextView find(View view, String prefix) {
        if (view instanceof TextView && (((TextView) view).getText().toString().equals(prefix) ||
            (prefix.equals("Запустить и") && ((TextView) view).getText().toString().startsWith(prefix))))
            return (TextView) view;
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); ++i) {
                TextView result = find(group.getChildAt(i), prefix);
                if (result != null) return result;
            }
        }
        return null;
    }

    private void checkLayout(int width, int height) {
        try (ActivityController<MainActivity> controller = Robolectric.buildActivity(MainActivity.class).create().start()) {
            MainActivity activity = controller.get();
            ViewGroup holder = activity.findViewById(android.R.id.content);
            View root = holder.getChildAt(0);
            root.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY));
            root.layout(0, 0, width, height);
            TextView start = find(root, "Запустить и");
            TextView modules = find(root, "Модули");
            assertNotNull(start);
            assertNotNull(modules);
            Rect startBounds = new Rect(0, 0, start.getWidth(), start.getHeight());
            Rect navBounds = new Rect(0, 0, modules.getWidth(), modules.getHeight());
            ((ViewGroup) root).offsetDescendantRectToMyCoords(start, startBounds);
            ((ViewGroup) root).offsetDescendantRectToMyCoords(modules, navBounds);
            assertTrue("start action clipped: " + startBounds, startBounds.height() >= 48);
            assertTrue("start action overlaps navigation", startBounds.bottom <= navBounds.top);
            assertTrue("navigation outside screen: " + navBounds + " root=" + root.getMeasuredWidth()
                + "x" + root.getMeasuredHeight() + " density=" + activity.getResources().getDisplayMetrics().density,
                navBounds.bottom <= height);
            modules.performClick();
            assertTrue(find(root, "Инструменты мира").getVisibility() == View.VISIBLE);
            View row = (View) find(root, "Авто-тотем").getParent().getParent();
            row.performClick();
            assertTrue(activity.getSharedPreferences(RelayService.PREFERENCES, Context.MODE_PRIVATE)
                .getBoolean(RelayService.KEY_AUTO_TOTEM, false));
            find(root, "Подключение").performClick();
        }
    }
    @Test public void portraitKeepsLaunchAndNavigationVisible() { checkLayout(360, 800); }
    @Test @Config(qualifiers = "land-mdpi") public void landscapeKeepsLaunchAndNavigationVisible() { checkLayout(800, 360); }
}
