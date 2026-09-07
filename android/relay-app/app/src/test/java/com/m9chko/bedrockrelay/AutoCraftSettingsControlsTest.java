package com.m9chko.bedrockrelay;

import android.app.Application;
import android.content.Context;
import android.content.SharedPreferences;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import java.lang.reflect.Method;
import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(sdk=28, application=Application.class)
public class AutoCraftSettingsControlsTest {
    @Test public void settingsPersistAndRefreshAcrossBothMenuInstances() {
        Context context=RuntimeEnvironment.getApplication();
        SharedPreferences prefs=context.getSharedPreferences("craft-controls-test",Context.MODE_PRIVATE);
        prefs.edit().clear().commit();
        int[] calls={0};
        AutoCraftSettingsControls app=new AutoCraftSettingsControls(context,prefs,()->calls[0]++);
        AutoCraftSettingsControls overlay=new AutoCraftSettingsControls(context,prefs,()->calls[0]++);
        SeekBar slider=app.findViewWithTag(AutoCraftSettings.CRAFT.key);
        assertEquals(AutoCraftSettings.CRAFT.progress(1000),slider.getProgress());
        assertTrue(slider.onKeyDown(KeyEvent.KEYCODE_DPAD_RIGHT,new KeyEvent(KeyEvent.ACTION_DOWN,KeyEvent.KEYCODE_DPAD_RIGHT)));
        int saved=prefs.getInt(AutoCraftSettings.CRAFT.key,-1);
        assertTrue(saved>=100 && saved<1000);
        assertEquals(1,calls[0]);
        overlay.refresh();
        assertEquals(slider.getProgress(),((SeekBar)overlay.findViewWithTag(AutoCraftSettings.CRAFT.key)).getProgress());
        assertEquals(1,calls[0]);
        Button reset=(Button)overlay.getChildAt(overlay.getChildCount()-1);
        reset.performClick(); app.refresh();
        assertEquals(2,calls[0]);
        assertEquals(1000,prefs.getInt(AutoCraftSettings.CRAFT.key,-1));
        assertEquals(700,prefs.getInt(AutoCraftSettings.WINDOW.key,-1));
        assertEquals(AutoCraftSettings.CRAFT.progress(1000),slider.getProgress());
        app.measure(View.MeasureSpec.makeMeasureSpec(280,View.MeasureSpec.EXACTLY),View.MeasureSpec.makeMeasureSpec(0,View.MeasureSpec.UNSPECIFIED));
        app.layout(0,0,280,app.getMeasuredHeight());
        assertTrue(slider.getHeight()>=48);
    }

    @Test public void inGameAutomationPageContainsBothSettings() throws Exception {
        Context context=RuntimeEnvironment.getApplication();
        SharedPreferences prefs=context.getSharedPreferences("craft-overlay-test",Context.MODE_PRIVATE);
        RelayOverlayController controller=new RelayOverlayController(context,prefs,()->{},(x,y,z)->{});
        try {
            Method build=RelayOverlayController.class.getDeclaredMethod("buildAutomationPage",LinearLayout.class);
            build.setAccessible(true);
            LinearLayout root=new LinearLayout(context); build.invoke(controller,root);
            assertNotNull(root.findViewWithTag(AutoCraftSettings.CRAFT.key));
            assertNotNull(root.findViewWithTag(AutoCraftSettings.WINDOW.key));
        } finally { controller.destroy(); }
    }
}
