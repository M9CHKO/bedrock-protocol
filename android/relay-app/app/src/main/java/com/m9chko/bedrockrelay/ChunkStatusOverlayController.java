package com.m9chko.bedrockrelay;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.drawable.GradientDrawable;
import android.provider.Settings;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.Locale;

/** Independent, draggable and scalable chunk-retention status widget. */
final class ChunkStatusOverlayController {
    private final Context context;
    private final SharedPreferences preferences;
    private final WindowManager windowManager;

    private boolean sessionVisible;
    private boolean uiBlocked;
    private boolean enabled;
    private int scalePercent = 100;
    private boolean minimized;
    private LinearLayout root;
    private TextView title;
    private TextView detail;
    private WindowManager.LayoutParams params;

    private boolean retentionEnabled;
    private int configuredRadiusChunks = 24;
    private long publisherUpdates;
    private long publisherRewrites;
    private int serverRadiusBlocks;
    private int effectiveRadiusBlocks;
    private long retainedChunks;
    private long retainedBytes;
    private long maximumBytes = 256L * 1024L * 1024L;
    private long evictedRadius;
    private long evictedMemory;
    private long parseFailures;

    ChunkStatusOverlayController(
        Context context,
        SharedPreferences preferences
    ) {
        this.context = context;
        this.preferences = preferences;
        this.windowManager = (WindowManager) context.getSystemService(
            Context.WINDOW_SERVICE
        );
        minimized = preferences.getBoolean(
            RelayService.KEY_CHUNK_WIDGET_MINIMIZED,
            false
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

    void configure(boolean show, int scale) {
        boolean changed = enabled != show ||
            scalePercent != RelayService.clampOverlayScale(scale);
        enabled = show;
        scalePercent = RelayService.clampOverlayScale(scale);
        if (changed && root != null) removeWindow();
        reconcile();
    }

    void updateStatus(
        boolean retention,
        int radiusChunks,
        long updates,
        long rewrites,
        int serverBlocks,
        int effectiveBlocks,
        long chunks,
        long bytes,
        long maxBytes,
        long radiusEvictions,
        long memoryEvictions,
        long failures
    ) {
        retentionEnabled = retention;
        configuredRadiusChunks = radiusChunks;
        publisherUpdates = updates;
        publisherRewrites = rewrites;
        serverRadiusBlocks = serverBlocks;
        effectiveRadiusBlocks = effectiveBlocks;
        retainedChunks = chunks;
        retainedBytes = bytes;
        maximumBytes = maxBytes;
        evictedRadius = radiusEvictions;
        evictedMemory = memoryEvictions;
        parseFailures = failures;
        reconcile();
        refreshText();
    }

    void hideImmediately() {
        sessionVisible = false;
        removeWindow();
    }

    private void reconcile() {
        if (sessionVisible && enabled && retentionEnabled && !uiBlocked) {
            addWindow();
        } else {
            removeWindow();
        }
    }

    private void addWindow() {
        if (root != null || !Settings.canDrawOverlays(context)) return;

        root = new LinearLayout(context);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(sdp(10), sdp(7), sdp(10), sdp(8));
        root.setBackground(background());
        root.setElevation(sdp(8));

        LinearLayout header = new LinearLayout(context);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        title = label("ЧАНКИ", 11, true);
        title.setTextColor(0xff93dcff);
        header.addView(title, new LinearLayout.LayoutParams(
            0,
            ViewGroup.LayoutParams.WRAP_CONTENT,
            1f
        ));
        TextView smaller = action("−");
        TextView larger = action("+");
        TextView close = action("×");
        header.addView(smaller);
        header.addView(larger);
        header.addView(close);
        root.addView(header, new LinearLayout.LayoutParams(
            minimized ? sdp(112) : sdp(205),
            ViewGroup.LayoutParams.WRAP_CONTENT
        ));

        detail = label("", minimized ? 11 : 12, !minimized);
        detail.setTextColor(Color.WHITE);
        detail.setPadding(0, sdp(3), 0, 0);
        root.addView(detail, new LinearLayout.LayoutParams(
            minimized ? sdp(112) : sdp(205),
            ViewGroup.LayoutParams.WRAP_CONTENT
        ));

        smaller.setOnClickListener(view -> changeScale(-10));
        larger.setOnClickListener(view -> changeScale(10));
        close.setOnClickListener(view -> {
            preferences.edit()
                .putBoolean(RelayService.KEY_CHUNK_WIDGET, false)
                .apply();
            enabled = false;
            removeWindow();
        });
        attachDragAndMinimize(header);
        attachDragAndMinimize(detail);

        params = new WindowManager.LayoutParams(
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE |
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL |
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSLUCENT
        );
        params.gravity = Gravity.TOP | Gravity.START;
        params.x = preferences.getInt(RelayService.KEY_CHUNK_WIDGET_X, dp(12));
        params.y = preferences.getInt(RelayService.KEY_CHUNK_WIDGET_Y, dp(150));
        try {
            windowManager.addView(root, params);
            refreshText();
        } catch (Throwable error) {
            root = null;
            params = null;
            DiagnosticsLog.appendError(
                context,
                "chunks",
                "Failed to show chunk status widget",
                error
            );
        }
    }

    private void removeWindow() {
        LinearLayout view = root;
        root = null;
        title = null;
        detail = null;
        params = null;
        if (view == null) return;
        try {
            windowManager.removeViewImmediate(view);
        } catch (Throwable ignored) {
        }
    }

    private void changeScale(int delta) {
        int next = RelayService.clampOverlayScale(scalePercent + delta);
        if (next == scalePercent) return;
        scalePercent = next;
        preferences.edit()
            .putInt(RelayService.KEY_CHUNK_WIDGET_SCALE, next)
            .apply();
        removeWindow();
        reconcile();
    }

    private void toggleMinimized() {
        minimized = !minimized;
        preferences.edit()
            .putBoolean(RelayService.KEY_CHUNK_WIDGET_MINIMIZED, minimized)
            .apply();
        removeWindow();
        reconcile();
    }

    private void attachDragAndMinimize(View target) {
        final int slop = ViewConfiguration.get(context)
            .getScaledTouchSlop();
        target.setOnTouchListener(new View.OnTouchListener() {
            private float downRawX;
            private float downRawY;
            private int downX;
            private int downY;
            private boolean moved;

            @Override
            public boolean onTouch(View view, MotionEvent event) {
                if (params == null || root == null) return false;
                switch (event.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        downRawX = event.getRawX();
                        downRawY = event.getRawY();
                        downX = params.x;
                        downY = params.y;
                        moved = false;
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        float dx = event.getRawX() - downRawX;
                        float dy = event.getRawY() - downRawY;
                        moved |= Math.abs(dx) > slop || Math.abs(dy) > slop;
                        if (moved) {
                            params.x = Math.max(0, downX + Math.round(dx));
                            params.y = Math.max(0, downY + Math.round(dy));
                            try {
                                windowManager.updateViewLayout(root, params);
                            } catch (Throwable ignored) {
                            }
                        }
                        return true;
                    case MotionEvent.ACTION_UP:
                    case MotionEvent.ACTION_CANCEL:
                        if (moved) {
                            preferences.edit()
                                .putInt(RelayService.KEY_CHUNK_WIDGET_X, params.x)
                                .putInt(RelayService.KEY_CHUNK_WIDGET_Y, params.y)
                                .apply();
                        } else if (event.getActionMasked() == MotionEvent.ACTION_UP) {
                            view.performClick();
                            toggleMinimized();
                        }
                        return true;
                    default:
                        return false;
                }
            }
        });
    }

    private void refreshText() {
        if (title == null || detail == null) return;
        int effectiveChunks = blocksToChunks(effectiveRadiusBlocks);
        int serverChunks = blocksToChunks(serverRadiusBlocks);
        title.setText(String.format(
            Locale.getDefault(),
            "ЧАНКИ  •  %,d",
            retainedChunks
        ));
        if (minimized) {
            detail.setText(String.format(
                Locale.getDefault(),
                "%s  •  %d чанков",
                formatBytes(retainedBytes),
                effectiveChunks > 0 ? effectiveChunks : configuredRadiusChunks
            ));
            return;
        }
        String radius = publisherUpdates == 0
            ? "радиус: ожидается"
            : serverChunks == effectiveChunks
                ? "радиус: " + effectiveChunks
                : "радиус: " + serverChunks + " → " + effectiveChunks;
        String memory = formatBytes(retainedBytes) + " / " +
            formatBytes(maximumBytes);
        String health = parseFailures == 0 && evictedMemory == 0
            ? "стабильно"
            : "ошибки " + parseFailures + " • память " + evictedMemory;
        detail.setText(String.format(
            Locale.getDefault(),
            "%s чанков  •  %s\n%s  •  команд %,d/%d\n%s  •  вне радиуса %,d",
            effectiveChunks > 0 ? effectiveChunks : configuredRadiusChunks,
            radius,
            memory,
            publisherUpdates,
            publisherRewrites,
            health,
            evictedRadius
        ));
    }

    private TextView action(String text) {
        TextView view = label(text, 16, true);
        view.setTextColor(0xffd7e7f7);
        view.setGravity(Gravity.CENTER);
        view.setBackground(actionBackground());
        view.setPadding(sdp(5), 0, sdp(5), 0);
        view.setMinWidth(sdp(28));
        view.setMinHeight(sdp(28));
        return view;
    }

    private TextView label(String text, int sp, boolean bold) {
        TextView view = new TextView(context);
        view.setText(text);
        view.setTextSize(sp * scalePercent / 100f);
        if (bold) {
            view.setTypeface(
                android.graphics.Typeface.DEFAULT,
                android.graphics.Typeface.BOLD
            );
        }
        return view;
    }

    private GradientDrawable background() {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(0xe6111822);
        drawable.setCornerRadius(sdp(12));
        drawable.setStroke(Math.max(1, sdp(1)), 0x664fd5ff);
        return drawable;
    }

    private GradientDrawable actionBackground() {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(0x442a3a4d);
        drawable.setCornerRadius(sdp(8));
        return drawable;
    }

    private int sdp(int value) {
        return Math.max(1, Math.round(dp(value) * scalePercent / 100f));
    }

    private int dp(int value) {
        return Math.round(
            value * context.getResources().getDisplayMetrics().density
        );
    }

    private static int blocksToChunks(int blocks) {
        return blocks <= 0 ? 0 : (blocks + 15) / 16;
    }

    private static String formatBytes(long bytes) {
        if (bytes >= 1024L * 1024L) {
            return String.format(
                Locale.getDefault(),
                "%.1f МБ",
                bytes / (1024.0 * 1024.0)
            );
        }
        if (bytes >= 1024L) {
            return String.format(
                Locale.getDefault(),
                "%.1f КБ",
                bytes / 1024.0
            );
        }
        return bytes + " Б";
    }
}
