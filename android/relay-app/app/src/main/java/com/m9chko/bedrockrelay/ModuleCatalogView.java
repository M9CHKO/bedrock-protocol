package com.m9chko.bedrockrelay;

import android.content.Context;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.util.LinkedHashMap;

/** Each feature owns a separate screen; only the selected screen is attached. */
final class ModuleCatalogView extends LinearLayout {
    private final LinearLayout catalog;
    private final ScrollView scroll;
    private final LinkedHashMap<String, LinearLayout> screens = new LinkedHashMap<>();
    private String selected;

    ModuleCatalogView(Context context) {
        super(context);
        setOrientation(VERTICAL);
        catalog = column();
        TextView title = RelayUi.text(context, "Инструменты мира", 25, true);
        catalog.addView(title);
        TextView intro = RelayUi.text(context, "Каждая функция — в своём модуле. Выбери, что настроить.", 13, false);
        intro.setTextColor(RelayUi.MUTED);
        catalog.addView(intro, margin(6, 18));
        scroll = new ScrollView(context);
        scroll.setVerticalScrollBarEnabled(false);
        scroll.setFillViewport(true);
        scroll.addView(catalog);
        addView(scroll, new LayoutParams(-1, -1));
    }

    void addModule(String id, String title, String description, View content) {
        if (screens.containsKey(id)) throw new IllegalArgumentException("Duplicate module: " + id);
        LinearLayout screen = column();
        Button back = RelayUi.button(getContext(), "‹  Все модули", false);
        back.setTag("modules-back");
        back.setOnClickListener(v -> showCatalog());
        screen.addView(back, margin(0, 18));
        screen.addView(RelayUi.text(getContext(), title, 25, true));
        TextView subtitle = RelayUi.text(getContext(), description, 13, false);
        subtitle.setTextColor(RelayUi.MUTED);
        screen.addView(subtitle, margin(6, 20));
        screen.addView(content, new LayoutParams(-1, -2));
        screens.put(id, screen);

        LinearLayout tile = new LinearLayout(getContext());
        tile.setTag("module-" + id);
        tile.setGravity(Gravity.CENTER_VERTICAL);
        tile.setPadding(dp(16), dp(18), dp(16), dp(18));
        tile.setMinimumHeight(dp(94));
        tile.setBackground(RelayUi.action(getContext(), RelayUi.SURFACE, 20, RelayUi.BORDER));
        TextView badge = RelayUi.text(getContext(), String.format(java.util.Locale.ROOT, "%02d", screens.size()), 14, true);
        badge.setTextColor(RelayUi.ACCENT);
        tile.addView(badge, new LayoutParams(dp(38), -2));
        LinearLayout labels = column();
        labels.addView(RelayUi.text(getContext(), title, 17, true));
        TextView detail = RelayUi.text(getContext(), description, 12, false);
        detail.setTextColor(RelayUi.MUTED);
        labels.addView(detail, margin(4, 0));
        tile.addView(labels, new LayoutParams(0, -2, 1));
        TextView arrow = RelayUi.text(getContext(), "›", 25, false);
        arrow.setTextColor(RelayUi.ACCENT);
        arrow.setGravity(Gravity.RIGHT);
        tile.addView(arrow, new LayoutParams(dp(24), -2));
        tile.setContentDescription(title + ". " + description);
        tile.setFocusable(true);
        tile.setOnClickListener(v -> openModule(id));
        catalog.addView(tile, margin(0, 10));
    }

    void openModule(String id) {
        LinearLayout screen = screens.get(id);
        if (screen == null) throw new IllegalArgumentException("Unknown module: " + id);
        selected = id;
        scroll.removeAllViews();
        scroll.addView(screen);
        scroll.scrollTo(0, 0);
    }

    void showCatalog() {
        selected = null;
        scroll.removeAllViews();
        scroll.addView(catalog);
        scroll.scrollTo(0, 0);
    }

    String selectedModule() { return selected; }
    int moduleCount() { return screens.size(); }
    private LinearLayout column() { LinearLayout result = new LinearLayout(getContext()); result.setOrientation(VERTICAL); result.setPadding(0, 0, 0, dp(12)); return result; }
    private int dp(int value) { return RelayUi.dp(getContext(), value); }
    private LayoutParams margin(int top, int bottom) { LayoutParams result = new LayoutParams(-1, -2); result.setMargins(0, dp(top), 0, dp(bottom)); return result; }
}
