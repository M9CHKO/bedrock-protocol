package com.m9chko.bedrockrelay;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;

import com.m9chko.bedrockrelay.schematic.SchematicActiveBlockSelector;
import com.m9chko.bedrockrelay.schematic.SchematicModel;

import org.json.JSONObject;

import java.util.Arrays;
import java.util.Comparator;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

/** Click-through 3D ghost blocks anchored to packet-derived world coordinates. */
final class SchematicOverlayController {
    private static final long SURFACE_WAIT_MILLIS = 1_800L;
    private static final int BLOCK_SNAPSHOT_MAGIC = 0x43504553; // CPES
    private static final int BLOCK_SNAPSHOT_VERSION = 2;
    private static final int BLOCK_SNAPSHOT_HEADER = 6;
    private static final int BLOCK_QUERY_BATCH_SIZE = 4_096;
    // Anchor buttons may be tapped several times a second. Keep the last
    // published plan visible while the placement is moving and only scan/send
    // the final position. Otherwise every intermediate coordinate produces a
    // full debug-drawer replacement containing hundreds of shapes.
    private static final long PLACEMENT_SETTLE_MILLIS = 350L;
    private static final long DEBUG_MARKER_RETRY_INITIAL_MILLIS = 1_000L;
    private static final long DEBUG_MARKER_RETRY_MAX_MILLIS = 30_000L;
    private static final long FORCE_BLOCK_SNAPSHOT = Long.MIN_VALUE;
    private static final byte BLOCK_UNKNOWN = 0;
    private static final byte BLOCK_MISSING = 1;
    private static final byte BLOCK_CORRECT = 2;
    private static final byte BLOCK_WRONG = 3;
    private final Context context;
    private final SharedPreferences preferences;
    private final WindowManager windowManager;
    private final BlockNameTranslator blockNameTranslator;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final AtomicReference<EntityOutlineOverlayController.CameraSample>
        pendingCamera = new AtomicReference<>();
    private final AtomicBoolean deliveryPosted = new AtomicBoolean(false);

    private volatile boolean sessionVisible;
    private volatile boolean uiBlocked;
    private volatile boolean enabled;
    private volatile int fieldOfView = 70;
    private volatile boolean texturesEnabled = true;
    private volatile int opacityPercent = 42;
    private volatile boolean outlinesEnabled = true;
    private volatile int outlineOpacityPercent = 68;
    private volatile int correctOutlineColor =
        RelayService.DEFAULT_SCHEMATIC_CORRECT_COLOR;
    private volatile int wrongOutlineColor =
        RelayService.DEFAULT_SCHEMATIC_WRONG_COLOR;
    private volatile int missingOutlineColor =
        RelayService.DEFAULT_SCHEMATIC_MISSING_COLOR;
    private volatile int maximumDistance = 96;
    private volatile int rotationQuarterTurns;
    private volatile boolean mirrored;
    private volatile int selectedLayer = -1;
    private volatile SchematicModel model;
    private volatile EntityOutlineOverlayController.CameraSample latestCamera =
        EntityOutlineOverlayController.CameraSample.unknown();
    private volatile boolean placementPending;
    private boolean placementTargetCaptured;
    private long placementRequest;
    private long placementDeadlineMs;
    private int placementX;
    private int placementZ;
    private int placementFallbackY;
    private int pendingPlacementShiftX;
    private int pendingPlacementShiftY;
    private int pendingPlacementShiftZ;
    private boolean missingPermissionLogged;
    private SchematicView view;
    private final Object blockQueryLock = new Object();
    private volatile BlockQuery blockQuery;
    private volatile boolean debugMarkersDirty = true;
    private volatile boolean debugMarkerPlanPublished;
    private volatile long debugMarkerRetryAfterMs;
    private volatile long debugMarkerRetryDelayMs;
    private volatile int publishedCameraChunkX = Integer.MIN_VALUE;
    private volatile int publishedCameraChunkY = Integer.MIN_VALUE;
    private volatile int publishedCameraChunkZ = Integer.MIN_VALUE;

    SchematicOverlayController(
        Context context,
        SharedPreferences preferences
    ) {
        this.context = context;
        this.preferences = preferences;
        windowManager = (WindowManager) context.getSystemService(
            Context.WINDOW_SERVICE
        );
        blockNameTranslator = new BlockNameTranslator(context);
    }

    void setSessionVisible(boolean visible) {
        if (sessionVisible && !visible) {
            latestCamera = EntityOutlineOverlayController.CameraSample.unknown();
            placementTargetCaptured = false;
            invalidateBlockQuery();
            clearDebugMarkers();
        }
        sessionVisible = visible;
        reconcileWindow();
    }

    void setUiBlocked(boolean blocked) {
        if (uiBlocked == blocked) return;
        uiBlocked = blocked;
        reconcileWindow();
    }

    void configure(
        boolean enabled,
        int fov,
        boolean textures,
        int opacity,
        boolean outlines,
        int outlineOpacity,
        int correctColor,
        int wrongColor,
        int missingColor,
        int distance,
        int rotation,
        boolean mirrored,
        int layer
    ) {
        int nextFov = RelayService.clampEntityFov(fov);
        int nextOpacity = RelayService.clampSchematicOpacity(opacity);
        int nextOutlineOpacity = RelayService.clampSchematicOpacity(
            outlineOpacity
        );
        int nextDistance = RelayService.clampSchematicDistance(distance);
        int nextRotation = Math.floorMod(rotation, 4);
        boolean queryChanged = rotationQuarterTurns != nextRotation ||
            this.mirrored != mirrored;
        boolean markerSelectionChanged = texturesEnabled != textures ||
            opacityPercent != nextOpacity || outlinesEnabled != outlines ||
            outlineOpacityPercent != nextOutlineOpacity ||
            correctOutlineColor != correctColor ||
            wrongOutlineColor != wrongColor ||
            missingOutlineColor != missingColor ||
            maximumDistance != nextDistance || selectedLayer != layer;
        boolean enabledChanged = this.enabled != enabled;

        this.enabled = enabled;
        fieldOfView = nextFov;
        texturesEnabled = textures;
        opacityPercent = nextOpacity;
        outlinesEnabled = outlines;
        outlineOpacityPercent = nextOutlineOpacity;
        correctOutlineColor = correctColor;
        wrongOutlineColor = wrongColor;
        missingOutlineColor = missingColor;
        maximumDistance = nextDistance;
        rotationQuarterTurns = nextRotation;
        this.mirrored = mirrored;
        selectedLayer = layer;
        if (!enabled) {
            if (queryChanged) invalidateBlockQuery();
            if (enabledChanged || debugMarkerPlanPublished) clearDebugMarkers();
        } else if (queryChanged) {
            invalidateBlockQuery();
        } else if (enabledChanged || markerSelectionChanged) {
            // The cached world comparison remains valid. Re-plan on the
            // snapshot executor without a visible clear/re-add flicker.
            debugMarkersDirty = true;
            debugMarkerRetryAfterMs = 0L;
            debugMarkerRetryDelayMs = 0L;
        }
        refreshPlacementRequest();
        SchematicView current = view;
        if (current != null) {
            current.configure(
                fieldOfView,
                opacityPercent,
                maximumDistance,
                rotationQuarterTurns,
                mirrored,
                selectedLayer
            );
            current.setAnchor(
                preferences.getInt(RelayService.KEY_SCHEMATIC_ANCHOR_X, 0),
                savedAnchorY(),
                preferences.getInt(RelayService.KEY_SCHEMATIC_ANCHOR_Z, 0)
            );
        }
        reconcileWindow();
    }

    void setModel(SchematicModel value) {
        if (model == value) return;
        invalidateBlockQuery();
        model = value;
        // A different file has no valid visual relationship to the active
        // plan. Clear it explicitly; anchor moves keep using the seamless
        // keyed replacement path instead.
        if (debugMarkerPlanPublished) clearDebugMarkers();
        SchematicView current = view;
        if (current != null) current.setModel(value);
        reconcileWindow();
    }

    SchematicModel model() {
        return model;
    }

    boolean wantsFrames() {
        return sessionVisible && enabled && model != null &&
            (placementPending || preferences.getBoolean(
                RelayService.KEY_SCHEMATIC_PLACED,
                false
            ));
    }

    void offerCameraSnapshot(String json) throws Exception {
        if (!wantsFrames()) return;
        offerCamera(EntityOutlineOverlayController.CameraSample.from(
            new JSONObject(json)
        ));
    }

    void offerCamera(EntityOutlineOverlayController.CameraSample camera) {
        if (!wantsFrames() || camera == null) return;
        latestCamera = camera;
        pendingCamera.set(camera);
        postDelivery();
    }

    boolean placeNearCamera() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            mainHandler.post(this::placeNearCamera);
            return false;
        }
        beginPlacement(
            preferences.getLong(
                RelayService.KEY_SCHEMATIC_PLACE_REQUEST,
                0L
            )
        );
        EntityOutlineOverlayController.CameraSample camera = latestCamera;
        if (camera == null || !camera.known) return false;
        return tryPlaceNearCamera(camera);
    }

    void shiftAnchor(int dx, int dy, int dz) {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            mainHandler.post(() -> shiftAnchor(dx, dy, dz));
            return;
        }
        boolean placed = preferences.getBoolean(
            RelayService.KEY_SCHEMATIC_PLACED,
            false
        );
        if (placementPending || !placed) {
            if (!placementPending) {
                beginPlacement(preferences.getLong(
                    RelayService.KEY_SCHEMATIC_PLACE_REQUEST,
                    0L
                ));
            }
            pendingPlacementShiftX += dx;
            pendingPlacementShiftY += dy;
            pendingPlacementShiftZ += dz;
            EntityOutlineOverlayController.CameraSample camera = latestCamera;
            if (camera != null && camera.known) {
                tryPlaceNearCamera(camera);
            }
            return;
        }
        int x = preferences.getInt(RelayService.KEY_SCHEMATIC_ANCHOR_X, 0);
        int y = savedAnchorY();
        int z = preferences.getInt(RelayService.KEY_SCHEMATIC_ANCHOR_Z, 0);
        int nextX = x + dx;
        int nextY = y + dy;
        int nextZ = z + dz;
        saveAnchor(nextX, nextY, nextZ);
        DiagnosticsLog.append(
            context,
            "INFO",
            "schematics",
            "Schematic anchor moved to X=" + nextX +
                " Y=" + nextY + " Z=" + nextZ
        );
    }

    void hideImmediately() {
        sessionVisible = false;
        clearDebugMarkers();
        removeWindow();
    }

    private void refreshPlacementRequest() {
        long requested = preferences.getLong(
            RelayService.KEY_SCHEMATIC_PLACE_REQUEST,
            0L
        );
        long handled = preferences.getLong(
            RelayService.KEY_SCHEMATIC_PLACE_REQUEST_HANDLED,
            0L
        );
        if (requested != 0L && requested != handled &&
            (!placementPending || placementRequest != requested)) {
            beginPlacement(requested);
        }
    }

    private void beginPlacement(long request) {
        invalidateBlockQuery();
        placementPending = true;
        placementTargetCaptured = false;
        placementRequest = request;
        placementDeadlineMs = 0L;
        pendingPlacementShiftX = 0;
        pendingPlacementShiftY = 0;
        pendingPlacementShiftZ = 0;
        reconcileWindow();
    }

    private boolean tryPlaceNearCamera(
        EntityOutlineOverlayController.CameraSample camera
    ) {
        if (!placementTargetCaptured) {
            double yaw = Math.toRadians(camera.yaw);
            placementX = (int) Math.floor(
                camera.x - Math.sin(yaw) * 5.0
            );
            placementZ = (int) Math.floor(
                camera.z + Math.cos(yaw) * 5.0
            );
            // Packet camera Y is eye level. This fallback puts the first
            // schematic layer at the player's feet if the target chunk has
            // not reached the terrain decoder yet.
            placementFallbackY = (int) Math.floor(camera.y - 1.62);
            placementDeadlineMs = SystemClock.uptimeMillis() +
                SURFACE_WAIT_MILLIS;
            placementTargetCaptured = true;
        }

        float surfaceY = NativeBridge.worldSurfaceY(placementX, placementZ);
        boolean collisionKnown = Float.isFinite(surfaceY);
        if (!collisionKnown &&
            SystemClock.uptimeMillis() < placementDeadlineMs) {
            return false;
        }
        int anchorY = collisionKnown
            ? SchematicPlacementTransform.placementAnchorY(surfaceY)
            : placementFallbackY;
        int anchorX = placementX + pendingPlacementShiftX;
        anchorY += pendingPlacementShiftY;
        int anchorZ = placementZ + pendingPlacementShiftZ;
        pendingPlacementShiftX = 0;
        pendingPlacementShiftY = 0;
        pendingPlacementShiftZ = 0;
        saveAnchor(anchorX, anchorY, anchorZ);
        DiagnosticsLog.append(
            context,
            "INFO",
            "schematics",
            "Schematic anchor fixed at X=" + anchorX +
                " Y=" + anchorY + " Z=" + anchorZ +
                " ground=" + (collisionKnown ? "block_collision" : "camera_fallback")
        );
        return true;
    }

    private int savedAnchorY() {
        int fallback = preferences.getInt(
            RelayService.KEY_SCHEMATIC_ANCHOR_Y,
            0
        );
        float exact = preferences.getFloat(
            RelayService.KEY_SCHEMATIC_ANCHOR_Y_EXACT,
            fallback
        );
        return Float.isFinite(exact)
            ? SchematicPlacementTransform.placementAnchorY(exact)
            : fallback;
    }

    private void saveAnchor(int x, int y, int z) {
        invalidateBlockQuery();
        SharedPreferences.Editor editor = preferences.edit()
            .putInt(RelayService.KEY_SCHEMATIC_ANCHOR_X, x)
            .putInt(RelayService.KEY_SCHEMATIC_ANCHOR_Y, y)
            .putFloat(RelayService.KEY_SCHEMATIC_ANCHOR_Y_EXACT, y)
            .putInt(RelayService.KEY_SCHEMATIC_ANCHOR_Z, z)
            .putBoolean(RelayService.KEY_SCHEMATIC_PLACED, true);
        if (placementRequest != 0L) {
            editor.putLong(
                RelayService.KEY_SCHEMATIC_PLACE_REQUEST_HANDLED,
                placementRequest
            );
        }
        editor.apply();
        placementPending = false;
        placementTargetCaptured = false;
        SchematicView current = view;
        if (current != null) current.setAnchor(x, y, z);
        reconcileWindow();
    }

    void pollWorldSnapshot() {
        BlockQuery query = currentBlockQuery();
        if (query == null) {
            // Placement deliberately has a short settling window. Keep the
            // previous anchor visible until a complete replacement plan is
            // ready instead of flashing an empty world after the button tap.
            if (!placementPending && debugMarkerPlanPublished) {
                clearDebugMarkers();
            }
            return;
        }
        if (!query.placementSettled()) return;
        long afterRevision;
        int batchStart;
        int batchCount;
        int[] worldCoordinates;
        synchronized (blockQueryLock) {
            if (blockQuery != query) return;
            batchStart = query.scanning ? query.nextIndex : 0;
            batchCount = Math.min(
                BLOCK_QUERY_BATCH_SIZE,
                query.blockCount() - batchStart
            );
            afterRevision = query.scanning
                ? FORCE_BLOCK_SNAPSHOT
                : query.completedRevision;
            worldCoordinates = query.worldCoordinates(
                batchStart,
                batchCount
            );
        }
        int[] snapshot = NativeBridge.schematicBlockSnapshot(
            afterRevision,
            worldCoordinates
        );
        if (snapshot == null || snapshot.length < BLOCK_SNAPSHOT_HEADER ||
            snapshot[0] != BLOCK_SNAPSHOT_MAGIC ||
            snapshot[1] != BLOCK_SNAPSHOT_VERSION) {
            throw new IllegalStateException("Invalid schematic block snapshot");
        }
        long revision = (snapshot[2] & 0xffffffffL) |
            ((long) snapshot[3] << 32);
        int count = snapshot[5];
        if (count == 0) {
            byte[] cachedStates = null;
            synchronized (blockQueryLock) {
                if (blockQuery == query && !query.scanning) {
                    query.completedRevision = revision;
                    cachedStates = query.states;
                }
            }
            if (cachedStates != null && shouldRefreshMarkersForCamera()) {
                publishDebugMarkers(query, cachedStates, revision, snapshot[4]);
            }
            return;
        }
        if (count != batchCount ||
            snapshot.length != BLOCK_SNAPSHOT_HEADER + count * 3) {
            throw new IllegalStateException(
                "Schematic block snapshot size does not match query"
            );
        }

        byte[] completedStates = null;
        synchronized (blockQueryLock) {
            if (blockQuery != query) return;
            if (query.scanning) {
                if (query.nextIndex != batchStart) return;
                if (query.cycleRevision != revision) {
                    // Finish this bounded scan even while unrelated chunks
                    // stream in. Restarting here could starve large schematics
                    // and repeatedly allocate their full state array.
                    query.cycleChanged = true;
                    query.cycleRevision = revision;
                }
            } else {
                if (batchStart != 0) return;
                query.beginCycle(revision);
            }

            for (int index = 0; index < count; ++index) {
                int actualOffset = BLOCK_SNAPSHOT_HEADER + index * 3;
                int presence = snapshot[actualOffset];
                int actualNameHash = snapshot[actualOffset + 1];
                int actualStateHash = snapshot[actualOffset + 2];
                byte state;
                if (presence == 0) {
                    state = BLOCK_UNKNOWN;
                } else if (presence == 1) {
                    state = BLOCK_MISSING;
                } else if (presence == 2) {
                    SchematicBlockMatcher.Status matched =
                        SchematicBlockMatcher.match(
                            query.expectedAt(batchStart + index),
                            true,
                            actualNameHash,
                            actualStateHash,
                            query.exactBedrockProperties
                        );
                    state = matched == SchematicBlockMatcher.Status.CORRECT
                        ? BLOCK_CORRECT
                        : matched == SchematicBlockMatcher.Status.WRONG
                            ? BLOCK_WRONG
                            : matched == SchematicBlockMatcher.Status.MISSING
                                ? BLOCK_MISSING
                                : BLOCK_UNKNOWN;
                } else {
                    state = BLOCK_UNKNOWN;
                }
                query.workingStates[batchStart + index] = state;
                if (state == BLOCK_CORRECT) ++query.workingCorrectBlocks;
                else if (state == BLOCK_WRONG) ++query.workingWrongBlocks;
            }
            query.nextIndex = batchStart + count;
            if (query.nextIndex == query.blockCount() &&
                query.finishCycle(revision)) {
                completedStates = query.states;
            }
        }
        if (completedStates == null) return;
        if (!query.lastCycleStatesChanged &&
            !shouldRefreshMarkersForCamera()) return;
        publishDebugMarkers(query, completedStates, revision, snapshot[4]);
    }

    private BlockQuery currentBlockQuery() {
        SchematicModel currentModel = model;
        if (placementPending || currentModel == null || !preferences.getBoolean(
                RelayService.KEY_SCHEMATIC_PLACED,
                false
            )) {
            return null;
        }
        int anchorX = preferences.getInt(
            RelayService.KEY_SCHEMATIC_ANCHOR_X,
            0
        );
        int anchorY = savedAnchorY();
        int anchorZ = preferences.getInt(
            RelayService.KEY_SCHEMATIC_ANCHOR_Z,
            0
        );
        int rotation = rotationQuarterTurns;
        boolean mirror = mirrored;
        EntityOutlineOverlayController.CameraSample camera = latestCamera;
        if (camera == null || !camera.known) return null;
        int cameraChunkX = blockCoordinate(camera.x);
        int cameraChunkY = blockCoordinate(camera.y);
        int cameraChunkZ = blockCoordinate(camera.z);
        int distance = maximumDistance;
        int layer = selectedLayer;
        BlockQuery cached = blockQuery;
        if (cached != null && cached.matches(
                currentModel,
                anchorX,
                anchorY,
                anchorZ,
                rotation,
                mirror,
                cameraChunkX,
                cameraChunkY,
                cameraChunkZ,
                distance,
                layer
            )) {
            return cached;
        }
        synchronized (blockQueryLock) {
            cached = blockQuery;
            if (cached != null && cached.matches(
                    currentModel,
                    anchorX,
                    anchorY,
                    anchorZ,
                    rotation,
                    mirror,
                    cameraChunkX,
                    cameraChunkY,
                    cameraChunkZ,
                    distance,
                    layer
                )) {
                return cached;
            }
            BlockQuery created = BlockQuery.create(
                currentModel,
                new SchematicPlacementTransform(
                    anchorX,
                    anchorY,
                    anchorZ,
                    currentModel.sizeX(),
                    rotation,
                    mirror
                ),
                blockNameTranslator,
                cameraChunkX,
                cameraChunkY,
                cameraChunkZ,
                distance,
                layer
            );
            blockQuery = created;
            return created;
        }
    }

    private void invalidateBlockQuery() {
        synchronized (blockQueryLock) {
            blockQuery = null;
        }
        debugMarkersDirty = true;
        debugMarkerRetryAfterMs = 0L;
        debugMarkerRetryDelayMs = 0L;
        publishedCameraChunkX = Integer.MIN_VALUE;
        publishedCameraChunkY = Integer.MIN_VALUE;
        publishedCameraChunkZ = Integer.MIN_VALUE;
        // Native replacement is now a keyed delta. Keep the last coherent
        // plan on screen while a new anchor/window/model is scanned, then let
        // the accepted replacement remove only cells that really changed.
        SchematicView current = view;
        if (current != null) current.clearWorldStates();
    }

    private boolean shouldRefreshMarkersForCamera() {
        if (debugMarkersDirty) return true;
        EntityOutlineOverlayController.CameraSample camera = latestCamera;
        if (camera == null || !camera.known) return false;
        return blockCoordinate(camera.x) != publishedCameraChunkX ||
            blockCoordinate(camera.y) != publishedCameraChunkY ||
            blockCoordinate(camera.z) != publishedCameraChunkZ;
    }

    private static int blockCoordinate(double coordinate) {
        return (int) Math.floor(coordinate / 16.0d);
    }

    private void publishDebugMarkers(
        BlockQuery query,
        byte[] states,
        long expectedWorldRevision,
        int expectedDimension
    ) {
        if (blockQuery != query || !wantsFrames()) return;
        long publishStartedAtMs = SystemClock.uptimeMillis();
        if (publishStartedAtMs < debugMarkerRetryAfterMs) {
            debugMarkersDirty = true;
            return;
        }
        EntityOutlineOverlayController.CameraSample camera = latestCamera;
        boolean cameraKnown = camera != null && camera.known;
        if (!cameraKnown || !query.matchesWindow(
                blockCoordinate(camera.x),
                blockCoordinate(camera.y),
                blockCoordinate(camera.z),
                maximumDistance,
                selectedLayer
            )) {
            debugMarkersDirty = true;
            return;
        }
        SchematicDebugMarkerPlanner.Result result =
            SchematicDebugMarkerPlanner.plan(
                query.model,
                query.transform,
                query.blockIndices,
                states,
                cameraKnown,
                cameraKnown ? camera.x : 0.0d,
                cameraKnown ? camera.y : 0.0d,
                cameraKnown ? camera.z : 0.0d,
                query.selectedLayer,
                query.maximumDistance,
                texturesEnabled || outlinesEnabled
                    ? Math.max(opacityPercent, outlineOpacityPercent)
                    : 0,
                texturesEnabled || outlinesEnabled,
                query.transformedBedrockPalette
            );
        synchronized (blockQueryLock) {
            // Serialize the last validity check, native publication and every
            // clear. A concurrent hide/anchor/model change must either happen
            // before this check or clear the plan immediately afterwards.
            EntityOutlineOverlayController.CameraSample currentCamera =
                latestCamera;
            if (blockQuery != query || !wantsFrames() ||
                currentCamera == null || !currentCamera.known ||
                !query.matchesWindow(
                    blockCoordinate(currentCamera.x),
                    blockCoordinate(currentCamera.y),
                    blockCoordinate(currentCamera.z),
                    maximumDistance,
                    selectedLayer
                )) {
                debugMarkersDirty = true;
                return;
            }
            boolean accepted = NativeBridge.replaceSchematicDebugMarkers(
                result.records(),
                result.expectedBlockStates(),
                texturesEnabled,
                opacityPercent,
                outlinesEnabled,
                outlineOpacityPercent,
                correctOutlineColor,
                wrongOutlineColor,
                missingOutlineColor,
                result.total(),
                result.correct(),
                result.missing(),
                result.wrong(),
                result.unknown(),
                expectedWorldRevision,
                expectedDimension
            );
            if (!accepted) {
                debugMarkersDirty = true;
                long nextDelay = debugMarkerRetryDelayMs <= 0L
                    ? DEBUG_MARKER_RETRY_INITIAL_MILLIS
                    : Math.min(
                        DEBUG_MARKER_RETRY_MAX_MILLIS,
                        debugMarkerRetryDelayMs * 2L
                    );
                debugMarkerRetryDelayMs = nextDelay;
                debugMarkerRetryAfterMs = SystemClock.uptimeMillis() + nextDelay;
                return;
            }
            debugMarkerPlanPublished = true;
            debugMarkersDirty = false;
            debugMarkerRetryAfterMs = 0L;
            debugMarkerRetryDelayMs = 0L;
            if (cameraKnown) {
                publishedCameraChunkX = blockCoordinate(camera.x);
                publishedCameraChunkY = blockCoordinate(camera.y);
                publishedCameraChunkZ = blockCoordinate(camera.z);
            }
            saveProgress(result);
        }
    }

    private void clearDebugMarkers() {
        synchronized (blockQueryLock) {
            if (debugMarkerPlanPublished) {
                try {
                    NativeBridge.clearSchematicDebugMarkers();
                } catch (Throwable error) {
                    DiagnosticsLog.appendError(
                        context,
                        "schematics",
                        "Failed to clear client-only schematic markers",
                        error
                    );
                }
            }
            debugMarkerPlanPublished = false;
            debugMarkersDirty = true;
            saveProgress(null);
        }
    }

    private void saveProgress(SchematicDebugMarkerPlanner.Result result) {
        int total = result == null ? 0 : result.total();
        int correct = result == null ? 0 : result.correct();
        int missing = result == null ? 0 : result.missing();
        int wrong = result == null ? 0 : result.wrong();
        int unknown = result == null ? 0 : result.unknown();
        int displayed = result == null ? 0 : result.displayed();
        if (preferences.getInt(RelayService.KEY_SCHEMATIC_TOTAL, -1) == total &&
            preferences.getInt(RelayService.KEY_SCHEMATIC_CORRECT, -1) == correct &&
            preferences.getInt(RelayService.KEY_SCHEMATIC_MISSING, -1) == missing &&
            preferences.getInt(RelayService.KEY_SCHEMATIC_WRONG, -1) == wrong &&
            preferences.getInt(RelayService.KEY_SCHEMATIC_UNKNOWN, -1) == unknown &&
            preferences.getInt(RelayService.KEY_SCHEMATIC_DISPLAYED, -1) == displayed) {
            return;
        }
        preferences.edit()
            .putInt(RelayService.KEY_SCHEMATIC_TOTAL, total)
            .putInt(RelayService.KEY_SCHEMATIC_CORRECT, correct)
            .putInt(RelayService.KEY_SCHEMATIC_MISSING, missing)
            .putInt(RelayService.KEY_SCHEMATIC_WRONG, wrong)
            .putInt(RelayService.KEY_SCHEMATIC_UNKNOWN, unknown)
            .putInt(RelayService.KEY_SCHEMATIC_DISPLAYED, displayed)
            .apply();
    }

    private static final class BlockQuery {
        final SchematicModel model;
        final int anchorX;
        final int anchorY;
        final int anchorZ;
        final int rotation;
        final boolean mirrored;
        final int cameraChunkX;
        final int cameraChunkY;
        final int cameraChunkZ;
        final int maximumDistance;
        final int selectedLayer;
        final long placementReadyAtMs;
        final boolean exactBedrockProperties;
        final SchematicPlacementTransform transform;
        final SchematicBlockMatcher.ExpectedBlock[] paletteExpectedBlocks;
        final String[] transformedBedrockPalette;
        final int[] blockIndices;
        long completedRevision = FORCE_BLOCK_SNAPSHOT;
        long cycleRevision = FORCE_BLOCK_SNAPSHOT;
        boolean cycleChanged;
        boolean scanning;
        int nextIndex;
        byte[] states;
        byte[] workingStates;
        int correctBlocks;
        int wrongBlocks;
        int workingCorrectBlocks;
        int workingWrongBlocks;
        boolean lastCycleStatesChanged;

        private BlockQuery(
            SchematicModel model,
            SchematicPlacementTransform transform,
            SchematicBlockMatcher.ExpectedBlock[] paletteExpectedBlocks,
            String[] transformedBedrockPalette,
            int[] blockIndices,
            int cameraChunkX,
            int cameraChunkY,
            int cameraChunkZ,
            int maximumDistance,
            int selectedLayer
        ) {
            this.model = model;
            anchorX = transform.anchorX();
            anchorY = transform.anchorY();
            anchorZ = transform.anchorZ();
            rotation = transform.rotationQuarterTurns();
            mirrored = transform.mirrored();
            this.cameraChunkX = cameraChunkX;
            this.cameraChunkY = cameraChunkY;
            this.cameraChunkZ = cameraChunkZ;
            this.maximumDistance = maximumDistance;
            this.selectedLayer = selectedLayer;
            placementReadyAtMs = SystemClock.uptimeMillis() +
                PLACEMENT_SETTLE_MILLIS;
            // .mcstructure states use the same Bedrock property names/values
            // as the decoded world cache. BlockQuery.create rotates and mirrors
            // directional palette properties before constructing this query,
            // so exact comparison remains valid for every placement transform.
            exactBedrockProperties = "Bedrock .mcstructure".equals(
                model.format()
            );
            this.transform = transform;
            this.paletteExpectedBlocks = paletteExpectedBlocks;
            this.transformedBedrockPalette = transformedBedrockPalette;
            this.blockIndices = blockIndices;
            if (blockIndices.length == 0) states = new byte[0];
        }

        static BlockQuery create(
            SchematicModel model,
            SchematicPlacementTransform transform,
            BlockNameTranslator translator,
            int cameraChunkX,
            int cameraChunkY,
            int cameraChunkZ,
            int maximumDistance,
            int selectedLayer
        ) {
            SchematicBlockMatcher.ExpectedBlock[] palette =
                new SchematicBlockMatcher.ExpectedBlock[model.paletteSize()];
            String[] bedrockPalette = new String[model.paletteSize()];
            boolean bedrockFormat = "Bedrock .mcstructure".equals(
                model.format()
            );
            // Palette size is capped at 65,536, while the boundary can contain
            // millions of cells. Build aliases once per palette entry so query
            // creation never scans the entire structure under blockQueryLock.
            for (int paletteIndex = 0;
                paletteIndex < palette.length;
                ++paletteIndex) {
                String state = SchematicBlockStateTransform.transform(
                    model.paletteState(paletteIndex),
                    transform.rotationQuarterTurns(),
                    transform.mirrored()
                );
                bedrockPalette[paletteIndex] = bedrockFormat
                    ? state
                    : translator.bedrockState(state);
                palette[paletteIndex] = SchematicBlockMatcher.expected(
                    state,
                    translator.bedrockCandidates(state)
                );
            }
            int[] blockIndices = SchematicActiveBlockSelector.select(
                model,
                transform,
                cameraChunkX,
                cameraChunkY,
                cameraChunkZ,
                maximumDistance,
                selectedLayer
            );
            return new BlockQuery(
                model,
                transform,
                palette,
                bedrockPalette,
                blockIndices,
                cameraChunkX,
                cameraChunkY,
                cameraChunkZ,
                maximumDistance,
                selectedLayer
            );
        }

        int blockCount() {
            return blockIndices.length;
        }

        boolean placementSettled() {
            return SystemClock.uptimeMillis() >= placementReadyAtMs;
        }

        int[] worldCoordinates(int start, int count) {
            if (start < 0 || count < 0 || start + count > blockCount()) {
                throw new IndexOutOfBoundsException("Invalid block query batch");
            }
            int[] coordinates = new int[Math.multiplyExact(count, 3)];
            for (int index = 0; index < count; ++index) {
                int blockIndex = blockIndices[start + index];
                SchematicPlacementTransform.BlockPosition position =
                    transform.worldBlock(
                        model.xFromIndex(blockIndex),
                        model.yFromIndex(blockIndex),
                        model.zFromIndex(blockIndex)
                    );
                int offset = index * 3;
                coordinates[offset] = position.x();
                coordinates[offset + 1] = position.y();
                coordinates[offset + 2] = position.z();
            }
            return coordinates;
        }

        SchematicBlockMatcher.ExpectedBlock expectedAt(int listIndex) {
            int blockIndex = blockIndices[listIndex];
            int paletteIndex = model.paletteIndexAtLinear(blockIndex);
            SchematicBlockMatcher.ExpectedBlock expected =
                paletteExpectedBlocks[paletteIndex];
            if (expected == null) {
                throw new IllegalStateException("Missing expected palette block");
            }
            return expected;
        }

        void beginCycle(long revision) {
            scanning = true;
            cycleRevision = revision;
            cycleChanged = false;
            nextIndex = 0;
            workingStates = new byte[blockCount()];
            workingCorrectBlocks = 0;
            workingWrongBlocks = 0;
        }

        void abortCycle() {
            scanning = false;
            cycleRevision = FORCE_BLOCK_SNAPSHOT;
            cycleChanged = false;
            nextIndex = 0;
            workingStates = null;
            workingCorrectBlocks = 0;
            workingWrongBlocks = 0;
        }

        boolean finishCycle(long revision) {
            // A global revision can advance because an unrelated loaded chunk
            // changed. Discarding a multi-batch scan in that case starves large
            // schematics while the player is moving. Publish the bounded
            // snapshot and let the next revision-driven pass converge any cell
            // that changed during it.
            boolean changedDuringCycle = cycleChanged;
            lastCycleStatesChanged = states == null ||
                !Arrays.equals(states, workingStates);
            states = workingStates;
            correctBlocks = workingCorrectBlocks;
            wrongBlocks = workingWrongBlocks;
            // The mixed pass is immediately useful, but it cannot be the
            // terminal cache snapshot. Force one more bounded pass so a cell
            // read before a concurrent UpdateBlock is guaranteed to converge
            // once the stream becomes quiet.
            completedRevision = changedDuringCycle
                ? FORCE_BLOCK_SNAPSHOT
                : revision;
            abortCycle();
            return true;
        }

        boolean matches(
            SchematicModel candidate,
            int x,
            int y,
            int z,
            int quarterTurns,
            boolean mirror,
            int candidateCameraChunkX,
            int candidateCameraChunkY,
            int candidateCameraChunkZ,
            int candidateMaximumDistance,
            int candidateSelectedLayer
        ) {
            return model == candidate && anchorX == x && anchorY == y &&
                anchorZ == z && rotation == Math.floorMod(quarterTurns, 4) &&
                mirrored == mirror && cameraChunkX == candidateCameraChunkX &&
                cameraChunkY == candidateCameraChunkY &&
                cameraChunkZ == candidateCameraChunkZ &&
                maximumDistance == candidateMaximumDistance &&
                selectedLayer == candidateSelectedLayer;
        }

        boolean matchesWindow(
            int candidateCameraChunkX,
            int candidateCameraChunkY,
            int candidateCameraChunkZ,
            int candidateMaximumDistance,
            int candidateSelectedLayer
        ) {
            return cameraChunkX == candidateCameraChunkX &&
                cameraChunkY == candidateCameraChunkY &&
                cameraChunkZ == candidateCameraChunkZ &&
                maximumDistance == candidateMaximumDistance &&
                selectedLayer == candidateSelectedLayer;
        }
    }

    private void postDelivery() {
        if (!deliveryPosted.compareAndSet(false, true)) return;
        mainHandler.post(() -> {
            EntityOutlineOverlayController.CameraSample camera =
                pendingCamera.getAndSet(null);
            // Camera snapshots arrive on RelayService's polling executor.
            // Placement mutates SharedPreferences, View state and
            // WindowManager state, so keep the entire commit on the UI
            // thread. The native surface lookup scans only one X/Z column.
            if (camera != null && camera.known && placementPending) {
                tryPlaceNearCamera(camera);
            }
            deliveryPosted.set(false);
            if (pendingCamera.get() != null) postDelivery();
        });
    }

    private void reconcileWindow() {
        // Schematics are rendered inside Minecraft with DebugRenderer
        // packets. The old Android full-screen camera projection must never
        // be attached: it caused view-dependent drift and could cover input.
        removeWindow();
    }

    private void addWindow() {
        if (view != null) return;
        if (!Settings.canDrawOverlays(context)) {
            if (!missingPermissionLogged) {
                missingPermissionLogged = true;
                DiagnosticsLog.append(
                    context,
                    "WARN",
                    "schematics",
                    "Overlay permission is missing; schematic was not shown"
                );
            }
            return;
        }
        missingPermissionLogged = false;
        SchematicView added = new SchematicView(context);
        added.setModel(model);
        added.configure(
            fieldOfView,
            opacityPercent,
            maximumDistance,
            rotationQuarterTurns,
            mirrored,
            selectedLayer
        );
        int addedAnchorX = preferences.getInt(
            RelayService.KEY_SCHEMATIC_ANCHOR_X,
            0
        );
        int addedAnchorY = savedAnchorY();
        int addedAnchorZ = preferences.getInt(
            RelayService.KEY_SCHEMATIC_ANCHOR_Z,
            0
        );
        added.setAnchor(addedAnchorX, addedAnchorY, addedAnchorZ);
        added.setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
        );
        WindowManager.LayoutParams params = new WindowManager.LayoutParams(
            WindowManager.LayoutParams.MATCH_PARENT,
            WindowManager.LayoutParams.MATCH_PARENT,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE |
                WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE |
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL |
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN |
                WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS |
                WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED,
            PixelFormat.TRANSLUCENT
        );
        params.gravity = Gravity.TOP | Gravity.START;
        // Combined with the entity projection window this stays below the
        // Android 12 obscured-touch threshold: 1 - (1-.54)^2 = .7884.
        params.alpha = 0.54f;
        if (Build.VERSION.SDK_INT >= 28) {
            params.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        try {
            windowManager.addView(added, params);
            view = added;
            BlockQuery cachedQuery;
            byte[] cachedStates;
            int cachedCorrect;
            int cachedWrong;
            synchronized (blockQueryLock) {
                cachedQuery = blockQuery;
                if (cachedQuery != null && cachedQuery.matches(
                        model,
                        addedAnchorX,
                        addedAnchorY,
                        addedAnchorZ,
                        rotationQuarterTurns,
                        mirrored,
                        cachedQuery.cameraChunkX,
                        cachedQuery.cameraChunkY,
                        cachedQuery.cameraChunkZ,
                        cachedQuery.maximumDistance,
                        cachedQuery.selectedLayer
                    )) {
                    cachedStates = cachedQuery.states;
                    cachedCorrect = cachedQuery.correctBlocks;
                    cachedWrong = cachedQuery.wrongBlocks;
                } else {
                    cachedStates = null;
                    cachedCorrect = 0;
                    cachedWrong = 0;
                }
            }
            if (cachedStates != null) {
                added.setWorldStates(
                    cachedQuery.model,
                    cachedQuery.blockIndices,
                    cachedStates,
                    cachedCorrect,
                    cachedWrong
                );
            }
            EntityOutlineOverlayController.CameraSample camera =
                pendingCamera.get();
            if (camera != null) added.submitCamera(camera);
            DiagnosticsLog.append(
                context,
                "INFO",
                "schematics",
                "Schematic 3D overlay opened; renderer=packet_world_projection"
            );
        } catch (Throwable error) {
            view = null;
            DiagnosticsLog.appendError(
                context,
                "schematics",
                "Failed to open schematic overlay",
                error
            );
        }
    }

    private void removeWindow() {
        SchematicView removed = view;
        view = null;
        pendingCamera.set(null);
        if (removed == null) return;
        try {
            windowManager.removeViewImmediate(removed);
        } catch (Throwable error) {
            DiagnosticsLog.appendError(
                context,
                "schematics",
                "Failed to close schematic overlay",
                error
            );
        }
    }

    private static final class SchematicView extends View {
        private static final double NEAR_PLANE = 0.10;
        private static final int MAX_RENDERED_CUBES = 1_800;
        private static final int MAX_TEXTURED_CUBES = 640;
        private static final Comparator<ProjectedCube> FAR_TO_NEAR =
            (left, right) -> Double.compare(right.depth, left.depth);
        private static final int[][] EDGES = {
            {0, 1}, {1, 3}, {3, 2}, {2, 0},
            {4, 5}, {5, 7}, {7, 6}, {6, 4},
            {0, 4}, {1, 5}, {2, 6}, {3, 7}
        };
        private static final int[] TOP_FACE = {4, 5, 7, 6};
        private static final int[] BOTTOM_FACE = {0, 1, 3, 2};
        private static final int[] X_MIN_FACE = {4, 6, 2, 0};
        private static final int[] X_MAX_FACE = {5, 7, 3, 1};
        private static final int[] Z_MIN_FACE = {4, 5, 1, 0};
        private static final int[] Z_MAX_FACE = {6, 7, 3, 2};
        private static final int FACE_TOP = 1;
        private static final int FACE_BOTTOM = 1 << 1;
        private static final int FACE_X_MIN = 1 << 2;
        private static final int FACE_X_MAX = 1 << 3;
        private static final int FACE_Z_MIN = 1 << 4;
        private static final int FACE_Z_MAX = 1 << 5;

        private final PacketCameraTracker camera = new PacketCameraTracker();
        private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint texturePaint = new Paint();
        private final Paint edgePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint statusPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint statusBackground = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path path = new Path();
        private final Matrix textureMatrix = new Matrix();
        private final float[] sourceQuad = new float[8];
        private final float[] targetQuad = new float[8];
        private final double[] screenX = new double[8];
        private final double[] screenY = new double[8];
        private final ProjectedCube[] renderQueue =
            new ProjectedCube[MAX_RENDERED_CUBES];
        private final SchematicTextureAtlas textureAtlas;
        private final float density;
        private SchematicModel model;
        private int fieldOfView = 70;
        private int opacityPercent = 42;
        private int maximumDistance = 96;
        private int rotation;
        private boolean mirrored;
        private int selectedLayer = -1;
        private int anchorX;
        private int anchorY;
        private int anchorZ;
        private SchematicPlacementTransform transform;
        private int[] worldBlockIndices;
        private byte[] worldStates;
        private int correctBlocks;
        private int wrongBlocks;
        private double projectedSpan;

        SchematicView(Context context) {
            super(context);
            textureAtlas = new SchematicTextureAtlas(
                context,
                this::requestFrame
            );
            density = context.getResources().getDisplayMetrics().density;
            setBackgroundColor(Color.TRANSPARENT);
            setWillNotDraw(false);
            fillPaint.setStyle(Paint.Style.FILL);
            texturePaint.setAntiAlias(false);
            texturePaint.setFilterBitmap(false);
            texturePaint.setDither(false);
            edgePaint.setStyle(Paint.Style.STROKE);
            edgePaint.setStrokeWidth(Math.max(1.0f, density * 0.85f));
            statusPaint.setColor(Color.WHITE);
            statusPaint.setTextSize(Math.max(11f, density * 9.5f));
            statusPaint.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
            statusBackground.setColor(0x99111720);
            for (int index = 0; index < renderQueue.length; ++index) {
                renderQueue[index] = new ProjectedCube();
            }
        }

        void setModel(SchematicModel value) {
            model = value;
            clearWorldStates();
            rebuildTransform();
            requestFrame();
        }

        void configure(
            int fov,
            int opacity,
            int distance,
            int rotation,
            boolean mirrored,
            int layer
        ) {
            fieldOfView = fov;
            opacityPercent = opacity;
            maximumDistance = distance;
            this.rotation = Math.floorMod(rotation, 4);
            this.mirrored = mirrored;
            selectedLayer = layer;
            rebuildTransform();
            requestFrame();
        }

        void setAnchor(int x, int y, int z) {
            anchorX = x;
            anchorY = y;
            anchorZ = z;
            rebuildTransform();
            requestFrame();
        }

        void setWorldStates(
            SchematicModel source,
            int[] blockIndices,
            byte[] states,
            int correct,
            int wrong
        ) {
            if (model != source || blockIndices == null || states == null ||
                states.length != blockIndices.length) {
                return;
            }
            worldBlockIndices = blockIndices;
            worldStates = states;
            correctBlocks = correct;
            wrongBlocks = wrong;
            requestFrame();
        }

        void clearWorldStates() {
            worldBlockIndices = null;
            worldStates = null;
            correctBlocks = 0;
            wrongBlocks = 0;
            requestFrame();
        }

        private void rebuildTransform() {
            SchematicModel current = model;
            transform = current == null
                ? null
                : new SchematicPlacementTransform(
                    anchorX,
                    anchorY,
                    anchorZ,
                    current.sizeX(),
                    rotation,
                    mirrored
                );
        }

        void submitCamera(EntityOutlineOverlayController.CameraSample value) {
            camera.update(value);
            requestFrame();
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            SchematicModel current = model;
            SchematicPlacementTransform placement = transform;
            if (current == null || placement == null || getWidth() <= 0 ||
                getHeight() <= 0) return;
            long now = System.nanoTime();
            PacketCameraTracker.State cameraState = camera.frame(
                now,
                fieldOfView,
                false
            );
            if (cameraState == null) return;
            double yaw = Math.toRadians(cameraState.yaw);
            double pitch = Math.toRadians(MotionSmoother.clamp(
                cameraState.pitch,
                -90.0,
                90.0
            ));
            double sinYaw = Math.sin(yaw);
            double cosYaw = Math.cos(yaw);
            double sinPitch = Math.sin(pitch);
            double cosPitch = Math.cos(pitch);
            double focal = ProjectionMath.focalPixels(
                getHeight(),
                cameraState.verticalFov
            );
            int trackedCount = worldBlockIndices != null &&
                    worldStates != null &&
                    worldBlockIndices.length == worldStates.length
                ? worldBlockIndices.length
                : current.boundaryBlockCount();
            int activeBlocks = Math.max(0, trackedCount - correctBlocks);
            int stride = Math.max(1, (activeBlocks + MAX_RENDERED_CUBES - 1) /
                MAX_RENDERED_CUBES);
            int queued = 0;
            int activeIndex = 0;
            for (int listIndex = 0; listIndex < trackedCount;
                ++listIndex) {
                byte worldState = worldStates != null &&
                    listIndex < worldStates.length
                        ? worldStates[listIndex]
                        : BLOCK_UNKNOWN;
                if (worldState == BLOCK_CORRECT) continue;
                if (activeIndex++ % stride != 0) continue;
                boolean wrong = worldState == BLOCK_WRONG;
                int blockIndex = worldBlockIndices != null &&
                        listIndex < worldBlockIndices.length
                    ? worldBlockIndices[listIndex]
                    : current.boundaryBlockIndexAt(listIndex);
                int localY = current.yFromIndex(blockIndex);
                if (selectedLayer >= 0 && localY != selectedLayer) continue;
                int localX = current.xFromIndex(blockIndex);
                int localZ = current.zFromIndex(blockIndex);
                double centerX = transformedX(localX + 0.5, localZ + 0.5);
                double centerZ = transformedZ(localX + 0.5, localZ + 0.5);
                double centerY = anchorY + localY + 0.5;
                double dx = centerX - cameraState.x;
                double dy = centerY - cameraState.y;
                double dz = centerZ - cameraState.z;
                if (dx * dx + dy * dy + dz * dz >
                    (double) maximumDistance * maximumDistance) continue;
                double depth = ProjectionMath.depth(
                    dx,
                    dy,
                    dz,
                    sinYaw,
                    cosYaw,
                    sinPitch,
                    cosPitch
                );
                if (depth <= NEAR_PLANE) continue;
                int exposedFaces = exposedFaces(
                    current,
                    blockIndex,
                    localX,
                    localY,
                    localZ
                );
                renderQueue[queued++].set(
                    blockIndex,
                    localX,
                    localY,
                    localZ,
                    centerX,
                    centerY,
                    centerZ,
                    depth,
                    exposedFaces,
                    wrong
                );
                if (queued >= MAX_RENDERED_CUBES) break;
            }
            Arrays.sort(renderQueue, 0, queued, FAR_TO_NEAR);
            int rendered = 0;
            int firstTextured = Math.max(0, queued - MAX_TEXTURED_CUBES);
            for (int queueIndex = 0; queueIndex < queued; ++queueIndex) {
                ProjectedCube cube = renderQueue[queueIndex];
                if (!projectCube(
                        cube.localX,
                        cube.localY,
                        cube.localZ,
                        cameraState,
                        sinYaw,
                        cosYaw,
                        sinPitch,
                        cosPitch,
                        focal
                    )) {
                    continue;
                }
                int paletteIndex = current.paletteIndexAtLinear(
                    cube.blockIndex
                );
                String state = current.paletteState(paletteIndex);
                int color = cube.wrong ? 0xffff3b30 : blockColor(state);
                boolean textureVisible = !cube.wrong &&
                    queueIndex >= firstTextured &&
                    projectedSpan >= 1.5;
                drawCube(
                    canvas,
                    textureVisible
                        ? textureAtlas.textureFor(
                            state,
                            cameraState.y >= cube.centerY ? "top" : "bottom"
                        )
                        : null,
                    textureVisible
                        ? textureAtlas.textureFor(state, "side")
                        : null,
                    color,
                    cube.centerX,
                    cube.centerY,
                    cube.centerZ,
                    cube.exposedFaces,
                    cameraState,
                    cube.wrong
                );
                ++rendered;
            }
            drawStatus(canvas, current, rendered, stride > 1);
            if (cameraState.animating) requestFrame();
        }

        private boolean projectCube(
            int localX,
            int localY,
            int localZ,
            PacketCameraTracker.State cameraState,
            double sinYaw,
            double cosYaw,
            double sinPitch,
            double cosPitch,
            double focal
        ) {
            double minimumX = Double.POSITIVE_INFINITY;
            double maximumX = Double.NEGATIVE_INFINITY;
            double minimumY = Double.POSITIVE_INFINITY;
            double maximumY = Double.NEGATIVE_INFINITY;
            for (int corner = 0; corner < 8; ++corner) {
                double lx = localX + ((corner & 1) != 0 ? 1.0 : 0.0);
                double lz = localZ + ((corner & 2) != 0 ? 1.0 : 0.0);
                double worldX = transformedX(lx, lz);
                double worldY = anchorY + localY +
                    ((corner & 4) != 0 ? 1.0 : 0.0);
                double worldZ = transformedZ(lx, lz);
                double dx = worldX - cameraState.x;
                double dy = worldY - cameraState.y;
                double dz = worldZ - cameraState.z;
                double depth = ProjectionMath.depth(
                    dx, dy, dz,
                    sinYaw, cosYaw, sinPitch, cosPitch
                );
                if (depth <= NEAR_PLANE) return false;
                double x = getWidth() * 0.5 + ProjectionMath.viewX(
                    dx, dz, sinYaw, cosYaw
                ) * focal / depth;
                double y = getHeight() * 0.5 - ProjectionMath.viewY(
                    dx, dy, dz,
                    sinYaw, cosYaw, sinPitch, cosPitch
                ) * focal / depth;
                screenX[corner] = x;
                screenY[corner] = y;
                minimumX = Math.min(minimumX, x);
                maximumX = Math.max(maximumX, x);
                minimumY = Math.min(minimumY, y);
                maximumY = Math.max(maximumY, y);
            }
            float margin = density * 20f;
            projectedSpan = Math.max(maximumX - minimumX, maximumY - minimumY);
            return maximumX >= -margin && minimumX <= getWidth() + margin &&
                maximumY >= -margin && minimumY <= getHeight() + margin;
        }

        private void drawCube(
            Canvas canvas,
            Bitmap horizontalTexture,
            Bitmap sideTexture,
            int color,
            double centerX,
            double centerY,
            double centerZ,
            int exposedFaces,
            PacketCameraTracker.State cameraState,
            boolean wrong
        ) {
            int fillAlpha = wrong
                ? Math.max(90, Math.round(40f + 95f * opacityPercent / 100f))
                : Math.round(20f + 60f * opacityPercent / 100f);
            int textureAlpha = Math.round(
                50f + 205f * opacityPercent / 100f
            );
            int edgeAlpha = wrong
                ? Math.max(170, Math.round(100f + 120f * opacityPercent / 100f))
                : Math.round(50f + 105f * opacityPercent / 100f);
            fillPaint.setColor((color & 0x00ffffff) | (fillAlpha << 24));
            texturePaint.setAlpha(Math.max(24, Math.min(255, textureAlpha)));
            edgePaint.setColor((color & 0x00ffffff) | (edgeAlpha << 24));
            if (cameraState.y >= centerY) {
                if ((exposedFaces & FACE_TOP) != 0) {
                    drawTexturedFace(canvas, horizontalTexture, TOP_FACE);
                }
            } else if ((exposedFaces & FACE_BOTTOM) != 0) {
                drawTexturedFace(canvas, horizontalTexture, BOTTOM_FACE);
            }
            if ((exposedFaces & FACE_X_MIN) != 0 && normalFacingCamera(
                -1.0, 0.0, centerX, centerZ, cameraState
            )) drawTexturedFace(canvas, sideTexture, X_MIN_FACE);
            if ((exposedFaces & FACE_X_MAX) != 0 && normalFacingCamera(
                1.0, 0.0, centerX, centerZ, cameraState
            )) drawTexturedFace(canvas, sideTexture, X_MAX_FACE);
            if ((exposedFaces & FACE_Z_MIN) != 0 && normalFacingCamera(
                0.0, -1.0, centerX, centerZ, cameraState
            )) drawTexturedFace(canvas, sideTexture, Z_MIN_FACE);
            if ((exposedFaces & FACE_Z_MAX) != 0 && normalFacingCamera(
                0.0, 1.0, centerX, centerZ, cameraState
            )) drawTexturedFace(canvas, sideTexture, Z_MAX_FACE);
            if (projectedSpan >= 5.0) {
                for (int[] edge : EDGES) {
                    canvas.drawLine(
                        (float) screenX[edge[0]],
                        (float) screenY[edge[0]],
                        (float) screenX[edge[1]],
                        (float) screenY[edge[1]],
                        edgePaint
                    );
                }
            }
        }

        private void drawTexturedFace(Canvas canvas, Bitmap texture, int[] face) {
            path.reset();
            path.moveTo((float) screenX[face[0]], (float) screenY[face[0]]);
            for (int index = 1; index < face.length; ++index) {
                path.lineTo(
                    (float) screenX[face[index]],
                    (float) screenY[face[index]]
                );
            }
            path.close();
            canvas.drawPath(path, fillPaint);
            if (texture == null || texture.isRecycled()) return;
            float width = texture.getWidth();
            float height = texture.getHeight();
            sourceQuad[0] = 0f;
            sourceQuad[1] = 0f;
            sourceQuad[2] = width;
            sourceQuad[3] = 0f;
            sourceQuad[4] = width;
            sourceQuad[5] = height;
            sourceQuad[6] = 0f;
            sourceQuad[7] = height;
            for (int index = 0; index < 4; ++index) {
                targetQuad[index * 2] = (float) screenX[face[index]];
                targetQuad[index * 2 + 1] = (float) screenY[face[index]];
            }
            textureMatrix.reset();
            if (textureMatrix.setPolyToPoly(
                sourceQuad,
                0,
                targetQuad,
                0,
                4
            )) {
                canvas.drawBitmap(texture, textureMatrix, texturePaint);
            }
        }

        private boolean normalFacingCamera(
            double localNormalX,
            double localNormalZ,
            double centerX,
            double centerZ,
            PacketCameraTracker.State cameraState
        ) {
            double nx = mirrored ? -localNormalX : localNormalX;
            double nz = localNormalZ;
            double worldNormalX;
            double worldNormalZ;
            switch (rotation) {
                case 1:
                    worldNormalX = -nz;
                    worldNormalZ = nx;
                    break;
                case 2:
                    worldNormalX = -nx;
                    worldNormalZ = -nz;
                    break;
                case 3:
                    worldNormalX = nz;
                    worldNormalZ = -nx;
                    break;
                default:
                    worldNormalX = nx;
                    worldNormalZ = nz;
                    break;
            }
            return worldNormalX * (cameraState.x - centerX) +
                worldNormalZ * (cameraState.z - centerZ) > 0.0;
        }

        private int exposedFaces(
            SchematicModel current,
            int blockIndex,
            int x,
            int y,
            int z
        ) {
            int faces = 0;
            if (selectedLayer >= 0 || current.isAirAt(x, y + 1, z) ||
                isCorrectNeighbour(current, blockIndex, 0, 1, 0)) {
                faces |= FACE_TOP;
            }
            if (selectedLayer >= 0 || current.isAirAt(x, y - 1, z) ||
                isCorrectNeighbour(current, blockIndex, 0, -1, 0)) {
                faces |= FACE_BOTTOM;
            }
            if (current.isAirAt(x - 1, y, z) ||
                isCorrectNeighbour(current, blockIndex, -1, 0, 0)) {
                faces |= FACE_X_MIN;
            }
            if (current.isAirAt(x + 1, y, z) ||
                isCorrectNeighbour(current, blockIndex, 1, 0, 0)) {
                faces |= FACE_X_MAX;
            }
            if (current.isAirAt(x, y, z - 1) ||
                isCorrectNeighbour(current, blockIndex, 0, 0, -1)) {
                faces |= FACE_Z_MIN;
            }
            if (current.isAirAt(x, y, z + 1) ||
                isCorrectNeighbour(current, blockIndex, 0, 0, 1)) {
                faces |= FACE_Z_MAX;
            }
            return faces;
        }

        private boolean isCorrectNeighbour(
            SchematicModel current,
            int blockIndex,
            int dx,
            int dy,
            int dz
        ) {
            if (worldBlockIndices == null || worldStates == null) return false;
            int x = current.xFromIndex(blockIndex) + dx;
            int y = current.yFromIndex(blockIndex) + dy;
            int z = current.zFromIndex(blockIndex) + dz;
            if (x < 0 || x >= current.sizeX() ||
                y < 0 || y >= current.sizeY() ||
                z < 0 || z >= current.sizeZ()) {
                return false;
            }
            int neighbour = (y * current.sizeZ() + z) *
                current.sizeX() + x;
            int listIndex = Arrays.binarySearch(
                worldBlockIndices,
                neighbour
            );
            return listIndex >= 0 && listIndex < worldStates.length &&
                worldStates[listIndex] == BLOCK_CORRECT;
        }

        private static final class ProjectedCube {
            int blockIndex;
            int localX;
            int localY;
            int localZ;
            double centerX;
            double centerY;
            double centerZ;
            double depth;
            int exposedFaces;
            boolean wrong;

            void set(
                int blockIndex,
                int localX,
                int localY,
                int localZ,
                double centerX,
                double centerY,
                double centerZ,
                double depth,
                int exposedFaces,
                boolean wrong
            ) {
                this.blockIndex = blockIndex;
                this.localX = localX;
                this.localY = localY;
                this.localZ = localZ;
                this.centerX = centerX;
                this.centerY = centerY;
                this.centerZ = centerZ;
                this.depth = depth;
                this.exposedFaces = exposedFaces;
                this.wrong = wrong;
            }
        }

        @Override
        protected void onDetachedFromWindow() {
            textureAtlas.close();
            super.onDetachedFromWindow();
        }

        private void requestFrame() {
            postInvalidateOnAnimation();
        }

        private void drawStatus(
            Canvas canvas,
            SchematicModel current,
            int rendered,
            boolean limited
        ) {
            String text = String.format(
                Locale.getDefault(),
                "СХЕМА • %s • %,d%s • готово %,d • ошибок %,d • Mojang %d%s",
                selectedLayer >= 0 ? "слой Y=" + selectedLayer : "все слои",
                rendered,
                limited ? " видимых (лимит)" : " видимых",
                correctBlocks,
                wrongBlocks,
                textureAtlas.officialTextureCount(),
                textureAtlas.pendingTextureCount() > 0
                    ? " (загрузка " + textureAtlas.pendingTextureCount() + ")"
                    : ""
            );
            float padding = density * 7f;
            float width = statusPaint.measureText(text) + padding * 2f;
            float left = (getWidth() - width) * 0.5f;
            float top = density * 5f;
            canvas.drawRoundRect(
                left,
                top,
                left + width,
                top + density * 25f,
                density * 8f,
                density * 8f,
                statusBackground
            );
            canvas.drawText(
                text,
                left + padding,
                top + density * 17f,
                statusPaint
            );
        }

        private double transformedX(double x, double z) {
            SchematicPlacementTransform current = transform;
            return current == null ? anchorX + x : current.worldX(x, z);
        }

        private double transformedZ(double x, double z) {
            SchematicPlacementTransform current = transform;
            return current == null ? anchorZ + z : current.worldZ(x, z);
        }

        private static int blockColor(String state) {
            String value = state == null ? "" : state.toLowerCase(Locale.ROOT);
            if (value.contains("water") || value.contains("ice")) return 0xff45a7ff;
            if (value.contains("lava") || value.contains("magma")) return 0xffff7b35;
            if (value.contains("leaves") || value.contains("grass") ||
                value.contains("moss") || value.contains("vine")) return 0xff5bd36f;
            if (value.contains("wood") || value.contains("planks") ||
                value.contains("log") || value.contains("stem")) return 0xffc99455;
            if (value.contains("glass")) return 0xff63e1ee;
            if (value.contains("redstone") || value.contains("nether_wart")) {
                return 0xffff5964;
            }
            if (value.contains("sand") || value.contains("end_stone")) return 0xffffd978;
            if (value.contains("deepslate") || value.contains("blackstone")) {
                return 0xff718093;
            }
            if (value.contains("stone") || value.contains("ore") ||
                value.contains("brick") || value.startsWith("legacy:")) {
                return 0xffaeb9c7;
            }
            int hash = value.hashCode();
            float hue = Math.floorMod(hash, 360);
            return Color.HSVToColor(new float[] { hue, 0.52f, 0.96f });
        }
    }
}
