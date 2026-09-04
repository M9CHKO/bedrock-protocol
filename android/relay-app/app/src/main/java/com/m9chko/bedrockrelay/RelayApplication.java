package com.m9chko.bedrockrelay;

import android.annotation.TargetApi;
import android.app.ActivityManager;
import android.app.Application;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;

import java.util.List;

/** Process-wide context for native-worker diagnostics without leaking an Activity. */
public final class RelayApplication extends Application {
    private static final String EXIT_HISTORY_PREFERENCES = "exit_history";
    private static final String KEY_LAST_EXIT_TIMESTAMP = "last_exit_timestamp";
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
        reportPreviousProcessTermination();
    }

    static Context context() {
        return applicationContext;
    }

    private void reportPreviousProcessTermination() {
        SharedPreferences relayPreferences = getSharedPreferences(
            RelayService.PREFERENCES,
            MODE_PRIVATE
        );
        if (relayPreferences.getBoolean(RelayService.KEY_RELAY_ACTIVE, false)) {
            DiagnosticsLog.append(
                applicationContext,
                "ERROR",
                "crash",
                "Previous app process ended while the embedded relay was " +
                    "active; graceful RelayService shutdown was not reached"
            );
            relayPreferences.edit()
                .putBoolean(RelayService.KEY_RELAY_ACTIVE, false)
                .commit();
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            reportAndroidExitHistory();
        }
    }

    @TargetApi(Build.VERSION_CODES.R)
    private void reportAndroidExitHistory() {
        try {
            ActivityManager manager = getSystemService(ActivityManager.class);
            if (manager == null) return;
            List<ApplicationExitInfo> exits = manager
                .getHistoricalProcessExitReasons(null, 0, 5);
            if (exits == null || exits.isEmpty()) return;

            SharedPreferences history = getSharedPreferences(
                EXIT_HISTORY_PREFERENCES,
                MODE_PRIVATE
            );
            long lastReported = history.getLong(KEY_LAST_EXIT_TIMESTAMP, 0);
            long newest = lastReported;
            for (ApplicationExitInfo exit : exits) {
                if (exit == null || exit.getTimestamp() <= lastReported) {
                    continue;
                }
                newest = Math.max(newest, exit.getTimestamp());
                String description = exit.getDescription();
                DiagnosticsLog.appendAt(
                    applicationContext,
                    isAbnormalExit(exit.getReason()) ? "ERROR" : "WARN",
                    "process_exit",
                    "previous_process=" + exit.getProcessName() +
                        " reason=" + exitReasonName(exit.getReason()) +
                        " reasonCode=" + exit.getReason() +
                        " status=" + exit.getStatus() +
                        " importance=" + exit.getImportance() +
                        " pssKb=" + exit.getPss() +
                        " rssKb=" + exit.getRss() +
                        (description == null || description.isEmpty()
                            ? ""
                            : " description=" + description),
                    exit.getTimestamp()
                );
            }
            if (newest > lastReported) {
                history.edit().putLong(KEY_LAST_EXIT_TIMESTAMP, newest).commit();
            }
        } catch (Throwable error) {
            DiagnosticsLog.appendError(
                applicationContext,
                "process_exit",
                "Failed to read Android process-exit history",
                error
            );
        }
    }

    @TargetApi(Build.VERSION_CODES.R)
    private static boolean isAbnormalExit(int reason) {
        return reason == ApplicationExitInfo.REASON_CRASH ||
            reason == ApplicationExitInfo.REASON_CRASH_NATIVE ||
            reason == ApplicationExitInfo.REASON_ANR ||
            reason == ApplicationExitInfo.REASON_LOW_MEMORY ||
            reason == ApplicationExitInfo.REASON_SIGNALED ||
            reason == ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE ||
            reason == ApplicationExitInfo.REASON_INITIALIZATION_FAILURE;
    }

    @TargetApi(Build.VERSION_CODES.R)
    private static String exitReasonName(int reason) {
        switch (reason) {
            case ApplicationExitInfo.REASON_EXIT_SELF: return "exit_self";
            case ApplicationExitInfo.REASON_SIGNALED: return "signaled";
            case ApplicationExitInfo.REASON_LOW_MEMORY: return "low_memory";
            case ApplicationExitInfo.REASON_CRASH: return "java_crash";
            case ApplicationExitInfo.REASON_CRASH_NATIVE: return "native_crash";
            case ApplicationExitInfo.REASON_ANR: return "anr";
            case ApplicationExitInfo.REASON_INITIALIZATION_FAILURE:
                return "initialization_failure";
            case ApplicationExitInfo.REASON_PERMISSION_CHANGE:
                return "permission_change";
            case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE:
                return "excessive_resource_usage";
            case ApplicationExitInfo.REASON_USER_REQUESTED:
                return "user_requested";
            case ApplicationExitInfo.REASON_USER_STOPPED: return "user_stopped";
            case ApplicationExitInfo.REASON_DEPENDENCY_DIED:
                return "dependency_died";
            case ApplicationExitInfo.REASON_OTHER: return "other";
            case ApplicationExitInfo.REASON_FREEZER: return "freezer";
            case ApplicationExitInfo.REASON_PACKAGE_STATE_CHANGE:
                return "package_state_change";
            case ApplicationExitInfo.REASON_PACKAGE_UPDATED:
                return "package_updated";
            case ApplicationExitInfo.REASON_UNKNOWN:
            default:
                return "unknown";
        }
    }

    @Override
    public void onLowMemory() {
        trimNativeMemory(80);
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
        trimNativeMemory(level);
        DiagnosticsLog.append(
            applicationContext,
            "DEBUG",
            "memory",
            "Android trim-memory level=" + level
        );
        super.onTrimMemory(level);
    }

    private static void trimNativeMemory(int level) {
        Context context = applicationContext;
        if (context == null || !context.getSharedPreferences(
                RelayService.PREFERENCES,
                MODE_PRIVATE
            ).getBoolean(RelayService.KEY_RELAY_ACTIVE, false)) {
            // Do not load the large native library for the first time while an
            // idle application process is already short on memory.
            return;
        }
        try {
            NativeBridge.trimMemory(level);
        } catch (Throwable ignored) {
            // Android can deliver this callback while the process is already
            // under severe pressure. Cleanup must stay allocation-free and
            // must not create another failure path.
        }
    }
}
