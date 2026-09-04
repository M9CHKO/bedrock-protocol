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
import android.view.WindowManager;
import android.widget.TextView;

import org.json.JSONObject;

import java.util.Locale;

/** Small draggable start/stop control for the server-confirmed area filler. */
final class AreaFillOverlayController {
    private final Context context;
    private final SharedPreferences preferences;
    private final WindowManager windowManager;

    private boolean sessionVisible;
    private boolean uiBlocked;
    private boolean enabled;
    private int scalePercent = 90;
    private TextView button;
    private WindowManager.LayoutParams params;

    private boolean running;
    private boolean waitingForBlocks;
    private int pointCount;
    private int requiredPoints = 2;
    private int cellCount;
    private int completed;
    private String status = "Добавьте точки области";

    AreaFillOverlayController(
        Context context,
        SharedPreferences preferences
    ) {
        this.context = context;
        this.preferences = preferences;
        this.windowManager = (WindowManager) context.getSystemService(
            Context.WINDOW_SERVICE
        );
    }

    void configure(boolean show, int scale) {
        int clampedScale = RelayService.clampOverlayScale(scale);
        boolean changed = enabled != show || scalePercent != clampedScale;
        enabled = show;
        scalePercent = clampedScale;
        if (changed && button != null) removeWindow();
        reconcile();
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

    void update(JSONObject value) {
        if (value == null) return;
        enabled = value.optBoolean("enabled", enabled);
        running = value.optBoolean("running", false);
        waitingForBlocks = value.optBoolean("waitingForBlocks", false);
        pointCount = value.optInt("pointCount", 0);
        requiredPoints = value.optInt("requiredPoints", requiredPoints);
        cellCount = value.optInt("cellCount", 0);
        completed = value.optInt("completed", 0);
        status = value.optString("status", status);
        reconcile();
        refreshText();
    }

    void destroy() {
        sessionVisible = false;
        removeWindow();
    }

    private void reconcile() {
        if (sessionVisible && enabled && !uiBlocked) addWindow();
        else removeWindow();
    }

    private void addWindow() {
        if (button != null || !Settings.canDrawOverlays(context)) return;
        button = new TextView(context);
        button.setGravity(Gravity.CENTER);
        button.setTextColor(Color.WHITE);
        button.setTextSize(10f * scalePercent / 100f);
        button.setTypeface(
            android.graphics.Typeface.DEFAULT,
            android.graphics.Typeface.BOLD
        );
        button.setMinWidth(sdp(76));
        button.setMinHeight(sdp(44));
        button.setPadding(sdp(8), sdp(5), sdp(8), sdp(5));
        button.setElevation(sdp(9));
        button.setClickable(true);
        button.setContentDescription(
            "Запустить или остановить автозаполнение; перетащите для перемещения"
        );
        attachDragAndClick(button);

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
        params.x = preferences.getInt(RelayService.KEY_AREA_FILL_BUTTON_X, dp(12));
        params.y = preferences.getInt(RelayService.KEY_AREA_FILL_BUTTON_Y, dp(245));
        try {
            windowManager.addView(button, params);
            refreshText();
        } catch (Throwable error) {
            button = null;
            params = null;
            DiagnosticsLog.appendError(
                context,
                "area_fill",
                "Failed to show area-fill control",
                error
            );
        }
    }

    private void removeWindow() {
        TextView view = button;
        button = null;
        params = null;
        if (view == null) return;
        try {
            windowManager.removeViewImmediate(view);
        } catch (Throwable ignored) {
        }
    }

    private void attachDragAndClick(View target) {
        final int slop = ViewConfiguration.get(context).getScaledTouchSlop();
        target.setOnTouchListener(new View.OnTouchListener() {
            private float downRawX;
            private float downRawY;
            private int downX;
            private int downY;
            private boolean moved;

            @Override
            public boolean onTouch(View view, MotionEvent event) {
                if (params == null || button == null) return false;
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
                                windowManager.updateViewLayout(button, params);
                            } catch (Throwable ignored) {
                            }
                        }
                        return true;
                    case MotionEvent.ACTION_UP:
                        if (moved) {
                            preferences.edit()
                                .putInt(RelayService.KEY_AREA_FILL_BUTTON_X, params.x)
                                .putInt(RelayService.KEY_AREA_FILL_BUTTON_Y, params.y)
                                .apply();
                        } else {
                            view.performClick();
                            toggleRun();
                        }
                        return true;
                    case MotionEvent.ACTION_CANCEL:
                        return true;
                    default:
                        return false;
                }
            }
        });
    }

    private void toggleRun() {
        try {
            update(new JSONObject(NativeBridge.toggleAreaFill()));
        } catch (Throwable error) {
            status = "Ошибка управления: " + safeMessage(error);
            running = false;
            refreshText();
            DiagnosticsLog.appendError(
                context,
                "area_fill",
                "Area-fill toggle failed",
                error
            );
        }
    }

    private void refreshText() {
        if (button == null) return;
        String progress = cellCount > 0
            ? String.format(Locale.getDefault(), "%d/%d", completed, cellCount)
            : String.format(Locale.getDefault(), "%d/%d точек", pointCount, requiredPoints);
        String symbol = running ? "■" : "▶";
        button.setText("ЗАЛИВКА  " + symbol + "\n" + progress);
        button.setTextColor(
            waitingForBlocks ? 0xffffcf72 : running ? 0xff8ff0ad : Color.WHITE
        );
        button.setBackground(background());
        button.setContentDescription(
            (status == null ? "Автозаполнение" : status) +
                ". Нажмите для запуска или остановки"
        );
    }

    private GradientDrawable background() {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(running ? 0xee153c2a : 0xe6111822);
        drawable.setCornerRadius(sdp(11));
        drawable.setStroke(
            Math.max(1, sdp(1)),
            waitingForBlocks ? 0xaaffb52e : running ? 0xaa36d67e : 0x884fd5ff
        );
        return drawable;
    }

    private static String safeMessage(Throwable error) {
        String message = error == null ? null : error.getMessage();
        return message == null || message.isEmpty()
            ? "неизвестная ошибка"
            : message;
    }

    private int sdp(int value) {
        return Math.max(1, Math.round(dp(value) * scalePercent / 100f));
    }

    private int dp(int value) {
        return Math.round(value * context.getResources().getDisplayMetrics().density);
    }
}
