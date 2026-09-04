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
        if (pendingOverlayRelayStart && Settings.canDrawOverlays(this)) {
            continuePendingOverlayStart();
        }
        refreshTexturePackStatus();
        handler.removeCallbacks(refreshTask);
        handler.post(refreshTask);
    }

    @Override
    protected void onPause() {
        handler.removeCallbacks(refreshTask);
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        textureImportExecutor.shutdownNow();
        schematicImportExecutor.shutdownNow();
        super.onDestroy();
    }

    private View buildContent() {
        LinearLayout shell = new LinearLayout(this);
        shell.setOrientation(LinearLayout.VERTICAL);
        shell.setPadding(dp(18), dp(18), dp(18), dp(14));
        shell.setBackgroundColor(0xff0b1118);

        LinearLayout heading = new LinearLayout(this);
        heading.setOrientation(LinearLayout.HORIZONTAL);
        heading.setGravity(Gravity.CENTER_VERTICAL);
        TextView brand = text("CPE RELAY", 26, true);
        heading.addView(brand, new LinearLayout.LayoutParams(
            0,
            -2,
            1f
        ));
        TextView version = text("v" + BuildConfig.VERSION_NAME, 11, true);
        version.setTextColor(0xff7ee6a4);
        version.setPadding(dp(9), dp(5), dp(9), dp(5));
        version.setBackground(pillBackground());
        heading.addView(version);
        shell.addView(heading);
        TextView subtitle = text(
            "Bedrock relay и пакетный HUD — без Termux и захвата экрана",
            12,
            false
        );
        subtitle.setTextColor(0xff91a0b2);
        shell.addView(subtitle, margins(-1, -2, 0, dp(3), 0, dp(14)));

        LinearLayout tabs = new LinearLayout(this);
        tabs.setOrientation(LinearLayout.HORIZONTAL);
        tabs.setPadding(dp(4), dp(4), dp(4), dp(4));
        tabs.setBackground(cardBackground());
        connectionTab = tab("ПОДКЛЮЧЕНИЕ");
        logsTab = tab("ЖУРНАЛ");
        tabs.addView(connectionTab, new LinearLayout.LayoutParams(0, dp(42), 1f));
        tabs.addView(logsTab, new LinearLayout.LayoutParams(0, dp(42), 1f));
        shell.addView(tabs, margins(-1, -2, 0, 0, 0, dp(12)));

        FrameLayout pages = new FrameLayout(this);
        connectionPage = buildConnectionPage();
        logsPage = buildLogsPage();
        pages.addView(connectionPage);
        pages.addView(logsPage);
        shell.addView(pages, new LinearLayout.LayoutParams(
            -1,
            0,
            1f
        ));
        connectionTab.setOnClickListener(view -> showMainPage(false));
        logsTab.setOnClickListener(view -> showMainPage(true));
        showMainPage(false);
        return shell;
    }

    private LinearLayout buildConnectionPage() {
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(2), 0, dp(2), dp(18));

        LinearLayout statusCard = card();
        statusText = text("Relay остановлен", 19, true);
        statusText.setTextColor(0xffedf5ff);
        statusCard.addView(statusText);
        detailText = text("Локальный порт: 19132", 12, false);
        detailText.setTextColor(0xff9dabbb);
        detailText.setPadding(0, dp(4), 0, 0);
        statusCard.addView(detailText);
        content.addView(statusCard, margins(-1, -2, 0, 0, 0, dp(10)));

        LinearLayout serverCard = card();
        TextView serverTitle = text("СЕРВЕР НАЗНАЧЕНИЯ", 12, true);
        serverTitle.setTextColor(0xff7fdcff);
        serverCard.addView(serverTitle, margins(-1, -2, 0, 0, 0, dp(8)));
        serverCard.addView(fieldLabel("Адрес"));
        hostInput = new EditText(this);
        hostInput.setSingleLine(true);
        hostInput.setHint("cpe.ign.gg");
        hostInput.setText(preferences.getString(
            RelayService.KEY_HOST,
            "cpe.ign.gg"
        ));
        serverCard.addView(hostInput, margins(-1, dp(48), 0, 0, 0, dp(7)));

        serverCard.addView(fieldLabel("UDP-порт"));
        portInput = new EditText(this);
        portInput.setSingleLine(true);
        portInput.setInputType(InputType.TYPE_CLASS_NUMBER);
        portInput.setText(String.valueOf(preferences.getInt(
            RelayService.KEY_PORT,
            19132
        )));
        serverCard.addView(portInput, margins(-1, dp(48), 0, 0, 0, dp(7)));

        serverCard.addView(fieldLabel("Версия Bedrock"));
        versionInput = new Spinner(this);
        configureVersions();
        serverCard.addView(versionInput, margins(-1, dp(48), 0, 0, 0, dp(6)));
        TextView versionHint = text(
            "Клиент и сервер должны использовать одну версию протокола.",
            10,
            false
        );
        versionHint.setTextColor(0xff8291a3);
        serverCard.addView(versionHint);
        content.addView(serverCard, margins(-1, -2, 0, 0, 0, dp(10)));

        LinearLayout texturesCard = card();
        TextView texturesTitle = text("ТЕКСТУРЫ HUD И СХЕМ", 12, true);
        texturesTitle.setTextColor(0xffc79aff);
        texturesCard.addView(
            texturesTitle,
            margins(-1, -2, 0, 0, 0, dp(7))
        );
        texturePackStatusText = text("", 10, false);
        texturePackStatusText.setTextColor(0xff9dabbb);
        texturesCard.addView(
            texturePackStatusText,
            margins(-1, -2, 0, 0, 0, dp(8))
        );
        texturePackImportButton = primaryButton(
            "Импортировать официальный FULL ZIP"
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
        texturePackDeleteButton = secondaryButton("Удалить импортированные PNG");
        texturePackDeleteButton.setTextColor(0xffff9aa5);
        texturePackDeleteButton.setOnClickListener(view -> {
            texturePack.clear();
            refreshTexturePackStatus();
            toast("Импортированные текстуры удалены");
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
            9,
            false
        );
        textureNote.setTextColor(0xff78889b);
        texturesCard.addView(textureNote);
        content.addView(texturesCard, margins(-1, -2, 0, 0, 0, dp(10)));
        refreshTexturePackStatus();

        TextView localAddress = text(
            "ЛОКАЛЬНЫЙ АДРЕС   127.0.0.1:19132   ⧉",
            11,
            true
        );
        localAddress.setGravity(Gravity.CENTER);
        localAddress.setPadding(dp(10), dp(10), dp(10), dp(10));
        localAddress.setBackground(pillBackground());
        localAddress.setOnClickListener(view -> copyText(
            "Адрес relay",
            "127.0.0.1:19132"
        ));
        content.addView(localAddress, margins(-1, -2, 0, 0, 0, dp(10)));

        Button start = primaryButton("Запустить relay и Minecraft");
        start.setOnClickListener(view -> startRelay(true));
        content.addView(start, margins(-1, dp(54), 0, 0, 0, dp(7)));
        Button relayOnly = secondaryButton("Запустить только relay");
        relayOnly.setOnClickListener(view -> startRelay(false));
        content.addView(relayOnly, margins(-1, dp(50), 0, 0, 0, dp(7)));
        Button stop = secondaryButton("Остановить relay");
        stop.setTextColor(0xffff7d89);
        stop.setOnClickListener(view -> stopRelay());
        content.addView(stop, margins(-1, dp(50), 0, 0, 0, dp(10)));

        authText = text("", 15, true);
        authText.setVisibility(View.GONE);
        content.addView(authText);
        authButton = primaryButton("Скопировать код и открыть Microsoft");
        authButton.setVisibility(View.GONE);
        authButton.setOnClickListener(view -> openAuthentication());
        content.addView(authButton, margins(-1, dp(52), 0, dp(5), 0, dp(8)));

        Button openMinecraft = secondaryButton("Открыть Minecraft");
        openMinecraft.setOnClickListener(view -> openMinecraft());
        content.addView(openMinecraft, margins(-1, dp(48), 0, 0, 0, dp(8)));
        TextView hint = text(
            "Разрешение «поверх других приложений» Android запрашивает один " +
                "раз. После входа в мир откроется аккуратное меню CPE; HUD-слои " +
                "можно включать независимо.",
            10,
            false
        );
        hint.setTextColor(0xff8291a3);
        content.addView(hint);

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(false);
        scroll.addView(content);
        LinearLayout wrapper = new LinearLayout(this);
        wrapper.setOrientation(LinearLayout.VERTICAL);
        wrapper.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1f));
        return wrapper;
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
            DiagnosticsLog.clear(this);
            refreshLogNow();
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
        logVisible = logs;
        if (connectionPage != null) {
            connectionPage.setVisibility(logs ? View.GONE : View.VISIBLE);
        }
        if (logsPage != null) {
            logsPage.setVisibility(logs ? View.VISIBLE : View.GONE);
        }
        if (connectionTab != null) {
            connectionTab.setBackground(tabBackground(!logs));
            connectionTab.setTextColor(!logs ? Color.WHITE : 0xff8291a3);
        }
        if (logsTab != null) {
            logsTab.setBackground(tabBackground(logs));
            logsTab.setTextColor(logs ? Color.WHITE : 0xff8291a3);
        }
        if (logs) refreshLogNow();
    }

    private void applyLoggingPreferenceToNative(boolean enabled) {
        try {
            NativeBridge.configureRuntime(
                enabled,
                preferences.getBoolean(RelayService.KEY_CHUNK_RETENTION, false),
                RelayService.clampRetainedRadius(preferences.getInt(
                    RelayService.KEY_RETAINED_RADIUS_CHUNKS,
                    24
                ))
            );
        } catch (Throwable ignored) {
        }
    }

    private void startRelay(boolean launchMinecraft) {
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
            startForegroundService(intent);
        } else {
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
                        ? "Заменить официальный FULL ZIP"
                        : "Импортировать официальный FULL ZIP"
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
        try {
            JSONObject state = new JSONObject(NativeBridge.snapshot());
            boolean running = state.optBoolean("running", false);
            boolean listening = state.optBoolean("listening", false);
            boolean pingOk = state.optBoolean("pingOk", false);
            boolean destinationPingDone = state.optBoolean(
                "destinationPingDone",
                false
            );
            boolean destinationPingOk = state.optBoolean(
                "destinationPingOk",
                false
            );
            int downstream = state.optInt("downstreamConnections", 0);
            boolean upstreamReady = state.optBoolean("upstreamReady", false);

            if (upstreamReady) {
                statusText.setText("Relay подключён к серверу");
            } else if (downstream > 0) {
                statusText.setText("Minecraft подключён, вход на сервер…");
            } else if (listening) {
                statusText.setText("Relay готов — открой Minecraft");
            } else if (running) {
                statusText.setText("Запуск relay…");
            } else {
                statusText.setText("Relay остановлен");
            }

            String host = preferences.getString(RelayService.KEY_HOST, "cpe.ign.gg");
            int port = preferences.getInt(RelayService.KEY_PORT, 19132);
            String version = preferences.getString(
                RelayService.KEY_VERSION,
                "1.21.100"
            );
            String error = preferences.getString(RelayService.KEY_LAST_ERROR, "");
            String details = "Версия " + version + "\n127.0.0.1:19132 → " +
                host + ":" + port;
            if (listening) {
                details += pingOk ? "\nRakNet pong: OK" : "\nUDP listener активен";
            }
            if (destinationPingDone && destinationPingOk) {
                String serverVersion = state.optString(
                    "destinationGameVersion",
                    ""
                );
                details += "\nСервер отвечает: " + serverVersion +
                    " (protocol " +
                    state.optInt("destinationProtocolVersion", -1) +
                    ", " + state.optLong("destinationLatencyMs", 0) + " ms)";
                if (!serverVersion.isEmpty() && !serverVersion.equals(version)) {
                    details += "\n⚠ Выбранная версия не совпадает с сервером";
                }
            } else if (destinationPingDone) {
                details += "\n⚠ Сервер не ответил на RakNet ping";
            }
            if (!error.isEmpty()) {
                details += "\nОшибка: " + error;
            }
            detailText.setText(details);

            if (listening && launchMinecraftWhenReady) {
                launchMinecraftWhenReady = false;
                openMinecraft();
            }
        } catch (Throwable error) {
            detailText.setText("Native relay недоступен: " + error.getMessage());
            DiagnosticsLog.appendError(
                this,
                "ui",
                "Failed to refresh native relay state",
                error
            );
        }

        if (logVisible && SystemClock.elapsedRealtime() >= nextLogRefreshAt) {
            refreshLogNow();
        }

        String code = preferences.getString(RelayService.KEY_AUTH_CODE, "");
        if (code.isEmpty()) {
            authText.setVisibility(View.GONE);
            authButton.setVisibility(View.GONE);
        } else {
            authText.setText("Код Xbox: " + code);
            authText.setVisibility(View.VISIBLE);
            authButton.setVisibility(View.VISIBLE);
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
        if (logText == null) return;
        logText.setText(DiagnosticsLog.readTail(this, 128 * 1024));
        nextLogRefreshAt = SystemClock.elapsedRealtime() + 2_000;
    }

    private void copyLog() {
        String value = DiagnosticsLog.readAll(this);
        ClipboardManager clipboard = (ClipboardManager)
            getSystemService(Context.CLIPBOARD_SERVICE);
        clipboard.setPrimaryClip(ClipData.newPlainText(
            "CPE Relay diagnostics",
            value
        ));
        DiagnosticsLog.append(
            this,
            "INFO",
            "ui",
            "Diagnostics log copied; characters=" + value.length()
        );
        toast("Подробный журнал скопирован");
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
        card.setPadding(dp(14), dp(12), dp(14), dp(12));
        card.setBackground(cardBackground());
        card.setElevation(dp(2));
        return card;
    }

    private TextView fieldLabel(String value) {
        TextView label = text(value, 11, true);
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
        Button button = new Button(this);
        button.setText(value);
        button.setTextSize(14);
        button.setTextColor(0xff061018);
        button.setAllCaps(false);
        GradientDrawable background = new GradientDrawable(
            GradientDrawable.Orientation.LEFT_RIGHT,
            new int[] {0xff57d7ff, 0xff71e6a4}
        );
        background.setCornerRadius(dp(13));
        button.setBackground(background);
        return button;
    }

    private Button secondaryButton(String value) {
        Button button = new Button(this);
        button.setText(value);
        button.setTextSize(13);
        button.setTextColor(0xffe8f2fc);
        button.setAllCaps(false);
        GradientDrawable background = new GradientDrawable();
        background.setColor(0xff1a2532);
        background.setCornerRadius(dp(12));
        background.setStroke(dp(1), 0xff3c5068);
        button.setBackground(background);
        return button;
    }

    private GradientDrawable cardBackground() {
        GradientDrawable background = new GradientDrawable();
        background.setColor(0xff131c27);
        background.setCornerRadius(dp(15));
        background.setStroke(dp(1), 0xff26384a);
        return background;
    }

    private GradientDrawable pillBackground() {
        GradientDrawable background = new GradientDrawable();
        background.setColor(0xff172532);
        background.setCornerRadius(dp(12));
        background.setStroke(dp(1), 0xff34506a);
        return background;
    }

    private GradientDrawable tabBackground(boolean selected) {
        GradientDrawable background = new GradientDrawable();
        background.setColor(selected ? 0xff26384b : Color.TRANSPARENT);
        background.setCornerRadius(dp(11));
        return background;
    }

    private TextView text(String value, int sp, boolean bold) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(sp);
        view.setTextColor(Color.WHITE);
        if (bold) {
            view.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        }
        return view;
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
