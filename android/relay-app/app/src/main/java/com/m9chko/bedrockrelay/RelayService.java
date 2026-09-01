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
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemClock;

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
    public static final String KEY_DETAILED_LOGS = "detailed_logs";
    public static final String KEY_CHUNK_RETENTION = "chunk_retention";
    public static final String KEY_RETAINED_RADIUS_CHUNKS =
        "retained_radius_chunks";
    public static final String KEY_ENTITY_OUTLINES = "entity_outlines";
    public static final String KEY_ENTITY_FOV = "entity_outline_fov";
    public static final String KEY_ENTITY_PLAYERS = "entity_outline_players";
    public static final String KEY_ENTITY_MOBS = "entity_outline_mobs";
    public static final String KEY_ENTITY_ITEMS = "entity_outline_items";
    public static final String KEY_PLAYER_COLOR = "entity_player_color";
    public static final String KEY_MOB_COLOR = "entity_mob_color";
    public static final String KEY_ITEM_COLOR = "entity_item_color";
    public static final String KEY_OUTLINE_THICKNESS_TENTHS =
        "entity_outline_thickness_tenths";
    public static final String KEY_ENTITY_MAX_DISTANCE =
        "entity_outline_max_distance";
    public static final String KEY_CHUNK_WIDGET = "chunk_widget";
    public static final String KEY_CHUNK_WIDGET_SCALE = "chunk_widget_scale";
    public static final String KEY_CHUNK_WIDGET_X = "chunk_widget_x";
    public static final String KEY_CHUNK_WIDGET_Y = "chunk_widget_y";
    public static final String KEY_CHUNK_WIDGET_MINIMIZED =
        "chunk_widget_minimized";
    public static final String KEY_EQUIPMENT_HUD = "equipment_hud";
    public static final String KEY_EQUIPMENT_HUD_SCALE = "equipment_hud_scale";

    public static final int DEFAULT_PLAYER_COLOR = 0xff4fd5ff;
    public static final int DEFAULT_MOB_COLOR = 0xffff5b62;
    public static final int DEFAULT_ITEM_COLOR = 0xffffcf4a;

    public static final int MIN_RETAINED_RADIUS_CHUNKS = 10;
    public static final int MAX_RETAINED_RADIUS_CHUNKS = 64;
    public static final int MIN_ENTITY_FOV = 50;
    public static final int MAX_ENTITY_FOV = 110;

    public static final String ACTION_START =
        "com.m9chko.bedrockrelay.action.START";
    public static final String ACTION_STOP =
        "com.m9chko.bedrockrelay.action.STOP";
    public static final String EXTRA_HOST = "host";
    public static final String EXTRA_PORT = "port";
    public static final String EXTRA_VERSION = "version";

    private static final String CHANNEL_ID = "bedrock_relay";
    private static final int NOTIFICATION_ID = 19132;
    private static final int OVERLAY_POLL_INTERVAL_MS = 12;
    private static final int ENTITY_SNAPSHOT_EVERY_POLLS = 4;

    private final ExecutorService commandExecutor =
        Executors.newSingleThreadExecutor();
    private final ScheduledExecutorService pollExecutor =
        Executors.newSingleThreadScheduledExecutor();
    private final ScheduledExecutorService entityPollExecutor =
        Executors.newSingleThreadScheduledExecutor();
    private final AtomicBoolean pollingStarted = new AtomicBoolean(false);
    private final AtomicBoolean entityPollingStarted = new AtomicBoolean(false);
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    private SharedPreferences preferences;
    private PowerManager.WakeLock wakeLock;
    private RelayOverlayController overlayController;
    private EntityOutlineOverlayController entityOverlayController;
    private ChunkStatusOverlayController chunkOverlayController;
    private EquipmentOverlayController equipmentOverlayController;
    private volatile boolean serviceStopping;
    private volatile boolean overlayShouldBeVisible;
    private volatile boolean overlaySessionReady;
    private volatile long nextOverlayShowRetryAtMs;
    private volatile String notificationStatus = "Запуск UDP relay…";
    private volatile String lastSnapshotFingerprint = "";
    private volatile long lastPollingErrorAt;
    private volatile long lastEntityPollingErrorAt;
    private int entityPollTick;

    @Override
    public void onCreate() {
        super.onCreate();
        preferences = getSharedPreferences(PREFERENCES, MODE_PRIVATE);
        overlayController = new RelayOverlayController(
            this,
            preferences,
            () -> applyRuntimeOptions(true)
        );
        entityOverlayController = new EntityOutlineOverlayController(this);
        chunkOverlayController = new ChunkStatusOverlayController(
            this,
            preferences
        );
        equipmentOverlayController = new EquipmentOverlayController(this);
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
        overlaySessionReady = false;
        setOverlayVisible(false);
        applyRuntimeOptions(false);
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
        overlaySessionReady = false;
        overlayShouldBeVisible = false;
        if (overlayController != null) overlayController.hide();
        if (entityOverlayController != null) {
            entityOverlayController.hideImmediately();
        }
        if (chunkOverlayController != null) {
            chunkOverlayController.hideImmediately();
        }
        if (equipmentOverlayController != null) {
            equipmentOverlayController.destroy();
        }
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
        entityPollExecutor.shutdownNow();
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
        startEntityPolling();
    }

    private void startEntityPolling() {
        if (!entityPollingStarted.compareAndSet(false, true)) return;
        entityPollExecutor.scheduleWithFixedDelay(() -> {
            if (serviceStopping || entityOverlayController == null ||
                !entityOverlayController.wantsFrames()) {
                return;
            }
            try {
                if ((entityPollTick++ % ENTITY_SNAPSHOT_EVERY_POLLS) == 0) {
                    entityOverlayController.offerSnapshot(
                        NativeBridge.entityOverlaySnapshot()
                    );
                } else {
                    entityOverlayController.offerCameraSnapshot(
                        NativeBridge.entityCameraSnapshot()
                    );
                }
            } catch (Throwable error) {
                long now = System.currentTimeMillis();
                if (now - lastEntityPollingErrorAt >= 10_000) {
                    lastEntityPollingErrorAt = now;
                    DiagnosticsLog.appendError(
                        this,
                        "entities",
                        "Entity snapshot polling failed; overlay continues",
                        error
                    );
                }
            }
        }, 0, OVERLAY_POLL_INTERVAL_MS, TimeUnit.MILLISECONDS);
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
            overlaySessionReady = false;
            setOverlayVisible(false);
            reportError(event.optString("message", "Неизвестная ошибка relay"));
            return;
        }

        String message = event.optString("message", "");
        String level = event.optString(
            "level",
            "ping_warning".equals(type) ? "WARN" : "INFO"
        );
        if ("DEBUG".equalsIgnoreCase(level) && !detailedLogsEnabled()) {
            return;
        }
        String component = event.optString("component", "native");
        DiagnosticsLog.appendAt(
            this,
            level,
            component,
            "event=" + type + (message.isEmpty() ? "" : " " + message),
            event.optLong("timestampMs", 0)
        );
        boolean sessionEnded = "disconnect".equals(type) ||
            "local_login_timeout".equals(type) ||
            "transport_error".equals(type) ||
            "parse_error".equals(type);
        if (sessionEnded) {
            overlaySessionReady = false;
            logMemorySnapshot(type);
            setOverlayVisible(false);
        }
        if ("upstream_ready".equals(type)) {
            overlaySessionReady = true;
            preferences.edit()
                .remove(KEY_AUTH_CODE)
                .remove(KEY_AUTH_URI)
                .apply();
            notificationStatus = "Relay подключён к серверу";
            refreshNotification();
            setOverlayVisible(true);
        }
    }

    private void logMemorySnapshot(String trigger) {
        if (!detailedLogsEnabled()) return;
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
        boolean running = state.optBoolean("running", false);
        boolean upstreamReady = state.optBoolean("upstreamReady", false);
        int downstreamConnections = state.optInt("downstreamConnections", 0);
        // The snapshot is authoritative and also repairs a missed/racing
        // upstream_ready callback. A later healthy poll must be able to show
        // the menu again after one transient incomplete snapshot.
        overlaySessionReady = running && upstreamReady &&
            downstreamConnections > 0;
        setOverlayVisible(overlaySessionReady);
        updateOverlayChunkStatus(state);
        updateOverlayEquipment(state);
        if (!running) {
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
            " upstreamReady=" + state.optInt("upstreamReadyCount", 0) +
            " detailedLogs=" + state.optBoolean("detailedLogging", true) +
            " chunkRetention=" +
                state.optBoolean("chunkRetentionEnabled", false) +
            " retainedRadiusChunks=" +
                state.optInt("retainedRadiusChunks", 24) +
            " entityOutlines=" + preferences.getBoolean(
                KEY_ENTITY_OUTLINES,
                true
            ) +
            " entityFov=" + clampEntityFov(preferences.getInt(
                KEY_ENTITY_FOV,
                70
            ));
        if (!fingerprint.equals(lastSnapshotFingerprint)) {
            lastSnapshotFingerprint = fingerprint;
            if (detailedLogsEnabled()) {
                DiagnosticsLog.append(this, "DEBUG", "snapshot", fingerprint);
            }
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

    private void updateOverlayChunkStatus(JSONObject state) {
        if (overlayController == null && chunkOverlayController == null) return;
        final boolean retentionEnabled = state.optBoolean(
            "chunkRetentionEnabled",
            false
        );
        final int configuredRadiusChunks = state.optInt(
            "retainedRadiusChunks",
            24
        );
        final long publisherUpdates = state.optLong(
            "chunkPublisherPacketsObserved",
            0
        );
        final long publisherRewrites = state.optLong(
            "chunkPublisherPacketsRewritten",
            0
        );
        final int serverRadiusBlocks = state.optInt(
            "lastServerPublisherRadiusBlocks",
            0
        );
        final int effectiveRadiusBlocks = state.optInt(
            "lastEffectivePublisherRadiusBlocks",
            0
        );
        final long retainedChunks = state.optLong(
            "retainedLevelChunkCount",
            0
        );
        final long retainedBytes = state.optLong(
            "retainedLevelChunkBytes",
            0
        );
        final long maximumBytes = state.optLong(
            "retainedLevelChunkMaximumBytes",
            256L * 1024L * 1024L
        );
        final long evictedRadius = state.optLong(
            "retainedLevelChunksEvictedRadius",
            0
        );
        final long evictedMemory = state.optLong(
            "retainedLevelChunksEvictedMemory",
            0
        );
        final long parseFailures = state.optLong(
            "retainedLevelChunkParseFailures",
            0
        ) + state.optLong("chunkPublisherDecodeFailures", 0);
        mainHandler.post(() -> {
            if (overlayController != null) {
                overlayController.updateChunkStatus(
                    retentionEnabled,
                    configuredRadiusChunks,
                    publisherUpdates,
                    publisherRewrites,
                    serverRadiusBlocks,
                    effectiveRadiusBlocks,
                    retainedChunks,
                    retainedBytes,
                    maximumBytes,
                    evictedRadius,
                    evictedMemory,
                    parseFailures
                );
            }
            if (chunkOverlayController != null) {
                chunkOverlayController.updateStatus(
                    retentionEnabled,
                    configuredRadiusChunks,
                    publisherUpdates,
                    publisherRewrites,
                    serverRadiusBlocks,
                    effectiveRadiusBlocks,
                    retainedChunks,
                    retainedBytes,
                    maximumBytes,
                    evictedRadius,
                    evictedMemory,
                    parseFailures
                );
            }
        });
    }

    private void updateOverlayEquipment(JSONObject state) {
        if (equipmentOverlayController == null) return;
        final JSONArray equipment = state.optJSONArray("equipment");
        final long revision = state.optLong("equipmentRevision", 0);
        mainHandler.post(() -> equipmentOverlayController.update(
            equipment,
            revision
        ));
    }

    private void stopRelayAndSelf() {
        if (serviceStopping) {
            return;
        }
        serviceStopping = true;
        overlaySessionReady = false;
        setOverlayVisible(false);
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

    private boolean detailedLogsEnabled() {
        return preferences.getBoolean(KEY_DETAILED_LOGS, true);
    }

    private void applyRuntimeOptions(boolean logChange) {
        boolean detailedLogs = detailedLogsEnabled();
        boolean retainChunks = preferences.getBoolean(
            KEY_CHUNK_RETENTION,
            false
        );
        int radiusChunks = clampRetainedRadius(preferences.getInt(
            KEY_RETAINED_RADIUS_CHUNKS,
            24
        ));
        boolean entityOutlines = preferences.getBoolean(
            KEY_ENTITY_OUTLINES,
            true
        );
        int entityFov = clampEntityFov(preferences.getInt(
            KEY_ENTITY_FOV,
            70
        ));
        boolean showPlayers = preferences.getBoolean(KEY_ENTITY_PLAYERS, true);
        boolean showMobs = preferences.getBoolean(KEY_ENTITY_MOBS, true);
        boolean showItems = preferences.getBoolean(KEY_ENTITY_ITEMS, true);
        int playerColor = preferences.getInt(
            KEY_PLAYER_COLOR,
            DEFAULT_PLAYER_COLOR
        );
        int mobColor = preferences.getInt(KEY_MOB_COLOR, DEFAULT_MOB_COLOR);
        int itemColor = preferences.getInt(KEY_ITEM_COLOR, DEFAULT_ITEM_COLOR);
        int thicknessTenths = clampOutlineThicknessTenths(preferences.getInt(
            KEY_OUTLINE_THICKNESS_TENTHS,
            17
        ));
        int maximumDistance = clampEntityDistance(preferences.getInt(
            KEY_ENTITY_MAX_DISTANCE,
            128
        ));
        boolean chunkWidget = preferences.getBoolean(KEY_CHUNK_WIDGET, true);
        int chunkWidgetScale = clampOverlayScale(preferences.getInt(
            KEY_CHUNK_WIDGET_SCALE,
            85
        ));
        boolean equipmentHud = preferences.getBoolean(KEY_EQUIPMENT_HUD, true);
        int equipmentScale = clampOverlayScale(preferences.getInt(
            KEY_EQUIPMENT_HUD_SCALE,
            80
        ));
        preferences.edit()
            .putInt(KEY_RETAINED_RADIUS_CHUNKS, radiusChunks)
            .putInt(KEY_ENTITY_FOV, entityFov)
            .putInt(KEY_OUTLINE_THICKNESS_TENTHS, thicknessTenths)
            .putInt(KEY_ENTITY_MAX_DISTANCE, maximumDistance)
            .putInt(KEY_CHUNK_WIDGET_SCALE, chunkWidgetScale)
            .putInt(KEY_EQUIPMENT_HUD_SCALE, equipmentScale)
            .apply();
        mainHandler.post(() -> {
            if (entityOverlayController != null) {
                entityOverlayController.setFieldOfView(entityFov);
                entityOverlayController.setDisplayOptions(
                    showPlayers,
                    showMobs,
                    showItems,
                    playerColor,
                    mobColor,
                    itemColor,
                    thicknessTenths / 10.0f,
                    maximumDistance
                );
                entityOverlayController.setEnabled(entityOutlines);
            }
            if (chunkOverlayController != null) {
                chunkOverlayController.configure(
                    retainChunks && chunkWidget,
                    chunkWidgetScale
                );
            }
            if (equipmentOverlayController != null) {
                equipmentOverlayController.configure(
                    equipmentHud,
                    equipmentScale
                );
            }
        });
        try {
            NativeBridge.configureRuntime(
                detailedLogs,
                retainChunks,
                radiusChunks
            );
            if (logChange) {
                DiagnosticsLog.append(
                    this,
                    "INFO",
                    "settings",
                    "Runtime settings changed: detailedLogs=" + detailedLogs +
                        " chunkRetention=" + retainChunks +
                        " retainedRadiusChunks=" + radiusChunks +
                        " entityOutlines=" + entityOutlines +
                        " entityFov=" + entityFov +
                        " players=" + showPlayers +
                        " mobs=" + showMobs +
                        " items=" + showItems +
                        " thickness=" + (thicknessTenths / 10.0f) +
                        " maxDistance=" + maximumDistance +
                        " chunkWidget=" + chunkWidget +
                        " equipmentHud=" + equipmentHud
                );
            }
        } catch (Throwable error) {
            DiagnosticsLog.appendError(
                this,
                "settings",
                "Failed to apply runtime relay settings",
                error
            );
        }
    }

    private void setOverlayVisible(boolean visible) {
        boolean changed = overlayShouldBeVisible != visible;
        overlayShouldBeVisible = visible;
        long now = SystemClock.elapsedRealtime();
        if (!changed) {
            if (!visible) return;
            RelayOverlayController controller = overlayController;
            if (controller != null && controller.isShowing()) return;
            // addView may fail during the activity/game transition. Retry at
            // a bounded rate instead of permanently trusting the desired bit.
            if (now < nextOverlayShowRetryAtMs) return;
        }
        nextOverlayShowRetryAtMs = visible ? now + 1_000L : 0L;
        mainHandler.post(() -> {
            if (overlayShouldBeVisible) {
                if (entityOverlayController != null) {
                    entityOverlayController.setSessionVisible(true);
                }
                if (chunkOverlayController != null) {
                    chunkOverlayController.setSessionVisible(true);
                }
                if (equipmentOverlayController != null) {
                    equipmentOverlayController.setSessionVisible(true);
                }
                if (overlayController != null) overlayController.show();
            } else {
                if (overlayController != null) overlayController.hide();
                if (entityOverlayController != null) {
                    entityOverlayController.setSessionVisible(false);
                }
                if (chunkOverlayController != null) {
                    chunkOverlayController.setSessionVisible(false);
                }
                if (equipmentOverlayController != null) {
                    equipmentOverlayController.setSessionVisible(false);
                }
            }
        });
    }

    public static int clampRetainedRadius(int radius) {
        return Math.max(
            MIN_RETAINED_RADIUS_CHUNKS,
            Math.min(MAX_RETAINED_RADIUS_CHUNKS, radius)
        );
    }

    public static int clampEntityFov(int fov) {
        return Math.max(MIN_ENTITY_FOV, Math.min(MAX_ENTITY_FOV, fov));
    }

    public static int clampOutlineThicknessTenths(int value) {
        return Math.max(8, Math.min(60, value));
    }

    public static int clampEntityDistance(int value) {
        return Math.max(16, Math.min(256, value));
    }

    public static int clampOverlayScale(int value) {
        return Math.max(70, Math.min(150, value));
    }
}
