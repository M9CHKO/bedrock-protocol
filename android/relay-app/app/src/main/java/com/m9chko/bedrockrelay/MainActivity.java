package com.m9chko.bedrockrelay;

import android.Manifest;
import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.provider.OpenableColumns;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.WindowInsets;
import android.content.res.ColorStateList;
import android.app.AlertDialog;
import android.widget.Button;
import android.widget.ArrayAdapter;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import com.m9chko.bedrockrelay.schematic.SchematicRepository;
import com.m9chko.bedrockrelay.schematic.SchematicSourceFolder;

import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class MainActivity extends Activity {
    public static final String ACTION_IMPORT_TEXTURE_PACK =
        "com.m9chko.bedrockrelay.action.IMPORT_TEXTURE_PACK";
    public static final String ACTION_IMPORT_SCHEMATIC =
        "com.m9chko.bedrockrelay.action.IMPORT_SCHEMATIC";
    public static final String ACTION_CHOOSE_SCHEMATIC_FOLDER =
        "com.m9chko.bedrockrelay.action.CHOOSE_SCHEMATIC_FOLDER";
    private static final int NOTIFICATION_PERMISSION_REQUEST = 100;
    private static final int OVERLAY_PERMISSION_REQUEST = 101;
    private static final int TEXTURE_PACK_REQUEST = 102;
    private static final int SCHEMATIC_REQUEST = 103;
    private static final int SCHEMATIC_FOLDER_REQUEST = 104;
    private static final String MINECRAFT_PACKAGE = "com.mojang.minecraftpe";
    private static final String OFFICIAL_TEXTURE_RELEASES =
        "https://github.com/Mojang/bedrock-samples/releases";

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final ExecutorService textureImportExecutor =
        Executors.newSingleThreadExecutor();
    private final ExecutorService schematicImportExecutor =
        Executors.newSingleThreadExecutor();
    private final Runnable refreshTask = new Runnable() {
        @Override public void run() {
            refreshState();
            handler.postDelayed(this, 500);
        }
    };

    private SharedPreferences preferences;
    private OfficialTexturePack texturePack;
    private SchematicRepository schematicRepository;
    private SchematicSourceFolder schematicSourceFolder;
    private EditText hostInput;
    private EditText portInput;
    private Spinner versionInput;
    private TextView statusText;
    private TextView detailText;
    private TextView authText;
    private Button authButton;
    private boolean launchMinecraftWhenReady;
    private TextView logText;
    private boolean logVisible;
    private int selectedPage;
    private LinearLayout modulesPage;
    private TextView modulesTab;
    private TextView connectionBadge;
    private TextView latencyText;
    private TextView localStateText;
    private Button startButton;
    private Button relayOnlyButton;
    private Button stopButton;
    private boolean relayRunning;
    private long startRequestedAt;
    private boolean snapshotPending;
    private boolean logReadPending;
    private boolean activityResumed;
    private final ExecutorService uiWorker = Executors.newSingleThreadExecutor();
    private final java.util.Map<String, Switch> moduleSwitches = new java.util.HashMap<>();
    private LinearLayout connectionPage;
    private LinearLayout logsPage;
    private TextView connectionTab;
    private TextView logsTab;
    private TextView texturePackStatusText;
    private Button texturePackImportButton;
    private Button texturePackDeleteButton;
    private boolean texturePackImportBusy;
    private boolean schematicImportBusy;
    private long nextLogRefreshAt;
    private boolean pendingOverlayRelayStart;
    private boolean pendingOverlayMinecraftLaunch;
    private String pendingOverlayHost;
    private int pendingOverlayPort;
    private String pendingOverlayVersion;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        preferences = getSharedPreferences(RelayService.PREFERENCES, MODE_PRIVATE);
        texturePack = new OfficialTexturePack(this);
        schematicRepository = new SchematicRepository(this);
        schematicSourceFolder = new SchematicSourceFolder(this);
        if (savedInstanceState != null) {
            pendingOverlayRelayStart = savedInstanceState.getBoolean(
                "pending_overlay_start",
                false
            );
            pendingOverlayMinecraftLaunch = savedInstanceState.getBoolean(
                "pending_overlay_launch_minecraft",
                false
            );
            pendingOverlayHost = savedInstanceState.getString(
                "pending_overlay_host"
            );
            pendingOverlayPort = savedInstanceState.getInt(
                "pending_overlay_port",
                19132
            );
            pendingOverlayVersion = savedInstanceState.getString(
                "pending_overlay_version"
            );
        }
        DiagnosticsLog.append(this, "INFO", "ui", "Main screen opened");
        setContentView(buildContent());
        showMainPage(savedInstanceState == null ? 0 : savedInstanceState.getInt("main_page", 0));
        if (savedInstanceState != null) {
            hostInput.setText(savedInstanceState.getString("draft_host", hostInput.getText().toString()));
            portInput.setText(savedInstanceState.getString("draft_port", portInput.getText().toString()));
        }
        maybeOpenImportPicker(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        maybeOpenImportPicker(intent);
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        outState.putInt("main_page", selectedPage);
        outState.putString("draft_host", hostInput.getText().toString());
        outState.putString("draft_port", portInput.getText().toString());
        outState.putBoolean("pending_overlay_start", pendingOverlayRelayStart);
        outState.putBoolean(
            "pending_overlay_launch_minecraft",
            pendingOverlayMinecraftLaunch
        );
        outState.putString("pending_overlay_host", pendingOverlayHost);
        outState.putInt("pending_overlay_port", pendingOverlayPort);
        outState.putString("pending_overlay_version", pendingOverlayVersion);
        super.onSaveInstanceState(outState);
    }

    @Override
    protected void onResume() {
        super.onResume();
        activityResumed = true;
        for (java.util.Map.Entry<String, Switch> entry : moduleSwitches.entrySet()) {
            entry.getValue().setChecked(preferences.getBoolean(entry.getKey(), entry.getValue().isChecked()));
        }
        if (pendingOverlayRelayStart && Settings.canDrawOverlays(this)) {
            continuePendingOverlayStart();
        }
        refreshTexturePackStatus();
        handler.removeCallbacks(refreshTask);
        handler.post(refreshTask);
    }

    @Override
    protected void onPause() {
        activityResumed = false;
        handler.removeCallbacks(refreshTask);
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        handler.removeCallbacksAndMessages(null);
        uiWorker.shutdownNow();
        textureImportExecutor.shutdownNow();
        schematicImportExecutor.shutdownNow();
        super.onDestroy();
    }

    private View buildContent() {
        LinearLayout shell = new LinearLayout(this);
        shell.setOrientation(LinearLayout.VERTICAL);
        shell.setBackgroundColor(RelayUi.BACKGROUND);
        shell.setPadding(dp(20), dp(12), dp(20), dp(8));
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
        }
        shell.setOnApplyWindowInsetsListener((view, insets) -> {
            if (Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets safe = insets.getInsets(
                    WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout() |
                        WindowInsets.Type.ime());
                view.setPadding(dp(20) + safe.left, dp(12) + safe.top,
                    dp(20) + safe.right, dp(8) + safe.bottom);
            }
            return insets;
        });
        LinearLayout heading = new LinearLayout(this);
        heading.setGravity(Gravity.CENTER_VERTICAL);
        TextView mark = text("C", 22, true);
        mark.setGravity(Gravity.CENTER);
        mark.setTextColor(RelayUi.ACCENT);
        mark.setBackground(RelayUi.surface(this, RelayUi.RAISED, 14, RelayUi.BORDER));
        heading.addView(mark, margins(dp(44), dp(44), 0, 0, dp(12), 0));
        LinearLayout brand = new LinearLayout(this);
        brand.setOrientation(LinearLayout.VERTICAL);
        brand.addView(text("CPE Relay", 23, true));
        TextView subtitle = text("Твой мир. Твои инструменты.", 12, false);
        subtitle.setTextColor(RelayUi.MUTED);
        brand.addView(subtitle);
        heading.addView(brand, new LinearLayout.LayoutParams(0, -2, 1));
        TextView version = text(BuildConfig.VERSION_NAME, 11, true);
        version.setTextColor(RelayUi.MUTED);
        heading.addView(version);
        shell.addView(heading, margins(-1, -2, 0, 0, 0, dp(18)));

        FrameLayout pages = new FrameLayout(this);
        connectionPage = buildConnectionPage();
        modulesPage = buildModulesPage();
        logsPage = buildLogsPage();
        pages.addView(connectionPage);
        pages.addView(modulesPage);
        pages.addView(logsPage);
        shell.addView(pages, new LinearLayout.LayoutParams(-1, 0, 1));

        LinearLayout tabs = new LinearLayout(this);
        tabs.setPadding(dp(4), dp(4), dp(4), dp(4));
        tabs.setBackground(cardBackground());
        connectionTab = tab("Подключение");
        modulesTab = tab("Модули");
        logsTab = tab("Журнал");
        tabs.addView(connectionTab, new LinearLayout.LayoutParams(0, dp(50), 1));
        tabs.addView(modulesTab, new LinearLayout.LayoutParams(0, dp(50), 1));
        tabs.addView(logsTab, new LinearLayout.LayoutParams(0, dp(50), 1));
        shell.addView(tabs, margins(-1, -2, 0, dp(8), 0, 0));
        connectionTab.setOnClickListener(view -> showMainPage(0));
        modulesTab.setOnClickListener(view -> showMainPage(1));
        logsTab.setOnClickListener(view -> showMainPage(2));
        shell.requestApplyInsets();
        return shell;
    }

    private LinearLayout buildConnectionPage() {
        LinearLayout content = column();
        LinearLayout hero = card();
        hero.setBackground(new GradientDrawable(GradientDrawable.Orientation.TL_BR,
            new int[] {0xff202f49, RelayUi.SURFACE}));
        ((GradientDrawable) hero.getBackground()).setCornerRadius(dp(22));
        connectionBadge = text("●  ГОТОВ К ЗАПУСКУ", 11, true);
        connectionBadge.setLetterSpacing(0.08f);
        connectionBadge.setTextColor(RelayUi.ACCENT);
        hero.addView(connectionBadge);
        statusText = text("Подключимся к миру", 25, true);
        hero.addView(statusText, margins(-1, -2, 0, dp(12), 0, dp(8)));
        detailText = text("Укажи сервер и запусти реле. Затем подключись к локальному адресу в Minecraft.", 13, false);
        detailText.setTextColor(RelayUi.MUTED);
        detailText.setLineSpacing(dp(3), 1);
        hero.addView(detailText);
        LinearLayout metrics = new LinearLayout(this);
        localStateText = text("РЕЛЕ\nОстановлено", 12, true);
        latencyText = text("ОТКЛИК\n—", 12, true);
        metrics.addView(localStateText, new LinearLayout.LayoutParams(0, -2, 1));
        metrics.addView(latencyText, new LinearLayout.LayoutParams(0, -2, 1));
        hero.addView(metrics, margins(-1, -2, 0, dp(18), 0, 0));
        content.addView(hero, margins(-1, -2, 0, 0, 0, dp(14)));

        authText = text("", 16, true);
        authText.setVisibility(View.GONE);
        content.addView(authText);
        authButton = primaryButton("Скопировать код и войти в Microsoft");
        authButton.setVisibility(View.GONE);
        authButton.setOnClickListener(view -> openAuthentication());
        content.addView(authButton, margins(-1, -2, 0, dp(6), 0, dp(10)));

        LinearLayout server = card();
        server.addView(sectionLabel("СЕРВЕР"));
        server.addView(fieldLabel("Адрес сервера"));
        hostInput = new EditText(this);
        styleInput(hostInput);
        hostInput.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        hostInput.setHint("play.example.com");
        hostInput.setText(preferences.getString(RelayService.KEY_HOST, "cpe.ign.gg"));
        server.addView(hostInput, margins(-1, dp(52), 0, dp(5), 0, dp(14)));
        LinearLayout fields = new LinearLayout(this);
        LinearLayout portField = column();
        portField.addView(fieldLabel("UDP-порт"));
        portInput = new EditText(this);
        styleInput(portInput);
        portInput.setInputType(InputType.TYPE_CLASS_NUMBER);
        portInput.setText(String.valueOf(preferences.getInt(RelayService.KEY_PORT, 19132)));
        portField.addView(portInput, margins(-1, dp(52), 0, dp(5), 0, 0));
        fields.addView(portField, new LinearLayout.LayoutParams(0, -2, 1));
        LinearLayout versionField = column();
        versionField.setPadding(dp(12), 0, 0, 0);
        versionField.addView(fieldLabel("Версия Bedrock"));
        versionInput = new Spinner(this);
        configureVersions();
        versionInput.setBackground(RelayUi.action(this, RelayUi.BACKGROUND, 12, RelayUi.BORDER));
        versionField.addView(versionInput, margins(-1, dp(52), 0, dp(5), 0, 0));
        fields.addView(versionField, new LinearLayout.LayoutParams(0, -2, 1.35f));
        server.addView(fields);
        TextView hint = text("Выбери ту же версию, что установлена в Minecraft.", 12, false);
        hint.setTextColor(RelayUi.MUTED);
        server.addView(hint, margins(-1, -2, 0, dp(12), 0, 0));
        content.addView(server, margins(-1, -2, 0, 0, 0, dp(12)));

        TextView local = text("АДРЕС В MINECRAFT\n127.0.0.1:19132     ·     Копировать", 13, true);
        local.setTextColor(RelayUi.ACCENT);
        local.setPadding(dp(16), dp(14), dp(16), dp(14));
        local.setBackground(RelayUi.action(this, RelayUi.SURFACE, 16, RelayUi.BORDER));
        local.setOnClickListener(view -> copyText("Адрес реле", "127.0.0.1:19132"));
        content.addView(local, margins(-1, -2, 0, 0, 0, dp(14)));
        TextView guide = text("Модули и оформление доступны на соседней вкладке. В игре подробные настройки открываются кнопкой CPE.", 12, false);
        guide.setTextColor(RelayUi.MUTED);
        content.addView(guide);

        LinearLayout wrapper = scrollPage(content);
        LinearLayout actions = column();
        startButton = primaryButton("Запустить и открыть Minecraft");
        startButton.setOnClickListener(view -> {
            if (relayRunning) openMinecraft(); else startRelay(true);
        });
        actions.addView(startButton, margins(-1, -2, 0, dp(8), 0, dp(8)));
        LinearLayout secondary = new LinearLayout(this);
        relayOnlyButton = secondaryButton("Только реле");
        relayOnlyButton.setOnClickListener(view -> startRelay(false));
        stopButton = secondaryButton("Остановить");
        stopButton.setTextColor(RelayUi.DANGER);
        stopButton.setEnabled(false);
        stopButton.setOnClickListener(view -> stopRelay());
        LinearLayout.LayoutParams left = new LinearLayout.LayoutParams(0, -2, 1);
        left.setMargins(0, 0, dp(8), 0);
        secondary.addView(relayOnlyButton, left);
        secondary.addView(stopButton, new LinearLayout.LayoutParams(0, -2, 1));
        actions.addView(secondary);
        wrapper.addView(actions);
        return wrapper;
    }

    private LinearLayout buildModulesPage() {
        LinearLayout content = column();
        content.addView(text("Инструменты мира", 24, true), margins(-1, -2, 0, 0, 0, dp(6)));
        TextView hint = text("Включи нужное здесь. Положение, цвет и точные параметры настраиваются через меню CPE в игре.", 13, false);
        hint.setTextColor(RelayUi.MUTED);
        content.addView(hint, margins(-1, -2, 0, 0, 0, dp(14)));
        content.addView(sectionLabel("ОТОБРАЖЕНИЕ"));
        addModule(content, "Обводка сущностей", "Игроки, мобы и предметы", RelayService.KEY_ENTITY_OUTLINES, true);
        addModule(content, "Мини-карта", "Поверхность и твоя позиция", RelayService.KEY_MINIMAP, false);
        addModule(content, "Снаряжение", "Руки, броня и прочность", RelayService.KEY_EQUIPMENT_HUD, false);
        addModule(content, "Анализ угроз", "Предупреждения о приближении мобов", RelayService.KEY_THREAT_ANALYSIS, false);
        content.addView(sectionLabel("СТРОИТЕЛЬСТВО И АВТОМАТИЗАЦИЯ"));
        addModule(content, "Автозаполнение", "Точки и запуск выбираются в игре", RelayService.KEY_AREA_FILL_ENABLED, false);
        addModule(content, "Авто-тотем", "Пополнение левой руки из инвентаря", RelayService.KEY_AUTO_TOTEM, false);
        addModule(content, "Авто-броня", "Выбор снаряжения из инвентаря", RelayService.KEY_AUTO_ARMOR, false);
        addModule(content, "Удержание чанков", "Больше загруженного мира · расход памяти", RelayService.KEY_CHUNK_RETENTION, false);
        LinearLayout schematics = card();
        schematics.addView(sectionLabel("СХЕМЫ"));
        schematics.addView(text("Библиотека построек", 18, true));
        TextView formats = text("mcstructure · nbt · litematic · schem · schematic", 12, false);
        formats.setTextColor(RelayUi.MUTED);
        schematics.addView(formats, margins(-1, -2, 0, dp(6), 0, dp(12)));
        Button importSchematic = secondaryButton("Импортировать схему");
        importSchematic.setOnClickListener(view -> openSchematicPicker());
        schematics.addView(importSchematic, margins(-1, -2, 0, 0, 0, dp(8)));
        Button folder = secondaryButton("Выбрать папку со схемами");
        folder.setOnClickListener(view -> openSchematicFolderPicker());
        schematics.addView(folder, margins(-1, -2, 0, 0, 0, 0));
        content.addView(schematics, margins(-1, -2, 0, dp(8), 0, dp(12)));
        LinearLayout texturesCard = card();
        TextView texturesTitle = text("ТЕКСТУРЫ HUD И СХЕМ", 12, true);
        texturesTitle.setTextColor(0xffc79aff);
        texturesCard.addView(
            texturesTitle,
            margins(-1, -2, 0, 0, 0, dp(7))
        );
        texturePackStatusText = text("", 13, false);
        texturePackStatusText.setTextColor(0xff9dabbb);
        texturesCard.addView(
            texturePackStatusText,
            margins(-1, -2, 0, 0, 0, dp(8))
        );
        texturePackImportButton = primaryButton(
            "Импортировать текстуры · ZIP"
        );
        texturePackImportButton.setOnClickListener(
            view -> openTexturePackPicker()
        );
        texturesCard.addView(
            texturePackImportButton,
            margins(-1, dp(48), 0, 0, 0, dp(6))
        );
        Button releases = secondaryButton("Открыть релизы Mojang");
        releases.setOnClickListener(view -> openOfficialTextureReleases());
        texturesCard.addView(releases, margins(-1, dp(46), 0, 0, 0, dp(6)));
        texturePackDeleteButton = secondaryButton("Удалить текстуры");
        texturePackDeleteButton.setTextColor(0xffff9aa5);
        texturePackDeleteButton.setOnClickListener(view -> {
            new AlertDialog.Builder(this)
                .setTitle("Удалить текстуры?")
                .setMessage("Их можно будет импортировать снова из ZIP-архива.")
                .setNegativeButton("Отмена", null)
                .setPositiveButton("Удалить", (dialog, which) -> {
                    texturePackImportBusy = true;
                    refreshTexturePackStatus();
                    textureImportExecutor.execute(() -> {
                        texturePack.clear();
                        handler.post(() -> {
                            if (isDestroyed()) return;
                            texturePackImportBusy = false;
                            refreshTexturePackStatus();
                            toast("Текстуры удалены");
                        });
                    });
                }).show();
        });
        texturesCard.addView(
            texturePackDeleteButton,
            margins(-1, dp(46), 0, 0, 0, dp(7))
        );
        TextView textureNote = text(
            "Выберите официальный FULL-архив bedrock-samples один раз. " +
                "Приложение сохранит item и block PNG во внутренней памяти. " +
                "Схемы используют их автоматически; файлы не добавляются в APK " +
                "или Git.",
            12,
            false
        );
        textureNote.setTextColor(0xff78889b);
        texturesCard.addView(textureNote);
        content.addView(texturesCard, margins(-1, -2, 0, 0, 0, dp(10)));
        refreshTexturePackStatus();


        return scrollPage(content);
    }

    private void addModule(LinearLayout content, String title, String description,
                           String key, boolean fallback) {
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(16), dp(12), dp(12), dp(12));
        row.setBackground(cardBackground());
        LinearLayout labels = column();
        labels.addView(text(title, 15, true));
        TextView detail = text(description, 12, false);
        detail.setTextColor(RelayUi.MUTED);
        labels.addView(detail, margins(-1, -2, 0, dp(3), dp(8), 0));
        row.addView(labels, new LinearLayout.LayoutParams(0, -2, 1));
        Switch toggle = new Switch(this);
        toggle.setContentDescription(title);
        toggle.setMinHeight(dp(48));
        toggle.setChecked(preferences.getBoolean(key, fallback));
        toggle.setThumbTintList(new ColorStateList(
            new int[][] {new int[] {android.R.attr.state_checked}, new int[0]},
            new int[] {RelayUi.ACCENT, RelayUi.MUTED}));
        toggle.setOnCheckedChangeListener((button, checked) -> {
            if (preferences.getBoolean(key, fallback) == checked) return;
            preferences.edit().putBoolean(key, checked).apply();
            if (relayRunning) startService(new Intent(this, RelayService.class)
                .setAction(RelayService.ACTION_APPLY_SETTINGS));
        });
        moduleSwitches.put(key, toggle);
        row.addView(toggle);
        row.setOnClickListener(view -> toggle.setChecked(!toggle.isChecked()));
        content.addView(row, margins(-1, -2, 0, 0, 0, dp(8)));
    }

    private LinearLayout column() {
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        return column;
    }

    private LinearLayout scrollPage(LinearLayout content) {
        content.setPadding(0, 0, 0, dp(12));
        ScrollView scroll = new ScrollView(this);
        scroll.setClipToPadding(false);
        scroll.setVerticalScrollBarEnabled(false);
        scroll.addView(content);
        LinearLayout wrapper = column();
        wrapper.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        return wrapper;
    }

    private TextView sectionLabel(String label) {
        TextView view = text(label, 11, true);
        view.setTextColor(RelayUi.ACCENT);
        view.setLetterSpacing(0.1f);
        view.setPadding(0, dp(6), 0, dp(12));
        return view;
    }

    private void styleInput(EditText input) {
        input.setSingleLine(true);
        input.setTextSize(16);
        input.setTextColor(RelayUi.TEXT);
        input.setHintTextColor(RelayUi.MUTED);
        input.setPadding(dp(12), 0, dp(12), 0);
        input.setBackground(RelayUi.action(this, RelayUi.BACKGROUND, 12, RelayUi.BORDER));
        input.setSelectAllOnFocus(true);
    }

    private LinearLayout buildLogsPage() {
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(2), 0, dp(2), dp(18));
        LinearLayout controls = card();
        Switch enabled = new Switch(this);
        enabled.setText("Записывать журнал");
        enabled.setTextSize(14);
        enabled.setTextColor(Color.WHITE);
        enabled.setChecked(preferences.getBoolean(
            RelayService.KEY_DETAILED_LOGS,
            true
        ));
        controls.addView(enabled);
        TextView note = text(
            enabled.isChecked()
                ? "Запись включена: повторы сжимаются, хранится не более " +
                    "512 КБ; неактивные файлы старше суток удаляются."
                : "Запись полностью выключена — новые строки не создаются.",
            10,
            true
        );
        note.setTextColor(enabled.isChecked() ? 0xff74df9c : 0xffffc76c);
        controls.addView(note, margins(-1, -2, 0, 0, 0, dp(8)));
        enabled.setOnCheckedChangeListener((button, checked) -> {
            preferences.edit()
                .putBoolean(RelayService.KEY_DETAILED_LOGS, checked)
                .apply();
            note.setText(checked
                ? "Запись включена: повторы сжимаются, хранится не более " +
                    "512 КБ; неактивные файлы старше суток удаляются."
                : "Запись полностью выключена — новые строки не создаются.");
            note.setTextColor(checked ? 0xff74df9c : 0xffffc76c);
            applyLoggingPreferenceToNative(checked);
        });
        Button copy = secondaryButton("Копировать журнал");
        copy.setOnClickListener(view -> copyLog());
        controls.addView(copy, margins(-1, dp(46), 0, 0, 0, dp(6)));
        Button clear = secondaryButton("Очистить журнал");
        clear.setOnClickListener(view -> {
            uiWorker.execute(() -> {
                DiagnosticsLog.clear(getApplicationContext());
                runOnUiThread(() -> { if (!isDestroyed()) refreshLogNow(); });
            });
            toast("Журнал очищен");
        });
        controls.addView(clear, margins(-1, dp(46), 0, 0, 0, dp(6)));
        TextView path = text(DiagnosticsLog.path(this), 9, false);
        path.setTextColor(0xff718094);
        path.setTextIsSelectable(true);
        controls.addView(path);
        content.addView(controls, margins(-1, -2, 0, 0, 0, dp(9)));

        logText = text("Журнал пока пуст.", 9, false);
        logText.setTypeface(Typeface.MONOSPACE);
        logText.setTextColor(0xffb9c7d6);
        logText.setTextIsSelectable(true);
        logText.setPadding(dp(10), dp(10), dp(10), dp(10));
        logText.setBackground(cardBackground());
        content.addView(logText);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(content);
        LinearLayout wrapper = new LinearLayout(this);
        wrapper.setOrientation(LinearLayout.VERTICAL);
        wrapper.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1f));
        return wrapper;
    }

    private void showMainPage(boolean logs) {
        showMainPage(logs ? 2 : 0);
    }

    private void showMainPage(int page) {
        selectedPage = Math.max(0, Math.min(2, page));
        logVisible = selectedPage == 2;
        View[] pages = {connectionPage, modulesPage, logsPage};
        TextView[] tabs = {connectionTab, modulesTab, logsTab};
        for (int index = 0; index < pages.length; index++) {
            pages[index].setVisibility(index == selectedPage ? View.VISIBLE : View.GONE);
            tabs[index].setBackground(tabBackground(index == selectedPage));
            tabs[index].setTextColor(index == selectedPage ? RelayUi.ACCENT : RelayUi.MUTED);
            tabs[index].setSelected(index == selectedPage);
        }
        if (logVisible) refreshLogNow();
    }

    private void applyLoggingPreferenceToNative(boolean enabled) {
        // Settings are serialized by the service; never enter JNI from a tap.
        if (relayRunning) startService(new Intent(this, RelayService.class)
            .setAction(RelayService.ACTION_APPLY_SETTINGS));
    }

    private void startRelay(boolean launchMinecraft) {
        if (relayRunning || (startRequestedAt != 0 && SystemClock.elapsedRealtime() - startRequestedAt < 15_000)) return;
        String host = hostInput.getText().toString().trim();
        int port;
        try {
            port = Integer.parseInt(portInput.getText().toString().trim());
        } catch (NumberFormatException error) {
            toast("Порт должен быть числом");
            return;
        }
        if (host.isEmpty() || host.contains("://") || host.contains("/") ||
            port < 1 || port > 65535) {
            toast("Проверь адрес сервера и порт");
            return;
        }
        Object selectedVersion = versionInput.getSelectedItem();
        String version = selectedVersion == null
            ? "1.21.100"
            : selectedVersion.toString();

        preferences.edit()
            .putString(RelayService.KEY_HOST, host)
            .putInt(RelayService.KEY_PORT, port)
            .putString(RelayService.KEY_VERSION, version)
            .remove(RelayService.KEY_LAST_ERROR)
            .apply();
        DiagnosticsLog.append(
            this,
            "INFO",
            "ui",
            "Start button: destination=" + host + ":" + port +
                " version=" + version +
                " launchMinecraft=" + launchMinecraft
        );
        if (!Settings.canDrawOverlays(this)) {
            pendingOverlayRelayStart = true;
            pendingOverlayMinecraftLaunch = launchMinecraft;
            pendingOverlayHost = host;
            pendingOverlayPort = port;
            pendingOverlayVersion = version;
            try {
                Intent permission = new Intent(
                    Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:" + getPackageName())
                );
                startActivityForResult(permission, OVERLAY_PERMISSION_REQUEST);
                toast("Разреши меню CPE Relay поверх Minecraft");
                return;
            } catch (Throwable error) {
                pendingOverlayRelayStart = false;
                DiagnosticsLog.appendError(
                    this,
                    "overlay",
                    "Could not open Android overlay permission screen",
                    error
                );
                toast("Не удалось открыть настройку плавающего меню");
            }
        }
        startRelayService(host, port, version, launchMinecraft);
    }

    private void startRelayService(
        String host,
        int port,
        String version,
        boolean launchMinecraft
    ) {
        requestNotificationPermissionIfNeeded();
        Intent intent = new Intent(this, RelayService.class)
            .setAction(RelayService.ACTION_START)
            .putExtra(RelayService.EXTRA_HOST, host)
            .putExtra(RelayService.EXTRA_PORT, port)
            .putExtra(RelayService.EXTRA_VERSION, version);
        if (Build.VERSION.SDK_INT >= 26) {
            startRequestedAt = SystemClock.elapsedRealtime();
            startForegroundService(intent);
        } else {
            startRequestedAt = SystemClock.elapsedRealtime();
            startService(intent);
        }
        launchMinecraftWhenReady = launchMinecraft;
        statusText.setText("Запуск relay…");
    }

    private void continuePendingOverlayStart() {
        if (!pendingOverlayRelayStart) return;
        boolean launchMinecraft = pendingOverlayMinecraftLaunch;
        String host = pendingOverlayHost;
        int port = pendingOverlayPort;
        String version = pendingOverlayVersion;
        pendingOverlayRelayStart = false;
        pendingOverlayHost = null;
        pendingOverlayVersion = null;
        if (!Settings.canDrawOverlays(this)) {
            toast("Плавающее меню отключено: разрешение не выдано");
        }
        startRelayService(host, port, version, launchMinecraft);
    }

    @Override
    protected void onActivityResult(
        int requestCode,
        int resultCode,
        Intent data
    ) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == OVERLAY_PERMISSION_REQUEST) {
            continuePendingOverlayStart();
        } else if (requestCode == TEXTURE_PACK_REQUEST &&
            resultCode == RESULT_OK && data != null && data.getData() != null) {
            Uri source = data.getData();
            try {
                if ((data.getFlags() & Intent.FLAG_GRANT_READ_URI_PERMISSION) != 0) {
                    getContentResolver().takePersistableUriPermission(
                        source,
                        Intent.FLAG_GRANT_READ_URI_PERMISSION
                    );
                }
            } catch (Throwable ignored) {
            }
            importTexturePack(source, displayName(source));
        } else if (requestCode == SCHEMATIC_REQUEST &&
            resultCode == RESULT_OK && data != null && data.getData() != null) {
            Uri source = data.getData();
            try {
                if ((data.getFlags() & Intent.FLAG_GRANT_READ_URI_PERMISSION) != 0) {
                    getContentResolver().takePersistableUriPermission(
                        source,
                        Intent.FLAG_GRANT_READ_URI_PERMISSION
                    );
                }
            } catch (Throwable ignored) {
            }
            importSchematic(source, displayName(source));
        } else if (requestCode == SCHEMATIC_FOLDER_REQUEST &&
            resultCode == RESULT_OK && data != null && data.getData() != null) {
            Uri tree = data.getData();
            try {
                getContentResolver().takePersistableUriPermission(
                    tree,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION
                );
                schematicSourceFolder.saveTree(tree);
                toast("Папка схем подключена");
            } catch (Throwable error) {
                DiagnosticsLog.appendError(
                    this,
                    "schematics",
                    "Could not persist schematic folder access",
                    error
                );
                toast("Не удалось сохранить доступ к папке");
            }
            if (preferences.getBoolean(RelayService.KEY_RELAY_ACTIVE, false)) {
                openMinecraft();
            }
        }
    }

    private void maybeOpenImportPicker(Intent intent) {
        if (intent == null) return;
        if (ACTION_IMPORT_TEXTURE_PACK.equals(intent.getAction())) {
            intent.setAction(null);
            handler.post(this::openTexturePackPicker);
        } else if (ACTION_IMPORT_SCHEMATIC.equals(intent.getAction())) {
            intent.setAction(null);
            handler.post(this::openSchematicPicker);
        } else if (ACTION_CHOOSE_SCHEMATIC_FOLDER.equals(intent.getAction())) {
            intent.setAction(null);
            handler.post(this::openSchematicFolderPicker);
        }
    }

    private void openSchematicFolderPicker() {
        Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        picker.addFlags(
            Intent.FLAG_GRANT_READ_URI_PERMISSION |
                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION |
                Intent.FLAG_GRANT_PREFIX_URI_PERMISSION
        );
        Uri current = schematicSourceFolder == null
            ? null
            : schematicSourceFolder.treeUri();
        if (current != null) {
            picker.putExtra(DocumentsContract.EXTRA_INITIAL_URI, current);
        }
        try {
            startActivityForResult(picker, SCHEMATIC_FOLDER_REQUEST);
        } catch (Throwable error) {
            DiagnosticsLog.appendError(
                this,
                "schematics",
                "Could not open schematic folder picker",
                error
            );
            toast("Не удалось открыть выбор папки");
        }
    }

    private void openSchematicPicker() {
        if (schematicImportBusy) return;
        Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        picker.addCategory(Intent.CATEGORY_OPENABLE);
        picker.setType("*/*");
        picker.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {
            "application/octet-stream",
            "application/gzip",
            "application/x-gzip",
            "application/nbt"
        });
        picker.addFlags(
            Intent.FLAG_GRANT_READ_URI_PERMISSION |
                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
        );
        try {
            startActivityForResult(picker, SCHEMATIC_REQUEST);
        } catch (Throwable error) {
            DiagnosticsLog.appendError(
                this,
                "schematics",
                "Could not open schematic picker",
                error
            );
            toast("Не удалось открыть выбор схемы");
        }
    }

    private void importSchematic(Uri source, String sourceName) {
        if (schematicImportBusy) return;
        schematicImportBusy = true;
        toast("Импорт схемы…");
        schematicImportExecutor.execute(() -> {
            try (InputStream input = getContentResolver().openInputStream(source)) {
                SchematicRepository.ImportResult result =
                    schematicRepository.importAndActivate(input, sourceName);
                preferences.edit()
                    .putBoolean(RelayService.KEY_SCHEMATIC_ENABLED, true)
                    .putBoolean(RelayService.KEY_SCHEMATIC_PLACED, false)
                    .apply();
                DiagnosticsLog.append(
                    this,
                    "INFO",
                    "schematics",
                    "Imported " + result.model.sourceName() + " " +
                        result.model.description()
                );
                runOnUiThread(() -> {
                    schematicImportBusy = false;
                    notifySchematicReload();
                    toast("Схема импортирована: " + result.model.description());
                    if (preferences.getBoolean(
                        RelayService.KEY_RELAY_ACTIVE,
                        false
                    )) {
                        openMinecraft();
                    }
                });
            } catch (Throwable error) {
                DiagnosticsLog.appendError(
                    this,
                    "schematics",
                    "Schematic import failed",
                    error
                );
                runOnUiThread(() -> {
                    schematicImportBusy = false;
                    String message = error.getMessage();
                    toast(message == null || message.trim().isEmpty()
                        ? "Не удалось импортировать схему"
                        : message);
                });
            }
        });
    }

    private void notifySchematicReload() {
        if (!preferences.getBoolean(RelayService.KEY_RELAY_ACTIVE, false)) {
            return;
        }
        try {
            startService(new Intent(this, RelayService.class)
                .setAction(RelayService.ACTION_RELOAD_SCHEMATIC));
        } catch (Throwable error) {
            DiagnosticsLog.appendError(
                this,
                "schematics",
                "Could not notify relay about imported schematic",
                error
            );
        }
    }

    private void openTexturePackPicker() {
        if (texturePackImportBusy) return;
        Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        picker.addCategory(Intent.CATEGORY_OPENABLE);
        picker.setType("*/*");
        picker.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {
            "application/zip",
            "application/x-zip-compressed",
            "application/octet-stream",
            "application/mcpack"
        });
        picker.addFlags(
            Intent.FLAG_GRANT_READ_URI_PERMISSION |
                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
        );
        try {
            startActivityForResult(picker, TEXTURE_PACK_REQUEST);
        } catch (Throwable error) {
            DiagnosticsLog.appendError(
                this,
                "textures",
                "Could not open texture archive picker",
                error
            );
            toast("Не удалось открыть выбор ZIP");
        }
    }

    private void importTexturePack(Uri source, String sourceName) {
        texturePackImportBusy = true;
        refreshTexturePackStatus();
        textureImportExecutor.execute(() -> {
            try (InputStream input = getContentResolver().openInputStream(source)) {
                OfficialTexturePack.ImportResult result =
                    texturePack.importArchive(input, sourceName);
                runOnUiThread(() -> {
                    texturePackImportBusy = false;
                    refreshTexturePackStatus();
                    toast(
                        "Импорт: предметы " + result.itemTextureCount +
                            ", блоки " + result.blockTextureCount +
                            (result.enchantmentGlint ? " + glint" : "")
                    );
                });
            } catch (Throwable error) {
                DiagnosticsLog.appendError(
                    this,
                    "textures",
                    "Official texture pack import failed",
                    error
                );
                runOnUiThread(() -> {
                    texturePackImportBusy = false;
                    refreshTexturePackStatus();
                    String message = error instanceof IOException
                        ? error.getMessage()
                        : "Не удалось импортировать архив";
                    toast(message == null ? "Ошибка импорта" : message);
                });
            }
        });
    }

    private void refreshTexturePackStatus() {
        if (texturePackStatusText == null || texturePack == null) return;
        OfficialTexturePack.Status status = texturePack.status();
        texturePackStatusText.setText(
            texturePackImportBusy
                ? "Импорт и проверка PNG…"
                : status.description()
        );
        texturePackStatusText.setTextColor(
            texturePackImportBusy
                ? 0xffffc76c
                : status.imported ? 0xff74df9c : 0xff9dabbb
        );
        if (texturePackImportButton != null) {
            texturePackImportButton.setEnabled(!texturePackImportBusy);
            texturePackImportButton.setText(
                texturePackImportBusy
                    ? "Импортируем…"
                    : status.imported
                        ? "Заменить текстуры"
                        : "Импортировать текстуры"
            );
        }
        if (texturePackDeleteButton != null) {
            texturePackDeleteButton.setVisibility(
                status.imported && !texturePackImportBusy
                    ? View.VISIBLE
                    : View.GONE
            );
        }
    }

    private String displayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(
            uri,
            new String[] { OpenableColumns.DISPLAY_NAME },
            null,
            null,
            null
        )) {
            if (cursor != null && cursor.moveToFirst()) {
                int column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (column >= 0) {
                    String value = cursor.getString(column);
                    if (value != null && !value.trim().isEmpty()) return value;
                }
            }
        } catch (Throwable ignored) {
        }
        String fallback = uri.getLastPathSegment();
        return fallback == null ? "bedrock-samples FULL" : fallback;
    }

    private void openOfficialTextureReleases() {
        try {
            startActivity(new Intent(
                Intent.ACTION_VIEW,
                Uri.parse(OFFICIAL_TEXTURE_RELEASES)
            ));
        } catch (Throwable error) {
            toast("Не удалось открыть официальный релиз");
        }
    }

    private void stopRelay() {
        launchMinecraftWhenReady = false;
        DiagnosticsLog.append(this, "INFO", "ui", "Stop button pressed");
        startService(new Intent(this, RelayService.class)
            .setAction(RelayService.ACTION_STOP));
    }

    private void refreshState() {
        if (!snapshotPending && !uiWorker.isShutdown()) {
            snapshotPending = true;
            uiWorker.execute(() -> {
                JSONObject snapshot = null;
                String failure = null;
                try {
                    snapshot = new JSONObject(NativeBridge.snapshot());
                } catch (Throwable error) {
                    failure = error.getMessage();
                }
                final JSONObject result = snapshot;
                final String message = failure;
                runOnUiThread(() -> {
                    snapshotPending = false;
                    if (isDestroyed() || !activityResumed) return;
                    if (result != null) renderState(result);
                    else detailText.setText("Не удалось получить состояние реле: " + message);
                });
            });
        }
        if (logVisible && SystemClock.elapsedRealtime() >= nextLogRefreshAt) refreshLogNow();
        String code = preferences.getString(RelayService.KEY_AUTH_CODE, "");
        authText.setVisibility(code.isEmpty() ? View.GONE : View.VISIBLE);
        authButton.setVisibility(code.isEmpty() ? View.GONE : View.VISIBLE);
        if (!code.isEmpty()) authText.setText("Вход Xbox · код " + code);
    }

    private void renderState(JSONObject state) {
        boolean running = state.optBoolean("running", false);
        boolean listening = state.optBoolean("listening", false);
        int downstream = state.optInt("downstreamConnections", 0);
        boolean joined = state.optInt("downstreamJoined", 0) > 0;
        boolean ready = state.optBoolean("upstreamReady", false);
        boolean starting = startRequestedAt != 0 &&
            SystemClock.elapsedRealtime() - startRequestedAt < 15_000 && !listening;
        if (listening) startRequestedAt = 0;
        relayRunning = running || listening;
        statusText.setText(ready ? "Ты в игре" : downstream > 0
            ? (joined ? "Входим на сервер…" : "Подключаем Minecraft…")
            : listening ? "Реле готово" : running || starting ? "Запускаем реле…" : "Подключимся к миру");
        connectionBadge.setText(ready ? "● ПОДКЛЮЧЕНО" : listening ? "● СЛУШАЕМ MINECRAFT"
            : running || starting ? "● ЗАПУСК" : "● ГОТОВ К ЗАПУСКУ");
        connectionBadge.setTextColor(listening ? RelayUi.SUCCESS : RelayUi.ACCENT);
        String host = preferences.getString(RelayService.KEY_HOST, "cpe.ign.gg");
        int port = preferences.getInt(RelayService.KEY_PORT, 19132);
        String error = preferences.getString(RelayService.KEY_LAST_ERROR, "");
        detailText.setText(!error.isEmpty() && !ready ? error :
            ready ? host + ":" + port + " · Соединение активно"
            : downstream > 0 ? "Локальное подключение принято. Ожидаем завершения входа."
            : listening ? "В Minecraft выбери сервер 127.0.0.1, порт 19132."
            : "Выбери сервер и версию Minecraft. Всё остальное настроим при запуске.");
        localStateText.setText("ЛОКАЛЬНОЕ РЕЛЕ\n" + (listening ? "127.0.0.1:19132" : "Остановлено"));
        latencyText.setText("СЕРВЕР\n" + (state.optBoolean("destinationPingOk", false)
            ? state.optLong("destinationLatencyMs", 0) + " мс" : ready ? "Подключён" : "—"));
        startButton.setText(listening ? "Открыть Minecraft" : starting || running ? "Запуск…" : "Запустить и играть");
        startButton.setEnabled(listening || (!starting && !running));
        relayOnlyButton.setEnabled(!relayRunning && !starting);
        stopButton.setEnabled(relayRunning || starting);
        hostInput.setEnabled(!relayRunning && !starting);
        portInput.setEnabled(!relayRunning && !starting);
        versionInput.setEnabled(!relayRunning && !starting);
        if (listening && launchMinecraftWhenReady) {
            launchMinecraftWhenReady = false;
            openMinecraft();
        }
    }

    private void openAuthentication() {
        String code = preferences.getString(RelayService.KEY_AUTH_CODE, "");
        String uri = preferences.getString(
            RelayService.KEY_AUTH_URI,
            "https://microsoft.com/link"
        );
        if (!code.isEmpty()) {
            copyText("Код Xbox", code);
        }
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(uri)));
        } catch (Exception error) {
            toast("Не удалось открыть браузер");
        }
    }

    private void openMinecraft() {
        Intent launch = getPackageManager().getLaunchIntentForPackage(
            MINECRAFT_PACKAGE
        );
        if (launch == null) {
            try {
                launch = new Intent(Intent.ACTION_VIEW, Uri.parse("minecraft://"));
                launch.setPackage(MINECRAFT_PACKAGE);
            } catch (Exception ignored) {
                launch = null;
            }
        }
        if (launch == null || launch.resolveActivity(getPackageManager()) == null) {
            DiagnosticsLog.append(this, "ERROR", "minecraft", "Minecraft package not found");
            toast("Minecraft не найден на устройстве");
            return;
        }
        launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        DiagnosticsLog.append(this, "INFO", "minecraft", "Launching Minecraft");
        startActivity(launch);
    }

    private void refreshLogNow() {
        if (logText == null || logReadPending || uiWorker.isShutdown()) return;
        logReadPending = true;
        nextLogRefreshAt = SystemClock.elapsedRealtime() + 2_000;
        uiWorker.execute(() -> {
            String value = DiagnosticsLog.readTail(getApplicationContext(), 32 * 1024);
            runOnUiThread(() -> {
                logReadPending = false;
                if (!isDestroyed() && logVisible && !value.contentEquals(logText.getText())) {
                    logText.setText(value.isEmpty() ? "Журнал пока пуст." : value);
                }
            });
        });
    }

    private void copyLog() {
        if (uiWorker.isShutdown()) return;
        uiWorker.execute(() -> {
            String value = DiagnosticsLog.readAll(getApplicationContext());
            runOnUiThread(() -> {
                if (isDestroyed()) return;
                copyText("CPE Relay diagnostics", value);
                toast("Журнал скопирован");
            });
        });
    }

    private void configureVersions() {
        ArrayList<String> versions = new ArrayList<>();
        String saved = preferences.getString(
            RelayService.KEY_VERSION,
            "1.21.100"
        );
        try {
            JSONArray values = new JSONArray(NativeBridge.supportedVersions());
            for (int index = 0; index < values.length(); ++index) {
                String version = values.optString(index, "");
                if (!version.isEmpty()) versions.add(version);
            }
            versions.sort((left, right) -> compareVersions(right, left));
        } catch (Throwable error) {
            DiagnosticsLog.appendError(
                this,
                "ui",
                "Failed to load supported protocol versions",
                error
            );
        }
        if (versions.isEmpty()) versions.add(saved);
        if (!versions.contains(saved)) versions.add(0, saved);

        ArrayAdapter<String> adapter = new ArrayAdapter<>(
            this,
            android.R.layout.simple_spinner_item,
            versions
        );
        adapter.setDropDownViewResource(
            android.R.layout.simple_spinner_dropdown_item
        );
        versionInput.setAdapter(adapter);
        versionInput.setSelection(Math.max(0, versions.indexOf(saved)));
    }

    private static int compareVersions(String left, String right) {
        String[] a = left.split("\\.");
        String[] b = right.split("\\.");
        int count = Math.max(a.length, b.length);
        for (int index = 0; index < count; ++index) {
            int av = index < a.length ? numberPrefix(a[index]) : 0;
            int bv = index < b.length ? numberPrefix(b[index]) : 0;
            if (av != bv) return Integer.compare(av, bv);
        }
        return left.compareTo(right);
    }

    private static int numberPrefix(String value) {
        int end = 0;
        while (end < value.length() && Character.isDigit(value.charAt(end))) {
            ++end;
        }
        if (end == 0) return 0;
        try {
            return Integer.parseInt(value.substring(0, end));
        } catch (NumberFormatException ignored) {
            return 0;
        }
    }

    private void requestNotificationPermissionIfNeeded() {
        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) !=
                PackageManager.PERMISSION_GRANTED) {
            requestPermissions(
                new String[] { Manifest.permission.POST_NOTIFICATIONS },
                NOTIFICATION_PERMISSION_REQUEST
            );
        }
    }

    private void copyText(String label, String value) {
        ClipboardManager clipboard = (ClipboardManager)
            getSystemService(Context.CLIPBOARD_SERVICE);
        clipboard.setPrimaryClip(ClipData.newPlainText(label, value));
        toast("Скопировано: " + value);
    }

    private LinearLayout card() {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(18), dp(16), dp(18), dp(16));
        card.setBackground(cardBackground());
        card.setElevation(0);
        return card;
    }

    private TextView fieldLabel(String value) {
        TextView label = text(value, 12, true);
        label.setTextColor(0xffa9b7c7);
        return label;
    }

    private TextView tab(String value) {
        TextView tab = text(value, 11, true);
        tab.setGravity(Gravity.CENTER);
        tab.setClickable(true);
        return tab;
    }

    private Button primaryButton(String value) {
        return RelayUi.button(this, value, true);
    }

    private Button secondaryButton(String value) {
        return RelayUi.button(this, value, false);
    }

    private GradientDrawable cardBackground() {
        return RelayUi.surface(this, RelayUi.SURFACE, 20, RelayUi.BORDER);
    }

    private GradientDrawable pillBackground() {
        return RelayUi.surface(this, RelayUi.RAISED, 12, RelayUi.BORDER);
    }

    private GradientDrawable tabBackground(boolean selected) {
        return RelayUi.surface(this, selected ? RelayUi.RAISED : Color.TRANSPARENT, 14, 0);
    }

    private TextView text(String value, int sp, boolean bold) {
        return RelayUi.text(this, value, sp, bold);
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

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private void toast(String message) {
        Toast.makeText(this, message, Toast.LENGTH_LONG).show();
    }
}
