package com.m9chko.bedrockrelay;
import android.app.Application;
import android.content.Context;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import java.lang.reflect.Field;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 28, application = Application.class)
public class DiagnosticsLogTest {
    private Object field(String name) throws Exception {
        Field field = DiagnosticsLog.class.getDeclaredField(name);
        field.setAccessible(true); return field.get(null);
    }
    @Test public void slowDiskHasBoundedBacklogAndClearInvalidatesOldEntries() throws Exception {
        Context context = RuntimeEnvironment.getApplication();
        DiagnosticsLog.clear(context);
        ThreadPoolExecutor writer = (ThreadPoolExecutor) field("WRITER");
        synchronized (field("LOCK")) {
            for (int i = 0; i < 1000; ++i)
                DiagnosticsLog.append(context, "INFO", "test", "queued_" + i);
            assertTrue(writer.getQueue().size() <= 64);
            DiagnosticsLog.clear(context);
        }
        writer.submit(() -> {}).get(5, TimeUnit.SECONDS);
        assertFalse(DiagnosticsLog.readAll(context).contains("queued_"));
    }
    @Test public void rotationAndRedactionAreBounded() {
        Context context = RuntimeEnvironment.getApplication();
        DiagnosticsLog.clear(context);
        for (int i = 0; i < 25; ++i)
            DiagnosticsLog.append(context, "FATAL", "test", i + "я".repeat(32000));
        long bytes = new java.io.File(DiagnosticsLog.path(context)).length()
            + new java.io.File(context.getFilesDir(), "relay-diagnostics.previous.log").length();
        assertTrue(bytes <= 512 * 1024);
        DiagnosticsLog.append(context, "FATAL", "test", "access_token=do_not_disclose\nlast line");
        String tail = DiagnosticsLog.readTail(context, 2048);
        assertFalse(tail.contains("do_not_disclose"));
        assertTrue(tail.contains("<redacted>"));
        assertTrue(tail.contains("last line"));
        assertTrue(tail.length() <= 2200);
    }
}
