package com.m9chko.bedrockrelay;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.TextView;

import org.json.JSONObject;

import java.util.Collections;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

/** Draggable compact warning HUD backed only by decoded Bedrock packets. */
final class ThreatAnalysisOverlayController {
    private final Context context;
    private final SharedPreferences preferences;
    private final WindowManager windowManager;
    private final EntityOutlineOverlayController outlineController;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final ThreatAnalyzer analyzer = new ThreatAnalyzer();
    private final AtomicReference<ThreatAnalyzer.Result> pendingResult =
        new AtomicReference<>();
    private final AtomicBoolean deliveryPosted = new AtomicBoolean(false);

    private volatile ThreatAnalyzer.DefenseState defense =
        ThreatAnalyzer.DefenseState.unknown();
    private volatile boolean sessionVisible;
    private volatile boolean uiBlocked;
    private volatile boolean enabled = true;
    private volatile boolean warningEnabled = true;
    private volatile int triggerDistance = 12;
    private volatile int warningScale = 90;
    private volatile int threatColor = 0xffff3b30;
    private ThreatAnalyzer.Result latest = ThreatAnalyzer.Result.none();
    private LinearLayout window;
    private TextView title;
    private TextView summary;
    private TextView damage;
    private TextView defenseText;
    private WindowManager.LayoutParams params;
    private String displayedFingerprint = "";

    ThreatAnalysisOverlayController(
        Context context,
        SharedPreferences preferences,
        EntityOutlineOverlayController outlineController
    ) {
        this.context = context;
        this.preferences = preferences;
        this.outlineController = outlineController;
        this.windowManager = (WindowManager) context.getSystemService(
            Context.WINDOW_SERVICE
        );
    }

    void configure(
        boolean analysisEnabled,
        boolean showWarning,
        int distance,
        int scale,
        int color
    ) {
        boolean recreate = warningScale != RelayService.clampOverlayScale(scale);
        enabled = analysisEnabled;
        warningEnabled = showWarning;
        triggerDistance = RelayService.clampThreatDistance(distance);
        warningScale = RelayService.clampOverlayScale(scale);
        threatColor = color | 0xff000000;
        if (recreate) removeWindow();
        if (!enabled) {
            analyzer.reset();
            latest = ThreatAnalyzer.Result.none();
            outlineController.setThreatHighlights(
                Collections.emptySet(),
                threatColor
            );
        } else {
            outlineController.setThreatHighlights(
                latest.dangerousEntityIds,
                threatColor
            );
        }
        reconcile();
    }

    void setSessionVisible(boolean visible) {
        if (sessionVisible == visible) return;
        sessionVisible = visible;
        if (!visible) {
            analyzer.reset();
            latest = ThreatAnalyzer.Result.none();
            outlineController.setThreatHighlights(
                Collections.emptySet(),
                threatColor
            );
        }
        reconcile();
    }

    void setUiBlocked(boolean blocked) {
        if (uiBlocked == blocked) return;
        uiBlocked = blocked;
        reconcile();
    }

    boolean wantsFrames() {
        return sessionVisible && enabled && !uiBlocked;
    }

    void updatePlayerState(JSONObject state) {
        if (state != null) defense = ThreatAnalyzer.DefenseState.from(state);
    }

    void offerFrame(EntityOutlineOverlayController.Frame frame) {
        if (!wantsFrames() || frame == null) return;
        ThreatAnalyzer.Result result = analyzer.analyze(
            frame,
            defense,
            triggerDistance
        );
        outlineController.setThreatHighlights(
            result.dangerousEntityIds,
            threatColor
        );
        pendingResult.set(result);
        postDelivery();
    }

    void destroy() {
        sessionVisible = false;
        analyzer.reset();
        outlineController.setThreatHighlights(
            Collections.emptySet(),
            threatColor
        );
        removeWindow();
    }

    private void postDelivery() {
        if (!deliveryPosted.compareAndSet(false, true)) return;
        mainHandler.post(() -> {
            ThreatAnalyzer.Result delivered = pendingResult.getAndSet(null);
            if (delivered != null) latest = delivered;
            updateCard();
            reconcile();
            deliveryPosted.set(false);
            if (pendingResult.get() != null) postDelivery();
        });
    }

    private void reconcile() {
        boolean shouldShow = sessionVisible && enabled && warningEnabled &&
            !uiBlocked && latest.primary != null;
        if (shouldShow) addWindow();
        else removeWindow();
    }

    private void addWindow() {
        if (window != null || !Settings.canDrawOverlays(context)) return;
        float scale = warningScale / 100f;
        LinearLayout card = new LinearLayout(context);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(px(12, scale), px(9, scale), px(12, scale), px(9, scale));
        card.setBackground(cardBackground());
        card.setElevation(px(10, scale));

        title = text("⚠  ОЖИДАЕТСЯ АТАКА", 11 * scale, true);
        title.setTextColor(0xffffd0cc);
        summary = text("", 11 * scale, true);
        summary.setPadding(0, px(3, scale), 0, 0);
        damage = text("", 10 * scale, true);
        damage.setTextColor(0xffffcf75);
        damage.setPadding(0, px(3, scale), 0, 0);
        defenseText = text("", 9 * scale, false);
        defenseText.setTextColor(0xffb6c3d2);
        defenseText.setPadding(0, px(3, scale), 0, 0);
        card.addView(title);
        card.addView(summary);
        card.addView(damage);
        card.addView(defenseText);

        WindowManager.LayoutParams addedParams = new WindowManager.LayoutParams(
            px(276, scale),
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE |
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL |
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSLUCENT
        );
        addedParams.gravity = Gravity.TOP | Gravity.START;
        int screenWidth = context.getResources().getDisplayMetrics().widthPixels;
        addedParams.x = preferences.contains(RelayService.KEY_THREAT_WARNING_X)
            ? preferences.getInt(RelayService.KEY_THREAT_WARNING_X, 0)
            : Math.max(0, (screenWidth - addedParams.width) / 2);
        addedParams.y = preferences.getInt(
            RelayService.KEY_THREAT_WARNING_Y,
            px(86, 1f)
        );
        int screenHeight = context.getResources().getDisplayMetrics().heightPixels;
        addedParams.x = Math.max(0, Math.min(
            Math.max(0, screenWidth - addedParams.width),
            addedParams.x
        ));
        addedParams.y = Math.max(0, Math.min(
            Math.max(0, screenHeight - px(110, scale)),
            addedParams.y
        ));
        attachDrag(card, addedParams);
        try {
            windowManager.addView(card, addedParams);
            window = card;
            params = addedParams;
            displayedFingerprint = "";
            updateCard();
        } catch (Throwable error) {
            DiagnosticsLog.appendError(
                context,
                "threats",
                "Failed to show threat warning HUD",
                error
            );
        }
    }

    private void updateCard() {
        ThreatAnalyzer.Threat threat = latest.primary;
        if (window == null || threat == null) return;
        String fingerprint = threat.fingerprint();
        if (fingerprint.equals(displayedFingerprint)) return;
        displayedFingerprint = fingerprint;
        title.setText(threat.imminent
            ? "⚠  АТАКА ОЖИДАЕТСЯ СЕЙЧАС"
            : "⚠  ВОЗМОЖНА АТАКА");
        String motion = threat.closingSpeed > 1.2
            ? " • быстро сближается"
            : threat.closingSpeed > 0.28
                ? " • приближается"
                : "";
        String confidence = threat.conditionalAggression
            ? " • агрессия не подтверждена"
            : "";
        summary.setText(String.format(
            Locale.getDefault(),
            "%s • %.1f м%s%s",
            threat.mobName,
            threat.distance,
            motion,
            confidence
        ));
        damage.setText(String.format(
            Locale.getDefault(),
            "Оценка урона: %.1f–%.1f ед. (%.1f–%.1f ❤)",
            threat.damageMinimum,
            threat.damageMaximum,
            threat.damageMinimum / 2.0,
            threat.damageMaximum / 2.0
        ));
        ThreatAnalyzer.DefenseState state = threat.defense;
        String health = state.healthKnown
            ? String.format(
                Locale.getDefault(),
                "%.1f/%.1f HP",
                state.health,
                state.maximumHealth
            )
            : "HP неизвестно";
        String hunger = state.hungerKnown
            ? String.format(Locale.getDefault(), " • еда %.0f/20", state.hunger)
            : "";
        String absorption = state.absorptionKnown && state.absorption > 0
            ? String.format(
                Locale.getDefault(),
                " • поглощение %.1f",
                state.absorption
            )
            : "";
        String resistance = state.resistanceLevel > 0
            ? " • сопротивление " + roman(state.resistanceLevel)
            : "";
        String enchantment = state.enchantedArmorPieces > 0
            ? " • чар. броня " + state.enchantedArmorPieces + "/4"
            : "";
        defenseText.setText(
            health + hunger + absorption + " • броня " +
                state.armorPoints + "/20" + resistance + enchantment +
                "\nПакетная оценка; сложность и цель моба могут быть неизвестны."
        );
    }

    private void attachDrag(
        View target,
        WindowManager.LayoutParams targetParams
    ) {
        int slop = ViewConfiguration.get(context).getScaledTouchSlop();
        target.setOnTouchListener(new View.OnTouchListener() {
            float downRawX;
            float downRawY;
            int downX;
            int downY;
            boolean moved;

            @Override public boolean onTouch(View view, MotionEvent event) {
                if (window != target || params != targetParams) return false;
                switch (event.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        downRawX = event.getRawX();
                        downRawY = event.getRawY();
                        downX = targetParams.x;
                        downY = targetParams.y;
                        moved = false;
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        float dx = event.getRawX() - downRawX;
                        float dy = event.getRawY() - downRawY;
                        moved |= Math.abs(dx) > slop || Math.abs(dy) > slop;
                        if (!moved) return true;
                        int screenWidth = context.getResources()
                            .getDisplayMetrics().widthPixels;
                        int screenHeight = context.getResources()
                            .getDisplayMetrics().heightPixels;
                        targetParams.x = Math.max(0, Math.min(
                            Math.max(0, screenWidth - target.getWidth()),
                            downX + Math.round(dx)
                        ));
                        targetParams.y = Math.max(0, Math.min(
                            Math.max(0, screenHeight - target.getHeight()),
                            downY + Math.round(dy)
                        ));
                        try {
                            windowManager.updateViewLayout(target, targetParams);
                        } catch (Throwable ignored) {
                        }
                        return true;
                    case MotionEvent.ACTION_UP:
                    case MotionEvent.ACTION_CANCEL:
                        if (moved) {
                            preferences.edit()
                                .putInt(
                                    RelayService.KEY_THREAT_WARNING_X,
                                    targetParams.x
                                )
                                .putInt(
                                    RelayService.KEY_THREAT_WARNING_Y,
                                    targetParams.y
                                )
                                .apply();
                        }
                        return true;
                    default:
                        return false;
                }
            }
        });
    }

    private void removeWindow() {
        LinearLayout current = window;
        window = null;
        params = null;
        title = null;
        summary = null;
        damage = null;
        defenseText = null;
        displayedFingerprint = "";
        if (current == null) return;
        try {
            windowManager.removeViewImmediate(current);
        } catch (Throwable ignored) {
        }
    }

    private TextView text(String value, float sizeSp, boolean bold) {
        TextView result = new TextView(context);
        result.setText(value);
        result.setTextSize(sizeSp);
        result.setTextColor(Color.WHITE);
        if (bold) {
            result.setTypeface(
                result.getTypeface(),
                android.graphics.Typeface.BOLD
            );
        }
        return result;
    }

    private GradientDrawable cardBackground() {
        GradientDrawable background = new GradientDrawable();
        background.setColor(0xee17151b);
        background.setCornerRadius(px(14, 1f));
        background.setStroke(px(1.5f, 1f), threatColor);
        return background;
    }

    private int px(float value, float scale) {
        return Math.max(1, Math.round(
            value * scale * context.getResources().getDisplayMetrics().density
        ));
    }

    private static String roman(int level) {
        switch (level) {
            case 1: return "I";
            case 2: return "II";
            case 3: return "III";
            case 4: return "IV";
            default: return Integer.toString(level);
        }
    }
}
