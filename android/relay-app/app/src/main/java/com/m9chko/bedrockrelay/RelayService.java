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
import android.view.WindowInsets;
import android.view.WindowManager;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import com.m9chko.bedrockrelay.schematic.SchematicModel;
import com.m9chko.bedrockrelay.schematic.SchematicRepository;

import java.io.File;
import java.io.InputStream;
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
    public static final String KEY_EQUIPMENT_HUD_X = "equipment_hud_x";
    public static final String KEY_EQUIPMENT_HUD_Y = "equipment_hud_y";
    public static final String KEY_MINIMAP = "mini_map";
    public static final String KEY_MINIMAP_RADIUS = "mini_map_radius_chunks";
    public static final String KEY_MINIMAP_SCALE = "mini_map_scale";
    public static final String KEY_MINIMAP_ROUND = "mini_map_round";
    public static final String KEY_MINIMAP_X = "mini_map_x";
    public static final String KEY_MINIMAP_Y = "mini_map_y";
    public static final String KEY_AUTO_ARMOR = "auto_armor";
    public static final String KEY_AUTO_TOTEM = "auto_totem";
    public static final String KEY_AREA_FILL_ENABLED = "area_fill_enabled";
    public static final String KEY_AREA_FILL_POINTS = "area_fill_points";
    public static final String KEY_AREA_FILL_BUTTON_SCALE =
        "area_fill_button_scale";
    public static final String KEY_AREA_FILL_BUTTON_X = "area_fill_button_x";
    public static final String KEY_AREA_FILL_BUTTON_Y = "area_fill_button_y";
    public static final String KEY_THREAT_ANALYSIS = "threat_analysis";
    public static final String KEY_THREAT_WARNING = "threat_warning";
    public static final String KEY_THREAT_DISTANCE = "threat_distance";
    public static final String KEY_THREAT_WARNING_SCALE =
        "threat_warning_scale";
    public static final String KEY_THREAT_COLOR = "threat_color";
    public static final String KEY_THREAT_WARNING_X = "threat_warning_x";
    public static final String KEY_THREAT_WARNING_Y = "threat_warning_y";
    public static final String KEY_SCHEMATIC_ENABLED = "schematic_enabled";
    public static final String KEY_SCHEMATIC_TEXTURES = "schematic_textures";
    public static final String KEY_SCHEMATIC_OPACITY = "schematic_opacity";
    public static final String KEY_SCHEMATIC_OUTLINES = "schematic_outlines";
    public static final String KEY_SCHEMATIC_OUTLINE_OPACITY =
        "schematic_outline_opacity";
    public static final String KEY_SCHEMATIC_CORRECT_COLOR =
        "schematic_correct_color";
    public static final String KEY_SCHEMATIC_WRONG_COLOR =
        "schematic_wrong_color";
    public static final String KEY_SCHEMATIC_MISSING_COLOR =
        "schematic_missing_color";
    public static final String KEY_SCHEMATIC_DISTANCE = "schematic_distance";
    public static final String KEY_SCHEMATIC_ROTATION = "schematic_rotation";
    public static final String KEY_SCHEMATIC_MIRROR = "schematic_mirror";
    public static final String KEY_SCHEMATIC_LAYER = "schematic_layer";
    public static final String KEY_SCHEMATIC_PLACED = "schematic_placed";
    public static final String KEY_SCHEMATIC_PLACE_REQUEST =
        "schematic_place_request";
    public static final String KEY_SCHEMATIC_PLACE_REQUEST_HANDLED =
        "schematic_place_request_handled";
    public static final String KEY_SCHEMATIC_ANCHOR_X = "schematic_anchor_x";
    public static final String KEY_SCHEMATIC_ANCHOR_Y = "schematic_anchor_y";
    public static final String KEY_SCHEMATIC_ANCHOR_Y_EXACT =
        "schematic_anchor_y_exact";
    public static final String KEY_SCHEMATIC_ANCHOR_Z = "schematic_anchor_z";
    public static final String KEY_SCHEMATIC_TOTAL = "schematic_total";
    public static final String KEY_SCHEMATIC_CORRECT = "schematic_correct";
    public static final String KEY_SCHEMATIC_MISSING = "schematic_missing";
    public static final String KEY_SCHEMATIC_WRONG = "schematic_wrong";
    public static final String KEY_SCHEMATIC_UNKNOWN = "schematic_unknown";
    public static final String KEY_SCHEMATIC_DISPLAYED = "schematic_displayed";

    public static final int DEFAULT_PLAYER_COLOR = 0xff4fd5ff;
    public static final int DEFAULT_MOB_COLOR = 0xffff5b62;
    public static final int DEFAULT_ITEM_COLOR = 0xffffcf4a;
    public static final int DEFAULT_THREAT_COLOR = 0xffff3b30;
    public static final int DEFAULT_SCHEMATIC_CORRECT_COLOR = 0xff5df0a2;
    public static final int DEFAULT_SCHEMATIC_WRONG_COLOR = 0xffff5b62;
    public static final int DEFAULT_SCHEMATIC_MISSING_COLOR = 0xffffcf4a;

    public static final int MIN_RETAINED_RADIUS_CHUNKS = 10;
    public static final int MAX_RETAINED_RADIUS_CHUNKS = 64;
    public static final int MIN_ENTITY_FOV = 50;
    public static final int MAX_ENTITY_FOV = 110;
    public static final int MIN_MINIMAP_RADIUS_CHUNKS = 2;
    public static final int MAX_MINIMAP_RADIUS_CHUNKS = 10;
    public static final int MIN_THREAT_DISTANCE = 3;
    public static final int MAX_THREAT_DISTANCE = 32;
    public static final int MIN_AREA_FILL_POINTS = 2;
    public static final int MAX_AREA_FILL_POINTS = 8;

    public static final String ACTION_START =
        "com.m9chko.bedrockrelay.action.START";
    public static final String ACTION_STOP =
        "com.m9chko.bedrockrelay.action.STOP";
    public static final String ACTION_RELOAD_SCHEMATIC =
        "com.m9chko.bedrockrelay.action.RELOAD_SCHEMATIC";
    public static final String ACTION_IMPORT_SCHEMATIC_DOCUMENT =
        "com.m9chko.bedrockrelay.action.IMPORT_SCHEMATIC_DOCUMENT";
    public static final String EXTRA_SCHEMATIC_URI = "schematic_uri";
    public static final String EXTRA_SCHEMATIC_NAME = "schematic_name";
    public static final String EXTRA_HOST = "host";
    public static final String EXTRA_PORT = "port";
    public static final String EXTRA_VERSION = "version";

    private static final String CHANNEL_ID = "bedrock_relay";
    private static final int NOTIFICATION_ID = 19132;
    private static final int OVERLAY_POLL_INTERVAL_MS = 12;
    private static final int ENTITY_SNAPSHOT_EVERY_POLLS = 4;

    private final ExecutorService commandExecutor =
        Executors.newSingleThreadExecutor();
    private final ExecutorService schematicExecutor =
        Executors.newSingleThreadExecutor();
    private final ScheduledExecutorService pollExecutor =
        Executors.newSingleThreadScheduledExecutor();
    private final ScheduledExecutorService entityPollExecutor =
        Executors.newSingleThreadScheduledExecutor();
    private final AtomicBoolean pollingStarted = new AtomicBoolean(false);
    private final AtomicBoolean entityPollingStarted = new AtomicBoolean(false);
    private final AtomicBoolean schematicSnapshotInFlight =
        new AtomicBoolean(false);
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    private SharedPreferences preferences;
    private PowerManager.WakeLock wakeLock;
    private RelayOverlayController overlayController;
    private EntityOutlineOverlayController entityOverlayController;
    private ChunkStatusOverlayController chunkOverlayController;
    private EquipmentOverlayController equipmentOverlayController;
    private MiniMapOverlayController miniMapOverlayController;
    private ThreatAnalysisOverlayController threatOverlayController;
    private SchematicOverlayController schematicOverlayController;
    private AreaFillOverlayController areaFillOverlayController;
    private SchematicRepository schematicRepository;
    private volatile boolean serviceStopping;
    private volatile boolean overlayShouldBeVisible;
    private volatile boolean overlayWindowsVisible;
    private volatile boolean minecraftUiBlocked;
    private volatile boolean overlaySessionReady;
    private volatile long nextOverlayShowRetryAtMs;
    private volatile String notificationStatus = "Запуск UDP relay…";
    private volatile String lastSnapshotFingerprint = "";
    private volatile long lastPollingErrorAt;
    private volatile long lastEntityPollingErrorAt;
    private volatile long lastSchematicSnapshotErrorAt;
    private int entityPollTick;
    private int uiPollTick;

    @Override
    public void onCreate() {
        super.onCreate();
        preferences = getSharedPreferences(PREFERENCES, MODE_PRIVATE);
        overlayController = new RelayOverlayController(
            this,
            preferences,
            () -> applyRuntimeOptions(true),
            (dx, dy, dz) -> {
                Runnable shiftAndRefresh = () -> {
                    SchematicOverlayController controller =
                        schematicOverlayController;
                    if (controller == null) return;
                    controller.shiftAnchor(dx, dy, dz);
                    scheduleSchematicWorldSnapshot(controller);
                };
                if (Looper.myLooper() == Looper.getMainLooper()) {
                    shiftAndRefresh.run();
                } else {
                    mainHandler.post(shiftAndRefresh);
                }
            }
        );
        entityOverlayController = new EntityOutlineOverlayController(this);
        chunkOverlayController = new ChunkStatusOverlayController(
            this,
            preferences
        );
        equipmentOverlayController = new EquipmentOverlayController(this);
        miniMapOverlayController = new MiniMapOverlayController(
            this,
            preferences
        );
        threatOverlayController = new ThreatAnalysisOverlayController(
            this,
            preferences,
            entityOverlayController
        );
        schematicRepository = new SchematicRepository(this);
        schematicOverlayController = new SchematicOverlayController(
            this,
            preferences
        );
        areaFillOverlayController = new AreaFillOverlayController(
            this,
            preferences
        );
        reloadSchematicModel();
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
        if (ACTION_RELOAD_SCHEMATIC.equals(action)) {
            boolean relayRunning = false;
            try {
                relayRunning = new JSONObject(NativeBridge.snapshot())
                    .optBoolean("running", false);
            } catch (Throwable ignored) {
            }
            if (relayRunning) {
                reloadSchematicModel();
            } else {
                stopSelf(startId);
            }
            return START_NOT_STICKY;
        }
        if (ACTION_IMPORT_SCHEMATIC_DOCUMENT.equals(action)) {
            boolean relayRunning = false;
            try {
                relayRunning = new JSONObject(NativeBridge.snapshot())
                    .optBoolean("running", false);
            } catch (Throwable ignored) {
            }
            if (relayRunning) {
                importSchematicDocument(
                    intent.getStringExtra(EXTRA_SCHEMATIC_URI),
                    intent.getStringExtra(EXTRA_SCHEMATIC_NAME)
                );
            } else {
                stopSelf(startId);
            }
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
        if (miniMapOverlayController != null) {
            miniMapOverlayController.destroy();
        }
        if (threatOverlayController != null) {
            threatOverlayController.destroy();
        }
        if (schematicOverlayController != null) {
            schematicOverlayController.hideImmediately();
        }
        if (areaFillOverlayController != null) {
            areaFillOverlayController.destroy();
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
        schematicExecutor.shutdownNow();
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

        String minecraftDataDirectory = "";
        try {
            minecraftDataDirectory = MinecraftDataAssets.prepareNativeDirectory(
                this,
                version
            );
        } catch (Throwable error) {
            DiagnosticsLog.appendError(
                this,
                "world",
                "Packaged minecraft-data could not be prepared; " +
                    "semantic map colors and collision placement may degrade",
                error
            );
        }

        try {
            JSONObject result = new JSONObject(NativeBridge.startRelay(
                host,
                port,
                version,
                cache.getAbsolutePath(),
                minecraftDataDirectory
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
            if (serviceStopping) {
                return;
            }
            try {
                final int pollTick = uiPollTick++;
                if ((pollTick & 3) == 0) {
                    updateMinecraftUiBlocked(
                        NativeBridge.minecraftUiBlocked() || isImeVisible()
                    );
                }
                boolean outlineFrames = entityOverlayController != null &&
                    entityOverlayController.wantsFrames();
                boolean threatFrames = threatOverlayController != null &&
                    threatOverlayController.wantsFrames();
                boolean schematicFrames = schematicOverlayController != null &&
                    schematicOverlayController.wantsFrames();
                boolean miniMapFrames = miniMapOverlayController != null &&
                    miniMapOverlayController.wantsFrames();
                if (!outlineFrames && !threatFrames && !schematicFrames &&
                    !miniMapFrames) return;

                // Deliver the latency-sensitive packet camera first. Minimap
                // copies and schematic/world matching must never hold up this
                // path and reintroduce visible projection jitter.
                if (outlineFrames || threatFrames || schematicFrames) {
                    boolean entityFrames = outlineFrames || threatFrames;
                    if (entityFrames &&
                        ((entityPollTick++ % ENTITY_SNAPSHOT_EVERY_POLLS) == 0 ||
                            !outlineFrames)) {
                        EntityOutlineOverlayController.Frame frame =
                            EntityOutlineOverlayController.Frame.parse(
                                NativeBridge.entityOverlaySnapshot()
                            );
                        if (outlineFrames) {
                            entityOverlayController.offerFrame(frame);
                        }
                        if (threatFrames) {
                            threatOverlayController.offerFrame(frame);
                        }
                        if (schematicFrames) {
                            schematicOverlayController.offerCamera(frame.camera);
                        }
                    } else {
                        String cameraJson = NativeBridge.entityCameraSnapshot();
                        if (outlineFrames) {
                            entityOverlayController.offerCameraSnapshot(cameraJson);
                        }
                        if (schematicFrames) {
                            schematicOverlayController.offerCameraSnapshot(
                                cameraJson
                            );
                        }
                    }
                }
                if (miniMapFrames && (pollTick & 7) == 0) {
                    miniMapOverlayController.offerSnapshot(
                        NativeBridge.miniMapSnapshot(
                            miniMapOverlayController.requestedRevision(),
                            miniMapOverlayController.requestedRadiusChunks()
                        )
                    );
                }
                if (schematicFrames && (pollTick & 7) == 0) {
                    scheduleSchematicWorldSnapshot(schematicOverlayController);
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

    private void scheduleSchematicWorldSnapshot(
        SchematicOverlayController controller
    ) {
        if (!schematicSnapshotInFlight.compareAndSet(false, true)) return;
        try {
            schematicExecutor.execute(() -> {
                try {
                    if (!serviceStopping && schematicOverlayController == controller &&
                        controller.wantsFrames()) {
                        controller.pollWorldSnapshot();
                    }
                } catch (Throwable error) {
                    long now = System.currentTimeMillis();
                    if (now - lastSchematicSnapshotErrorAt >= 10_000) {
                        lastSchematicSnapshotErrorAt = now;
                        DiagnosticsLog.appendError(
                            this,
                            "schematics",
                            "Schematic world matching failed; overlay continues",
                            error
                        );
                    }
                } finally {
                    schematicSnapshotInFlight.set(false);
                }
            });
        } catch (Throwable error) {
            schematicSnapshotInFlight.set(false);
            if (!serviceStopping) throw error;
        }
    }

    private void reloadSchematicModel() {
        final SchematicRepository repository = schematicRepository;
        if (repository == null) return;
        schematicExecutor.execute(() -> {
            try {
                SchematicModel loaded = repository.loadActive();
                mainHandler.post(() -> {
                    if (schematicOverlayController != null) {
                        schematicOverlayController.setModel(loaded);
                    }
                    applyRuntimeOptions(false);
                });
                DiagnosticsLog.append(
                    this,
                    "INFO",
                    "schematics",
                    loaded == null
                        ? "No active schematic"
                        : "Loaded schematic " + loaded.sourceName() +
                            " " + loaded.description()
                );
            } catch (Throwable error) {
                DiagnosticsLog.appendError(
                    this,
                    "schematics",
                    "Failed to load active schematic",
                    error
                );
                mainHandler.post(() -> {
                    if (schematicOverlayController != null) {
                        schematicOverlayController.setModel(null);
                    }
                });
            }
        });
    }

    private void importSchematicDocument(String uriValue, String sourceName) {
        if (uriValue == null || uriValue.trim().isEmpty()) {
            if (overlayController != null) {
                overlayController.updateSchematicImportStatus(
                    "Не выбран файл схемы",
                    true
                );
            }
            return;
        }
        String safeName = sourceName == null || sourceName.trim().isEmpty()
            ? "scheme.nbt"
            : sourceName.trim();
        if (overlayController != null) {
            overlayController.updateSchematicImportStatus(
                "Импортируем " + safeName + "…",
                false
            );
        }
        final SchematicRepository repository = schematicRepository;
        schematicExecutor.execute(() -> {
            try (InputStream input = getContentResolver().openInputStream(
                Uri.parse(uriValue)
            )) {
                if (input == null) throw new IllegalStateException(
                    "Файл схемы не открылся"
                );
                SchematicRepository.ImportResult result =
                    repository.importAndActivate(input, safeName);
                preferences.edit()
                    .putBoolean(KEY_SCHEMATIC_ENABLED, true)
                    .putBoolean(KEY_SCHEMATIC_PLACED, false)
                    .apply();
                DiagnosticsLog.append(
                    this,
                    "INFO",
                    "schematics",
                    "Imported from in-game folder: " +
                        result.model.sourceName() + " " +
                        result.model.description()
                );
                mainHandler.post(() -> {
                    if (schematicOverlayController != null) {
                        schematicOverlayController.setModel(result.model);
                    }
                    applyRuntimeOptions(false);
                    if (overlayController != null) {
                        overlayController.updateSchematicImportStatus(
                            "Готово: " + result.model.description(),
                            false
                        );
                    }
                    Toast.makeText(
                        this,
                        "Схема импортирована",
                        Toast.LENGTH_SHORT
                    ).show();
                });
            } catch (Throwable error) {
                DiagnosticsLog.appendError(
                    this,
                    "schematics",
                    "In-game schematic import failed",
                    error
                );
                String message = error.getMessage();
                String visible = message == null || message.trim().isEmpty()
                    ? "Не удалось импортировать схему"
                    : message;
                mainHandler.post(() -> {
                    if (overlayController != null) {
                        overlayController.updateSchematicImportStatus(
                            visible,
                            true
                        );
                    }
                    Toast.makeText(this, visible, Toast.LENGTH_LONG).show();
                });
            }
        });
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
        if ("local_login_timeout".equals(type)) {
            notificationStatus =
                "Вход Minecraft завис — подключитесь к relay ещё раз";
            refreshNotification();
            mainHandler.post(() -> Toast.makeText(
                this,
                "Minecraft не завершил вход в relay. Подключитесь ещё раз.",
                Toast.LENGTH_LONG
            ).show());
        }
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
        updateOverlayGameplayStatus(state);
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
            48L * 1024L * 1024L
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

    private void updateOverlayGameplayStatus(JSONObject state) {
        if (threatOverlayController != null) {
            threatOverlayController.updatePlayerState(state);
        }
        if (overlayController == null) return;
        final String automationStatus = state.optString(
            "automationStatus",
            "Ожидание инвентаря"
        );
        final boolean inventoryReady = state.optBoolean(
            "playerInventoryReady",
            false
        );
        final boolean pending = state.optBoolean("automationPending", false);
        final long accepted = state.optLong("automationAccepted", 0);
        final long rejected = state.optLong("automationRejected", 0);
        final long mapDecoded = state.optLong("miniMapDecodedChunks", 0);
        final long mapFailures = state.optLong("miniMapDecodeFailures", 0);
        final JSONObject areaFill = state.optJSONObject("areaFill");
        mainHandler.post(() -> {
            overlayController.updateAutomationStatus(
                automationStatus,
                inventoryReady,
                pending,
                accepted,
                rejected
            );
            overlayController.updateMiniMapStatus(mapDecoded, mapFailures);
            overlayController.updateAreaFillStatus(areaFill);
            if (areaFillOverlayController != null) {
                areaFillOverlayController.update(areaFill);
            }
        });
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
        boolean miniMap = preferences.getBoolean(KEY_MINIMAP, true);
        int miniMapRadius = clampMiniMapRadius(preferences.getInt(
            KEY_MINIMAP_RADIUS,
            4
        ));
        int miniMapScale = clampOverlayScale(preferences.getInt(
            KEY_MINIMAP_SCALE,
            90
        ));
        boolean miniMapRound = preferences.getBoolean(KEY_MINIMAP_ROUND, true);
        boolean autoArmor = preferences.getBoolean(KEY_AUTO_ARMOR, false);
        boolean autoTotem = preferences.getBoolean(KEY_AUTO_TOTEM, false);
        boolean areaFillEnabled = preferences.getBoolean(
            KEY_AREA_FILL_ENABLED,
            false
        );
        int areaFillPoints = clampAreaFillPoints(preferences.getInt(
            KEY_AREA_FILL_POINTS,
            2
        ));
        int areaFillButtonScale = clampOverlayScale(preferences.getInt(
            KEY_AREA_FILL_BUTTON_SCALE,
            90
        ));
        boolean threatAnalysis = preferences.getBoolean(
            KEY_THREAT_ANALYSIS,
            true
        );
        boolean threatWarning = preferences.getBoolean(
            KEY_THREAT_WARNING,
            true
        );
        int threatDistance = clampThreatDistance(preferences.getInt(
            KEY_THREAT_DISTANCE,
            12
        ));
        int threatWarningScale = clampOverlayScale(preferences.getInt(
            KEY_THREAT_WARNING_SCALE,
            90
        ));
        int threatColor = preferences.getInt(
            KEY_THREAT_COLOR,
            DEFAULT_THREAT_COLOR
        );
        boolean schematicEnabled = preferences.getBoolean(
            KEY_SCHEMATIC_ENABLED,
            false
        );
        boolean schematicTextures = preferences.getBoolean(
            KEY_SCHEMATIC_TEXTURES,
            true
        );
        int schematicOpacity = clampSchematicOpacity(preferences.getInt(
            KEY_SCHEMATIC_OPACITY,
            42
        ));
        boolean schematicOutlines = preferences.getBoolean(
            KEY_SCHEMATIC_OUTLINES,
            true
        );
        int schematicOutlineOpacity = clampSchematicOpacity(preferences.getInt(
            KEY_SCHEMATIC_OUTLINE_OPACITY,
            68
        ));
        int schematicCorrectColor = preferences.getInt(
            KEY_SCHEMATIC_CORRECT_COLOR,
            DEFAULT_SCHEMATIC_CORRECT_COLOR
        );
        int schematicWrongColor = preferences.getInt(
            KEY_SCHEMATIC_WRONG_COLOR,
            DEFAULT_SCHEMATIC_WRONG_COLOR
        );
        int schematicMissingColor = preferences.getInt(
            KEY_SCHEMATIC_MISSING_COLOR,
            DEFAULT_SCHEMATIC_MISSING_COLOR
        );
        int schematicDistance = clampSchematicDistance(preferences.getInt(
            KEY_SCHEMATIC_DISTANCE,
            96
        ));
        int schematicRotation = Math.floorMod(preferences.getInt(
            KEY_SCHEMATIC_ROTATION,
            0
        ), 4);
        boolean schematicMirror = preferences.getBoolean(
            KEY_SCHEMATIC_MIRROR,
            false
        );
        int schematicLayer = preferences.getInt(KEY_SCHEMATIC_LAYER, -1);
        preferences.edit()
            .putInt(KEY_RETAINED_RADIUS_CHUNKS, radiusChunks)
            .putInt(KEY_ENTITY_FOV, entityFov)
            .putInt(KEY_OUTLINE_THICKNESS_TENTHS, thicknessTenths)
            .putInt(KEY_ENTITY_MAX_DISTANCE, maximumDistance)
            .putInt(KEY_CHUNK_WIDGET_SCALE, chunkWidgetScale)
            .putInt(KEY_EQUIPMENT_HUD_SCALE, equipmentScale)
            .putInt(KEY_MINIMAP_RADIUS, miniMapRadius)
            .putInt(KEY_MINIMAP_SCALE, miniMapScale)
            .putInt(KEY_AREA_FILL_POINTS, areaFillPoints)
            .putInt(KEY_AREA_FILL_BUTTON_SCALE, areaFillButtonScale)
            .putInt(KEY_THREAT_DISTANCE, threatDistance)
            .putInt(KEY_THREAT_WARNING_SCALE, threatWarningScale)
            .putInt(KEY_SCHEMATIC_OPACITY, schematicOpacity)
            .putInt(KEY_SCHEMATIC_OUTLINE_OPACITY, schematicOutlineOpacity)
            .putInt(KEY_SCHEMATIC_DISTANCE, schematicDistance)
            .putInt(KEY_SCHEMATIC_ROTATION, schematicRotation)
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
            if (miniMapOverlayController != null) {
                miniMapOverlayController.configure(
                    miniMap,
                    miniMapRadius,
                    miniMapScale,
                    miniMapRound
                );
            }
            if (threatOverlayController != null) {
                threatOverlayController.configure(
                    threatAnalysis,
                    threatWarning,
                    threatDistance,
                    threatWarningScale,
                    threatColor
                );
            }
            if (schematicOverlayController != null) {
                schematicOverlayController.configure(
                    schematicEnabled,
                    entityFov,
                    schematicTextures,
                    schematicOpacity,
                    schematicOutlines,
                    schematicOutlineOpacity,
                    schematicCorrectColor,
                    schematicWrongColor,
                    schematicMissingColor,
                    schematicDistance,
                    schematicRotation,
                    schematicMirror,
                    schematicLayer
                );
            }
            if (areaFillOverlayController != null) {
                areaFillOverlayController.configure(
                    areaFillEnabled,
                    areaFillButtonScale
                );
            }
        });
        try {
            boolean schematicWorldTracking = schematicEnabled;
            NativeBridge.configureRuntime(
                detailedLogs,
                retainChunks,
                radiusChunks
            );
            NativeBridge.configureGameplayFeatures(
                autoArmor,
                autoTotem,
                miniMap,
                schematicWorldTracking
            );
            NativeBridge.configureAreaFill(areaFillEnabled, areaFillPoints);
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
                        " equipmentHud=" + equipmentHud +
                        " miniMap=" + miniMap +
                        " miniMapRadius=" + miniMapRadius +
                        " autoArmor=" + autoArmor +
                        " autoTotem=" + autoTotem +
                        " areaFill=" + areaFillEnabled +
                        " areaFillPoints=" + areaFillPoints +
                        " threatAnalysis=" + threatAnalysis +
                        " threatDistance=" + threatDistance +
                        " schematic=" + schematicEnabled +
                        " schematicTextures=" + schematicTextures +
                        " schematicTextureOpacity=" + schematicOpacity +
                        " schematicOutlines=" + schematicOutlines +
                        " schematicOutlineOpacity=" + schematicOutlineOpacity +
                        " schematicDistance=" + schematicDistance +
                        " schematicRotation=" + schematicRotation +
                        " schematicMirror=" + schematicMirror +
                        " schematicLayer=" + schematicLayer
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
        overlayShouldBeVisible = visible;
        reconcileOverlayVisibility();
    }

    private void updateMinecraftUiBlocked(boolean blocked) {
        if (minecraftUiBlocked == blocked) return;
        minecraftUiBlocked = blocked;
        reconcileOverlayVisibility();
    }

    private void reconcileOverlayVisibility() {
        boolean visible = overlayShouldBeVisible && !minecraftUiBlocked;
        boolean changed = overlayWindowsVisible != visible;
        overlayWindowsVisible = visible;
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
            boolean showNow = overlayShouldBeVisible && !minecraftUiBlocked;
            if (entityOverlayController != null) {
                entityOverlayController.setUiBlocked(minecraftUiBlocked);
            }
            if (chunkOverlayController != null) {
                chunkOverlayController.setUiBlocked(minecraftUiBlocked);
            }
            if (equipmentOverlayController != null) {
                equipmentOverlayController.setUiBlocked(minecraftUiBlocked);
            }
            if (miniMapOverlayController != null) {
                miniMapOverlayController.setUiBlocked(minecraftUiBlocked);
            }
            if (threatOverlayController != null) {
                threatOverlayController.setUiBlocked(minecraftUiBlocked);
            }
            if (schematicOverlayController != null) {
                schematicOverlayController.setUiBlocked(minecraftUiBlocked);
            }
            if (areaFillOverlayController != null) {
                areaFillOverlayController.setUiBlocked(minecraftUiBlocked);
            }
            if (showNow) {
                if (entityOverlayController != null) {
                    entityOverlayController.setSessionVisible(true);
                }
                if (chunkOverlayController != null) {
                    chunkOverlayController.setSessionVisible(true);
                }
                if (equipmentOverlayController != null) {
                    equipmentOverlayController.setSessionVisible(true);
                }
                if (miniMapOverlayController != null) {
                    miniMapOverlayController.setSessionVisible(true);
                }
                if (threatOverlayController != null) {
                    threatOverlayController.setSessionVisible(true);
                }
                if (schematicOverlayController != null) {
                    schematicOverlayController.setSessionVisible(true);
                }
                if (areaFillOverlayController != null) {
                    areaFillOverlayController.setSessionVisible(true);
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
                if (miniMapOverlayController != null) {
                    miniMapOverlayController.setSessionVisible(false);
                }
                if (threatOverlayController != null) {
                    threatOverlayController.setSessionVisible(false);
                }
                if (schematicOverlayController != null) {
                    schematicOverlayController.setSessionVisible(false);
                }
                if (areaFillOverlayController != null) {
                    areaFillOverlayController.setSessionVisible(false);
                }
            }
        });
    }

    private boolean isImeVisible() {
        if (Build.VERSION.SDK_INT < 30) return false;
        try {
            WindowManager manager = (WindowManager) getSystemService(
                WINDOW_SERVICE
            );
            return manager != null && manager.getCurrentWindowMetrics()
                .getWindowInsets()
                .isVisible(WindowInsets.Type.ime());
        } catch (Throwable ignored) {
            return false;
        }
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

    public static int clampAreaFillPoints(int value) {
        return Math.max(
            MIN_AREA_FILL_POINTS,
            Math.min(MAX_AREA_FILL_POINTS, value)
        );
    }

    public static int clampMiniMapRadius(int value) {
        return Math.max(
            MIN_MINIMAP_RADIUS_CHUNKS,
            Math.min(MAX_MINIMAP_RADIUS_CHUNKS, value)
        );
    }

    public static int clampThreatDistance(int value) {
        return Math.max(
            MIN_THREAT_DISTANCE,
            Math.min(MAX_THREAT_DISTANCE, value)
        );
    }

    public static int clampSchematicOpacity(int value) {
        return Math.max(10, Math.min(100, value));
    }

    public static int clampSchematicDistance(int value) {
        return Math.max(16, Math.min(192, value));
    }
}
