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
import android.widget.ScrollView;
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
    private TextView chunkStatus;
    private ValueAnimator drawerAnimator;
    private int drawerPanelWidth;
    private boolean drawerOpen;
    private boolean missingPermissionLogged;
    private boolean statusRetentionEnabled;
    private int statusConfiguredRadiusChunks = 24;
    private long statusPublisherUpdates;
    private long statusPublisherRewrites;
    private int statusServerRadiusBlocks;
    private int statusEffectiveRadiusBlocks;
    private long statusRetainedChunks;
    private long statusRetainedBytes;
    private long statusMaximumBytes = 256L * 1024L * 1024L;
    private long statusEvictedRadius;
    private long statusEvictedMemory;
    private long statusParseFailures;

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
        chunkStatus = null;
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
        BoundedScrollView scroll = new BoundedScrollView(context);
        scroll.setBackground(panelBackground(dp(18)));
        scroll.setElevation(dp(10));
        scroll.setClipToOutline(true);
        scroll.setFillViewport(false);
        scroll.setVerticalScrollBarEnabled(true);

        LinearLayout panel = new LinearLayout(context);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(14), dp(10), dp(14), dp(14));

        TextView title = text("CPE Relay  •  подключено", 16, true);
        title.setTextColor(Color.WHITE);
        title.setPadding(dp(2), dp(6), dp(4), dp(10));
        panel.addView(title);

        Switch entityOutlines = new Switch(context);
        entityOutlines.setText("Обводка сущностей (2D)");
        entityOutlines.setTextSize(15);
        entityOutlines.setTextColor(Color.WHITE);
        entityOutlines.setChecked(preferences.getBoolean(
            RelayService.KEY_ENTITY_OUTLINES,
            true
        ));
        panel.addView(entityOutlines, margins(-1, -2, 0, 2, 0, 0));

        int initialEntityFov = RelayService.clampEntityFov(
            preferences.getInt(RelayService.KEY_ENTITY_FOV, 70)
        );
        TextView entityFovLabel = text("", 12, true);
        entityFovLabel.setTextColor(0xffdce6f5);
        panel.addView(
            entityFovLabel,
            margins(-1, -2, dp(2), 0, dp(2), 0)
        );
        SeekBar entityFov = new SeekBar(context);
        entityFov.setMin(RelayService.MIN_ENTITY_FOV);
        entityFov.setMax(RelayService.MAX_ENTITY_FOV);
        entityFov.setProgress(initialEntityFov);
        panel.addView(entityFov, margins(-1, -2, 0, 0, 0, 0));
        TextView entityHint = text(
            "Поставьте такое же значение, как FOV в настройках Minecraft; " +
                "точнее всего совпадает вид от первого лица.",
            11,
            false
        );
        entityHint.setTextColor(0xffc4cad3);
        panel.addView(
            entityHint,
            margins(-1, -2, dp(2), 0, dp(2), dp(8))
        );

        Runnable refreshEntityFov = () -> entityFovLabel.setText(
            String.format(
                Locale.getDefault(),
                "FOV Minecraft: %d°",
                entityFov.getProgress()
            )
        );
        Runnable updateEntityControls = () -> {
            boolean enabled = entityOutlines.isChecked();
            entityFov.setEnabled(enabled);
            entityFovLabel.setAlpha(enabled ? 1f : 0.55f);
            entityHint.setAlpha(enabled ? 1f : 0.55f);
        };
        refreshEntityFov.run();
        updateEntityControls.run();

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
        statusRetentionEnabled = retention.isChecked();
        statusConfiguredRadiusChunks = initialRadius;
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

        chunkStatus = text("", 12, true);
        chunkStatus.setPadding(dp(10), dp(8), dp(10), dp(8));
        chunkStatus.setBackground(statusBackground());
        panel.addView(
            chunkStatus,
            margins(-1, -2, dp(2), dp(4), dp(2), dp(8))
        );
        refreshChunkStatus();

        TextView chunksHint = text(
            "Туман и дальность отрисовки Minecraft не показывают, выгружен " +
                "ли чанк. Проверяйте кэш и переданный клиенту радиус по " +
                "статусу выше. Новые чанки по-прежнему ограничены сервером.",
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
        entityOutlines.setOnCheckedChangeListener((button, checked) -> {
            preferences.edit()
                .putBoolean(RelayService.KEY_ENTITY_OUTLINES, checked)
                .apply();
            updateEntityControls.run();
            settingsChanged.run();
        });
        entityFov.setOnSeekBarChangeListener(
            new SeekBar.OnSeekBarChangeListener() {
                @Override public void onProgressChanged(
                    SeekBar seekBar,
                    int progress,
                    boolean fromUser
                ) {
                    refreshEntityFov.run();
                }

                @Override public void onStartTrackingTouch(SeekBar seekBar) {}

                @Override public void onStopTrackingTouch(SeekBar seekBar) {
                    preferences.edit()
                        .putInt(
                            RelayService.KEY_ENTITY_FOV,
                            RelayService.clampEntityFov(seekBar.getProgress())
                        )
                        .apply();
                    settingsChanged.run();
                }
            }
        );
        retention.setOnCheckedChangeListener((button, checked) -> {
            preferences.edit()
                .putBoolean(RelayService.KEY_CHUNK_RETENTION, checked)
                .apply();
            statusRetentionEnabled = checked;
            statusConfiguredRadiusChunks = radius.getProgress();
            if (!checked) clearLiveChunkStatus();
            refreshChunkStatus();
            updateEnabled.run();
            settingsChanged.run();
        });
        radius.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(
                SeekBar seekBar,
                int progress,
                boolean fromUser
            ) {
                statusConfiguredRadiusChunks = progress;
                refreshRadius.run();
                refreshChunkStatus();
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
        scroll.addView(
            panel,
            new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        );
        return scroll;
    }

    void updateChunkStatus(
        boolean retentionEnabled,
        int configuredRadiusChunks,
        long publisherUpdates,
        long publisherRewrites,
        int serverRadiusBlocks,
        int effectiveRadiusBlocks,
        long retainedChunks,
        long retainedBytes,
        long maximumBytes,
        long evictedRadius,
        long evictedMemory,
        long parseFailures
    ) {
        statusRetentionEnabled = retentionEnabled;
        statusConfiguredRadiusChunks = configuredRadiusChunks;
        statusPublisherUpdates = publisherUpdates;
        statusPublisherRewrites = publisherRewrites;
        statusServerRadiusBlocks = serverRadiusBlocks;
        statusEffectiveRadiusBlocks = effectiveRadiusBlocks;
        statusRetainedChunks = retainedChunks;
        statusRetainedBytes = retainedBytes;
        statusMaximumBytes = maximumBytes;
        statusEvictedRadius = evictedRadius;
        statusEvictedMemory = evictedMemory;
        statusParseFailures = parseFailures;
        refreshChunkStatus();
    }

    private void clearLiveChunkStatus() {
        statusPublisherUpdates = 0;
        statusPublisherRewrites = 0;
        statusServerRadiusBlocks = 0;
        statusEffectiveRadiusBlocks = 0;
        statusRetainedChunks = 0;
        statusRetainedBytes = 0;
        statusEvictedRadius = 0;
        statusEvictedMemory = 0;
        statusParseFailures = 0;
    }

    private void refreshChunkStatus() {
        if (chunkStatus == null) return;
        if (!statusRetentionEnabled) {
            chunkStatus.setText("Проверка чанков: удержание выключено");
            chunkStatus.setTextColor(0xffc4cad3);
            return;
        }

        String commandStatus;
        int serverRadiusChunks = blocksToChunks(statusServerRadiusBlocks);
        int effectiveRadiusChunks = blocksToChunks(
            statusEffectiveRadiusBlocks
        );
        if (statusPublisherUpdates == 0 || effectiveRadiusChunks == 0) {
            commandStatus = String.format(
                Locale.getDefault(),
                "Команда Minecraft: ожидается (цель %d чанков)",
                statusConfiguredRadiusChunks
            );
        } else if (statusEffectiveRadiusBlocks > statusServerRadiusBlocks) {
            commandStatus = String.format(
                Locale.getDefault(),
                "✓ Minecraft: сервер %d → relay %d чанков",
                serverRadiusChunks,
                effectiveRadiusChunks
            );
        } else {
            commandStatus = String.format(
                Locale.getDefault(),
                "✓ Minecraft: сервер уже дал %d чанков",
                effectiveRadiusChunks
            );
        }

        String cacheStatus = String.format(
            Locale.getDefault(),
            "Кэш relay: %,d чанков • %s / %s",
            statusRetainedChunks,
            formatBytes(statusRetainedBytes),
            formatBytes(statusMaximumBytes)
        );
        String counters = String.format(
            Locale.getDefault(),
            "Команд: %,d (изменено: %,d) • удалено: %,d",
            statusPublisherUpdates,
            statusPublisherRewrites,
            statusEvictedRadius
        );
        if (statusEvictedMemory > 0) {
            counters += String.format(
                Locale.getDefault(),
                " • память: %,d",
                statusEvictedMemory
            );
        }
        if (statusParseFailures > 0) {
            counters += String.format(
                Locale.getDefault(),
                " • ошибок: %,d",
                statusParseFailures
            );
        }

        chunkStatus.setText(commandStatus + "\n" + cacheStatus + "\n" + counters);
        boolean active = statusPublisherUpdates > 0 &&
            statusRetainedChunks > 0 && statusParseFailures == 0;
        chunkStatus.setTextColor(active ? 0xff9ee493 : 0xffffd27a);
    }

    private static int blocksToChunks(int blocks) {
        if (blocks <= 0) return 0;
        return (blocks + 15) / 16;
    }

    private static String formatBytes(long bytes) {
        if (bytes < 1024L * 1024L) {
            return String.format(
                Locale.getDefault(),
                "%.0f КБ",
                bytes / 1024.0
            );
        }
        return String.format(
            Locale.getDefault(),
            "%.1f МБ",
            bytes / (1024.0 * 1024.0)
        );
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

    private GradientDrawable statusBackground() {
        GradientDrawable background = new GradientDrawable();
        background.setColor(0x66101720);
        background.setCornerRadius(dp(10));
        background.setStroke(dp(1), 0x665f789d);
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

    private final class BoundedScrollView extends ScrollView {
        BoundedScrollView(Context context) {
            super(context);
        }

        @Override
        protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
            int maximumHeight = Math.max(
                dp(120),
                context.getResources().getDisplayMetrics().heightPixels -
                    dp(20)
            );
            int boundedHeight = View.MeasureSpec.makeMeasureSpec(
                maximumHeight,
                View.MeasureSpec.AT_MOST
            );
            super.onMeasure(widthMeasureSpec, boundedHeight);
        }
    }

    private int dp(float value) {
        return Math.round(
            value * context.getResources().getDisplayMetrics().density
        );
    }
}
