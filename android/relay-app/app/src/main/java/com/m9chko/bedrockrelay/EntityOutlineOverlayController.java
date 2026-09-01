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
import java.util.Locale;
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
    private final AtomicBoolean deliveryPosted = new AtomicBoolean(false);

    private volatile boolean sessionVisible;
    private volatile boolean enabled = true;
    private volatile int horizontalFov = 70;
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

    void setHorizontalFov(int value) {
        horizontalFov = RelayService.clampEntityFov(value);
        if (outlineView != null) {
            outlineView.setHorizontalFov(horizontalFov);
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

    void hideImmediately() {
        sessionVisible = false;
        removeWindow();
    }

    private void postFrameDelivery() {
        if (!deliveryPosted.compareAndSet(false, true)) return;
        mainHandler.post(() -> {
            Frame frame = pendingFrame.getAndSet(null);
            if (outlineView != null && frame != null) {
                outlineView.submit(frame);
            }
            deliveryPosted.set(false);
            if (pendingFrame.get() != null) postFrameDelivery();
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
        view.setHorizontalFov(horizontalFov);
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
                WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
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
            DiagnosticsLog.append(
                context,
                "INFO",
                "entities",
                "Entity outline overlay opened; mode=2d_boxes"
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

        CameraSample(
            boolean known,
            double x,
            double y,
            double z,
            double pitch,
            double yaw
        ) {
            this.known = known;
            this.x = x;
            this.y = y;
            this.z = z;
            this.pitch = pitch;
            this.yaw = yaw;
        }

        static CameraSample unknown() {
            return new CameraSample(false, 0, 0, 0, 0, 0);
        }

        static CameraSample from(JSONObject value) {
            boolean known = value.optBoolean("known", false);
            double x = value.optDouble("x", 0);
            double y = value.optDouble("y", 0);
            double z = value.optDouble("z", 0);
            double pitch = value.optDouble("pitch", 0);
            double yaw = value.optDouble("yaw", 0);
            if (!finite(x, y, z, pitch, yaw)) known = false;
            return new CameraSample(known, x, y, z, pitch, yaw);
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

        EntitySample(
            String id,
            String label,
            boolean player,
            double x,
            double y,
            double z,
            double width,
            double height
        ) {
            this.id = id;
            this.label = label;
            this.player = player;
            this.x = x;
            this.y = y;
            this.z = z;
            this.width = width;
            this.height = height;
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
                Math.min(height, 16.0)
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
        final String id;
        String label;
        boolean player;
        double fromX;
        double fromY;
        double fromZ;
        double toX;
        double toY;
        double toZ;
        double width;
        double height;
        long transitionStartedNanos;
        long seenGeneration;

        RenderTrack(EntitySample sample, long now, long generation) {
            id = sample.id;
            label = sample.label;
            player = sample.player;
            fromX = toX = sample.x;
            fromY = toY = sample.y;
            fromZ = toZ = sample.z;
            width = sample.width;
            height = sample.height;
            transitionStartedNanos = now;
            seenGeneration = generation;
        }

        void retarget(EntitySample sample, long now, long generation) {
            double progress = progress(now, transitionStartedNanos);
            fromX = mix(fromX, toX, progress);
            fromY = mix(fromY, toY, progress);
            fromZ = mix(fromZ, toZ, progress);
            toX = sample.x;
            toY = sample.y;
            toZ = sample.z;
            width = sample.width;
            height = sample.height;
            label = sample.label;
            player = sample.player;
            transitionStartedNanos = now;
            seenGeneration = generation;
        }
    }

    private static final class EntityOutlineView extends View {
        private static final long TransitionNanos = 90_000_000L;
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
        private double cameraFromX;
        private double cameraFromY;
        private double cameraFromZ;
        private double cameraFromPitch;
        private double cameraFromYaw;
        private double cameraToX;
        private double cameraToY;
        private double cameraToZ;
        private double cameraToPitch;
        private double cameraToYaw;
        private long cameraTransitionStartedNanos;
        private long generation;
        private int horizontalFov = 70;

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

        void setHorizontalFov(int value) {
            horizontalFov = RelayService.clampEntityFov(value);
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

        private void updateCamera(CameraSample sample, long now) {
            if (!sample.known) {
                cameraKnown = false;
                return;
            }
            if (!cameraKnown) {
                cameraFromX = cameraToX = sample.x;
                cameraFromY = cameraToY = sample.y;
                cameraFromZ = cameraToZ = sample.z;
                cameraFromPitch = cameraToPitch = sample.pitch;
                cameraFromYaw = cameraToYaw = sample.yaw;
                cameraKnown = true;
            } else {
                double progress = progress(now, cameraTransitionStartedNanos);
                cameraFromX = mix(cameraFromX, cameraToX, progress);
                cameraFromY = mix(cameraFromY, cameraToY, progress);
                cameraFromZ = mix(cameraFromZ, cameraToZ, progress);
                cameraFromPitch = mixAngle(
                    cameraFromPitch,
                    cameraToPitch,
                    progress
                );
                cameraFromYaw = mixAngle(
                    cameraFromYaw,
                    cameraToYaw,
                    progress
                );
                cameraToX = sample.x;
                cameraToY = sample.y;
                cameraToZ = sample.z;
                cameraToPitch = sample.pitch;
                cameraToYaw = sample.yaw;
            }
            cameraTransitionStartedNanos = now;
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            if (!cameraKnown || getWidth() <= 0 || getHeight() <= 0) return;

            long now = System.nanoTime();
            double cameraProgress = progress(
                now,
                cameraTransitionStartedNanos
            );
            double cameraX = mix(cameraFromX, cameraToX, cameraProgress);
            double cameraY = mix(cameraFromY, cameraToY, cameraProgress);
            double cameraZ = mix(cameraFromZ, cameraToZ, cameraProgress);
            double pitch = Math.toRadians(mixAngle(
                cameraFromPitch,
                cameraToPitch,
                cameraProgress
            ));
            double yaw = Math.toRadians(mixAngle(
                cameraFromYaw,
                cameraToYaw,
                cameraProgress
            ));

            double sinYaw = Math.sin(yaw);
            double cosYaw = Math.cos(yaw);
            double sinPitch = Math.sin(pitch);
            double cosPitch = Math.cos(pitch);
            double focal = (getWidth() * 0.5) /
                Math.tan(Math.toRadians(horizontalFov * 0.5));

            boolean animating = cameraProgress < 1.0;
            for (RenderTrack track : tracks.values()) {
                double entityProgress = progress(
                    now,
                    track.transitionStartedNanos
                );
                animating |= entityProgress < 1.0;
                drawTrack(
                    canvas,
                    track,
                    entityProgress,
                    cameraX,
                    cameraY,
                    cameraZ,
                    sinYaw,
                    cosYaw,
                    sinPitch,
                    cosPitch,
                    focal
                );
            }
            if (animating) postInvalidateOnAnimation();
        }

        private void drawTrack(
            Canvas canvas,
            RenderTrack track,
            double progress,
            double cameraX,
            double cameraY,
            double cameraZ,
            double sinYaw,
            double cosYaw,
            double sinPitch,
            double cosPitch,
            double focal
        ) {
            double entityX = mix(track.fromX, track.toX, progress);
            double entityY = mix(track.fromY, track.toY, progress);
            double entityZ = mix(track.fromZ, track.toZ, progress);
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
            if (centerDepth <= NearPlane) return;

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
            if (projected < 4) return;

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
                top > getHeight()) return;

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
            String text = String.format(
                Locale.getDefault(),
                "%s  %.0f м",
                track.label,
                distance
            );
            float textWidth = textPaint.measureText(text);
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

    private static double progress(long now, long started) {
        if (started == 0) return 1.0;
        return Math.max(
            0.0,
            Math.min(
                1.0,
                (double) (now - started) /
                    EntityOutlineView.TransitionNanos
            )
        );
    }

    private static double mix(double from, double to, double progress) {
        return from + (to - from) * progress;
    }

    private static double mixAngle(
        double from,
        double to,
        double progress
    ) {
        double difference = (to - from) % 360.0;
        if (difference > 180.0) difference -= 360.0;
        if (difference < -180.0) difference += 360.0;
        return from + difference * progress;
    }
}
