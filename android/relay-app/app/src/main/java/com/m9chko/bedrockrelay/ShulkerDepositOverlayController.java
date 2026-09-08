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

/** Separate, non-focusable control: deliberately survives container GUI opens. */
final class ShulkerDepositOverlayController {
    private static final String KEY_X = "shulker_deposit_button_x";
    private static final String KEY_Y = "shulker_deposit_button_y";
    private final Context context;
    private final SharedPreferences preferences;
    private final Runnable settingsChanged;
    private final WindowManager windows;
    private boolean sessionVisible;
    private TextView button;
    private WindowManager.LayoutParams params;
    private String status = "Откройте сундук";
    private int sent;
    private final boolean autoCraft;
    private final boolean mapQueue;
    private boolean mapLoaded;
    private int mapTotal;
    private boolean running;
    private boolean busy;
    private int crafted;
    private String template = "";
    private long lastClick;

    ShulkerDepositOverlayController(Context context, SharedPreferences preferences,
            Runnable settingsChanged) {
        this(context, preferences, settingsChanged, false);
    }

    ShulkerDepositOverlayController(Context context, SharedPreferences preferences,
            Runnable settingsChanged, boolean autoCraft) {
        this(context,preferences,settingsChanged,autoCraft,false);
    }
    ShulkerDepositOverlayController(Context context, SharedPreferences preferences,
            Runnable settingsChanged, boolean autoCraft,boolean mapQueue) {
        this.context = context;
        this.mapQueue=mapQueue;
        this.autoCraft = autoCraft;
        this.preferences = preferences;
        this.settingsChanged = settingsChanged;
        windows = (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);
    }

    // There is intentionally no setUiBlocked(). Only session/background and
    // the explicit show-button setting control this window's visibility.
    static boolean shouldShow(boolean sessionVisible, boolean showButton) {
        return sessionVisible && showButton;
    }

    void setSessionVisible(boolean visible) {
        sessionVisible = visible;
        configure();
    }

    void update(JSONObject value) {
        if (value == null) return;
        String previousStatus = status;
        sent = value.optInt(autoCraft ? "stored" : "sent", 0);
        crafted = value.optInt("crafted", 0);
        template = value.optString("template", "");
        running = value.optBoolean("running", false);
        busy = value.optBoolean("busy", false);
        status = value.optString("status", status);
        if(mapQueue){sent=value.optInt("completed");crafted=value.optInt("maps");template=value.optString("file");mapTotal=value.optInt("total");boolean loaded=value.optBoolean("loaded");if(loaded!=mapLoaded){mapLoaded=loaded;configure();}}
        if (autoCraft && sessionVisible && !busy && !status.equals(previousStatus)) {
            android.widget.Toast.makeText(context, status, android.widget.Toast.LENGTH_LONG).show();
        }
        refreshText();
    }

    void configure() {
        if ((!mapQueue||mapLoaded) && shouldShow(sessionVisible, preferences.getBoolean(
                mapQueue ? "map_queue_button" : autoCraft ? RelayService.KEY_AUTO_CRAFT_STORE_BUTTON : RelayService.KEY_SHULKER_DEPOSIT_BUTTON, true))) addWindow();
        else removeWindow();
        refreshText();
    }

    void destroy() { sessionVisible = false; removeWindow(); }

    private void addWindow() {
        if (button != null || !Settings.canDrawOverlays(context)) return;
        button = new TextView(context);
        button.setTextColor(Color.WHITE);
        button.setTextSize(11);
        button.setGravity(Gravity.CENTER);
        button.setMinWidth(dp(96));
        button.setMinHeight(dp(46));
        button.setPadding(dp(8), dp(6), dp(8), dp(6));
        button.setElevation(dp(7));
        button.setOnClickListener(view -> {
            if (autoCraft) {
                long now = android.os.SystemClock.elapsedRealtime();
                if (now - lastClick < 350) return;
                lastClick = now;
                settingsChanged.run(); return;
            }
            boolean active = preferences.getBoolean(RelayService.KEY_SHULKER_DEPOSIT_ENABLED, false);
            preferences.edit().putBoolean(RelayService.KEY_SHULKER_DEPOSIT_ENABLED, !active).apply();
            settingsChanged.run();
            refreshText();
        });
        button.setOnLongClickListener(view -> {
            android.widget.Toast.makeText(context, (autoCraft && !template.isEmpty() ? "NBT: " + template + "\n" : "") +
                status, android.widget.Toast.LENGTH_LONG).show();
            return true;
        });
        params = new WindowManager.LayoutParams(
            WindowManager.LayoutParams.WRAP_CONTENT, WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE |
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL |
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSLUCENT);
        params.gravity = Gravity.TOP | Gravity.START;
        params.x = preferences.getInt(mapQueue?"maps_button_x":autoCraft ? "auto2_button_x" : KEY_X, dp(mapQueue?260:autoCraft ? 140 : 12));
        params.y = preferences.getInt(mapQueue?"maps_button_y":autoCraft ? "auto2_button_y" : KEY_Y, dp(310));
        // Minecraft is usually landscape; keep a new/saved button onscreen.
        params.x = Math.max(0, Math.min(params.x, context.getResources().getDisplayMetrics().widthPixels - dp(110)));
        params.y = Math.max(0, Math.min(params.y, context.getResources().getDisplayMetrics().heightPixels - dp(70)));
        attachDrag();
        try { windows.addView(button, params); }
        catch (RuntimeException error) {
            button = null;
            params = null;
            DiagnosticsLog.appendError(context, "shulker_deposit", "Could not show control", error);
        }
    }

    private void attachDrag() {
        final int slop = ViewConfiguration.get(context).getScaledTouchSlop();
        button.setOnTouchListener(new View.OnTouchListener() {
            float x, y;
            int originalX, originalY;
            boolean dragged;
            long downTime;
            @Override public boolean onTouch(View view, MotionEvent event) {
                if (params == null) return false;
                switch (event.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        x = event.getRawX(); y = event.getRawY();
                        originalX = params.x; originalY = params.y;
                        dragged = false; downTime = event.getEventTime();
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        float dx = event.getRawX() - x, dy = event.getRawY() - y;
                        dragged |= Math.abs(dx) > slop || Math.abs(dy) > slop;
                        if (dragged) {
                            int maxX = Math.max(0, context.getResources().getDisplayMetrics().widthPixels - view.getWidth());
                            int maxY = Math.max(0, context.getResources().getDisplayMetrics().heightPixels - view.getHeight());
                            params.x = Math.max(0, Math.min(maxX, originalX + Math.round(dx)));
                            params.y = Math.max(0, Math.min(maxY, originalY + Math.round(dy)));
                            try { windows.updateViewLayout(button, params); }
                            catch (RuntimeException ignored) { }
                        }
                        return true;
                    case MotionEvent.ACTION_UP:
                        if (dragged) preferences.edit().putInt(mapQueue?"maps_button_x":autoCraft ? "auto2_button_x" : KEY_X, params.x)
                            .putInt(mapQueue?"maps_button_y":autoCraft ? "auto2_button_y" : KEY_Y, params.y).apply();
                        else if (event.getEventTime() - downTime >= ViewConfiguration.getLongPressTimeout()) view.performLongClick();
                        else view.performClick();
                        return true;
                    case MotionEvent.ACTION_CANCEL: return true;
                    default: return false;
                }
            }
        });
    }

    private void refreshText() {
        if (button == null) return;
        boolean enabled = autoCraft ? running : preferences.getBoolean(RelayService.KEY_SHULKER_DEPOSIT_ENABLED, false);
        String label = autoCraft ? (busy ? (running ? "Авто 2: СТОП" : "Авто 2: завершение") +
            "\nКрафт: " + crafted + " · В сундук: " + sent : "Авто 2: СТАРТ") :
            enabled ? "Разгрузка: ВКЛ\nОтправлено: " + sent : "Разгрузка: ВЫКЛ";
        if(mapQueue)label=(busy?"Карты ZIP: СТОП":"Карты ZIP: СТАРТ")+"\n"+sent+" / "+mapTotal+" · Карт: "+crafted;
        if (label.contentEquals(button.getText())) return;
        button.setText(label);
        button.setContentDescription(label + (autoCraft ? ". NBT: " + template : "") +
            ". Нажмите для переключения; удерживайте для статуса; перетащите для перемещения.");
        GradientDrawable background = new GradientDrawable();
        background.setColor(enabled ? 0xe6256547 : 0xe62c3443);
        background.setCornerRadius(dp(12));
        background.setStroke(dp(1), enabled ? 0xff78d59d : 0xff92a1b7);
        button.setBackground(background);
    }

    private void removeWindow() {
        TextView old = button;
        button = null; params = null;
        if (old != null) {
            try { windows.removeViewImmediate(old); }
            catch (RuntimeException ignored) { }
        }
    }

    private int dp(int value) {
        return Math.round(value * context.getResources().getDisplayMetrics().density);
    }
}
