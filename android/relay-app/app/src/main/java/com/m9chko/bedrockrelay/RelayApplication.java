package com.m9chko.bedrockrelay;

import android.app.Application;
import android.content.Context;

/** Process-wide context for native-worker diagnostics without leaking an Activity. */
public final class RelayApplication extends Application {
    private static volatile Context applicationContext;

    @Override
    public void onCreate() {
        super.onCreate();
        applicationContext = getApplicationContext();
        Thread.UncaughtExceptionHandler previous =
            Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, error) -> {
            DiagnosticsLog.appendError(
                applicationContext,
                "crash",
                "Uncaught Java exception on thread=" + thread.getName(),
                error
            );
            if (previous != null) previous.uncaughtException(thread, error);
        });
        DiagnosticsLog.append(
            applicationContext,
            "INFO",
            "app",
            "CPE Relay process started; version=" + BuildConfig.VERSION_NAME
        );
    }

    static Context context() {
        return applicationContext;
    }

    @Override
    public void onLowMemory() {
        DiagnosticsLog.append(
            applicationContext,
            "WARN",
            "memory",
            "Android reported low memory"
        );
        super.onLowMemory();
    }

    @Override
    public void onTrimMemory(int level) {
        DiagnosticsLog.append(
            applicationContext,
            "DEBUG",
            "memory",
            "Android trim-memory level=" + level
        );
        super.onTrimMemory(level);
    }
}
