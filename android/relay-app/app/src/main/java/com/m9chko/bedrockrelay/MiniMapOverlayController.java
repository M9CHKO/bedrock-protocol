package com.m9chko.bedrockrelay;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.RectF;
import android.provider.Settings;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.WindowManager;

import java.util.HashMap;
import java.util.Map;

/** Packet-only, draggable terrain mini-map rendered from LevelChunk packets. */
final class MiniMapOverlayController {
    private static final int MAGIC = 0x4350454d;
    private static final int HEADER_INTS = 12;
    private static final int TILE_INTS = 258;

    private final Context context;
    private final SharedPreferences preferences;
    private final WindowManager windowManager;
    private final Object modelLock = new Object();
    private final Map<Long, Bitmap> tiles = new HashMap<>();

    private boolean sessionVisible;
    private boolean uiBlocked;
    private boolean enabled = true;
    private boolean round = true;
    private int radiusChunks = 4;
    private int scalePercent = 90;
    private MiniMapView view;
    private WindowManager.LayoutParams windowParams;

    private long revision;
    private int generation = Integer.MIN_VALUE;
    private int dimension = Integer.MIN_VALUE;
    private boolean cameraKnown;
    private float cameraX;
    private float cameraY;
    private float cameraZ;
    private float cameraYaw;
    private int requestedCameraChunkX = Integer.MIN_VALUE;
    private int requestedCameraChunkZ = Integer.MIN_VALUE;

    MiniMapOverlayController(Context context, SharedPreferences preferences) {
        this.context = context;
        this.preferences = preferences;
        this.windowManager = (WindowManager) context.getSystemService(
            Context.WINDOW_SERVICE
        );
    }

    void setSessionVisible(boolean visible) {
        sessionVisible = visible;
        reconcile();
    }

    void setUiBlocked(boolean blocked) {
        if (uiBlocked == blocked) return;
        uiBlocked = blocked;
        reconcile();
    }

    void configure(
        boolean show,
        int radius,
        int scale,
        boolean circular
    ) {
        int safeRadius = RelayService.clampMiniMapRadius(radius);
        int safeScale = RelayService.clampOverlayScale(scale);
        boolean recreate = scalePercent != safeScale;
        enabled = show;
        radiusChunks = safeRadius;
        scalePercent = safeScale;
        round = circular;
        if (recreate) removeWindow();
        MiniMapView current = view;
        if (current != null) current.postInvalidateOnAnimation();
        reconcile();
    }

    boolean wantsFrames() {
        return sessionVisible && enabled && !uiBlocked;
    }

    int requestedRadiusChunks() {
        return radiusChunks;
    }

    long requestedRevision() {
        synchronized (modelLock) {
            if (!cameraKnown) return revision;
            int chunkX = floorChunk(cameraX);
            int chunkZ = floorChunk(cameraZ);
            if (chunkX != requestedCameraChunkX ||
                chunkZ != requestedCameraChunkZ) {
                requestedCameraChunkX = chunkX;
                requestedCameraChunkZ = chunkZ;
                return 0L;
            }
            return revision;
        }
    }

    void offerSnapshot(int[] data) {
        if (data == null || data.length < HEADER_INTS ||
            data[0] != MAGIC || data[1] != 1) {
            return;
        }
        int tileCount = Math.max(0, data[11]);
        if (data.length < HEADER_INTS + tileCount * TILE_INTS) return;

        boolean nextCameraKnown = data[2] != 0;
        float nextCameraX = Float.intBitsToFloat(data[3]);
        float nextCameraY = Float.intBitsToFloat(data[4]);
        float nextCameraZ = Float.intBitsToFloat(data[5]);
        float nextCameraYaw = Float.intBitsToFloat(data[6]);
        int nextDimension = data[7];
        long nextRevision = (data[8] & 0xffffffffL) |
            ((data[9] & 0xffffffffL) << 32);
        int nextGeneration = data[10];

        synchronized (modelLock) {
            if (generation != nextGeneration || dimension != nextDimension) {
                clearTilesLocked();
                generation = nextGeneration;
                dimension = nextDimension;
                revision = 0L;
                requestedCameraChunkX = Integer.MIN_VALUE;
                requestedCameraChunkZ = Integer.MIN_VALUE;
            }
            int offset = HEADER_INTS;
            for (int tileIndex = 0; tileIndex < tileCount; ++tileIndex) {
                int tileX = data[offset++];
                int tileZ = data[offset++];
                int[] pixels = new int[256];
                System.arraycopy(data, offset, pixels, 0, pixels.length);
                offset += pixels.length;
                Bitmap bitmap = Bitmap.createBitmap(
                    pixels,
                    16,
                    16,
                    Bitmap.Config.ARGB_8888
                );
                Bitmap previous = tiles.put(tileKey(tileX, tileZ), bitmap);
                if (previous != null && previous != bitmap) previous.recycle();
            }
            while (tiles.size() > 1024) {
                Long first = tiles.keySet().iterator().next();
                Bitmap removed = tiles.remove(first);
                if (removed != null) removed.recycle();
            }
            revision = nextRevision;
            cameraKnown = nextCameraKnown;
            cameraX = nextCameraX;
            cameraY = nextCameraY;
            cameraZ = nextCameraZ;
            cameraYaw = nextCameraYaw;
        }
        MiniMapView current = view;
        if (current != null) current.postInvalidateOnAnimation();
    }

    void hideImmediately() {
        sessionVisible = false;
        removeWindow();
    }

    void destroy() {
        hideImmediately();
        synchronized (modelLock) {
            clearTilesLocked();
        }
    }

    private void reconcile() {
        if (sessionVisible && enabled && !uiBlocked) addWindow();
        else removeWindow();
    }

    private void addWindow() {
        if (view != null || !Settings.canDrawOverlays(context)) return;
        MiniMapView added = new MiniMapView(context, scalePercent);
        WindowManager.LayoutParams params = new WindowManager.LayoutParams(
            added.preferredSize(),
            added.preferredSize(),
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE |
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL |
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSLUCENT
        );
        params.gravity = Gravity.TOP | Gravity.START;
        params.x = preferences.getInt(RelayService.KEY_MINIMAP_X, dp(92));
        params.y = preferences.getInt(RelayService.KEY_MINIMAP_Y, dp(142));
        int screenWidth = context.getResources().getDisplayMetrics().widthPixels;
        int screenHeight = context.getResources().getDisplayMetrics().heightPixels;
        params.x = Math.max(0, Math.min(screenWidth - added.preferredSize(), params.x));
        params.y = Math.max(0, Math.min(screenHeight - added.preferredSize(), params.y));
        attachDrag(added, params);
        try {
            windowManager.addView(added, params);
            view = added;
            windowParams = params;
        } catch (Throwable error) {
            DiagnosticsLog.appendError(
                context,
                "minimap",
                "Failed to show mini-map HUD",
                error
            );
        }
    }

    private void removeWindow() {
        MiniMapView current = view;
        view = null;
        windowParams = null;
        if (current == null) return;
        try {
            windowManager.removeViewImmediate(current);
        } catch (Throwable ignored) {
        }
    }

    private void attachDrag(
        MiniMapView target,
        WindowManager.LayoutParams targetParams
    ) {
        final int slop = ViewConfiguration.get(context).getScaledTouchSlop();
        target.setOnTouchListener(new View.OnTouchListener() {
            private float downRawX;
            private float downRawY;
            private int downX;
            private int downY;
            private boolean moved;

            @Override
            public boolean onTouch(View touched, MotionEvent event) {
                if (view != target || windowParams != targetParams) return false;
                switch (event.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        downRawX = event.getRawX();
                        downRawY = event.getRawY();
                        downX = targetParams.x;
                        downY = targetParams.y;
                        moved = false;
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        float dx = event.getRawX() - downRawX;
                        float dy = event.getRawY() - downRawY;
                        moved |= Math.abs(dx) > slop || Math.abs(dy) > slop;
                        if (!moved) return true;
                        int screenWidth = context.getResources()
                            .getDisplayMetrics().widthPixels;
                        int screenHeight = context.getResources()
                            .getDisplayMetrics().heightPixels;
                        targetParams.x = Math.max(0, Math.min(
                            Math.max(0, screenWidth - target.getWidth()),
                            downX + Math.round(dx)
                        ));
                        targetParams.y = Math.max(0, Math.min(
                            Math.max(0, screenHeight - target.getHeight()),
                            downY + Math.round(dy)
                        ));
                        try {
                            windowManager.updateViewLayout(target, targetParams);
                        } catch (Throwable ignored) {
                        }
                        return true;
                    case MotionEvent.ACTION_UP:
                    case MotionEvent.ACTION_CANCEL:
                        if (moved) {
                            preferences.edit()
                                .putInt(RelayService.KEY_MINIMAP_X, targetParams.x)
                                .putInt(RelayService.KEY_MINIMAP_Y, targetParams.y)
                                .apply();
                        }
                        return true;
                    default:
                        return false;
                }
            }
        });
    }

    private final class MiniMapView extends View {
        private final float density;
        private final float scale;
        private final int size;
        private final Paint background = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint border = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint bitmapPaint = new Paint();
        private final Paint marker = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Rect source = new Rect(0, 0, 16, 16);
        private final RectF destination = new RectF();
        private final RectF bounds = new RectF();
        private final Path clip = new Path();
        private final Path arrow = new Path();

        MiniMapView(Context context, int scalePercent) {
            super(context);
            density = context.getResources().getDisplayMetrics().density;
            scale = scalePercent / 100f;
            size = px(172);
            setBackgroundColor(Color.TRANSPARENT);
            setWillNotDraw(false);
            background.setColor(0xe20d141d);
            border.setStyle(Paint.Style.STROKE);
            border.setStrokeWidth(Math.max(1f, px(1.2f)));
            border.setColor(0x8892a4b7);
            bitmapPaint.setAntiAlias(false);
            bitmapPaint.setFilterBitmap(false);
            marker.setColor(0xffff6b68);
            marker.setStyle(Paint.Style.FILL);
            label.setColor(0xffe8f1fa);
            label.setTextSize(px(9));
            label.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        }

        int preferredSize() { return size; }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            float inset = px(4);
            bounds.set(inset, inset, getWidth() - inset, getHeight() - inset);
            float radius = round ? bounds.width() * 0.5f : px(14);
            canvas.drawRoundRect(bounds, radius, radius, background);

            int saved = canvas.save();
            clip.reset();
            clip.addRoundRect(bounds, radius, radius, Path.Direction.CW);
            canvas.clipPath(clip);
            drawTerrain(canvas, bounds);
            canvas.restoreToCount(saved);
            canvas.drawRoundRect(bounds, radius, radius, border);
            drawPlayerMarker(canvas, bounds.centerX(), bounds.centerY());
            canvas.drawText("N", bounds.centerX() - px(3), bounds.top + px(12), label);
        }

        private void drawTerrain(Canvas canvas, RectF area) {
            synchronized (modelLock) {
                if (!cameraKnown) {
                    label.setColor(0xff91a2b4);
                    canvas.drawText(
                        "ожидание чанков",
                        area.left + px(26),
                        area.centerY(),
                        label
                    );
                    label.setColor(0xffe8f1fa);
                    return;
                }
                float blockSpan = radiusChunks * 32f;
                float pixelsPerBlock = area.width() / Math.max(32f, blockSpan);
                float originX = area.centerX() - cameraX * pixelsPerBlock;
                float originY = area.centerY() - cameraZ * pixelsPerBlock;
                for (Map.Entry<Long, Bitmap> entry : tiles.entrySet()) {
                    int chunkX = keyX(entry.getKey());
                    int chunkZ = keyZ(entry.getKey());
                    float left = originX + chunkX * 16f * pixelsPerBlock;
                    float top = originY + chunkZ * 16f * pixelsPerBlock;
                    float tileSize = 16f * pixelsPerBlock;
                    if (left > area.right || top > area.bottom ||
                        left + tileSize < area.left || top + tileSize < area.top) {
                        continue;
                    }
                    destination.set(left, top, left + tileSize, top + tileSize);
                    canvas.drawBitmap(entry.getValue(), source, destination, bitmapPaint);
                }
            }
        }

        private void drawPlayerMarker(Canvas canvas, float x, float y) {
            float markerSize = px(9);
            float yaw;
            synchronized (modelLock) {
                yaw = cameraYaw;
            }
            int saved = canvas.save();
            // Bedrock yaw 0 faces +Z (south); the north-up map has -Z at the
            // top, so the on-screen arrow needs a 180-degree basis offset.
            canvas.rotate(yaw + 180f, x, y);
            arrow.reset();
            arrow.moveTo(x, y - markerSize);
            arrow.lineTo(x - markerSize * 0.62f, y + markerSize * 0.72f);
            arrow.lineTo(x, y + markerSize * 0.34f);
            arrow.lineTo(x + markerSize * 0.62f, y + markerSize * 0.72f);
            arrow.close();
            canvas.drawPath(arrow, marker);
            canvas.restoreToCount(saved);
        }

        private int px(float value) {
            return Math.max(1, Math.round(value * density * scale));
        }
    }

    private int dp(int value) {
        return Math.round(
            value * context.getResources().getDisplayMetrics().density
        );
    }

    private static int floorChunk(float block) {
        return (int) Math.floor(block / 16f);
    }

    private static long tileKey(int x, int z) {
        return ((long) x << 32) ^ (z & 0xffffffffL);
    }

    private static int keyX(long key) {
        return (int) (key >> 32);
    }

    private static int keyZ(long key) {
        return (int) key;
    }

    private void clearTilesLocked() {
        for (Bitmap bitmap : tiles.values()) {
            if (bitmap != null && !bitmap.isRecycled()) bitmap.recycle();
        }
        tiles.clear();
    }
}
