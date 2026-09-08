package com.m9chko.bedrockrelay;

import android.app.Application;
import android.content.Context;
import android.content.SharedPreferences;
import android.widget.EditText;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(sdk=28,application=Application.class)
public class MapQueueControlsTest {
    @Test public void timingLimitsAreIndependent(){
        assertEquals(500,MapQueueControls.clamp("transfer",0));
        assertEquals(300,MapQueueControls.clamp("hold",-1));
        assertEquals(3000,MapQueueControls.clamp("break",100));
        assertEquals(15000,MapQueueControls.clamp("break",99999));
        assertEquals(120000,MapQueueControls.clamp("timeout",Integer.MAX_VALUE));
        assertEquals(100,MapQueueControls.clamp("place",0));
        assertEquals(950,MapQueueControls.clamp("next",950));
    }
    @Test public void separateCardHasAllElevenFieldsWithoutStarting(){
        Context context=RuntimeEnvironment.getApplication();
        SharedPreferences prefs=context.getSharedPreferences("map-controls-test",Context.MODE_PRIVATE);
        prefs.edit().clear().putString(MapQueueControls.TIMING,"{\"hold\":2222}").commit();
        MapQueueControls card=new MapQueueControls(context,prefs);
        int count=0;boolean held=false;
        for(int i=0;i<card.getChildCount();i++)if(card.getChildAt(i) instanceof EditText){++count;held|=((EditText)card.getChildAt(i)).getText().toString().equals("2222");}
        assertEquals(11,count);assertTrue(held);
        assertFalse(prefs.contains(MapQueueControls.ARCHIVE));
    }
}
