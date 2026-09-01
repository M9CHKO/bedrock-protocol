package com.m9chko.bedrockrelay;

import android.animation.ValueAnimator;
import android.content.Context;
import android.content.Intent;
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
    private volatile LinearLayout windowRoot;
    private TextView drawerTab;
    private TextView chunkStatus;
    private TextView pageTitle;
    private TextView backButton;
    private LinearLayout pageContent;
    private TextView logText;
    private String currentPage = "home";
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

    boolean isShowing() {
        return windowRoot != null;
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
        pageTitle = null;
        backButton = null;
        pageContent = null;
        logText = null;
        currentPage = "home";
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
        panel.setPadding(dp(12), dp(10), dp(12), dp(14));

        LinearLayout header = new LinearLayout(context);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        backButton = text("‹", 25, true);
        backButton.setGravity(Gravity.CENTER);
        backButton.setBackground(actionBackground());
        backButton.setOnClickListener(view -> showPage("home"));
        header.addView(backButton, new LinearLayout.LayoutParams(dp(38), dp(38)));
        pageTitle = text("CPE RELAY", 16, true);
        pageTitle.setPadding(dp(10), 0, 0, 0);
        header.addView(pageTitle, new LinearLayout.LayoutParams(
            0,
            ViewGroup.LayoutParams.WRAP_CONTENT,
            1f
        ));
        TextView live = text("● LIVE", 10, true);
        live.setTextColor(0xff73e49a);
        live.setPadding(dp(8), dp(5), dp(8), dp(5));
        live.setBackground(statusBackground());
        header.addView(live);
        panel.addView(header, margins(-1, dp(42), 0, 0, 0, dp(8)));

        pageContent = new LinearLayout(context);
        pageContent.setOrientation(LinearLayout.VERTICAL);
        panel.addView(pageContent, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ));
        showPage("home");

        scroll.addView(
            panel,
            new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        );
        return scroll;
    }

    private void showPage(String page) {
        if (pageContent == null || pageTitle == null || backButton == null) {
            return;
        }
        currentPage = page;
        pageContent.removeAllViews();
        chunkStatus = null;
        logText = null;
        backButton.setVisibility("home".equals(page) ? View.INVISIBLE : View.VISIBLE);
        switch (page) {
            case "outline":
                pageTitle.setText("ОБВОДКА");
                buildOutlinePage(pageContent);
                break;
            case "chunks":
                pageTitle.setText("ЧАНКИ");
                buildChunksPage(pageContent);
                break;
            case "equipment":
                pageTitle.setText("СНАРЯЖЕНИЕ");
                buildEquipmentPage(pageContent);
                break;
            case "logs":
                pageTitle.setText("ЖУРНАЛ");
                buildLogsPage(pageContent);
                break;
            default:
                currentPage = "home";
                pageTitle.setText("CPE RELAY");
                buildHomePage(pageContent);
                break;
        }
    }

    private void buildHomePage(LinearLayout root) {
        TextView subtitle = text(
            "Пакетный HUD работает отдельными слоями поверх Minecraft",
            11,
            false
        );
        subtitle.setTextColor(0xffaab8c8);
        subtitle.setPadding(dp(2), 0, dp(2), dp(8));
        root.addView(subtitle);
        root.addView(menuCard(
            "◎  ОБВОДКА",
            "Игроки, мобы, предметы • цвета • толщина",
            "outline"
        ));
        root.addView(menuCard(
            "▦  ЧАНКИ",
            "Удержание и отдельный перемещаемый счётчик",
            "chunks"
        ));
        root.addView(menuCard(
            "♢  СНАРЯЖЕНИЕ",
            "Прочность брони и перелив зачарований",
            "equipment"
        ));
        root.addView(menuCard(
            "≡  ЖУРНАЛ",
            "Запись полностью отключается одним переключателем",
            "logs"
        ));
        TextView note = text(
            "Точная привязка зависит от FOV и пакетной камеры. " +
                "Захват экрана и вмешательство в процесс Minecraft не используются.",
            10,
            false
        );
        note.setTextColor(0xff8391a2);
        note.setPadding(dp(3), dp(7), dp(3), 0);
        root.addView(note);
    }

    private View menuCard(String title, String subtitle, String page) {
        LinearLayout card = new LinearLayout(context);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(13), dp(11), dp(13), dp(11));
        card.setBackground(cardBackground(false));
        TextView heading = text(title, 13, true);
        heading.setTextColor(0xffedf5ff);
        card.addView(heading);
        TextView detail = text(subtitle, 10, false);
        detail.setTextColor(0xff9eacbc);
        detail.setPadding(0, dp(3), 0, 0);
        card.addView(detail);
        card.setOnClickListener(view -> showPage(page));
        card.setClickable(true);
        card.setElevation(dp(3));
        card.setLayoutParams(margins(-1, -2, 0, 0, 0, dp(7)));
        return card;
    }

    private void buildOutlinePage(LinearLayout root) {
        root.addView(toggle(
            "Показывать обводку",
            RelayService.KEY_ENTITY_OUTLINES,
            true
        ));
        TextView hint = text(
            "Микродвижения камеры фильтруются отдельно; несколько " +
                "согласованных малых пакетов запускают плавный поворот.",
            10,
            false
        );
        hint.setTextColor(0xff98a7b8);
        root.addView(hint, margins(-1, -2, dp(3), 0, dp(3), dp(8)));

        addOutlineCategory(
            root,
            "Игроки",
            RelayService.KEY_ENTITY_PLAYERS,
            RelayService.KEY_PLAYER_COLOR,
            RelayService.DEFAULT_PLAYER_COLOR
        );
        addOutlineCategory(
            root,
            "Мобы",
            RelayService.KEY_ENTITY_MOBS,
            RelayService.KEY_MOB_COLOR,
            RelayService.DEFAULT_MOB_COLOR
        );
        addOutlineCategory(
            root,
            "Выпавшие предметы",
            RelayService.KEY_ENTITY_ITEMS,
            RelayService.KEY_ITEM_COLOR,
            RelayService.DEFAULT_ITEM_COLOR
        );

        int fovValue = RelayService.clampEntityFov(preferences.getInt(
            RelayService.KEY_ENTITY_FOV,
            70
        ));
        TextView fovLabel = settingLabel("FOV Minecraft: " + fovValue + "°");
        root.addView(fovLabel);
        SeekBar fov = slider(
            RelayService.MIN_ENTITY_FOV,
            RelayService.MAX_ENTITY_FOV,
            fovValue
        );
        root.addView(fov);
        fov.setOnSeekBarChangeListener(seekListener(
            progress -> fovLabel.setText("FOV Minecraft: " + progress + "°"),
            progress -> saveInt(
                RelayService.KEY_ENTITY_FOV,
                RelayService.clampEntityFov(progress)
            )
        ));

        int thickness = RelayService.clampOutlineThicknessTenths(
            preferences.getInt(RelayService.KEY_OUTLINE_THICKNESS_TENTHS, 17)
        );
        TextView thicknessLabel = settingLabel(String.format(
            Locale.getDefault(),
            "Толщина рамки: %.1f",
            thickness / 10.0
        ));
        root.addView(thicknessLabel);
        SeekBar thicknessSlider = slider(8, 60, thickness);
        root.addView(thicknessSlider);
        thicknessSlider.setOnSeekBarChangeListener(seekListener(
            progress -> thicknessLabel.setText(String.format(
                Locale.getDefault(),
                "Толщина рамки: %.1f",
                progress / 10.0
            )),
            progress -> saveInt(
                RelayService.KEY_OUTLINE_THICKNESS_TENTHS,
                RelayService.clampOutlineThicknessTenths(progress)
            )
        ));

        int distance = RelayService.clampEntityDistance(preferences.getInt(
            RelayService.KEY_ENTITY_MAX_DISTANCE,
            128
        ));
        TextView distanceLabel = settingLabel("Максимальная дальность: " +
            distance + " м");
        root.addView(distanceLabel);
        SeekBar distanceSlider = slider(16, 256, distance);
        root.addView(distanceSlider);
        distanceSlider.setOnSeekBarChangeListener(seekListener(
            progress -> distanceLabel.setText(
                "Максимальная дальность: " + progress + " м"
            ),
            progress -> saveInt(
                RelayService.KEY_ENTITY_MAX_DISTANCE,
                RelayService.clampEntityDistance(progress)
            )
        ));
    }

    private void addOutlineCategory(
        LinearLayout root,
        String title,
        String enabledKey,
        String colorKey,
        int defaultColor
    ) {
        LinearLayout card = new LinearLayout(context);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(10), dp(5), dp(10), dp(8));
        card.setBackground(cardBackground(false));
        card.addView(toggle(title, enabledKey, true));
        card.addView(colorPicker(colorKey, defaultColor));
        root.addView(card, margins(-1, -2, 0, 0, 0, dp(7)));
    }

    private View colorPicker(String key, int defaultColor) {
        int selected = preferences.getInt(key, defaultColor) | 0xff000000;
        int[] colors = {
            0xff4fd5ff, 0xff5df0a2, 0xffffcf4a, 0xffff8a4c,
            0xffff5b62, 0xffd56cff, 0xffffffff, 0xff8094aa
        };
        LinearLayout row = new LinearLayout(context);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        for (int color : colors) {
            TextView swatch = text("", 1, false);
            swatch.setBackground(colorBackground(color, color == selected));
            swatch.setContentDescription("Выбрать цвет обводки");
            swatch.setOnClickListener(view -> {
                preferences.edit().putInt(key, color).apply();
                settingsChanged.run();
                showPage("outline");
            });
            row.addView(swatch, margins(dp(28), dp(28), dp(2), 0, dp(3), 0));
        }
        return row;
    }

    private void buildChunksPage(LinearLayout root) {
        Switch retention = toggle(
            "Удерживать старые чанки",
            RelayService.KEY_CHUNK_RETENTION,
            false
        );
        root.addView(retention);
        root.addView(toggle(
            "Отдельный счётчик на экране",
            RelayService.KEY_CHUNK_WIDGET,
            true
        ));

        int radiusValue = RelayService.clampRetainedRadius(preferences.getInt(
            RelayService.KEY_RETAINED_RADIUS_CHUNKS,
            24
        ));
        statusRetentionEnabled = retention.isChecked();
        statusConfiguredRadiusChunks = radiusValue;
        TextView radiusLabel = settingLabel(
            "Радиус: " + radiusValue + " чанков (" +
                radiusValue * 16 + " блоков)"
        );
        root.addView(radiusLabel);
        SeekBar radius = slider(
            RelayService.MIN_RETAINED_RADIUS_CHUNKS,
            RelayService.MAX_RETAINED_RADIUS_CHUNKS,
            radiusValue
        );
        root.addView(radius);
        radius.setOnSeekBarChangeListener(seekListener(progress -> {
            statusConfiguredRadiusChunks = progress;
            radiusLabel.setText("Радиус: " + progress + " чанков (" +
                progress * 16 + " блоков)");
            refreshChunkStatus();
        }, this::saveRadius));

        int scale = RelayService.clampOverlayScale(preferences.getInt(
            RelayService.KEY_CHUNK_WIDGET_SCALE,
            85
        ));
        TextView scaleLabel = settingLabel("Размер счётчика: " + scale + "%");
        root.addView(scaleLabel);
        SeekBar scaleSlider = slider(70, 150, scale);
        root.addView(scaleSlider);
        scaleSlider.setOnSeekBarChangeListener(seekListener(
            progress -> scaleLabel.setText("Размер счётчика: " + progress + "%"),
            progress -> saveInt(
                RelayService.KEY_CHUNK_WIDGET_SCALE,
                RelayService.clampOverlayScale(progress)
            )
        ));

        chunkStatus = text("", 11, true);
        chunkStatus.setPadding(dp(10), dp(9), dp(10), dp(9));
        chunkStatus.setBackground(statusBackground());
        root.addView(chunkStatus, margins(-1, -2, 0, dp(6), 0, dp(7)));
        refreshChunkStatus();
        TextView note = text(
            "Счётчик можно перетаскивать. Коснитесь его без движения, чтобы " +
                "свернуть; кнопки −/+ меняют размер. Туман Minecraft не " +
                "доказывает выгрузку чанка — ориентируйтесь на этот счётчик.",
            10,
            false
        );
        note.setTextColor(0xff98a7b8);
        root.addView(note);
    }

    private void buildEquipmentPage(LinearLayout root) {
        root.addView(toggle(
            "HUD снаряжения справа",
            RelayService.KEY_EQUIPMENT_HUD,
            true
        ));
        int scale = RelayService.clampOverlayScale(preferences.getInt(
            RelayService.KEY_EQUIPMENT_HUD_SCALE,
            80
        ));
        TextView scaleLabel = settingLabel("Размер HUD: " + scale + "%");
        root.addView(scaleLabel);
        SeekBar scaleSlider = slider(70, 150, scale);
        root.addView(scaleSlider);
        scaleSlider.setOnSeekBarChangeListener(seekListener(
            progress -> scaleLabel.setText("Размер HUD: " + progress + "%"),
            progress -> saveInt(
                RelayService.KEY_EQUIPMENT_HUD_SCALE,
                RelayService.clampOverlayScale(progress)
            )
        ));
        TextView info = text(
            "HUD показывает предмет в руке, четыре слота брони и остаток " +
                "прочности. Зачарованные вещи получают плавный фиолетовый " +
                "перелив. Данные приходят только из пакетов текущего игрока.",
            10,
            false
        );
        info.setTextColor(0xffaab8c8);
        info.setPadding(dp(3), dp(8), dp(3), dp(8));
        root.addView(info);
        OfficialTexturePack.Status textures =
            new OfficialTexturePack(context).status();
        TextView textureStatus = text(textures.description(), 9, true);
        textureStatus.setTextColor(
            textures.imported ? 0xff74df9c : 0xffffc76c
        );
        textureStatus.setPadding(dp(8), dp(8), dp(8), dp(8));
        textureStatus.setBackground(statusBackground());
        root.addView(
            textureStatus,
            margins(-1, -2, 0, dp(2), 0, dp(7))
        );
        TextView importTextures = text(
            textures.imported
                ? "ЗАМЕНИТЬ ОФИЦИАЛЬНЫЙ FULL ZIP"
                : "ИМПОРТИРОВАТЬ ОФИЦИАЛЬНЫЙ FULL ZIP",
            10,
            true
        );
        importTextures.setGravity(Gravity.CENTER);
        importTextures.setPadding(dp(9), dp(9), dp(9), dp(9));
        importTextures.setBackground(actionBackground());
        importTextures.setOnClickListener(view -> {
            Intent intent = new Intent(context, MainActivity.class)
                .setAction(MainActivity.ACTION_IMPORT_TEXTURE_PACK)
                .addFlags(
                    Intent.FLAG_ACTIVITY_NEW_TASK |
                        Intent.FLAG_ACTIVITY_CLEAR_TOP |
                        Intent.FLAG_ACTIVITY_SINGLE_TOP
                );
            context.startActivity(intent);
        });
        root.addView(
            importTextures,
            margins(-1, -2, 0, 0, 0, dp(7))
        );
        TextView attribution = text(
            textures.imported
                ? "Item PNG читаются из выбранного официального архива и " +
                    "хранятся только во внутренней памяти приложения."
                : "Запасные силуэты: Game-icons.net — Lorc и Delapouite, " +
                    "CC BY 3.0.",
            9,
            false
        );
        attribution.setTextColor(0xff78889b);
        root.addView(attribution);
    }

    private void buildLogsPage(LinearLayout root) {
        Switch logging = toggle(
            "Записывать журнал",
            RelayService.KEY_DETAILED_LOGS,
            true
        );
        root.addView(logging);
        TextView rule = text(
            logging.isChecked()
                ? "Запись включена: события и ошибки сохраняются."
                : "Запись выключена полностью: новые строки и аварийный буфер не пишутся.",
            10,
            true
        );
        rule.setTextColor(logging.isChecked() ? 0xff73e49a : 0xffffc76c);
        root.addView(rule, margins(-1, -2, dp(3), 0, dp(3), dp(7)));
        TextView clear = text("ОЧИСТИТЬ ЖУРНАЛ", 11, true);
        clear.setGravity(Gravity.CENTER);
        clear.setPadding(dp(10), dp(9), dp(10), dp(9));
        clear.setBackground(actionBackground());
        clear.setOnClickListener(view -> {
            DiagnosticsLog.clear(context);
            if (logText != null) logText.setText("Журнал пока пуст.");
        });
        root.addView(clear, margins(-1, -2, 0, 0, 0, dp(7)));
        logText = text(DiagnosticsLog.readTail(context, 32 * 1024), 8, false);
        logText.setTypeface(android.graphics.Typeface.MONOSPACE);
        logText.setTextColor(0xffb9c7d6);
        logText.setTextIsSelectable(true);
        logText.setPadding(dp(8), dp(8), dp(8), dp(8));
        logText.setBackground(statusBackground());
        root.addView(logText);
    }

    private Switch toggle(String label, String key, boolean defaultValue) {
        Switch control = new Switch(context);
        control.setText(label);
        control.setTextSize(13);
        control.setTextColor(Color.WHITE);
        control.setChecked(preferences.getBoolean(key, defaultValue));
        control.setPadding(dp(1), dp(2), dp(1), dp(2));
        control.setOnCheckedChangeListener((button, checked) -> {
            preferences.edit().putBoolean(key, checked).apply();
            if (RelayService.KEY_CHUNK_RETENTION.equals(key)) {
                statusRetentionEnabled = checked;
                if (!checked) clearLiveChunkStatus();
                refreshChunkStatus();
            }
            settingsChanged.run();
            if (RelayService.KEY_DETAILED_LOGS.equals(key)) {
                showPage("logs");
            }
        });
        return control;
    }

    private SeekBar slider(int minimum, int maximum, int value) {
        SeekBar slider = new SeekBar(context);
        slider.setMin(minimum);
        slider.setMax(maximum);
        slider.setProgress(value);
        slider.setPadding(0, 0, 0, 0);
        return slider;
    }

    private interface ProgressAction { void apply(int value); }

    private SeekBar.OnSeekBarChangeListener seekListener(
        ProgressAction changed,
        ProgressAction saved
    ) {
        return new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(
                SeekBar seekBar,
                int progress,
                boolean fromUser
            ) {
                changed.apply(progress);
            }

            @Override public void onStartTrackingTouch(SeekBar seekBar) {}

            @Override public void onStopTrackingTouch(SeekBar seekBar) {
                saved.apply(seekBar.getProgress());
            }
        };
    }

    private TextView settingLabel(String value) {
        TextView label = text(value, 11, true);
        label.setTextColor(0xffdce6f5);
        label.setPadding(dp(3), dp(6), dp(3), 0);
        return label;
    }

    private void saveInt(String key, int value) {
        preferences.edit().putInt(key, value).apply();
        settingsChanged.run();
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
        if (logText != null && "logs".equals(currentPage)) {
            logText.setText(DiagnosticsLog.readTail(context, 32 * 1024));
        }
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

    private GradientDrawable actionBackground() {
        GradientDrawable background = new GradientDrawable();
        background.setColor(0xff263446);
        background.setCornerRadius(dp(10));
        background.setStroke(dp(1), 0x665f789d);
        return background;
    }

    private GradientDrawable cardBackground(boolean selected) {
        GradientDrawable background = new GradientDrawable();
        background.setColor(selected ? 0xff24364a : 0xbb1d2734);
        background.setCornerRadius(dp(12));
        background.setStroke(
            dp(1),
            selected ? 0xff4fd5ff : 0x554f6682
        );
        return background;
    }

    private GradientDrawable colorBackground(int color, boolean selected) {
        GradientDrawable background = new GradientDrawable();
        background.setShape(GradientDrawable.OVAL);
        background.setColor(color);
        background.setStroke(
            dp(selected ? 3 : 1),
            selected ? Color.WHITE : 0x99667688
        );
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
