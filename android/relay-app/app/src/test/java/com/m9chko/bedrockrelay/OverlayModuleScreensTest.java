package com.m9chko.bedrockrelay;

import android.app.Application;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.view.View;
import android.view.ViewGroup;
import android.view.MotionEvent;
import android.view.WindowManager;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.Switch;
import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Method;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.android.controller.ActivityController;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(sdk=28, application=Application.class, qualifiers="w390dp-h844dp-mdpi",
    shadows=MainActivityLayoutTest.NativeShadow.class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class OverlayModuleScreensTest {
    private void setField(Object target,String name,Object value) throws Exception {
        java.lang.reflect.Field field=target.getClass().getDeclaredField(name);
        field.setAccessible(true);field.set(target,value);
    }
    private View panel(RelayOverlayController overlay) throws Exception {
        Method method=RelayOverlayController.class.getDeclaredMethod("buildPanel");
        method.setAccessible(true);
        return (View)method.invoke(overlay);
    }
    private void layout(View view, int screenHeight) {
        view.measure(View.MeasureSpec.makeMeasureSpec(320,View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(screenHeight,View.MeasureSpec.AT_MOST));
        view.layout(0,0,view.getMeasuredWidth(),view.getMeasuredHeight());
        assertTrue("Panel must leave space below its 72dp offset: " + view.getHeight(),view.getHeight() <= screenHeight-92);
        View back=view.findViewWithTag("overlay-back");
        if (back.getVisibility()==View.VISIBLE) {
            assertTrue(back.getHeight()>=48);
            assertTrue(back.getWidth()>=48);
        }
    }
    private void render(View view, String name) throws Exception {
        Bitmap bitmap=Bitmap.createBitmap(view.getWidth(),view.getHeight(),Bitmap.Config.ARGB_8888);
        view.draw(new Canvas(bitmap));
        File dir=new File("build/module-previews");
        assertTrue(dir.isDirectory() || dir.mkdirs());
        try(FileOutputStream out=new FileOutputStream(new File(dir,name+".png"))) {
            assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG,100,out));
        }
        bitmap.recycle();
    }
    private void checkScreens(int height, String suffix) throws Exception {
        try(ActivityController<MainActivity> activity=Robolectric.buildActivity(MainActivity.class).create().start()) {
            SharedPreferences prefs=activity.get().getSharedPreferences(RelayService.PREFERENCES,Context.MODE_PRIVATE);
            RelayOverlayController overlay=new RelayOverlayController(activity.get(),prefs,()->{},(x,y,z)->{});
            try {
                View root=panel(overlay);
                layout(root,height);render(root,"overlay-home-"+suffix);
                assertEquals(16,OverlayModuleCatalog.PAGES.length);
                for(String page:OverlayModuleCatalog.PAGES) {
                    View tile=root.findViewWithTag("overlay-module-"+page);
                    assertNotNull(page,tile);
                    assertTrue("Touch target height: "+page,tile.getHeight()>=48);
                    tile.performClick();
                    assertNotNull(root.findViewWithTag("overlay-page-"+page));
                    assertNull("Other modules detached",root.findViewWithTag("overlay-module-map_streaming"));
                    layout(root,height);
                    if(page.equals("zip") || page.equals("no_render") || page.equals("map_streaming")) render(root,"overlay-"+page+"-"+suffix);
                    root.findViewWithTag("overlay-back").performClick();
                    layout(root,height);
                }
                assertFalse("Menu must not change map visibility",prefs.getBoolean("no_render_maps",true));
            } finally { overlay.destroy(); }
        }
    }
    @Test public void portraitHasSeparateCompactModules() throws Exception { checkScreens(844,"portrait"); }
    @Test @Config(qualifiers="w844dp-h390dp-land-mdpi")
    public void landscapeKeepsHeaderAndBackVisible() throws Exception { checkScreens(390,"landscape"); }
    @Test public void entityToggleIsIndependentAndSurvivesNavigation() throws Exception {
        try(ActivityController<MainActivity> activity=Robolectric.buildActivity(MainActivity.class).create().start()) {
            SharedPreferences prefs=activity.get().getSharedPreferences(RelayService.PREFERENCES,Context.MODE_PRIVATE);
            AtomicInteger changed=new AtomicInteger();
            RelayOverlayController overlay=new RelayOverlayController(activity.get(),prefs,changed::incrementAndGet,(x,y,z)->{});
            try {
                View root=panel(overlay);
                for(int i=0;i<6;++i) {
                    root.findViewWithTag("overlay-module-no_render").performClick();
                    Switch entities=root.findViewWithTag("overlay-setting-no_render_enabled");
                    assertEquals(i%2!=0,entities.isChecked());
                    entities.setChecked(!entities.isChecked());
                    assertFalse(prefs.getBoolean("no_render_maps",true));
                    root.findViewWithTag("overlay-back").performClick();
                }
                assertEquals(6,changed.get());
                root.findViewWithTag("overlay-module-totem").performClick();
                assertNull(root.findViewWithTag("overlay-setting-"+RelayService.KEY_AUTO_ARMOR));
                ((Switch)root.findViewWithTag("overlay-setting-"+RelayService.KEY_AUTO_TOTEM)).setChecked(true);
                root.findViewWithTag("overlay-back").performClick();
                root.findViewWithTag("overlay-module-armor").performClick();
                assertNull(root.findViewWithTag("overlay-setting-"+RelayService.KEY_AUTO_TOTEM));
                assertFalse(((Switch)root.findViewWithTag("overlay-setting-"+RelayService.KEY_AUTO_ARMOR)).isChecked());
                assertTrue(prefs.getBoolean(RelayService.KEY_AUTO_TOTEM,false));
            } finally { overlay.destroy(); }
        }
    }
    @Test public void keyboardFocusOnlyAfterTapAndReleasedOnDoneOrBack() throws Exception {
        try(ActivityController<MainActivity> activity=Robolectric.buildActivity(MainActivity.class).create().start()) {
            SharedPreferences prefs=activity.get().getSharedPreferences(RelayService.PREFERENCES,Context.MODE_PRIVATE);
            RelayOverlayController overlay=new RelayOverlayController(activity.get(),prefs,()->{},(x,y,z)->{});
            try {
                View root=panel(overlay);
                WindowManager.LayoutParams params=new WindowManager.LayoutParams();
                params.flags=WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL;
                // No real overlay window is installed: only exercise focus policy.
                setField(overlay,"windowRoot",(LinearLayout)root);setField(overlay,"windowParams",params);
                for(int pass=0;pass<2;pass++) {
                    root.findViewWithTag("overlay-module-zip").performClick();
                    assertTrue((params.flags & WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE)!=0);
                    root.findViewWithTag("zip-timing-toggle").performClick();
                    ViewGroup timing=root.findViewWithTag("zip-timing");
                    EditText input=null;
                    for(int i=0;i<timing.getChildCount();i++) if(timing.getChildAt(i) instanceof EditText) {input=(EditText)timing.getChildAt(i);break;}
                    assertNotNull(input);
                    MotionEvent tap=MotionEvent.obtain(0,10,MotionEvent.ACTION_UP,1,1,0);
                    try {input.dispatchTouchEvent(tap);} finally {tap.recycle();}
                    assertEquals(0,params.flags & WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
                    assertTrue((params.flags & WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL)!=0);
                    if(pass==0) input.onEditorAction(android.view.inputmethod.EditorInfo.IME_ACTION_DONE);
                    root.findViewWithTag("overlay-back").performClick();
                    assertTrue((params.flags & WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE)!=0);
                }
                setField(overlay,"windowRoot",null);setField(overlay,"windowParams",null);
            } finally {overlay.destroy();}
        }
    }
}
