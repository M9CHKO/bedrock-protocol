package com.m9chko.bedrockrelay;

import android.animation.ValueAnimator;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.drawable.GradientDrawable;
import android.provider.Settings;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewConfiguration;
import android.view.WindowManager;
import android.view.animation.DecelerateInterpolator;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

import java.util.Locale;

/** Session-scoped Toolbox-style controls displayed over Minecraft. */
final class RelayOverlayController {
    private final Context context;
    private final SharedPreferences preferences;
    private final Runnable settingsChanged;
    private final WindowManager windowManager;

    private WindowManager.LayoutParams windowParams;
    private LinearLayout windowRoot;
    private TextView drawerTab;
    private ValueAnimator drawerAnimator;
    private int drawerPanelWidth;
    private boolean drawerOpen;
    private boolean missingPermissionLogged;

    RelayOverlayController(
        Context context,
        SharedPreferences preferences,
        Runnable settingsChanged
    ) {
        this.context = context;
        this.preferences = preferences;
        this.settingsChanged = settingsChanged;
        this.windowManager = (WindowManager) context.getSystemService(
            Context.WINDOW_SERVICE
        );
    }

    void show() {
        if (windowRoot != null) return;
        if (!Settings.canDrawOverlays(context)) {
            if (!missingPermissionLogged) {
                missingPermissionLogged = true;
                DiagnosticsLog.append(
                    context,
                    "WARN",
                    "overlay",
                    "Overlay permission is missing; in-game menu was not shown"
                );
            }
            return;
        }

        missingPermissionLogged = false;
        drawerPanelWidth = expandedPanelWidth();
        drawerOpen = false;
        windowRoot = new LinearLayout(context);
        windowRoot.setOrientation(LinearLayout.HORIZONTAL);
        windowRoot.setGravity(Gravity.TOP);
        windowRoot.setClipChildren(false);
        windowRoot.addView(
            buildPanel(),
            new LinearLayout.LayoutParams(
                drawerPanelWidth,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        );
        drawerTab = buildDrawerTab();
        windowRoot.addView(
            drawerTab,
            new LinearLayout.LayoutParams(dp(52), dp(68))
        );
        windowParams = new WindowManager.LayoutParams(
            drawerPanelWidth + dp(52),
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE |
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL |
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN |
                WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
            PixelFormat.TRANSLUCENT
        );
        windowParams.gravity = Gravity.TOP | Gravity.START;
        windowParams.x = -drawerPanelWidth;
        windowParams.y = dp(72);
        try {
            windowManager.addView(windowRoot, windowParams);
            final LinearLayout addedRoot = windowRoot;
            addedRoot.post(() -> {
                if (windowRoot == addedRoot) {
                    animateDrawer(true);
                }
            });
            DiagnosticsLog.append(
                context,
                "INFO",
                "overlay",
                "In-game relay menu opened"
            );
        } catch (Throwable error) {
            windowRoot = null;
            windowParams = null;
            DiagnosticsLog.appendError(
                context,
                "overlay",
                "Failed to open in-game relay menu",
                error
            );
        }
    }

    void hide() {
        LinearLayout root = windowRoot;
        if (drawerAnimator != null) {
            drawerAnimator.cancel();
            drawerAnimator = null;
        }
        windowRoot = null;
        drawerTab = null;
        windowParams = null;
        if (root == null) return;
        try {
            windowManager.removeViewImmediate(root);
            DiagnosticsLog.append(
                context,
                "INFO",
                "overlay",
                "In-game relay menu closed"
            );
        } catch (Throwable error) {
            DiagnosticsLog.appendError(
                context,
                "overlay",
                "Failed to close in-game relay menu",
                error
            );
        }
    }

    private TextView buildDrawerTab() {
        TextView tab = text("CPE\n›", 13, true);
        tab.setGravity(Gravity.CENTER);
        tab.setTextColor(Color.WHITE);
        tab.setBackground(tabBackground());
        tab.setElevation(dp(12));
        tab.setContentDescription(
            "Открыть меню CPE Relay; удерживайте и двигайте по вертикали"
        );
        tab.setOnClickListener(view -> animateDrawer(!drawerOpen));
        attachDrawerTabDrag(tab);
        return tab;
    }

    private View buildPanel() {
        LinearLayout panel = new LinearLayout(context);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(14), dp(10), dp(14), dp(14));
        panel.setBackground(panelBackground(dp(18)));
        panel.setElevation(dp(10));

        TextView title = text("CPE Relay  •  подключено", 16, true);
        title.setTextColor(Color.WHITE);
        title.setPadding(dp(2), dp(6), dp(4), dp(10));
        panel.addView(title);

        Switch detailedLogs = new Switch(context);
        detailedLogs.setText("Подробные логи");
        detailedLogs.setTextSize(15);
        detailedLogs.setTextColor(Color.WHITE);
        detailedLogs.setChecked(preferences.getBoolean(
            RelayService.KEY_DETAILED_LOGS,
            true
        ));
        panel.addView(detailedLogs, margins(-1, -2, 0, 4, 0, 0));
        TextView logsHint = text(
            "При выключении ошибки и основные события всё равно сохраняются.",
            12,
            false
        );
        logsHint.setTextColor(0xffc4cad3);
        panel.addView(logsHint, margins(-1, -2, dp(2), 0, dp(2), dp(10)));

        Switch retention = new Switch(context);
        retention.setText("Удерживать старые чанки");
        retention.setTextSize(15);
        retention.setTextColor(Color.WHITE);
        retention.setChecked(preferences.getBoolean(
            RelayService.KEY_CHUNK_RETENTION,
            false
        ));
        panel.addView(retention, margins(-1, -2, 0, 2, 0, 2));

        int initialRadius = RelayService.clampRetainedRadius(
            preferences.getInt(RelayService.KEY_RETAINED_RADIUS_CHUNKS, 24)
        );
        TextView radiusLabel = text("", 13, true);
        radiusLabel.setTextColor(0xffdce6f5);
        panel.addView(radiusLabel, margins(-1, -2, dp(2), 2, dp(2), 0));

        LinearLayout radiusRow = new LinearLayout(context);
        radiusRow.setOrientation(LinearLayout.HORIZONTAL);
        radiusRow.setGravity(Gravity.CENTER_VERTICAL);
        Button minus = smallButton("−");
        minus.setContentDescription("Уменьшить радиус удержания");
        radiusRow.addView(minus, new LinearLayout.LayoutParams(dp(46), dp(42)));
        SeekBar radius = new SeekBar(context);
        radius.setMin(RelayService.MIN_RETAINED_RADIUS_CHUNKS);
        radius.setMax(RelayService.MAX_RETAINED_RADIUS_CHUNKS);
        radius.setProgress(initialRadius);
        radiusRow.addView(radius, new LinearLayout.LayoutParams(
            0,
            ViewGroup.LayoutParams.WRAP_CONTENT,
            1f
        ));
        Button plus = smallButton("+");
        plus.setContentDescription("Увеличить радиус удержания");
        radiusRow.addView(plus, new LinearLayout.LayoutParams(dp(46), dp(42)));
        panel.addView(radiusRow);

        TextView chunksHint = text(
            "Relay хранит полученные level_chunk в памяти текущей сессии и " +
                "просит Minecraft не выгружать их в выбранном радиусе. Сервер " +
                "не начнёт присылать новые дальше своего лимита. При выходе " +
                "кэш очищается; защитный лимит памяти relay — 256 МБ.",
            12,
            false
        );
        chunksHint.setTextColor(0xffc4cad3);
        panel.addView(chunksHint, margins(-1, -2, dp(2), 2, dp(2), 0));

        Runnable refreshRadius = () -> radiusLabel.setText(String.format(
            Locale.getDefault(),
            "Радиус удержания: %d чанков (%d блоков)",
            radius.getProgress(),
            radius.getProgress() * 16
        ));
        Runnable updateEnabled = () -> {
            boolean enabled = retention.isChecked();
            radius.setEnabled(enabled);
            minus.setEnabled(enabled);
            plus.setEnabled(enabled);
            radiusLabel.setAlpha(enabled ? 1f : 0.55f);
        };
        refreshRadius.run();
        updateEnabled.run();

        detailedLogs.setOnCheckedChangeListener((button, checked) -> {
            preferences.edit()
                .putBoolean(RelayService.KEY_DETAILED_LOGS, checked)
                .apply();
            settingsChanged.run();
        });
        retention.setOnCheckedChangeListener((button, checked) -> {
            preferences.edit()
                .putBoolean(RelayService.KEY_CHUNK_RETENTION, checked)
                .apply();
            updateEnabled.run();
            settingsChanged.run();
        });
        radius.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(
                SeekBar seekBar,
                int progress,
                boolean fromUser
            ) {
                refreshRadius.run();
            }

            @Override public void onStartTrackingTouch(SeekBar seekBar) {}

            @Override public void onStopTrackingTouch(SeekBar seekBar) {
                saveRadius(seekBar.getProgress());
            }
        });
        minus.setOnClickListener(view -> {
            radius.setProgress(Math.max(
                RelayService.MIN_RETAINED_RADIUS_CHUNKS,
                radius.getProgress() - 1
            ));
            saveRadius(radius.getProgress());
        });
        plus.setOnClickListener(view -> {
            radius.setProgress(Math.min(
                RelayService.MAX_RETAINED_RADIUS_CHUNKS,
                radius.getProgress() + 1
            ));
            saveRadius(radius.getProgress());
        });
        return panel;
    }

    private void saveRadius(int radius) {
        preferences.edit()
            .putInt(
                RelayService.KEY_RETAINED_RADIUS_CHUNKS,
                RelayService.clampRetainedRadius(radius)
            )
            .apply();
        settingsChanged.run();
    }

    private void animateDrawer(boolean open) {
        if (windowRoot == null || windowParams == null) return;
        if (drawerAnimator != null) {
            drawerAnimator.cancel();
        }

        drawerOpen = open;
        updateDrawerTab();
        final int startX = windowParams.x;
        final int startY = windowParams.y;
        final int targetX = open ? 0 : -drawerPanelWidth;
        final int targetY = open
            ? Math.min(startY, maximumDrawerY(true))
            : startY;
        if (startX == targetX && startY == targetY) return;

        drawerAnimator = ValueAnimator.ofFloat(0f, 1f);
        drawerAnimator.setDuration(240L);
        drawerAnimator.setInterpolator(new DecelerateInterpolator());
        drawerAnimator.addUpdateListener(animation -> {
            if (windowRoot == null || windowParams == null) return;
            float progress = (float) animation.getAnimatedValue();
            windowParams.x = Math.round(
                startX + (targetX - startX) * progress
            );
            windowParams.y = Math.round(
                startY + (targetY - startY) * progress
            );
            updateWindowLayout();
        });
        drawerAnimator.start();
    }

    private void updateDrawerTab() {
        if (drawerTab == null) return;
        if (drawerOpen) {
            drawerTab.setText("‹\nCPE");
            drawerTab.setContentDescription(
                "Скрыть меню CPE Relay; удерживайте и двигайте по вертикали"
            );
        } else {
            drawerTab.setText("CPE\n›");
            drawerTab.setContentDescription(
                "Открыть меню CPE Relay; удерживайте и двигайте по вертикали"
            );
        }
    }

    private void attachDrawerTabDrag(View handle) {
        final int touchSlop = ViewConfiguration.get(context)
            .getScaledTouchSlop();
        handle.setOnTouchListener(new View.OnTouchListener() {
            private int originalY;
            private float downX;
            private float downY;
            private boolean moved;

            @Override public boolean onTouch(View view, MotionEvent event) {
                if (windowParams == null || windowRoot == null) return false;
                switch (event.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        if (drawerAnimator != null) {
                            drawerAnimator.cancel();
                            drawerAnimator = null;
                        }
                        originalY = windowParams.y;
                        downX = event.getRawX();
                        downY = event.getRawY();
                        moved = false;
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        int deltaX = Math.round(event.getRawX() - downX);
                        int deltaY = Math.round(event.getRawY() - downY);
                        moved |= Math.abs(deltaX) > touchSlop ||
                            Math.abs(deltaY) > touchSlop;
                        windowParams.y = Math.max(
                            0,
                            Math.min(
                                maximumDrawerY(drawerOpen),
                                originalY + deltaY
                            )
                        );
                        updateWindowLayout();
                        return true;
                    case MotionEvent.ACTION_UP:
                        if (!moved) view.performClick();
                        return true;
                    case MotionEvent.ACTION_CANCEL:
                        return true;
                    default:
                        return false;
                }
            }
        });
    }

    private int maximumDrawerY(boolean open) {
        int visibleHeight = dp(68);
        if (open && windowRoot != null && windowRoot.getHeight() > 0) {
            visibleHeight = windowRoot.getHeight();
        }
        return Math.max(
            0,
            context.getResources().getDisplayMetrics().heightPixels -
                visibleHeight
        );
    }

    private void updateWindowLayout() {
        if (windowRoot == null || windowParams == null) return;
        try {
            windowManager.updateViewLayout(windowRoot, windowParams);
        } catch (Throwable ignored) {
            // The service may remove the overlay while an animation frame is
            // already queued on the main thread.
        }
    }

    private GradientDrawable panelBackground(float radius) {
        GradientDrawable background = new GradientDrawable();
        background.setColor(0xeb151a22);
        background.setCornerRadius(radius);
        background.setStroke(dp(1), 0xff5f789d);
        return background;
    }

    private GradientDrawable tabBackground() {
        GradientDrawable background = new GradientDrawable();
        background.setColor(0xf21b2330);
        background.setCornerRadii(new float[] {
            0, 0,
            dp(16), dp(16),
            dp(16), dp(16),
            0, 0
        });
        background.setStroke(dp(1), 0xff7f9bc4);
        return background;
    }

    private Button smallButton(String label) {
        Button button = new Button(context);
        button.setText(label);
        button.setTextSize(18);
        button.setTextColor(Color.WHITE);
        button.setAllCaps(false);
        button.setPadding(0, 0, 0, 0);
        return button;
    }

    private TextView text(String value, int sizeSp, boolean bold) {
        TextView text = new TextView(context);
        text.setText(value);
        text.setTextSize(sizeSp);
        text.setTextColor(Color.WHITE);
        if (bold) text.setTypeface(text.getTypeface(), android.graphics.Typeface.BOLD);
        return text;
    }

    private LinearLayout.LayoutParams margins(
        int width,
        int height,
        int left,
        int top,
        int right,
        int bottom
    ) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
            width,
            height
        );
        params.setMargins(left, top, right, bottom);
        return params;
    }

    private int expandedPanelWidth() {
        return Math.min(
            dp(320),
            context.getResources().getDisplayMetrics().widthPixels - dp(60)
        );
    }

    private int dp(float value) {
        return Math.round(
            value * context.getResources().getDisplayMetrics().density
        );
    }
}
