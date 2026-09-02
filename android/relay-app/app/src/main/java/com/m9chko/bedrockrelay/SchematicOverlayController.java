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
    private final Context context;
    private final SharedPreferences preferences;
    private final WindowManager windowManager;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final AtomicReference<EntityOutlineOverlayController.CameraSample>
        pendingCamera = new AtomicReference<>();
    private final AtomicBoolean deliveryPosted = new AtomicBoolean(false);

    private volatile boolean sessionVisible;
    private volatile boolean uiBlocked;
    private volatile boolean enabled;
    private volatile int fieldOfView = 70;
    private volatile int opacityPercent = 42;
    private volatile int maximumDistance = 96;
    private volatile int rotationQuarterTurns;
    private volatile boolean mirrored;
    private volatile int selectedLayer = -1;
    private volatile SchematicModel model;
    private volatile EntityOutlineOverlayController.CameraSample latestCamera =
        EntityOutlineOverlayController.CameraSample.unknown();
    private boolean placementPending;
    private boolean placementTargetCaptured;
    private long placementRequest;
    private long placementDeadlineMs;
    private int placementX;
    private int placementZ;
    private double placementFallbackY;
    private boolean missingPermissionLogged;
    private SchematicView view;

    SchematicOverlayController(
        Context context,
        SharedPreferences preferences
    ) {
        this.context = context;
        this.preferences = preferences;
        windowManager = (WindowManager) context.getSystemService(
            Context.WINDOW_SERVICE
        );
    }

    void setSessionVisible(boolean visible) {
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
        int opacity,
        int distance,
        int rotation,
        boolean mirrored,
        int layer
    ) {
        this.enabled = enabled;
        fieldOfView = RelayService.clampEntityFov(fov);
        opacityPercent = RelayService.clampSchematicOpacity(opacity);
        maximumDistance = RelayService.clampSchematicDistance(distance);
        rotationQuarterTurns = Math.floorMod(rotation, 4);
        this.mirrored = mirrored;
        selectedLayer = layer;
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
        model = value;
        SchematicView current = view;
        if (current != null) current.setModel(value);
        reconcileWindow();
    }

    SchematicModel model() {
        return model;
    }

    boolean wantsFrames() {
        return sessionVisible && enabled && !uiBlocked && model != null &&
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
        if (placementPending && camera.known) tryPlaceNearCamera(camera);
        postDelivery();
    }

    boolean placeNearCamera() {
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
        int x = preferences.getInt(RelayService.KEY_SCHEMATIC_ANCHOR_X, 0);
        double y = savedAnchorY();
        int z = preferences.getInt(RelayService.KEY_SCHEMATIC_ANCHOR_Z, 0);
        saveAnchor(x + dx, y + dy, z + dz);
    }

    void hideImmediately() {
        sessionVisible = false;
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
        placementPending = true;
        placementTargetCaptured = false;
        placementRequest = request;
        placementDeadlineMs = 0L;
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
        double anchorY = collisionKnown ? surfaceY : placementFallbackY;
        saveAnchor(placementX, anchorY, placementZ);
        DiagnosticsLog.append(
            context,
            "INFO",
            "schematics",
            "Schematic anchor fixed at X=" + placementX +
                " Y=" + anchorY + " Z=" + placementZ +
                " ground=" + (collisionKnown ? "block_collision" : "camera_fallback")
        );
        return true;
    }

    private double savedAnchorY() {
        return preferences.getFloat(
            RelayService.KEY_SCHEMATIC_ANCHOR_Y_EXACT,
            preferences.getInt(RelayService.KEY_SCHEMATIC_ANCHOR_Y, 0)
        );
    }

    private void saveAnchor(int x, double y, int z) {
        SharedPreferences.Editor editor = preferences.edit()
            .putInt(RelayService.KEY_SCHEMATIC_ANCHOR_X, x)
            .putInt(RelayService.KEY_SCHEMATIC_ANCHOR_Y, (int) Math.floor(y))
            .putFloat(RelayService.KEY_SCHEMATIC_ANCHOR_Y_EXACT, (float) y)
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

    private void postDelivery() {
        if (!deliveryPosted.compareAndSet(false, true)) return;
        mainHandler.post(() -> {
            EntityOutlineOverlayController.CameraSample camera =
                pendingCamera.getAndSet(null);
            SchematicView current = view;
            if (current != null && camera != null) current.submitCamera(camera);
            deliveryPosted.set(false);
            if (pendingCamera.get() != null) postDelivery();
        });
    }

    private void reconcileWindow() {
        if (sessionVisible && enabled && !uiBlocked && model != null &&
            preferences.getBoolean(
                RelayService.KEY_SCHEMATIC_PLACED,
                false
            )) {
            addWindow();
        } else {
            removeWindow();
        }
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
        added.setAnchor(
            preferences.getInt(RelayService.KEY_SCHEMATIC_ANCHOR_X, 0),
            savedAnchorY(),
            preferences.getInt(RelayService.KEY_SCHEMATIC_ANCHOR_Z, 0)
        );
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
        private static final int MAX_TEXTURED_CUBES = 1_200;
        private static final long FRAME_INTERVAL_MILLIS = 16L;
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
        private double anchorY;
        private int anchorZ;
        private double projectedSpan;
        private long lastFrameRequestAtMs;
        private boolean delayedFramePosted;

        SchematicView(Context context) {
            super(context);
            textureAtlas = new SchematicTextureAtlas(
                context,
                () -> requestFrame(false)
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
            requestFrame(true);
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
            requestFrame(true);
        }

        void setAnchor(int x, double y, int z) {
            anchorX = x;
            anchorY = y;
            anchorZ = z;
            requestFrame(true);
        }

        void submitCamera(EntityOutlineOverlayController.CameraSample value) {
            camera.update(value);
            requestFrame(false);
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            SchematicModel current = model;
            if (current == null || getWidth() <= 0 || getHeight() <= 0) return;
            long now = System.nanoTime();
            PacketCameraTracker.State cameraState = camera.frame(
                now,
                fieldOfView
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
            int boundaryCount = current.boundaryBlockCount();
            int stride = Math.max(1, (boundaryCount + MAX_RENDERED_CUBES - 1) /
                MAX_RENDERED_CUBES);
            int queued = 0;
            for (int listIndex = 0; listIndex < boundaryCount;
                listIndex += stride) {
                int blockIndex = current.boundaryBlockIndexAt(listIndex);
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
                    exposedFaces
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
                int color = blockColor(state);
                boolean textureVisible = queueIndex >= firstTextured &&
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
                    cameraState
                );
                ++rendered;
            }
            drawStatus(canvas, current, rendered, stride > 1);
            if (cameraState.animating) requestFrame(false);
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
            PacketCameraTracker.State cameraState
        ) {
            int fillAlpha = Math.round(25f + 55f * opacityPercent / 100f);
            int textureAlpha = Math.round(
                185f + 70f * opacityPercent / 100f
            );
            int edgeAlpha = Math.round(55f + 95f * opacityPercent / 100f);
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
            int x,
            int y,
            int z
        ) {
            int faces = 0;
            if (selectedLayer >= 0 || current.isAirAt(x, y + 1, z)) {
                faces |= FACE_TOP;
            }
            if (selectedLayer >= 0 || current.isAirAt(x, y - 1, z)) {
                faces |= FACE_BOTTOM;
            }
            if (current.isAirAt(x - 1, y, z)) faces |= FACE_X_MIN;
            if (current.isAirAt(x + 1, y, z)) faces |= FACE_X_MAX;
            if (current.isAirAt(x, y, z - 1)) faces |= FACE_Z_MIN;
            if (current.isAirAt(x, y, z + 1)) faces |= FACE_Z_MAX;
            return faces;
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

            void set(
                int blockIndex,
                int localX,
                int localY,
                int localZ,
                double centerX,
                double centerY,
                double centerZ,
                double depth,
                int exposedFaces
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
            }
        }

        @Override
        protected void onDetachedFromWindow() {
            textureAtlas.close();
            super.onDetachedFromWindow();
        }

        private void requestFrame(boolean immediate) {
            long now = SystemClock.uptimeMillis();
            if (immediate || now - lastFrameRequestAtMs >=
                FRAME_INTERVAL_MILLIS) {
                lastFrameRequestAtMs = now;
                postInvalidateOnAnimation();
                return;
            }
            if (delayedFramePosted) return;
            delayedFramePosted = true;
            long delay = Math.max(
                1L,
                FRAME_INTERVAL_MILLIS - (now - lastFrameRequestAtMs)
            );
            postDelayed(() -> {
                delayedFramePosted = false;
                lastFrameRequestAtMs = SystemClock.uptimeMillis();
                postInvalidateOnAnimation();
            }, delay);
        }

        private void drawStatus(
            Canvas canvas,
            SchematicModel current,
            int rendered,
            boolean limited
        ) {
            String text = String.format(
                Locale.getDefault(),
                "СХЕМА • %s • %,d%s • Mojang %d%s",
                selectedLayer >= 0 ? "слой Y=" + selectedLayer : "все слои",
                rendered,
                limited ? " видимых (лимит)" : " видимых",
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
            SchematicModel current = model;
            if (mirrored && current != null) x = current.sizeX() - x;
            switch (rotation) {
                case 1: return anchorX - z;
                case 2: return anchorX - x;
                case 3: return anchorX + z;
                default: return anchorX + x;
            }
        }

        private double transformedZ(double x, double z) {
            SchematicModel current = model;
            if (mirrored && current != null) x = current.sizeX() - x;
            switch (rotation) {
                case 1: return anchorZ + x;
                case 2: return anchorZ - z;
                case 3: return anchorZ - x;
                default: return anchorZ + z;
            }
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
