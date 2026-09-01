package com.m9chko.bedrockrelay;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.RectF;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

/** Click-through, session-scoped 2D entity boxes rendered over Minecraft. */
final class EntityOutlineOverlayController {
    private final Context context;
    private final WindowManager windowManager;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final AtomicReference<Frame> pendingFrame =
        new AtomicReference<>();
    private final AtomicReference<CameraSample> pendingCamera =
        new AtomicReference<>();
    private final AtomicBoolean deliveryPosted = new AtomicBoolean(false);

    private volatile boolean sessionVisible;
    private volatile boolean enabled = true;
    private volatile int fieldOfView = 70;
    private boolean missingPermissionLogged;
    private EntityOutlineView outlineView;

    EntityOutlineOverlayController(Context context) {
        this.context = context;
        this.windowManager = (WindowManager) context.getSystemService(
            Context.WINDOW_SERVICE
        );
    }

    void setSessionVisible(boolean visible) {
        sessionVisible = visible;
        reconcileWindow();
    }

    void setEnabled(boolean value) {
        enabled = value;
        reconcileWindow();
    }

    void setFieldOfView(int value) {
        fieldOfView = RelayService.clampEntityFov(value);
        if (outlineView != null) {
            outlineView.setFieldOfView(fieldOfView);
        }
    }

    boolean wantsFrames() {
        return sessionVisible && enabled;
    }

    void offerSnapshot(String json) throws Exception {
        if (!wantsFrames()) return;
        Frame frame = Frame.parse(json);
        pendingFrame.set(frame);
        postFrameDelivery();
    }

    void offerCameraSnapshot(String json) throws Exception {
        if (!wantsFrames()) return;
        CameraSample camera = CameraSample.from(new JSONObject(json));
        pendingCamera.set(camera);
        postFrameDelivery();
    }

    void hideImmediately() {
        sessionVisible = false;
        removeWindow();
    }

    private void postFrameDelivery() {
        if (!deliveryPosted.compareAndSet(false, true)) return;
        mainHandler.post(() -> {
            Frame frame = pendingFrame.getAndSet(null);
            CameraSample camera = pendingCamera.getAndSet(null);
            if (outlineView != null) {
                if (frame != null) outlineView.submit(frame);
                if (camera != null) outlineView.submitCamera(camera);
            }
            deliveryPosted.set(false);
            if (pendingFrame.get() != null || pendingCamera.get() != null) {
                postFrameDelivery();
            }
        });
    }

    private void reconcileWindow() {
        if (sessionVisible && enabled) {
            addWindow();
        } else {
            removeWindow();
        }
    }

    private void addWindow() {
        if (outlineView != null) return;
        if (!Settings.canDrawOverlays(context)) {
            if (!missingPermissionLogged) {
                missingPermissionLogged = true;
                DiagnosticsLog.append(
                    context,
                    "WARN",
                    "entities",
                    "Overlay permission is missing; entity boxes were not shown"
                );
            }
            return;
        }

        missingPermissionLogged = false;
        EntityOutlineView view = new EntityOutlineView(context);
        view.setFieldOfView(fieldOfView);
        view.setSystemUiVisibility(
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
        if (Build.VERSION.SDK_INT >= 28) {
            params.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }

        try {
            windowManager.addView(view, params);
            outlineView = view;
            Frame frame = pendingFrame.get();
            if (frame != null) view.submit(frame);
            CameraSample camera = pendingCamera.get();
            if (camera != null) view.submitCamera(camera);
            DiagnosticsLog.append(
                context,
                "INFO",
                "entities",
                "Entity outline overlay opened; mode=2d_boxes " +
                    "smoothing=world+global_camera_direct+stationary_screen " +
                    "predictor=monotonic cameraSample=atomic cameraPollMs=12"
            );
        } catch (Throwable error) {
            outlineView = null;
            DiagnosticsLog.appendError(
                context,
                "entities",
                "Failed to open entity outline overlay",
                error
            );
        }
    }

    private void removeWindow() {
        EntityOutlineView view = outlineView;
        outlineView = null;
        pendingFrame.set(null);
        pendingCamera.set(null);
        if (view == null) return;
        try {
            windowManager.removeViewImmediate(view);
            DiagnosticsLog.append(
                context,
                "INFO",
                "entities",
                "Entity outline overlay closed"
            );
        } catch (Throwable error) {
            DiagnosticsLog.appendError(
                context,
                "entities",
                "Failed to close entity outline overlay",
                error
            );
        }
    }

    static final class Frame {
        final CameraSample camera;
        final List<EntitySample> entities;
        final int totalTrackedEntities;
        final long parseFailures;

        Frame(
            CameraSample camera,
            List<EntitySample> entities,
            int totalTrackedEntities,
            long parseFailures
        ) {
            this.camera = camera;
            this.entities = entities;
            this.totalTrackedEntities = totalTrackedEntities;
            this.parseFailures = parseFailures;
        }

        static Frame parse(String json) throws Exception {
            JSONObject root = new JSONObject(json);
            JSONObject cameraObject = root.optJSONObject("camera");
            CameraSample camera = cameraObject == null
                ? CameraSample.unknown()
                : CameraSample.from(cameraObject);

            JSONArray array = root.optJSONArray("entities");
            int count = array == null ? 0 : Math.min(array.length(), 96);
            List<EntitySample> entities = new ArrayList<>(count);
            for (int index = 0; index < count; ++index) {
                JSONObject value = array.optJSONObject(index);
                if (value == null) continue;
                EntitySample entity = EntitySample.from(value);
                if (entity != null) entities.add(entity);
            }
            return new Frame(
                camera,
                entities,
                root.optInt("totalTrackedEntities", entities.size()),
                root.optLong("parseFailures", 0)
            );
        }
    }

    private static final class CameraSample {
        final boolean known;
        final double x;
        final double y;
        final double z;
        final double pitch;
        final double yaw;
        final long updatedAtMs;
        final long ageMs;

        CameraSample(
            boolean known,
            double x,
            double y,
            double z,
            double pitch,
            double yaw,
            long updatedAtMs,
            long ageMs
        ) {
            this.known = known;
            this.x = x;
            this.y = y;
            this.z = z;
            this.pitch = pitch;
            this.yaw = yaw;
            this.updatedAtMs = updatedAtMs;
            this.ageMs = ageMs;
        }

        static CameraSample unknown() {
            return new CameraSample(false, 0, 0, 0, 0, 0, 0, 0);
        }

        static CameraSample from(JSONObject value) {
            boolean known = value.optBoolean("known", false);
            double x = value.optDouble("x", 0);
            double y = value.optDouble("y", 0);
            double z = value.optDouble("z", 0);
            double pitch = value.optDouble("pitch", 0);
            double yaw = value.optDouble("yaw", 0);
            if (!finite(x, y, z, pitch, yaw)) known = false;
            return new CameraSample(
                known,
                x,
                y,
                z,
                pitch,
                yaw,
                value.optLong("updatedAtMs", 0),
                Math.max(0, value.optLong("ageMs", 0))
            );
        }
    }

    private static final class EntitySample {
        final String id;
        final String label;
        final boolean player;
        final double x;
        final double y;
        final double z;
        final double width;
        final double height;
        final long updatedAtMs;
        final long ageMs;

        EntitySample(
            String id,
            String label,
            boolean player,
            double x,
            double y,
            double z,
            double width,
            double height,
            long updatedAtMs,
            long ageMs
        ) {
            this.id = id;
            this.label = label;
            this.player = player;
            this.x = x;
            this.y = y;
            this.z = z;
            this.width = width;
            this.height = height;
            this.updatedAtMs = updatedAtMs;
            this.ageMs = ageMs;
        }

        static EntitySample from(JSONObject value) {
            String id = value.optString("id", "");
            if (id.isEmpty()) return null;
            String label = value.optString("label", "Сущность")
                .replace('_', ' ')
                .replace('\n', ' ')
                .replace('\r', ' ');
            if (label.length() > 32) label = label.substring(0, 32);
            double x = value.optDouble("x", Double.NaN);
            double y = value.optDouble("y", Double.NaN);
            double z = value.optDouble("z", Double.NaN);
            double width = value.optDouble("width", 0.8);
            double height = value.optDouble("height", 1.8);
            if (!finite(x, y, z, width, height) || width <= 0 || height <= 0) {
                return null;
            }
            return new EntitySample(
                id,
                label,
                value.optBoolean("player", false),
                x,
                y,
                z,
                Math.min(width, 16.0),
                Math.min(height, 16.0),
                value.optLong("updatedAtMs", 0),
                Math.max(0, value.optLong("ageMs", 0))
            );
        }
    }

    private static boolean finite(double... values) {
        for (double value : values) {
            if (!Double.isFinite(value)) return false;
        }
        return true;
    }

    private static final class RenderTrack {
        private static final long LinearPredictionNanos = 65_000_000L;
        private static final long MaxPredictionNanos = 130_000_000L;
        private static final long RenderLeadNanos = 18_000_000L;
        private static final long MaximumFrameGapNanos = 250_000_000L;
        private static final double VelocityResponseSeconds = 0.075;

        final String id;
        String label;
        boolean player;
        final MotionSmoother.Axis renderX = new MotionSmoother.Axis();
        final MotionSmoother.Axis renderY = new MotionSmoother.Axis();
        final MotionSmoother.Axis renderZ = new MotionSmoother.Axis();
        final MotionSmoother.ScreenBox screenBox =
            new MotionSmoother.ScreenBox();
        double sampleX;
        double sampleY;
        double sampleZ;
        double velocityX;
        double velocityY;
        double velocityZ;
        double width;
        double height;
        long seenGeneration;
        long nativeUpdatedAtMs;
        long sampleReceivedNanos;
        long sampleAgeNanos;
        long lastFrameNanos;
        String displayLabel = "";
        String displayText = "";
        int displayDistance = Integer.MIN_VALUE;
        float displayTextWidth;

        RenderTrack(EntitySample sample, long now, long generation) {
            id = sample.id;
            label = sample.label;
            player = sample.player;
            sampleX = sample.x;
            sampleY = sample.y;
            sampleZ = sample.z;
            renderX.reset(sample.x, 0.0);
            renderY.reset(sample.y, 0.0);
            renderZ.reset(sample.z, 0.0);
            width = sample.width;
            height = sample.height;
            seenGeneration = generation;
            nativeUpdatedAtMs = sample.updatedAtMs;
            sampleReceivedNanos = now;
            sampleAgeNanos = Math.min(sample.ageMs, 500) * 1_000_000L;
            lastFrameNanos = now;
        }

        void retarget(EntitySample sample, long now, long generation) {
            seenGeneration = generation;
            label = sample.label;
            player = sample.player;
            width = sample.width;
            height = sample.height;
            if (nativeUpdatedAtMs > 0 && sample.updatedAtMs > 0 &&
                sample.updatedAtMs < nativeUpdatedAtMs) {
                return;
            }
            if (sample.updatedAtMs == nativeUpdatedAtMs &&
                sample.x == sampleX && sample.y == sampleY &&
                sample.z == sampleZ) return;

            double deltaX = sample.x - sampleX;
            double deltaY = sample.y - sampleY;
            double deltaZ = sample.z - sampleZ;
            double elapsedSeconds = (sample.updatedAtMs -
                nativeUpdatedAtMs) / 1000.0;
            boolean teleport = deltaX * deltaX + deltaY * deltaY +
                deltaZ * deltaZ > 64.0;
            if (!teleport && elapsedSeconds >= 0.005 &&
                elapsedSeconds <= 0.5) {
                velocityX = MotionSmoother.filterVelocity(
                    velocityX,
                    MotionSmoother.clamp(
                        deltaX / elapsedSeconds,
                        -80.0,
                        80.0
                    ),
                    elapsedSeconds,
                    VelocityResponseSeconds
                );
                velocityY = MotionSmoother.filterVelocity(
                    velocityY,
                    MotionSmoother.clamp(
                        deltaY / elapsedSeconds,
                        -80.0,
                        80.0
                    ),
                    elapsedSeconds,
                    VelocityResponseSeconds
                );
                velocityZ = MotionSmoother.filterVelocity(
                    velocityZ,
                    MotionSmoother.clamp(
                        deltaZ / elapsedSeconds,
                        -80.0,
                        80.0
                    ),
                    elapsedSeconds,
                    VelocityResponseSeconds
                );
            } else if (teleport) {
                velocityX = velocityY = velocityZ = 0.0;
                renderX.reset(sample.x, 0.0);
                renderY.reset(sample.y, 0.0);
                renderZ.reset(sample.z, 0.0);
            } else if (elapsedSeconds > 0.5) {
                // Do not extrapolate a fresh packet with velocity measured
                // before a long network pause.
                velocityX = velocityY = velocityZ = 0.0;
            }

            sampleX = sample.x;
            sampleY = sample.y;
            sampleZ = sample.z;
            nativeUpdatedAtMs = sample.updatedAtMs;
            sampleReceivedNanos = now;
            sampleAgeNanos = Math.min(sample.ageMs, 500) * 1_000_000L;
        }

        boolean advance(long now) {
            long elapsedSinceReceipt = Math.max(0, now - sampleReceivedNanos);
            long totalAgeNanos = Math.min(
                10_000_000_000L,
                sampleAgeNanos + elapsedSinceReceipt
            );
            double predictionSeconds = MotionSmoother.predictionSeconds(
                totalAgeNanos,
                RenderLeadNanos,
                LinearPredictionNanos,
                MaxPredictionNanos
            );
            double predictionVelocityScale =
                MotionSmoother.predictionVelocityScale(
                    totalAgeNanos,
                    RenderLeadNanos,
                    LinearPredictionNanos,
                    MaxPredictionNanos
                );
            boolean predictionActive = predictionVelocityScale > 0.0001;
            double targetVelocityX = velocityX * predictionVelocityScale;
            double targetVelocityY = velocityY * predictionVelocityScale;
            double targetVelocityZ = velocityZ * predictionVelocityScale;
            double targetX = sampleX + velocityX * predictionSeconds;
            double targetY = sampleY + velocityY * predictionSeconds;
            double targetZ = sampleZ + velocityZ * predictionSeconds;

            long frameGap = Math.max(0, now - lastFrameNanos);
            if (lastFrameNanos == 0 || frameGap > MaximumFrameGapNanos) {
                renderX.reset(targetX, targetVelocityX);
                renderY.reset(targetY, targetVelocityY);
                renderZ.reset(targetZ, targetVelocityZ);
            } else {
                double deltaSeconds = frameGap / 1_000_000_000.0;
                double speed = Math.sqrt(
                    velocityX * velocityX + velocityY * velocityY +
                        velocityZ * velocityZ
                );
                double smoothTime = 0.095 -
                    MotionSmoother.clamp(speed / 20.0, 0.0, 1.0) * 0.025;
                renderX.step(
                    targetX,
                    targetVelocityX,
                    smoothTime,
                    deltaSeconds
                );
                renderY.step(
                    targetY,
                    targetVelocityY,
                    smoothTime,
                    deltaSeconds
                );
                renderZ.step(
                    targetZ,
                    targetVelocityZ,
                    smoothTime,
                    deltaSeconds
                );
            }
            lastFrameNanos = now;

            return (predictionActive &&
                    (Math.abs(velocityX) > 0.002 ||
                     Math.abs(velocityY) > 0.002 ||
                     Math.abs(velocityZ) > 0.002)) ||
                !renderX.isSettled(targetX, targetVelocityX, 0.0005, 0.005) ||
                !renderY.isSettled(targetY, targetVelocityY, 0.0005, 0.005) ||
                !renderZ.isSettled(targetZ, targetVelocityZ, 0.0005, 0.005);
        }
    }

    private static final class EntityOutlineView extends View {
        private static final long LinearCameraPredictionNanos = 40_000_000L;
        private static final long MaxCameraPredictionNanos = 95_000_000L;
        private static final long CameraRenderLeadNanos = 18_000_000L;
        private static final long MaximumFrameGapNanos = 250_000_000L;
        private static final double CameraPositionVelocityResponse = 0.045;
        private static final double CameraAngleVelocityResponse = 0.028;
        private static final double NearPlane = 0.12;

        private final Map<String, RenderTrack> tracks = new HashMap<>();
        private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint glowPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint outlinePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint textBackgroundPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF rectangle = new RectF();
        private final RectF labelRectangle = new RectF();
        private final float density;

        private boolean cameraKnown;
        private final MotionSmoother.Axis renderedCameraX =
            new MotionSmoother.Axis();
        private final MotionSmoother.Axis renderedCameraY =
            new MotionSmoother.Axis();
        private final MotionSmoother.Axis renderedCameraZ =
            new MotionSmoother.Axis();
        private final MotionSmoother.Axis renderedCameraPitch =
            new MotionSmoother.Axis();
        private final MotionSmoother.Axis renderedCameraYaw =
            new MotionSmoother.Axis();
        private double cameraSampleX;
        private double cameraSampleY;
        private double cameraSampleZ;
        private double cameraSamplePitch;
        private double cameraSampleYaw;
        private double cameraVelocityX;
        private double cameraVelocityY;
        private double cameraVelocityZ;
        private double cameraVelocityPitch;
        private double cameraVelocityYaw;
        private long cameraUpdatedAtMs;
        private long cameraReceivedNanos;
        private long cameraSampleAgeNanos;
        private long cameraLastFrameNanos;
        private long generation;
        private int fieldOfView = 70;

        EntityOutlineView(Context context) {
            super(context);
            density = context.getResources().getDisplayMetrics().density;
            setBackgroundColor(Color.TRANSPARENT);
            setWillNotDraw(false);

            fillPaint.setStyle(Paint.Style.FILL);
            glowPaint.setStyle(Paint.Style.STROKE);
            glowPaint.setStrokeWidth(Math.max(3f, density * 4.5f));
            outlinePaint.setStyle(Paint.Style.STROKE);
            outlinePaint.setStrokeWidth(Math.max(1.5f, density * 1.7f));
            textPaint.setColor(Color.WHITE);
            textPaint.setTextSize(Math.max(11f, density * 10.5f));
            textPaint.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
            textBackgroundPaint.setColor(0xcc111720);
            textBackgroundPaint.setStyle(Paint.Style.FILL);
        }

        void setFieldOfView(int value) {
            fieldOfView = RelayService.clampEntityFov(value);
            postInvalidateOnAnimation();
        }

        void submit(Frame frame) {
            long now = System.nanoTime();
            ++generation;
            updateCamera(frame.camera, now);

            for (EntitySample sample : frame.entities) {
                RenderTrack track = tracks.get(sample.id);
                if (track == null) {
                    tracks.put(sample.id, new RenderTrack(sample, now, generation));
                } else {
                    track.retarget(sample, now, generation);
                }
            }
            Iterator<RenderTrack> iterator = tracks.values().iterator();
            while (iterator.hasNext()) {
                if (iterator.next().seenGeneration != generation) {
                    iterator.remove();
                }
            }
            postInvalidateOnAnimation();
        }

        void submitCamera(CameraSample camera) {
            updateCamera(camera, System.nanoTime());
            postInvalidateOnAnimation();
        }

        private void updateCamera(CameraSample sample, long now) {
            if (!sample.known) {
                cameraKnown = false;
                return;
            }
            if (!cameraKnown) {
                cameraKnown = true;
                cameraSampleX = sample.x;
                cameraSampleY = sample.y;
                cameraSampleZ = sample.z;
                cameraSamplePitch = sample.pitch;
                cameraSampleYaw = sample.yaw;
                cameraVelocityX = cameraVelocityY = cameraVelocityZ = 0;
                cameraVelocityPitch = cameraVelocityYaw = 0;
                renderedCameraX.reset(sample.x, 0.0);
                renderedCameraY.reset(sample.y, 0.0);
                renderedCameraZ.reset(sample.z, 0.0);
                renderedCameraPitch.reset(sample.pitch, 0.0);
                renderedCameraYaw.reset(sample.yaw, 0.0);
                cameraLastFrameNanos = now;
            } else {
                if (cameraUpdatedAtMs > 0 && sample.updatedAtMs > 0 &&
                    sample.updatedAtMs < cameraUpdatedAtMs) return;

                double unwrappedYaw = MotionSmoother.unwrapAngle(
                    cameraSampleYaw,
                    sample.yaw
                );
                boolean duplicate = sample.updatedAtMs == cameraUpdatedAtMs &&
                    sample.x == cameraSampleX && sample.y == cameraSampleY &&
                    sample.z == cameraSampleZ &&
                    sample.pitch == cameraSamplePitch &&
                    unwrappedYaw == cameraSampleYaw;
                if (duplicate) return;

                double elapsedSeconds = (sample.updatedAtMs -
                    cameraUpdatedAtMs) / 1000.0;
                double deltaX = sample.x - cameraSampleX;
                double deltaY = sample.y - cameraSampleY;
                double deltaZ = sample.z - cameraSampleZ;
                boolean teleport = deltaX * deltaX + deltaY * deltaY +
                    deltaZ * deltaZ > 64.0;
                if (elapsedSeconds >= 0.005 && elapsedSeconds <= 0.25) {
                    cameraVelocityX = filteredCameraVelocity(
                        cameraVelocityX,
                        deltaX,
                        elapsedSeconds,
                        100.0,
                        CameraPositionVelocityResponse
                    );
                    cameraVelocityY = filteredCameraVelocity(
                        cameraVelocityY,
                        deltaY,
                        elapsedSeconds,
                        100.0,
                        CameraPositionVelocityResponse
                    );
                    cameraVelocityZ = filteredCameraVelocity(
                        cameraVelocityZ,
                        deltaZ,
                        elapsedSeconds,
                        100.0,
                        CameraPositionVelocityResponse
                    );
                    cameraVelocityPitch = filteredCameraVelocity(
                        cameraVelocityPitch,
                        sample.pitch - cameraSamplePitch,
                        elapsedSeconds,
                        2160.0,
                        CameraAngleVelocityResponse
                    );
                    cameraVelocityYaw = filteredCameraVelocity(
                        cameraVelocityYaw,
                        unwrappedYaw - cameraSampleYaw,
                        elapsedSeconds,
                        2160.0,
                        CameraAngleVelocityResponse
                    );
                } else if (elapsedSeconds > 0.25) {
                    cameraVelocityX = cameraVelocityY = cameraVelocityZ = 0;
                    cameraVelocityPitch = cameraVelocityYaw = 0;
                }
                if (teleport) {
                    cameraVelocityX = cameraVelocityY = cameraVelocityZ = 0;
                    renderedCameraX.reset(sample.x, 0.0);
                    renderedCameraY.reset(sample.y, 0.0);
                    renderedCameraZ.reset(sample.z, 0.0);
                }

                cameraSampleX = sample.x;
                cameraSampleY = sample.y;
                cameraSampleZ = sample.z;
                cameraSamplePitch = sample.pitch;
                cameraSampleYaw = unwrappedYaw;
            }

            cameraUpdatedAtMs = sample.updatedAtMs;
            cameraReceivedNanos = now;
            cameraSampleAgeNanos = Math.min(sample.ageMs, 500) *
                1_000_000L;
        }

        private static double filteredCameraVelocity(
            double current,
            double delta,
            double elapsedSeconds,
            double maximum,
            double responseSeconds
        ) {
            double measured = Math.abs(delta) < 0.00001
                ? 0.0
                : MotionSmoother.clamp(
                    delta / elapsedSeconds,
                    -maximum,
                    maximum
                );
            return MotionSmoother.filterVelocity(
                current,
                measured,
                elapsedSeconds,
                responseSeconds
            );
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            if (!cameraKnown || getWidth() <= 0 || getHeight() <= 0) return;

            long now = System.nanoTime();
            long elapsedSinceReceipt = Math.max(0, now - cameraReceivedNanos);
            long totalCameraAgeNanos = Math.min(
                10_000_000_000L,
                cameraSampleAgeNanos + elapsedSinceReceipt
            );
            double predictionSeconds = MotionSmoother.predictionSeconds(
                totalCameraAgeNanos,
                CameraRenderLeadNanos,
                LinearCameraPredictionNanos,
                MaxCameraPredictionNanos
            );
            double predictionVelocityScale =
                MotionSmoother.predictionVelocityScale(
                    totalCameraAgeNanos,
                    CameraRenderLeadNanos,
                    LinearCameraPredictionNanos,
                    MaxCameraPredictionNanos
                );
            boolean predictionActive = predictionVelocityScale > 0.0001;
            double targetVelocityX = cameraVelocityX *
                predictionVelocityScale;
            double targetVelocityY = cameraVelocityY *
                predictionVelocityScale;
            double targetVelocityZ = cameraVelocityZ *
                predictionVelocityScale;
            double targetVelocityPitch = cameraVelocityPitch *
                predictionVelocityScale;
            double targetVelocityYaw = cameraVelocityYaw *
                predictionVelocityScale;
            double targetCameraX = cameraSampleX +
                cameraVelocityX * predictionSeconds;
            double targetCameraY = cameraSampleY +
                cameraVelocityY * predictionSeconds;
            double targetCameraZ = cameraSampleZ +
                cameraVelocityZ * predictionSeconds;
            double rawTargetPitch = cameraSamplePitch +
                cameraVelocityPitch * predictionSeconds;
            double targetCameraPitch = MotionSmoother.clamp(
                rawTargetPitch,
                -90.0,
                90.0
            );
            if (targetCameraPitch != rawTargetPitch) targetVelocityPitch = 0.0;
            double targetCameraYaw = cameraSampleYaw +
                cameraVelocityYaw * predictionSeconds;

            long frameGap = Math.max(0, now - cameraLastFrameNanos);
            if (cameraLastFrameNanos == 0 ||
                frameGap > MaximumFrameGapNanos) {
                renderedCameraX.reset(targetCameraX, targetVelocityX);
                renderedCameraY.reset(targetCameraY, targetVelocityY);
                renderedCameraZ.reset(targetCameraZ, targetVelocityZ);
                renderedCameraPitch.reset(
                    targetCameraPitch,
                    targetVelocityPitch
                );
                renderedCameraYaw.reset(targetCameraYaw, targetVelocityYaw);
            } else {
                double deltaSeconds = frameGap / 1_000_000_000.0;
                double positionSpeed = Math.sqrt(
                    cameraVelocityX * cameraVelocityX +
                        cameraVelocityY * cameraVelocityY +
                        cameraVelocityZ * cameraVelocityZ
                );
                double angleSpeed = Math.max(
                    Math.abs(cameraVelocityPitch),
                    Math.abs(cameraVelocityYaw)
                );
                double positionSmoothTime = 0.042 -
                    MotionSmoother.clamp(
                        positionSpeed / 30.0,
                        0.0,
                        1.0
                    ) * 0.018;
                double angleSmoothTime = 0.025 -
                    MotionSmoother.clamp(angleSpeed / 720.0, 0.0, 1.0) *
                        0.013;
                renderedCameraX.step(
                    targetCameraX,
                    targetVelocityX,
                    positionSmoothTime,
                    deltaSeconds
                );
                renderedCameraY.step(
                    targetCameraY,
                    targetVelocityY,
                    positionSmoothTime,
                    deltaSeconds
                );
                renderedCameraZ.step(
                    targetCameraZ,
                    targetVelocityZ,
                    positionSmoothTime,
                    deltaSeconds
                );
                renderedCameraPitch.step(
                    targetCameraPitch,
                    targetVelocityPitch,
                    angleSmoothTime,
                    deltaSeconds
                );
                renderedCameraYaw.step(
                    targetCameraYaw,
                    targetVelocityYaw,
                    angleSmoothTime,
                    deltaSeconds
                );
            }
            cameraLastFrameNanos = now;

            double cameraX = renderedCameraX.value();
            double cameraY = renderedCameraY.value();
            double cameraZ = renderedCameraZ.value();
            double pitch = Math.toRadians(MotionSmoother.clamp(
                renderedCameraPitch.value(),
                -90.0,
                90.0
            ));
            double yaw = Math.toRadians(renderedCameraYaw.value());

            double sinYaw = Math.sin(yaw);
            double cosYaw = Math.cos(yaw);
            double sinPitch = Math.sin(pitch);
            double cosPitch = Math.cos(pitch);
            // Minecraft's FOV setting is applied vertically. Using the view
            // height keeps horizontal projection correct on wide phones.
            double focal = (getHeight() * 0.5) /
                Math.tan(Math.toRadians(fieldOfView * 0.5));

            boolean cameraVelocityActive = predictionActive &&
                (Math.abs(cameraVelocityX) > 0.001 ||
                 Math.abs(cameraVelocityY) > 0.001 ||
                 Math.abs(cameraVelocityZ) > 0.001 ||
                 Math.abs(cameraVelocityPitch) > 0.01 ||
                 Math.abs(cameraVelocityYaw) > 0.01);
            boolean cameraProjectionActive = cameraVelocityActive ||
                !renderedCameraX.isSettled(
                    targetCameraX,
                    targetVelocityX,
                    0.0001,
                    0.002
                ) ||
                !renderedCameraY.isSettled(
                    targetCameraY,
                    targetVelocityY,
                    0.0001,
                    0.002
                ) ||
                !renderedCameraZ.isSettled(
                    targetCameraZ,
                    targetVelocityZ,
                    0.0001,
                    0.002
                ) ||
                !renderedCameraPitch.isSettled(
                    targetCameraPitch,
                    targetVelocityPitch,
                    0.002,
                    0.02
                ) ||
                !renderedCameraYaw.isSettled(
                    targetCameraYaw,
                    targetVelocityYaw,
                    0.002,
                    0.02
                );
            boolean animating = cameraProjectionActive;
            for (RenderTrack track : tracks.values()) {
                animating |= track.advance(now);
                animating |= drawTrack(
                    canvas,
                    track,
                    cameraX,
                    cameraY,
                    cameraZ,
                    sinYaw,
                    cosYaw,
                    sinPitch,
                    cosPitch,
                    focal,
                    now,
                    cameraProjectionActive
                );
            }
            if (animating) postInvalidateOnAnimation();
        }

        private boolean drawTrack(
            Canvas canvas,
            RenderTrack track,
            double cameraX,
            double cameraY,
            double cameraZ,
            double sinYaw,
            double cosYaw,
            double sinPitch,
            double cosPitch,
            double focal,
            long now,
            boolean cameraProjectionActive
        ) {
            double entityX = track.renderX.value();
            double entityY = track.renderY.value();
            double entityZ = track.renderZ.value();
            double centerDx = entityX - cameraX;
            double centerDy = entityY + track.height * 0.5 - cameraY;
            double centerDz = entityZ - cameraZ;
            double centerDepth = depth(
                centerDx,
                centerDy,
                centerDz,
                sinYaw,
                cosYaw,
                sinPitch,
                cosPitch
            );
            if (centerDepth <= NearPlane) {
                track.screenBox.hide();
                return false;
            }

            double halfWidth = track.width * 0.5;
            double minimumX = Double.POSITIVE_INFINITY;
            double minimumY = Double.POSITIVE_INFINITY;
            double maximumX = Double.NEGATIVE_INFINITY;
            double maximumY = Double.NEGATIVE_INFINITY;
            int projected = 0;
            for (int xSide = -1; xSide <= 1; xSide += 2) {
                for (int zSide = -1; zSide <= 1; zSide += 2) {
                    for (int ySide = 0; ySide <= 1; ++ySide) {
                        double dx = entityX + xSide * halfWidth - cameraX;
                        double dy = entityY + ySide * track.height - cameraY;
                        double dz = entityZ + zSide * halfWidth - cameraZ;

                        // Bedrock yaw 0 faces +Z and positive yaw turns
                        // towards -X, so screen-right points along the
                        // negative of this horizontal basis.
                        double viewX = -(dx * cosYaw + dz * sinYaw);
                        double viewY =
                            dx * (-sinYaw * sinPitch) +
                            dy * cosPitch +
                            dz * (cosYaw * sinPitch);
                        double viewZ = depth(
                            dx,
                            dy,
                            dz,
                            sinYaw,
                            cosYaw,
                            sinPitch,
                            cosPitch
                        );
                        if (viewZ <= NearPlane) continue;

                        double screenX = getWidth() * 0.5 +
                            viewX * focal / viewZ;
                        double screenY = getHeight() * 0.5 -
                            viewY * focal / viewZ;
                        if (!Double.isFinite(screenX) ||
                            !Double.isFinite(screenY)) continue;
                        minimumX = Math.min(minimumX, screenX);
                        minimumY = Math.min(minimumY, screenY);
                        maximumX = Math.max(maximumX, screenX);
                        maximumY = Math.max(maximumY, screenY);
                        ++projected;
                    }
                }
            }
            if (projected < 4) {
                track.screenBox.hide();
                return false;
            }

            float left = (float) minimumX;
            float top = (float) minimumY;
            float right = (float) maximumX;
            float bottom = (float) maximumY;
            float minimumWidth = density * 8f;
            float minimumHeight = density * 15f;
            if (right - left < minimumWidth) {
                float center = (left + right) * 0.5f;
                left = center - minimumWidth * 0.5f;
                right = center + minimumWidth * 0.5f;
            }
            if (bottom - top < minimumHeight) {
                float center = (top + bottom) * 0.5f;
                top = center - minimumHeight * 0.5f;
                bottom = center + minimumHeight * 0.5f;
            }
            if (right < 0 || bottom < 0 || left > getWidth() ||
                top > getHeight()) {
                track.screenBox.hide();
                return false;
            }

            boolean screenAnimating = track.screenBox.step(
                left,
                top,
                right,
                bottom,
                now,
                getWidth(),
                getHeight(),
                cameraProjectionActive
            );
            left = (float) track.screenBox.left();
            top = (float) track.screenBox.top();
            right = (float) track.screenBox.right();
            bottom = (float) track.screenBox.bottom();

            int color = track.player ? 0xff4fd5ff : 0xffff5b62;
            rectangle.set(left, top, right, bottom);
            fillPaint.setColor(withAlpha(color, 24));
            glowPaint.setColor(withAlpha(color, 78));
            outlinePaint.setColor(color);
            float corner = density * 2f;
            canvas.drawRoundRect(rectangle, corner, corner, fillPaint);
            canvas.drawRoundRect(rectangle, corner, corner, glowPaint);
            canvas.drawRoundRect(rectangle, corner, corner, outlinePaint);

            double distance = Math.sqrt(
                (entityX - cameraX) * (entityX - cameraX) +
                (entityY - cameraY) * (entityY - cameraY) +
                (entityZ - cameraZ) * (entityZ - cameraZ)
            );
            int roundedDistance = (int) Math.round(distance);
            if (!track.label.equals(track.displayLabel) ||
                roundedDistance != track.displayDistance) {
                track.displayLabel = track.label;
                track.displayDistance = roundedDistance;
                track.displayText = track.label + "  " +
                    roundedDistance + " м";
                track.displayTextWidth = textPaint.measureText(
                    track.displayText
                );
            }
            String text = track.displayText;
            float textWidth = track.displayTextWidth;
            float textHeight = textPaint.getTextSize();
            float textX = Math.max(
                density * 3f,
                Math.min(
                    left,
                    getWidth() - textWidth - density * 6f
                )
            );
            float baseline = Math.max(textHeight + density * 4f, top - density * 3f);
            labelRectangle.set(
                textX - density * 3f,
                baseline - textHeight - density * 3f,
                textX + textWidth + density * 3f,
                baseline + density * 2f
            );
            canvas.drawRoundRect(
                labelRectangle,
                density * 3f,
                density * 3f,
                textBackgroundPaint
            );
            canvas.drawText(text, textX, baseline, textPaint);
            return screenAnimating;
        }

        private static double depth(
            double dx,
            double dy,
            double dz,
            double sinYaw,
            double cosYaw,
            double sinPitch,
            double cosPitch
        ) {
            return dx * (-sinYaw * cosPitch) +
                dy * (-sinPitch) +
                dz * (cosYaw * cosPitch);
        }

        private static int withAlpha(int color, int alpha) {
            return Color.argb(
                alpha,
                Color.red(color),
                Color.green(color),
                Color.blue(color)
            );
        }
    }

}
