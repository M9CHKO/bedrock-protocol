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
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
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
    private FrameLayout windowRoot;
    private boolean collapsed;
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
        collapsed = false;
        windowRoot = new FrameLayout(context);
        windowParams = new WindowManager.LayoutParams(
            expandedWidth(),
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE |
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL |
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSLUCENT
        );
        windowParams.gravity = Gravity.TOP | Gravity.START;
        windowParams.x = dp(12);
        windowParams.y = dp(72);
        render();
        try {
            windowManager.addView(windowRoot, windowParams);
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
        FrameLayout root = windowRoot;
        windowRoot = null;
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

    private void render() {
        if (windowRoot == null || windowParams == null) return;
        windowRoot.removeAllViews();
        if (collapsed) {
            windowParams.width = dp(74);
            windowParams.height = dp(50);
            TextView bubble = text("CPE", 15, true);
            bubble.setGravity(Gravity.CENTER);
            bubble.setTextColor(Color.WHITE);
            bubble.setBackground(panelBackground(dp(16)));
            bubble.setElevation(dp(8));
            attachDragHandle(bubble, () -> {
                collapsed = false;
                render();
            });
            windowRoot.addView(bubble, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            ));
        } else {
            windowParams.width = expandedWidth();
            windowParams.height = WindowManager.LayoutParams.WRAP_CONTENT;
            windowRoot.addView(
                buildPanel(),
                new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT
                )
            );
        }
        try {
            windowManager.updateViewLayout(windowRoot, windowParams);
        } catch (Throwable ignored) {
            // The first render happens before addView().
        }
    }

    private View buildPanel() {
        LinearLayout panel = new LinearLayout(context);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(14), dp(10), dp(14), dp(14));
        panel.setBackground(panelBackground(dp(18)));
        panel.setElevation(dp(10));

        LinearLayout titleRow = new LinearLayout(context);
        titleRow.setOrientation(LinearLayout.HORIZONTAL);
        titleRow.setGravity(Gravity.CENTER_VERTICAL);
        TextView title = text("CPE Relay  •  подключено", 16, true);
        title.setTextColor(Color.WHITE);
        title.setPadding(dp(2), dp(6), dp(4), dp(8));
        attachDragHandle(title, null);
        titleRow.addView(title, new LinearLayout.LayoutParams(
            0,
            ViewGroup.LayoutParams.WRAP_CONTENT,
            1f
        ));
        Button collapse = smallButton("—");
        collapse.setContentDescription("Свернуть меню CPE Relay");
        collapse.setOnClickListener(view -> {
            collapsed = true;
            render();
        });
        titleRow.addView(collapse, new LinearLayout.LayoutParams(
            dp(48),
            dp(42)
        ));
        panel.addView(titleRow);

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
            "Relay сохраняет видимыми уже полученные чанки. Сервер не начнёт " +
                "присылать новые дальше своего лимита. Большой радиус расходует " +
                "больше памяти Minecraft.",
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

    private void attachDragHandle(View handle, Runnable tapAction) {
        if (tapAction != null) {
            handle.setOnClickListener(view -> tapAction.run());
        }
        handle.setOnTouchListener(new View.OnTouchListener() {
            private int originalX;
            private int originalY;
            private float downX;
            private float downY;
            private boolean moved;

            @Override public boolean onTouch(View view, MotionEvent event) {
                if (windowParams == null || windowRoot == null) return false;
                switch (event.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        originalX = windowParams.x;
                        originalY = windowParams.y;
                        downX = event.getRawX();
                        downY = event.getRawY();
                        moved = false;
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        int deltaX = Math.round(event.getRawX() - downX);
                        int deltaY = Math.round(event.getRawY() - downY);
                        moved |= Math.abs(deltaX) > dp(4) ||
                            Math.abs(deltaY) > dp(4);
                        windowParams.x = Math.max(0, originalX + deltaX);
                        windowParams.y = Math.max(0, originalY + deltaY);
                        try {
                            windowManager.updateViewLayout(windowRoot, windowParams);
                        } catch (Throwable ignored) {
                        }
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

    private GradientDrawable panelBackground(float radius) {
        GradientDrawable background = new GradientDrawable();
        background.setColor(0xeb151a22);
        background.setCornerRadius(radius);
        background.setStroke(dp(1), 0xff5f789d);
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

    private int expandedWidth() {
        return Math.min(
            dp(320),
            context.getResources().getDisplayMetrics().widthPixels - dp(24)
        );
    }

    private int dp(float value) {
        return Math.round(
            value * context.getResources().getDisplayMetrics().density
        );
    }
}
