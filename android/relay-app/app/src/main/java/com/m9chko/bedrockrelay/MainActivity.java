package com.m9chko.bedrockrelay;

import android.Manifest;
import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.provider.Settings;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.ArrayAdapter;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;

public final class MainActivity extends Activity {
    private static final int NOTIFICATION_PERMISSION_REQUEST = 100;
    private static final String MINECRAFT_PACKAGE = "com.mojang.minecraftpe";

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable refreshTask = new Runnable() {
        @Override public void run() {
            refreshState();
            handler.postDelayed(this, 500);
        }
    };

    private SharedPreferences preferences;
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
    private long nextLogRefreshAt;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        preferences = getSharedPreferences(RelayService.PREFERENCES, MODE_PRIVATE);
        DiagnosticsLog.append(this, "INFO", "ui", "Main screen opened");
        setContentView(buildContent());
    }

    @Override
    protected void onResume() {
        super.onResume();
        handler.removeCallbacks(refreshTask);
        handler.post(refreshTask);
    }

    @Override
    protected void onPause() {
        handler.removeCallbacks(refreshTask);
        super.onPause();
    }

    private View buildContent() {
        int padding = dp(20);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(padding, dp(18), padding, dp(28));

        TextView title = text("CPE Relay", 30, true);
        root.addView(title);
        TextView subtitle = text(
            "Автономный Minecraft Bedrock relay без Termux",
            15,
            false
        );
        subtitle.setAlpha(0.75f);
        root.addView(subtitle, margins(-1, -2, 0, 4, 0, 22));

        root.addView(text("Сервер назначения", 14, true));
        hostInput = new EditText(this);
        hostInput.setSingleLine(true);
        hostInput.setHint("cpe.ign.gg");
        hostInput.setText(preferences.getString(RelayService.KEY_HOST, "cpe.ign.gg"));
        root.addView(hostInput, margins(-1, -2, 0, 2, 0, 12));

        root.addView(text("UDP-порт назначения", 14, true));
        portInput = new EditText(this);
        portInput.setSingleLine(true);
        portInput.setInputType(InputType.TYPE_CLASS_NUMBER);
        portInput.setText(String.valueOf(
            preferences.getInt(RelayService.KEY_PORT, 19132)
        ));
        root.addView(portInput, margins(-1, -2, 0, 2, 0, 18));

        root.addView(text("Версия Minecraft Bedrock", 14, true));
        versionInput = new Spinner(this);
        configureVersions();
        root.addView(versionInput, margins(-1, dp(52), 0, 2, 0, 18));

        TextView versionHint = text(
            "Выбери точную версию клиента и сервера. Relay не переводит " +
                "пакеты между разными версиями.",
            12,
            false
        );
        versionHint.setAlpha(0.65f);
        root.addView(versionHint, margins(-1, -2, 0, -10, 0, 16));

        TextView localAddress = text(
            "Minecraft подключается к 127.0.0.1:19132",
            14,
            true
        );
        localAddress.setOnClickListener(view -> copyText(
            "Адрес relay",
            "127.0.0.1:19132"
        ));
        root.addView(localAddress, margins(-1, -2, 0, 0, 0, 18));

        Button start = new Button(this);
        start.setText("Запустить relay и Minecraft");
        start.setAllCaps(false);
        start.setOnClickListener(view -> startRelay(true));
        root.addView(start, margins(-1, dp(54), 0, 0, 0, 8));

        Button relayOnly = new Button(this);
        relayOnly.setText("Запустить только relay");
        relayOnly.setAllCaps(false);
        relayOnly.setOnClickListener(view -> startRelay(false));
        root.addView(relayOnly, margins(-1, dp(50), 0, 0, 0, 8));

        Button stop = new Button(this);
        stop.setText("Остановить relay");
        stop.setAllCaps(false);
        stop.setOnClickListener(view -> stopRelay());
        root.addView(stop, margins(-1, dp(50), 0, 0, 0, 22));

        statusText = text("Relay остановлен", 20, true);
        root.addView(statusText);
        detailText = text("Локальный порт: 19132", 14, false);
        detailText.setAlpha(0.8f);
        root.addView(detailText, margins(-1, -2, 0, 4, 0, 20));

        authText = text("", 16, true);
        authText.setVisibility(View.GONE);
        root.addView(authText);
        authButton = new Button(this);
        authButton.setText("Скопировать код и открыть вход Microsoft");
        authButton.setAllCaps(false);
        authButton.setVisibility(View.GONE);
        authButton.setOnClickListener(view -> openAuthentication());
        root.addView(authButton, margins(-1, dp(52), 0, 6, 0, 14));

        Button openMinecraft = new Button(this);
        openMinecraft.setText("Открыть Minecraft");
        openMinecraft.setAllCaps(false);
        openMinecraft.setOnClickListener(view -> openMinecraft());
        root.addView(openMinecraft, margins(-1, dp(50), 0, 8, 0, 8));

        TextView hint = text(
            "При первом входе код Xbox появится здесь и в уведомлении. " +
                "После авторизации токены сохраняются только во внутреннем " +
                "хранилище приложения.",
            13,
            false
        );
        hint.setAlpha(0.7f);
        root.addView(hint);

        Button showLog = new Button(this);
        showLog.setText("Показать подробный журнал");
        showLog.setAllCaps(false);
        showLog.setOnClickListener(view -> {
            logVisible = !logVisible;
            logText.setVisibility(logVisible ? View.VISIBLE : View.GONE);
            showLog.setText(logVisible
                ? "Скрыть подробный журнал"
                : "Показать подробный журнал");
            if (logVisible) refreshLogNow();
        });
        root.addView(showLog, margins(-1, dp(50), 0, 18, 0, 8));

        Button copyLog = new Button(this);
        copyLog.setText("Копировать журнал");
        copyLog.setAllCaps(false);
        copyLog.setOnClickListener(view -> copyLog());
        root.addView(copyLog, margins(-1, dp(48), 0, 0, 0, 8));

        Button clearLog = new Button(this);
        clearLog.setText("Очистить журнал");
        clearLog.setAllCaps(false);
        clearLog.setOnClickListener(view -> {
            DiagnosticsLog.clear(this);
            DiagnosticsLog.append(this, "INFO", "ui", "Diagnostics log cleared");
            refreshLogNow();
            toast("Журнал очищен");
        });
        root.addView(clearLog, margins(-1, dp(48), 0, 0, 0, 8));

        TextView logPath = text(
            "Файл журнала: " + DiagnosticsLog.path(this),
            11,
            false
        );
        logPath.setAlpha(0.6f);
        logPath.setTextIsSelectable(true);
        root.addView(logPath, margins(-1, -2, 0, 0, 0, 6));

        logText = text("", 11, false);
        logText.setTypeface(Typeface.MONOSPACE);
        logText.setTextIsSelectable(true);
        logText.setVisibility(View.GONE);
        root.addView(logText, margins(-1, -2, 0, 0, 0, 18));

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        return scroll;
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

    private TextView text(String value, int sp, boolean bold) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(sp);
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
        params.setMargins(dp(left), dp(top), dp(right), dp(bottom));
        return params;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private void toast(String message) {
        Toast.makeText(this, message, Toast.LENGTH_LONG).show();
    }
}
