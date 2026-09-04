package com.m9chko.bedrockrelay;

import android.animation.ValueAnimator;
import android.app.AlertDialog;
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

import com.m9chko.bedrockrelay.schematic.SchematicRepository;
import com.m9chko.bedrockrelay.schematic.SchematicSourceFolder;

import java.util.List;
import java.util.Locale;

/** Session-scoped Toolbox-style controls displayed over Minecraft. */
final class RelayOverlayController {
    private static final String KEY_DRAWER_OPEN = "relay_overlay_drawer_open";
    interface SchematicAnchorShift {
        void shift(int dx, int dy, int dz);
    }

    private final Context context;
    private final SharedPreferences preferences;
    private final Runnable settingsChanged;
    private final SchematicAnchorShift schematicAnchorShift;
    private final WindowManager windowManager;

    private WindowManager.LayoutParams windowParams;
    private volatile LinearLayout windowRoot;
    private TextView drawerTab;
    private TextView chunkStatus;
    private TextView miniMapStatus;
    private TextView automationStatus;
    private TextView pageTitle;
    private TextView backButton;
    private LinearLayout pageContent;
    private TextView logText;
    private String currentPage = "home";
    private ValueAnimator drawerAnimator;
    private int drawerPanelWidth;
    private boolean drawerOpen;
    private boolean drawerShouldBeOpen;
    private boolean missingPermissionLogged;
    private boolean statusRetentionEnabled;
    private int statusConfiguredRadiusChunks = 24;
    private long statusPublisherUpdates;
    private long statusPublisherRewrites;
    private int statusServerRadiusBlocks;
    private int statusEffectiveRadiusBlocks;
    private long statusRetainedChunks;
    private long statusRetainedBytes;
    private long statusMaximumBytes = 48L * 1024L * 1024L;
    private long statusEvictedRadius;
    private long statusEvictedMemory;
    private long statusParseFailures;
    private long statusMiniMapDecoded;
    private long statusMiniMapFailures;
    private String statusAutomationText = "Ожидание инвентаря";
    private boolean statusInventoryReady;
    private boolean statusAutomationPending;
    private long statusAutomationAccepted;
    private long statusAutomationRejected;
    private String schematicImportStatus = "";
    private boolean schematicImportError;

    RelayOverlayController(
        Context context,
        SharedPreferences preferences,
        Runnable settingsChanged,
        SchematicAnchorShift schematicAnchorShift
    ) {
        this.context = context;
        this.preferences = preferences;
        this.settingsChanged = settingsChanged;
        this.schematicAnchorShift = schematicAnchorShift;
        drawerShouldBeOpen = preferences.getBoolean(KEY_DRAWER_OPEN, true);
        this.windowManager = (WindowManager) context.getSystemService(
            Context.WINDOW_SERVICE
        );
    }

    void updateSchematicImportStatus(String value, boolean error) {
        schematicImportStatus = value == null ? "" : value;
        schematicImportError = error;
        if ("schematics".equals(currentPage) && pageContent != null) {
            showPage("schematics");
        }
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
                    animateDrawer(drawerShouldBeOpen);
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
        miniMapStatus = null;
        automationStatus = null;
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
        tab.setOnClickListener(view -> {
            drawerShouldBeOpen = !drawerOpen;
            preferences.edit()
                .putBoolean(KEY_DRAWER_OPEN, drawerShouldBeOpen)
                .apply();
            animateDrawer(drawerShouldBeOpen);
        });
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
        miniMapStatus = null;
        automationStatus = null;
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
            case "minimap":
                pageTitle.setText("МИНИ-КАРТА");
                buildMiniMapPage(pageContent);
                break;
            case "automation":
                pageTitle.setText("АВТОМАТИЗАЦИЯ");
                buildAutomationPage(pageContent);
                break;
            case "threats":
                pageTitle.setText("АНАЛИЗ УГРОЗ");
                buildThreatsPage(pageContent);
                break;
            case "schematics":
                pageTitle.setText("СХЕМЫ");
                buildSchematicsPage(pageContent);
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
        root.addView(sectionHeader("ВИЗУАЛИЗАЦИЯ"));
        root.addView(menuCard(
            "◎  ОБВОДКА",
            "Игроки, мобы, предметы • цвета • толщина",
            "outline"
        ));
        root.addView(menuCard(
            "⚠  АНАЛИЗ УГРОЗ",
            "Враждебные мобы • оценка урона • свой порог",
            "threats"
        ));
        root.addView(menuCard(
            "♢  СНАРЯЖЕНИЕ",
            "Обе руки • броня • прочность • зачарования",
            "equipment"
        ));
        root.addView(sectionHeader("КАРТА И МИР"));
        root.addView(menuCard(
            "⌖  МИНИ-КАРТА",
            "Карта поверхности из пакетов чанков",
            "minimap"
        ));
        root.addView(menuCard(
            "▦  ЧАНКИ",
            "Удержание и отдельный перемещаемый счётчик",
            "chunks"
        ));
        root.addView(menuCard(
            "⌂  СХЕМЫ",
            "3D-проекция построек • слои • поворот • импорт NBT",
            "schematics"
        ));
        root.addView(sectionHeader("АВТОМАТИЗАЦИЯ"));
        root.addView(menuCard(
            "⇄  АВТО-ЭКИПИРОВКА",
            "Тотем в левую руку и лучшая броня",
            "automation"
        ));
        root.addView(sectionHeader("ДИАГНОСТИКА"));
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

    private View sectionHeader(String value) {
        TextView header = text("└─ " + value, 9, true);
        header.setTextColor(0xff6f91bd);
        header.setPadding(dp(4), dp(5), dp(3), dp(5));
        return header;
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
        return colorPicker(key, defaultColor, "outline");
    }

    private View colorPicker(String key, int defaultColor, String returnPage) {
        int selected = preferences.getInt(key, defaultColor) | 0xff000000;
        int[] colors = {
            0xff4fd5ff, 0xff5df0a2, 0xffffcf4a, 0xffff8a4c,
            0xffff5b62, 0xffd56cff, 0xffffffff, 0xff8094aa,
            0xff14687a, 0xff287a46, 0xff8a6418, 0xff7a4428,
            0xff842f38, 0xff64317d, 0xff2d477a, 0xff252a31
        };
        LinearLayout picker = new LinearLayout(context);
        picker.setOrientation(LinearLayout.VERTICAL);
        for (int start = 0; start < colors.length; start += 8) {
            LinearLayout row = new LinearLayout(context);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            for (int index = start; index < Math.min(start + 8, colors.length);
                ++index) {
                int color = colors[index];
                TextView swatch = text("", 1, false);
                swatch.setBackground(colorBackground(color, color == selected));
                swatch.setContentDescription("Выбрать цвет обводки");
                swatch.setOnClickListener(view -> {
                    preferences.edit().putInt(key, color).apply();
                    settingsChanged.run();
                    showPage(returnPage);
                });
                row.addView(
                    swatch,
                    margins(dp(28), dp(28), dp(2), dp(2), dp(3), dp(2))
                );
            }
            picker.addView(row);
        }
        TextView custom = text(
            String.format(Locale.ROOT, "СВОЙ ЦВЕТ  •  #%06X", selected & 0xffffff),
            10,
            true
        );
        custom.setGravity(Gravity.CENTER);
        custom.setPadding(dp(8), dp(8), dp(8), dp(8));
        custom.setBackground(actionBackground());
        custom.setOnClickListener(view -> showColorDialog(
            key,
            selected,
            returnPage
        ));
        picker.addView(custom, margins(-1, -2, 0, dp(4), 0, dp(2)));
        return picker;
    }

    private void showColorDialog(String key, int initialColor, String returnPage) {
        LinearLayout root = new LinearLayout(context);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(18), dp(8), dp(18), 0);
        int[] color = {
            Color.red(initialColor),
            Color.green(initialColor),
            Color.blue(initialColor)
        };
        TextView preview = text("", 13, true);
        preview.setGravity(Gravity.CENTER);
        preview.setTextColor(Color.WHITE);
        preview.setPadding(dp(8), dp(12), dp(8), dp(12));
        root.addView(preview, margins(-1, -2, 0, 0, 0, dp(8)));
        String[] names = {"Красный", "Зелёный", "Синий"};
        for (int channel = 0; channel < color.length; ++channel) {
            final int selectedChannel = channel;
            TextView label = settingLabel(names[channel] + ": " + color[channel]);
            SeekBar value = slider(0, 255, color[channel]);
            value.setOnSeekBarChangeListener(seekListener(
                progress -> {
                    color[selectedChannel] = progress;
                    label.setText(names[selectedChannel] + ": " + progress);
                    updateColorPreview(preview, color);
                },
                progress -> color[selectedChannel] = progress
            ));
            root.addView(label);
            root.addView(value);
        }
        updateColorPreview(preview, color);
        AlertDialog dialog = new AlertDialog.Builder(context)
            .setTitle("Свой цвет контура")
            .setView(root)
            .setNegativeButton("Отмена", null)
            .setPositiveButton("Сохранить", (clickedDialog, which) -> {
                int selected = Color.rgb(color[0], color[1], color[2]);
                preferences.edit().putInt(key, selected).apply();
                settingsChanged.run();
                showPage(returnPage);
            })
            .create();
        if (dialog.getWindow() != null) {
            dialog.getWindow().setType(
                android.os.Build.VERSION.SDK_INT >= 26
                    ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                    : WindowManager.LayoutParams.TYPE_SYSTEM_ALERT
            );
        }
        dialog.show();
    }

    private static void updateColorPreview(TextView preview, int[] channels) {
        int color = Color.rgb(channels[0], channels[1], channels[2]);
        preview.setText(String.format(Locale.ROOT, "#%06X", color & 0xffffff));
        GradientDrawable background = new GradientDrawable();
        background.setColor(color);
        background.setCornerRadius(18f);
        background.setStroke(2, Color.WHITE);
        preview.setBackground(background);
        double brightness = channels[0] * 0.299 + channels[1] * 0.587 +
            channels[2] * 0.114;
        preview.setTextColor(brightness > 160.0 ? Color.BLACK : Color.WHITE);
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
            "HUD показывает правую и левую руки, четыре слота брони и остаток " +
                "прочности. Зачарованные вещи получают плавный фиолетовый " +
                "перелив. Окно можно перетаскивать и оно автоматически " +
                "скрывается в инвентаре, сундуке, шалкере и чате.",
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
                ? "Item PNG используются в HUD, block PNG — в визуализации " +
                    "схем. Они хранятся только во внутренней памяти приложения."
                : "Запасные силуэты: Game-icons.net — Lorc и Delapouite, " +
                    "CC BY 3.0.",
            9,
            false
        );
        attribution.setTextColor(0xff78889b);
        root.addView(attribution);
    }

    private void buildMiniMapPage(LinearLayout root) {
        root.addView(toggle(
            "Показывать мини-карту",
            RelayService.KEY_MINIMAP,
            true
        ));
        root.addView(toggle(
            "Круглая форма",
            RelayService.KEY_MINIMAP_ROUND,
            true
        ));
        int radius = RelayService.clampMiniMapRadius(preferences.getInt(
            RelayService.KEY_MINIMAP_RADIUS,
            4
        ));
        TextView radiusLabel = settingLabel(
            "Радиус карты: " + radius + " чанка"
        );
        root.addView(radiusLabel);
        SeekBar radiusSlider = slider(
            RelayService.MIN_MINIMAP_RADIUS_CHUNKS,
            RelayService.MAX_MINIMAP_RADIUS_CHUNKS,
            radius
        );
        root.addView(radiusSlider);
        radiusSlider.setOnSeekBarChangeListener(seekListener(
            progress -> radiusLabel.setText(
                "Радиус карты: " + progress + " чанков"
            ),
            progress -> saveInt(
                RelayService.KEY_MINIMAP_RADIUS,
                RelayService.clampMiniMapRadius(progress)
            )
        ));
        int scale = RelayService.clampOverlayScale(preferences.getInt(
            RelayService.KEY_MINIMAP_SCALE,
            90
        ));
        TextView scaleLabel = settingLabel("Размер карты: " + scale + "%");
        root.addView(scaleLabel);
        SeekBar scaleSlider = slider(70, 150, scale);
        root.addView(scaleSlider);
        scaleSlider.setOnSeekBarChangeListener(seekListener(
            progress -> scaleLabel.setText("Размер карты: " + progress + "%"),
            progress -> saveInt(
                RelayService.KEY_MINIMAP_SCALE,
                RelayService.clampOverlayScale(progress)
            )
        ));
        miniMapStatus = text("", 10, true);
        miniMapStatus.setPadding(dp(10), dp(8), dp(10), dp(8));
        miniMapStatus.setBackground(statusBackground());
        root.addView(miniMapStatus, margins(-1, -2, 0, dp(5), 0, dp(7)));
        refreshMiniMapStatus();
        TextView note = text(
            "Мини-карта строится из LevelChunk без захвата экрана. Её можно " +
                "перетаскивать; при открытом интерфейсе Minecraft она скрывается.",
            10,
            false
        );
        note.setTextColor(0xff98a7b8);
        root.addView(note);
    }

    private void buildSchematicsPage(LinearLayout root) {
        SchematicRepository repository = new SchematicRepository(context);
        SchematicSourceFolder sourceFolder = new SchematicSourceFolder(context);
        SchematicSourceFolder.ScanResult folder = sourceFolder.scan();
        List<SchematicRepository.Entry> entries = repository.list();
        SchematicRepository.Entry active = null;
        for (SchematicRepository.Entry entry : entries) {
            if (entry.active) {
                active = entry;
                break;
            }
        }
        final SchematicRepository.Entry activeEntry = active;

        root.addView(toggle(
            "Фантомная схема в мире (без коллизии)",
            RelayService.KEY_SCHEMATIC_ENABLED,
            false
        ));

        int progressTotal = preferences.getInt(
            RelayService.KEY_SCHEMATIC_TOTAL,
            0
        );
        int progressCorrect = preferences.getInt(
            RelayService.KEY_SCHEMATIC_CORRECT,
            0
        );
        int progressMissing = preferences.getInt(
            RelayService.KEY_SCHEMATIC_MISSING,
            0
        );
        int progressWrong = preferences.getInt(
            RelayService.KEY_SCHEMATIC_WRONG,
            0
        );
        int progressUnknown = preferences.getInt(
            RelayService.KEY_SCHEMATIC_UNKNOWN,
            0
        );
        int progressDisplayed = preferences.getInt(
            RelayService.KEY_SCHEMATIC_DISPLAYED,
            0
        );

        TextView status = text(
            active == null
                ? "Активная схема не выбрана"
                : active.sourceName + "\n" + active.format + " • " +
                    active.sizeX + "×" + active.sizeY + "×" + active.sizeZ +
                    " • " + String.format(
                        Locale.getDefault(),
                        "%,d блоков",
                        active.nonAirBlocks
                    ) + (progressTotal > 0
                        ? "\nГотово " + progressCorrect + " • нет " +
                            progressMissing + " • неверно " + progressWrong +
                            " • не загружено " + progressUnknown +
                            " • показано " + progressDisplayed
                        : ""),
            10,
            true
        );
        status.setTextColor(active == null ? 0xffffc76c : 0xff82e6b1);
        status.setPadding(dp(10), dp(9), dp(10), dp(9));
        status.setBackground(statusBackground());
        root.addView(status, margins(-1, -2, 0, dp(4), 0, dp(7)));

        root.addView(sectionHeader("ФАЙЛЫ НА ТЕЛЕФОНЕ"));
        TextView folderStatus = text(
            folder.configured
                ? "Папка: " + folder.folderName + "\n" +
                    (folder.errorMessage.isEmpty()
                        ? folder.entries.size() + " совместимых файлов"
                        : folder.errorMessage)
                : "Выберите папку один раз. После этого схемы можно " +
                    "импортировать здесь, не закрывая Minecraft и сервер.",
            9,
            true
        );
        folderStatus.setTextColor(
            !folder.errorMessage.isEmpty()
                ? 0xffff8e99
                : folder.configured ? 0xff82e6b1 : 0xffffc76c
        );
        folderStatus.setPadding(dp(10), dp(8), dp(10), dp(8));
        folderStatus.setBackground(statusBackground());
        root.addView(folderStatus, margins(-1, -2, 0, dp(2), 0, dp(6)));

        root.addView(schematicAction(
            folder.configured ? "СМЕНИТЬ ПАПКУ СХЕМ" : "ВЫБРАТЬ ПАПКУ СХЕМ",
            () -> {
                Intent intent = new Intent(context, MainActivity.class)
                    .setAction(MainActivity.ACTION_CHOOSE_SCHEMATIC_FOLDER)
                    .addFlags(
                        Intent.FLAG_ACTIVITY_NEW_TASK |
                            Intent.FLAG_ACTIVITY_CLEAR_TOP |
                            Intent.FLAG_ACTIVITY_SINGLE_TOP
                    );
                context.startActivity(intent);
            }
        ), margins(-1, -2, 0, 0, 0, dp(6)));

        if (!schematicImportStatus.isEmpty()) {
            TextView importStatus = text(schematicImportStatus, 9, true);
            importStatus.setTextColor(
                schematicImportError ? 0xffff8e99 : 0xff68ddff
            );
            importStatus.setPadding(dp(9), dp(7), dp(9), dp(7));
            importStatus.setBackground(statusBackground());
            root.addView(importStatus, margins(-1, -2, 0, 0, 0, dp(6)));
        }

        int sourceShown = 0;
        for (SchematicSourceFolder.SourceEntry entry : folder.entries) {
            if (sourceShown >= 24) break;
            ++sourceShown;
            LinearLayout fileCard = new LinearLayout(context);
            fileCard.setOrientation(LinearLayout.VERTICAL);
            fileCard.setPadding(dp(10), dp(7), dp(10), dp(7));
            fileCard.setBackground(cardBackground(false));
            TextView fileName = text(entry.name, 10, true);
            fileName.setTextColor(0xffe7eef8);
            fileCard.addView(fileName);
            TextView fileDetail = text(
                entry.relativePath + " • " + formatBytes(entry.sizeBytes),
                8,
                false
            );
            fileDetail.setTextColor(0xff8495a8);
            fileDetail.setPadding(0, dp(2), 0, dp(5));
            fileCard.addView(fileDetail);
            TextView use = schematicAction("ИМПОРТИРОВАТЬ В МИР", () -> {
                schematicImportStatus = "Импортируем " + entry.name + "…";
                schematicImportError = false;
                showPage("schematics");
                try {
                    context.startService(new Intent(context, RelayService.class)
                        .setAction(RelayService.ACTION_IMPORT_SCHEMATIC_DOCUMENT)
                        .putExtra(
                            RelayService.EXTRA_SCHEMATIC_URI,
                            entry.uri.toString()
                        )
                        .putExtra(RelayService.EXTRA_SCHEMATIC_NAME, entry.name));
                } catch (Throwable error) {
                    updateSchematicImportStatus(
                        "Не удалось запустить импорт",
                        true
                    );
                    DiagnosticsLog.appendError(
                        context,
                        "schematics",
                        "Could not start in-game schematic import",
                        error
                    );
                }
            });
            fileCard.addView(use, new LinearLayout.LayoutParams(-1, dp(34)));
            root.addView(fileCard, margins(-1, -2, 0, 0, 0, dp(5)));
        }

        if (folder.entries.size() > sourceShown) {
            TextView more = text(
                "Показаны первые 24 файла из " + folder.entries.size(),
                8,
                false
            );
            more.setTextColor(0xff8797aa);
            root.addView(more, margins(-1, -2, 0, 0, 0, dp(5)));
        }

        TextView importButton = schematicAction(
            "ВЫБРАТЬ ОДИН ФАЙЛ (ЗАПАСНОЙ СПОСОБ)",
            () -> {
                Intent intent = new Intent(context, MainActivity.class)
                    .setAction(MainActivity.ACTION_IMPORT_SCHEMATIC)
                    .addFlags(
                        Intent.FLAG_ACTIVITY_NEW_TASK |
                            Intent.FLAG_ACTIVITY_CLEAR_TOP |
                            Intent.FLAG_ACTIVITY_SINGLE_TOP
                    );
                context.startActivity(intent);
            }
        );
        root.addView(importButton, margins(-1, -2, 0, 0, 0, dp(8)));

        if (!entries.isEmpty()) {
            root.addView(sectionHeader("БИБЛИОТЕКА"));
            int shown = 0;
            for (SchematicRepository.Entry entry : entries) {
                if (++shown > 16) break;
                LinearLayout card = new LinearLayout(context);
                card.setOrientation(LinearLayout.VERTICAL);
                card.setPadding(dp(10), dp(8), dp(10), dp(8));
                card.setBackground(cardBackground(entry.active));
                TextView name = text(
                    (entry.active ? "●  " : "○  ") + entry.sourceName,
                    11,
                    true
                );
                name.setTextColor(entry.active ? 0xff68ddff : 0xffe5edf8);
                card.addView(name);
                TextView detail = text(
                    entry.format + " • " + entry.sizeX + "×" + entry.sizeY +
                        "×" + entry.sizeZ,
                    9,
                    false
                );
                detail.setTextColor(0xff91a2b6);
                detail.setPadding(0, dp(2), 0, dp(5));
                card.addView(detail);
                LinearLayout actions = new LinearLayout(context);
                actions.setOrientation(LinearLayout.HORIZONTAL);
                if (!entry.active) {
                    TextView select = schematicAction("ВЫБРАТЬ", () -> {
                        if (repository.activate(entry.id)) {
                            preferences.edit()
                                .putBoolean(
                                    RelayService.KEY_SCHEMATIC_PLACED,
                                    false
                                )
                                .apply();
                            notifySchematicReload();
                            showPage("schematics");
                        }
                    });
                    actions.addView(select, new LinearLayout.LayoutParams(
                        0,
                        dp(34),
                        1f
                    ));
                }
                TextView remove = schematicAction("УДАЛИТЬ", () -> {
                    boolean wasActive = entry.active;
                    if (repository.delete(entry.id)) {
                        if (wasActive) {
                            List<SchematicRepository.Entry> remaining =
                                repository.list();
                            if (!remaining.isEmpty()) {
                                repository.activate(remaining.get(0).id);
                                preferences.edit()
                                    .putBoolean(
                                        RelayService.KEY_SCHEMATIC_PLACED,
                                        false
                                    )
                                    .apply();
                            }
                            notifySchematicReload();
                        }
                        showPage("schematics");
                    }
                });
                actions.addView(remove, new LinearLayout.LayoutParams(
                    0,
                    dp(34),
                    1f
                ));
                card.addView(actions);
                root.addView(card, margins(-1, -2, 0, 0, 0, dp(6)));
            }
        }

        if (active == null) {
            TextView formats = text(
                "Поддерживаются .mcstructure, .nbt, .litematic, .schem и " +
                    ".schematic. GZip и обычный NBT определяются автоматически.",
                10,
                false
            );
            formats.setTextColor(0xff9cabbc);
            root.addView(formats);
            return;
        }

        root.addView(sectionHeader("ОТОБРАЖЕНИЕ"));
        root.addView(toggle(
            "Текстуры блоков схемы",
            RelayService.KEY_SCHEMATIC_TEXTURES,
            true
        ));
        int opacity = RelayService.clampSchematicOpacity(preferences.getInt(
            RelayService.KEY_SCHEMATIC_OPACITY,
            42
        ));
        TextView opacityLabel = settingLabel("Прозрачность текстур: " +
            opacity + "%");
        root.addView(opacityLabel);
        SeekBar opacitySlider = slider(10, 100, opacity);
        root.addView(opacitySlider);
        opacitySlider.setOnSeekBarChangeListener(seekListener(
            value -> opacityLabel.setText(
                "Прозрачность текстур: " + value + "%"
            ),
            value -> saveInt(
                RelayService.KEY_SCHEMATIC_OPACITY,
                RelayService.clampSchematicOpacity(value)
            )
        ));

        root.addView(toggle(
            "Контуры блоков схемы",
            RelayService.KEY_SCHEMATIC_OUTLINES,
            true
        ));
        int outlineOpacity = RelayService.clampSchematicOpacity(
            preferences.getInt(
                RelayService.KEY_SCHEMATIC_OUTLINE_OPACITY,
                68
            )
        );
        TextView outlineOpacityLabel = settingLabel(
            "Видимость контуров: " + outlineOpacity + "%"
        );
        root.addView(outlineOpacityLabel);
        SeekBar outlineOpacitySlider = slider(10, 100, outlineOpacity);
        root.addView(outlineOpacitySlider);
        outlineOpacitySlider.setOnSeekBarChangeListener(seekListener(
            value -> outlineOpacityLabel.setText(
                "Видимость контуров: " + value + "%"
            ),
            value -> saveInt(
                RelayService.KEY_SCHEMATIC_OUTLINE_OPACITY,
                RelayService.clampSchematicOpacity(value)
            )
        ));

        root.addView(settingLabel("Цвет правильного блока"));
        root.addView(colorPicker(
            RelayService.KEY_SCHEMATIC_CORRECT_COLOR,
            RelayService.DEFAULT_SCHEMATIC_CORRECT_COLOR,
            "schematics"
        ));
        root.addView(settingLabel("Цвет неправильного блока"));
        root.addView(colorPicker(
            RelayService.KEY_SCHEMATIC_WRONG_COLOR,
            RelayService.DEFAULT_SCHEMATIC_WRONG_COLOR,
            "schematics"
        ));
        root.addView(settingLabel("Цвет отсутствующего блока"));
        root.addView(colorPicker(
            RelayService.KEY_SCHEMATIC_MISSING_COLOR,
            RelayService.DEFAULT_SCHEMATIC_MISSING_COLOR,
            "schematics"
        ));

        int distance = RelayService.clampSchematicDistance(preferences.getInt(
            RelayService.KEY_SCHEMATIC_DISTANCE,
            96
        ));
        TextView distanceLabel = settingLabel("Дальность схемы: " + distance +
            " м");
        root.addView(distanceLabel);
        SeekBar distanceSlider = slider(16, 192, distance);
        root.addView(distanceSlider);
        distanceSlider.setOnSeekBarChangeListener(seekListener(
            value -> distanceLabel.setText("Дальность схемы: " + value + " м"),
            value -> saveInt(
                RelayService.KEY_SCHEMATIC_DISTANCE,
                RelayService.clampSchematicDistance(value)
            )
        ));

        int rotation = Math.floorMod(preferences.getInt(
            RelayService.KEY_SCHEMATIC_ROTATION,
            0
        ), 4);
        root.addView(schematicAction(
            "ПОВОРОТ: " + (rotation * 90) + "°  ↻",
            () -> {
                preferences.edit().putInt(
                    RelayService.KEY_SCHEMATIC_ROTATION,
                    (rotation + 1) & 3
                ).apply();
                settingsChanged.run();
                showPage("schematics");
            }
        ), margins(-1, -2, 0, dp(5), 0, dp(6)));
        boolean mirrored = preferences.getBoolean(
            RelayService.KEY_SCHEMATIC_MIRROR,
            false
        );
        root.addView(schematicAction(
            mirrored ? "ОТРАЖЕНИЕ: ВКЛЮЧЕНО" : "ОТРАЖЕНИЕ: ВЫКЛЮЧЕНО",
            () -> {
                preferences.edit().putBoolean(
                    RelayService.KEY_SCHEMATIC_MIRROR,
                    !mirrored
                ).apply();
                settingsChanged.run();
                showPage("schematics");
            }
        ), margins(-1, -2, 0, 0, 0, dp(6)));

        int selectedLayer = preferences.getInt(
            RelayService.KEY_SCHEMATIC_LAYER,
            -1
        );
        if (selectedLayer >= activeEntry.sizeY) {
            selectedLayer = activeEntry.sizeY - 1;
        }
        final int layer = selectedLayer;
        LinearLayout layerRow = new LinearLayout(context);
        layerRow.setOrientation(LinearLayout.HORIZONTAL);
        TextView previousLayer = schematicAction("−", () -> {
            int next = layer < 0
                ? Math.max(0, activeEntry.sizeY - 1)
                : layer - 1;
            preferences.edit().putInt(
                RelayService.KEY_SCHEMATIC_LAYER,
                Math.max(-1, next)
            ).apply();
            settingsChanged.run();
            showPage("schematics");
        });
        TextView layerMode = schematicAction(
            layer < 0 ? "ВСЕ СЛОИ" : "СЛОЙ Y=" + layer,
            () -> {
                preferences.edit().putInt(
                    RelayService.KEY_SCHEMATIC_LAYER,
                    layer < 0 ? 0 : -1
                ).apply();
                settingsChanged.run();
                showPage("schematics");
            }
        );
        TextView nextLayer = schematicAction("+", () -> {
            int next = layer < 0
                ? 0
                : Math.min(activeEntry.sizeY - 1, layer + 1);
            preferences.edit().putInt(
                RelayService.KEY_SCHEMATIC_LAYER,
                next
            ).apply();
            settingsChanged.run();
            showPage("schematics");
        });
        layerRow.addView(previousLayer, new LinearLayout.LayoutParams(dp(42), dp(36)));
        layerRow.addView(layerMode, new LinearLayout.LayoutParams(0, dp(36), 1f));
        layerRow.addView(nextLayer, new LinearLayout.LayoutParams(dp(42), dp(36)));
        root.addView(layerRow, margins(-1, -2, 0, 0, 0, dp(8)));

        root.addView(sectionHeader("ПОЗИЦИЯ В МИРЕ"));
        int anchorX = preferences.getInt(RelayService.KEY_SCHEMATIC_ANCHOR_X, 0);
        float anchorY = preferences.getFloat(
            RelayService.KEY_SCHEMATIC_ANCHOR_Y_EXACT,
            preferences.getInt(RelayService.KEY_SCHEMATIC_ANCHOR_Y, 0)
        );
        int anchorZ = preferences.getInt(RelayService.KEY_SCHEMATIC_ANCHOR_Z, 0);
        boolean schematicPlaced = preferences.getBoolean(
            RelayService.KEY_SCHEMATIC_PLACED,
            false
        );
        TextView coordinates = text(
            schematicPlaced
                ? "Фиксированный якорь: X " + anchorX + "  Y " +
                    formatSchematicY(anchorY) +
                    "  Z " + anchorZ
                : "Схема не размещена — нажмите кнопку ниже",
            10,
            true
        );
        coordinates.setTextColor(0xffa9d9ff);
        coordinates.setPadding(dp(4), dp(3), dp(4), dp(5));
        root.addView(coordinates);
        root.addView(schematicAction("ПОСТАВИТЬ ПЕРЕД ИГРОКОМ", () -> {
            long request = preferences.getLong(
                RelayService.KEY_SCHEMATIC_PLACE_REQUEST,
                0L
            ) + 1L;
            preferences.edit()
                .putBoolean(RelayService.KEY_SCHEMATIC_PLACED, false)
                .putLong(RelayService.KEY_SCHEMATIC_PLACE_REQUEST, request)
                .apply();
            settingsChanged.run();
        }), margins(-1, -2, 0, 0, 0, dp(6)));
        root.addView(schematicMoveRow("X −", -1, 0, 0, "X +", 1, 0, 0));
        root.addView(schematicMoveRow("Y −", 0, -1, 0, "Y +", 0, 1, 0));
        root.addView(schematicMoveRow("Z −", 0, 0, -1, "Z +", 0, 0, 1));

        TextView note = text(
            "После размещения якорь остаётся неподвижным в координатах мира и " +
                "меняется только кнопками X/Y/Z, поворота и зеркала. " +
                "Кнопки X/Y/Z можно нажимать сразу после размещения — смещение " +
                "применится, как только будет найдена поверхность. " +
                "Текстуры отсутствующих блоков получают повёрнутое Bedrock-" +
                "состояние. Для Java-схем facing/half/axis и другие свойства " +
                "сначала точно переводятся в Bedrock. Двойная рамка повторяет " +
                "форму ступеней, плит и других неполных блоков. Фантомы не " +
                "имеют коллизии и не меняют настоящий чанк.",
            9,
            false
        );
        note.setTextColor(0xff8291a3);
        note.setPadding(dp(3), dp(8), dp(3), 0);
        root.addView(note);
    }

    private View schematicMoveRow(
        String firstLabel,
        int firstX,
        int firstY,
        int firstZ,
        String secondLabel,
        int secondX,
        int secondY,
        int secondZ
    ) {
        LinearLayout row = new LinearLayout(context);
        row.setOrientation(LinearLayout.HORIZONTAL);
        TextView first = schematicAction(firstLabel, () ->
            adjustSchematicAnchor(firstX, firstY, firstZ));
        TextView second = schematicAction(secondLabel, () ->
            adjustSchematicAnchor(secondX, secondY, secondZ));
        row.addView(first, new LinearLayout.LayoutParams(0, dp(35), 1f));
        row.addView(second, new LinearLayout.LayoutParams(0, dp(35), 1f));
        row.setPadding(0, 0, 0, dp(4));
        return row;
    }

    private TextView schematicAction(String label, Runnable action) {
        TextView button = text(label, 10, true);
        button.setGravity(Gravity.CENTER);
        button.setPadding(dp(7), dp(7), dp(7), dp(7));
        button.setBackground(actionBackground());
        button.setClickable(true);
        button.setOnClickListener(view -> action.run());
        return button;
    }

    private void adjustSchematicAnchor(int dx, int dy, int dz) {
        schematicAnchorShift.shift(dx, dy, dz);
        showPage("schematics");
    }

    private static String formatSchematicY(float value) {
        if (Math.abs(value - Math.round(value)) < 0.0001f) {
            return Integer.toString(Math.round(value));
        }
        return String.format(Locale.getDefault(), "%.3f", value)
            .replaceAll("0+$", "")
            .replaceAll("[.,]$", "");
    }

    private void notifySchematicReload() {
        try {
            context.startService(new Intent(context, RelayService.class)
                .setAction(RelayService.ACTION_RELOAD_SCHEMATIC));
        } catch (Throwable error) {
            DiagnosticsLog.appendError(
                context,
                "schematics",
                "Could not reload selected schematic",
                error
            );
        }
    }

    private void buildAutomationPage(LinearLayout root) {
        root.addView(toggle(
            "Авто-тотем в левую руку",
            RelayService.KEY_AUTO_TOTEM,
            false
        ));
        root.addView(toggle(
            "Автоматически надевать лучшую броню",
            RelayService.KEY_AUTO_ARMOR,
            false
        ));
        automationStatus = text("", 10, true);
        automationStatus.setPadding(dp(10), dp(8), dp(10), dp(8));
        automationStatus.setBackground(statusBackground());
        root.addView(automationStatus, margins(-1, -2, 0, dp(5), 0, dp(7)));
        refreshAutomationStatus();
        TextView note = text(
            "Тотем и лучшая броня ищутся во всём уже синхронизированном " +
                "инвентаре, а перемещение подтверждается сервером. Во время " +
                "сундука, шалкера, инвентаря или чата автоматизация " +
                "приостанавливается.",
            10,
            false
        );
        note.setTextColor(0xff98a7b8);
        root.addView(note);
    }

    private void buildThreatsPage(LinearLayout root) {
        root.addView(toggle(
            "Умный анализ враждебных мобов",
            RelayService.KEY_THREAT_ANALYSIS,
            true
        ));
        root.addView(toggle(
            "Показывать перемещаемое предупреждение",
            RelayService.KEY_THREAT_WARNING,
            true
        ));
        int distance = RelayService.clampThreatDistance(preferences.getInt(
            RelayService.KEY_THREAT_DISTANCE,
            12
        ));
        TextView distanceLabel = settingLabel(
            "Срабатывать не дальше: " + distance + " м"
        );
        root.addView(distanceLabel);
        SeekBar distanceSlider = slider(
            RelayService.MIN_THREAT_DISTANCE,
            RelayService.MAX_THREAT_DISTANCE,
            distance
        );
        root.addView(distanceSlider);
        distanceSlider.setOnSeekBarChangeListener(seekListener(
            progress -> distanceLabel.setText(
                "Срабатывать не дальше: " + progress + " м"
            ),
            progress -> saveInt(
                RelayService.KEY_THREAT_DISTANCE,
                RelayService.clampThreatDistance(progress)
            )
        ));
        int scale = RelayService.clampOverlayScale(preferences.getInt(
            RelayService.KEY_THREAT_WARNING_SCALE,
            90
        ));
        TextView scaleLabel = settingLabel(
            "Размер предупреждения: " + scale + "%"
        );
        root.addView(scaleLabel);
        SeekBar scaleSlider = slider(70, 150, scale);
        root.addView(scaleSlider);
        scaleSlider.setOnSeekBarChangeListener(seekListener(
            progress -> scaleLabel.setText(
                "Размер предупреждения: " + progress + "%"
            ),
            progress -> saveInt(
                RelayService.KEY_THREAT_WARNING_SCALE,
                RelayService.clampOverlayScale(progress)
            )
        ));
        TextView colorLabel = settingLabel("Цвет опасного моба");
        root.addView(colorLabel);
        root.addView(colorPicker(
            RelayService.KEY_THREAT_COLOR,
            RelayService.DEFAULT_THREAT_COLOR,
            "threats"
        ));
        TextView note = text(
            "Учитываются тип моба, дистанция, скорость сближения, здоровье, " +
                "голод, поглощение, броня, наличие чар и Сопротивление. " +
                "Предупреждение можно перетащить. Без пакета цели агрессия и " +
                "урон показываются как диапазон-оценка.",
            10,
            false
        );
        note.setTextColor(0xff98a7b8);
        note.setPadding(dp(3), dp(7), dp(3), 0);
        root.addView(note);
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
                ? "Запись включена: повторы сжимаются, хранится до 512 КБ; " +
                    "неактивные файлы старше суток удаляются."
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
        if (preferences.getInt(key, Integer.MIN_VALUE) == value) return;
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

    void updateMiniMapStatus(long decodedChunks, long decodeFailures) {
        statusMiniMapDecoded = decodedChunks;
        statusMiniMapFailures = decodeFailures;
        refreshMiniMapStatus();
    }

    void updateAutomationStatus(
        String text,
        boolean inventoryReady,
        boolean pending,
        long accepted,
        long rejected
    ) {
        statusAutomationText = text == null || text.isEmpty()
            ? "Ожидание инвентаря"
            : text;
        statusInventoryReady = inventoryReady;
        statusAutomationPending = pending;
        statusAutomationAccepted = accepted;
        statusAutomationRejected = rejected;
        refreshAutomationStatus();
    }

    private void refreshMiniMapStatus() {
        if (miniMapStatus == null) return;
        boolean enabled = preferences.getBoolean(RelayService.KEY_MINIMAP, true);
        if (!enabled) {
            miniMapStatus.setText("Мини-карта выключена");
            miniMapStatus.setTextColor(0xffc4cad3);
            return;
        }
        miniMapStatus.setText(String.format(
            Locale.getDefault(),
            "Декодировано чанков: %,d%s",
            statusMiniMapDecoded,
            statusMiniMapFailures > 0
                ? " • ошибок: " + statusMiniMapFailures
                : " • ошибок нет"
        ));
        miniMapStatus.setTextColor(
            statusMiniMapFailures == 0 ? 0xff9ee493 : 0xffffd27a
        );
    }

    private void refreshAutomationStatus() {
        if (automationStatus == null) return;
        boolean enabled = preferences.getBoolean(
            RelayService.KEY_AUTO_ARMOR,
            false
        ) || preferences.getBoolean(RelayService.KEY_AUTO_TOTEM, false);
        if (!enabled) {
            automationStatus.setText("Автоматизация выключена");
            automationStatus.setTextColor(0xffc4cad3);
            return;
        }
        automationStatus.setText(String.format(
            Locale.getDefault(),
            "%s\nИнвентарь: %s%s • принято: %,d • отклонено: %,d",
            statusAutomationText,
            statusInventoryReady ? "готов" : "ожидание",
            statusAutomationPending ? " • запрос выполняется" : "",
            statusAutomationAccepted,
            statusAutomationRejected
        ));
        automationStatus.setTextColor(
            statusAutomationRejected > 0 ? 0xffffd27a : 0xff9ee493
        );
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
