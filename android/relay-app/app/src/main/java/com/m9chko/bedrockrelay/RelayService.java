package com.m9chko.bedrockrelay;

import android.app.Notification;
import android.app.ActivityManager;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ServiceInfo;
import android.net.Uri;
import android.os.Build;
import android.os.Debug;
import android.os.IBinder;
import android.os.PowerManager;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

public final class RelayService extends Service {
    public static final String PREFERENCES = "relay";
    public static final String KEY_HOST = "destination_host";
    public static final String KEY_PORT = "destination_port";
    public static final String KEY_VERSION = "minecraft_version";
    public static final String KEY_LAST_ERROR = "last_error";
    public static final String KEY_AUTH_CODE = "auth_code";
    public static final String KEY_AUTH_URI = "auth_uri";
    public static final String KEY_RELAY_ACTIVE = "relay_active";

    public static final String ACTION_START =
        "com.m9chko.bedrockrelay.action.START";
    public static final String ACTION_STOP =
        "com.m9chko.bedrockrelay.action.STOP";
    public static final String EXTRA_HOST = "host";
    public static final String EXTRA_PORT = "port";
    public static final String EXTRA_VERSION = "version";

    private static final String CHANNEL_ID = "bedrock_relay";
    private static final int NOTIFICATION_ID = 19132;

    private final ExecutorService commandExecutor =
        Executors.newSingleThreadExecutor();
    private final ScheduledExecutorService pollExecutor =
        Executors.newSingleThreadScheduledExecutor();
    private final AtomicBoolean pollingStarted = new AtomicBoolean(false);

    private SharedPreferences preferences;
    private PowerManager.WakeLock wakeLock;
    private volatile boolean serviceStopping;
    private volatile String notificationStatus = "Запуск UDP relay…";
    private volatile String lastSnapshotFingerprint = "";
    private volatile long lastPollingErrorAt;

    @Override
    public void onCreate() {
        super.onCreate();
        preferences = getSharedPreferences(PREFERENCES, MODE_PRIVATE);
        createNotificationChannel();
        DiagnosticsLog.append(
            this,
            "INFO",
            "service",
            "Foreground service created; app=" + BuildConfig.VERSION_NAME +
                " sdk=" + Build.VERSION.SDK_INT +
                " abi=" + Build.SUPPORTED_ABIS[0]
        );
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent == null ? null : intent.getAction();
        if (ACTION_STOP.equals(action)) {
            DiagnosticsLog.append(this, "INFO", "service", "Stop action received");
            stopRelayAndSelf();
            return START_NOT_STICKY;
        }
        if (!ACTION_START.equals(action)) {
            DiagnosticsLog.append(
                this,
                "WARN",
                "service",
                "Ignored service start without ACTION_START"
            );
            return START_NOT_STICKY;
        }

        String host = intent.getStringExtra(EXTRA_HOST);
        int port = intent.getIntExtra(EXTRA_PORT, 19132);
        String version = intent.getStringExtra(EXTRA_VERSION);
        if (host == null || host.trim().isEmpty()) {
            host = preferences.getString(KEY_HOST, "cpe.ign.gg");
        }
        host = host.trim();
        if (version == null || version.trim().isEmpty()) {
            version = preferences.getString(KEY_VERSION, "1.21.100");
        }
        version = version.trim();

        DiagnosticsLog.append(
            this,
            "INFO",
            "service",
            "Start requested: local=0.0.0.0:19132 destination=" +
                host + ":" + port
                + " version=" + version
        );

        promoteToForeground(buildNotification());
        acquireWakeLock();
        startPolling();
        serviceStopping = false;
        final String destinationHost = host;
        final int destinationPort = port;
        final String minecraftVersion = version;
        commandExecutor.execute(() -> startNativeRelay(
            destinationHost,
            destinationPort,
            minecraftVersion
        ));
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        serviceStopping = true;
        DiagnosticsLog.append(this, "INFO", "service", "Service destroying");
        try {
            NativeBridge.stopRelay();
        } catch (Throwable error) {
            DiagnosticsLog.appendError(
                this,
                "native",
                "Native stop failed during service destruction",
                error
            );
        }
        preferences.edit().putBoolean(KEY_RELAY_ACTIVE, false).commit();
        releaseWakeLock();
        pollExecutor.shutdownNow();
        commandExecutor.shutdownNow();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void startNativeRelay(String host, int port, String version) {
        DiagnosticsLog.append(
            this,
            "INFO",
            "native",
            "Starting embedded relay for " + host + ":" + port +
                " version=" + version
        );
        preferences.edit()
            .putString(KEY_HOST, host)
            .putInt(KEY_PORT, port)
            .putString(KEY_VERSION, version)
            .remove(KEY_LAST_ERROR)
            .remove(KEY_AUTH_CODE)
            .remove(KEY_AUTH_URI)
            .apply();
        File cache = new File(getFilesDir(), "auth-cache");
        if (!cache.isDirectory() && !cache.mkdirs()) {
            reportError("Не удалось создать внутренний кэш авторизации");
            return;
        }

        try {
            JSONObject result = new JSONObject(NativeBridge.startRelay(
                host,
                port,
                version,
                cache.getAbsolutePath()
            ));
            if (!result.optBoolean("ok", false)) {
                reportError(result.optString("error", "Не удалось запустить relay"));
                return;
            }
            DiagnosticsLog.append(
                this,
                "INFO",
                "native",
                "Embedded relay started; boundPort=" +
                    result.optInt("boundPort", 0) +
                    " listening=" + result.optBoolean("listening", false)
            );
            // A synchronous marker survives a native abort/process kill. The
            // next Application instance can then distinguish an abrupt relay
            // death from a normal user stop even on Android 8/9.
            preferences.edit().putBoolean(KEY_RELAY_ACTIVE, true).commit();
            notificationStatus = "Relay готов: 127.0.0.1:19132";
            refreshNotification();
        } catch (Throwable error) {
            reportError(error.getMessage() == null
                ? error.getClass().getSimpleName()
                : error.getMessage(), error);
        }
    }

    private void startPolling() {
        if (!pollingStarted.compareAndSet(false, true)) {
            return;
        }
        pollExecutor.scheduleWithFixedDelay(() -> {
            if (serviceStopping) {
                return;
            }
            try {
                JSONArray events = new JSONArray(NativeBridge.pollEvents());
                for (int index = 0; index < events.length(); ++index) {
                    handleEvent(events.getJSONObject(index));
                }
                updateStatusFromSnapshot(
                    new JSONObject(NativeBridge.snapshot())
                );
            } catch (Throwable error) {
                long now = System.currentTimeMillis();
                if (now - lastPollingErrorAt >= 10_000) {
                    lastPollingErrorAt = now;
                    DiagnosticsLog.appendError(
                        this,
                        "poll",
                        "Native event/snapshot polling failed; polling continues",
                        error
                    );
                }
            }
        }, 0, 500, TimeUnit.MILLISECONDS);
    }

    private void handleEvent(JSONObject event) {
        String type = event.optString("type", "");
        if ("msa_code".equals(type)) {
            String code = event.optString("userCode", "");
            String uri = event.optString(
                "verificationUri",
                "https://microsoft.com/link"
            );
            preferences.edit()
                .putString(KEY_AUTH_CODE, code)
                .putString(KEY_AUTH_URI, uri)
                .apply();
            notificationStatus = code.isEmpty()
                ? "Требуется вход Xbox"
                : "Код Xbox: " + code;
            DiagnosticsLog.append(
                this,
                "INFO",
                "auth",
                "Xbox device authorization requested; user code omitted from log"
            );
            refreshNotification();
            return;
        }
        if ("error".equals(type)) {
            reportError(event.optString("message", "Неизвестная ошибка relay"));
            return;
        }

        String message = event.optString("message", "");
        String level = event.optString(
            "level",
            "ping_warning".equals(type) ? "WARN" : "INFO"
        );
        String component = event.optString("component", "native");
        DiagnosticsLog.appendAt(
            this,
            level,
            component,
            "event=" + type + (message.isEmpty() ? "" : " " + message),
            event.optLong("timestampMs", 0)
        );
        if ("disconnect".equals(type) ||
            "local_login_timeout".equals(type) ||
            "transport_error".equals(type) ||
            "parse_error".equals(type)) {
            logMemorySnapshot(type);
        }
        if ("upstream_ready".equals(type)) {
            preferences.edit()
                .remove(KEY_AUTH_CODE)
                .remove(KEY_AUTH_URI)
                .apply();
            notificationStatus = "Relay подключён к серверу";
            refreshNotification();
        }
    }

    private void logMemorySnapshot(String trigger) {
        try {
            Runtime runtime = Runtime.getRuntime();
            long javaUsed = runtime.totalMemory() - runtime.freeMemory();
            ActivityManager.MemoryInfo system = new ActivityManager.MemoryInfo();
            ActivityManager manager =
                (ActivityManager) getSystemService(ACTIVITY_SERVICE);
            manager.getMemoryInfo(system);
            DiagnosticsLog.append(
                this,
                "DEBUG",
                "memory",
                "trigger=" + trigger +
                    " javaUsedBytes=" + javaUsed +
                    " javaMaxBytes=" + runtime.maxMemory() +
                    " nativeAllocatedBytes=" +
                        Debug.getNativeHeapAllocatedSize() +
                    " processPssKb=" + Debug.getPss() +
                    " systemAvailBytes=" + system.availMem +
                    " systemLowMemory=" + system.lowMemory
            );
        } catch (Throwable error) {
            DiagnosticsLog.appendError(
                this,
                "memory",
                "Failed to capture relay memory snapshot",
                error
            );
        }
    }

    private void updateStatusFromSnapshot(JSONObject state) {
        if (!state.optBoolean("running", false)) {
            return;
        }
        String fingerprint =
            "running=" + state.optBoolean("running", false) +
            " listening=" + state.optBoolean("listening", false) +
            " version=" + state.optString("version", "") +
            " ping=" + state.optBoolean("pingOk", false) +
            " downstream=" + state.optInt("downstreamConnections", 0) +
            " downstreamJoined=" + state.optInt("downstreamJoinedCount", 0) +
            " destinationPing=" + state.optBoolean("destinationPingOk", false) +
            " destinationVersion=" + state.optString("destinationGameVersion", "") +
            " upstreamStarted=" + state.optInt("upstreamStartedCount", 0) +
            " upstreamReady=" + state.optInt("upstreamReadyCount", 0);
        if (!fingerprint.equals(lastSnapshotFingerprint)) {
            lastSnapshotFingerprint = fingerprint;
            DiagnosticsLog.append(this, "DEBUG", "snapshot", fingerprint);
        }
        String authCode = preferences.getString(KEY_AUTH_CODE, "");
        if (!authCode.isEmpty()) {
            return;
        }
        if (state.optBoolean("upstreamReady", false)) {
            notificationStatus = "Relay подключён к серверу";
        } else if (state.optInt("downstreamConnections", 0) > 0) {
            notificationStatus = "Minecraft подключён — вход на сервер…";
        } else if (state.optBoolean("listening", false)) {
            notificationStatus = "Relay готов: 127.0.0.1:19132";
        }
        refreshNotification();
    }

    private void stopRelayAndSelf() {
        if (serviceStopping) {
            return;
        }
        serviceStopping = true;
        DiagnosticsLog.append(
            this,
            "INFO",
            "service",
            "Stopping relay by user request"
        );
        notificationStatus = "Остановка relay…";
        refreshNotification();
        commandExecutor.execute(() -> {
            try {
                NativeBridge.stopRelay();
            } catch (Throwable error) {
                DiagnosticsLog.appendError(
                    this,
                    "native",
                    "Native stop failed",
                    error
                );
            }
            preferences.edit()
                .remove(KEY_AUTH_CODE)
                .remove(KEY_AUTH_URI)
                .putBoolean(KEY_RELAY_ACTIVE, false)
                .commit();
            releaseWakeLock();
            DiagnosticsLog.append(this, "INFO", "service", "Relay stopped");
            stopForeground(STOP_FOREGROUND_REMOVE);
            stopSelf();
        });
    }

    private void reportError(String message) {
        reportError(message, null);
    }

    private void reportError(String message, Throwable error) {
        String safe = message == null || message.isEmpty()
            ? "Неизвестная ошибка"
            : message;
        if (error == null) {
            DiagnosticsLog.append(this, "ERROR", "relay", safe);
        } else {
            DiagnosticsLog.appendError(this, "relay", safe, error);
        }
        preferences.edit().putString(KEY_LAST_ERROR, safe).apply();
        notificationStatus = "Ошибка relay: " + safe;
        refreshNotification();
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT < 26) {
            return;
        }
        NotificationChannel channel = new NotificationChannel(
            CHANNEL_ID,
            getString(R.string.notification_channel_name),
            NotificationManager.IMPORTANCE_LOW
        );
        channel.setDescription(
            getString(R.string.notification_channel_description)
        );
        getSystemService(NotificationManager.class)
            .createNotificationChannel(channel);
    }

    private Notification buildNotification() {
        PendingIntent contentIntent = PendingIntent.getActivity(
            this,
            1,
            new Intent(this, MainActivity.class),
            PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE
        );
        PendingIntent stopIntent = PendingIntent.getService(
            this,
            2,
            new Intent(this, RelayService.class).setAction(ACTION_STOP),
            PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE
        );

        Notification.Builder builder = Build.VERSION.SDK_INT >= 26
            ? new Notification.Builder(this, CHANNEL_ID)
            : new Notification.Builder(this);
        String version = preferences == null
            ? "1.21.100"
            : preferences.getString(KEY_VERSION, "1.21.100");
        builder.setSmallIcon(R.drawable.ic_relay)
            .setContentTitle("CPE Relay " + version)
            .setContentText(notificationStatus)
            .setStyle(new Notification.BigTextStyle().bigText(notificationStatus))
            .setContentIntent(contentIntent)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setCategory(Notification.CATEGORY_SERVICE)
            .addAction(new Notification.Action.Builder(
                0,
                "Остановить",
                stopIntent
            ).build());

        String code = preferences == null
            ? ""
            : preferences.getString(KEY_AUTH_CODE, "");
        String uri = preferences == null
            ? ""
            : preferences.getString(KEY_AUTH_URI, "");
        if (!code.isEmpty() && !uri.isEmpty()) {
            PendingIntent browserIntent = PendingIntent.getActivity(
                this,
                3,
                new Intent(Intent.ACTION_VIEW, Uri.parse(uri)),
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE
            );
            builder.addAction(new Notification.Action.Builder(
                0,
                "Открыть вход (" + code + ")",
                browserIntent
            ).build());
        }
        return builder.build();
    }

    private void promoteToForeground(Notification notification) {
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE
            );
        } else {
            startForeground(NOTIFICATION_ID, notification);
        }
    }

    private void refreshNotification() {
        NotificationManager manager =
            (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        manager.notify(NOTIFICATION_ID, buildNotification());
    }

    private void acquireWakeLock() {
        if (wakeLock != null && wakeLock.isHeld()) {
            return;
        }
        PowerManager manager = (PowerManager) getSystemService(POWER_SERVICE);
        wakeLock = manager.newWakeLock(
            PowerManager.PARTIAL_WAKE_LOCK,
            "CpeRelay:NetworkRelay"
        );
        wakeLock.setReferenceCounted(false);
        wakeLock.acquire();
        DiagnosticsLog.append(this, "DEBUG", "power", "Partial wake lock acquired");
    }

    private void releaseWakeLock() {
        if (wakeLock != null && wakeLock.isHeld()) {
            wakeLock.release();
            DiagnosticsLog.append(this, "DEBUG", "power", "Partial wake lock released");
        }
        wakeLock = null;
    }
}
