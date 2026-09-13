package com.m9chko.bedrockrelay;

import android.content.Context;
import android.view.Gravity;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.util.function.Consumer;

/** Compact navigation only: each tile opens one independently built module. */
final class OverlayModuleCatalog extends LinearLayout {
    static final String[] PAGES = {"map_streaming", "no_render", "minimap", "chunks",
        "outline", "equipment", "threats", "schematics", "platform", "zip",
        "craft", "deposit", "totem", "armor", "area_fill", "logs"};

    OverlayModuleCatalog(Context context, Consumer<String> open) {
        super(context);
        setOrientation(VERTICAL);
        group("МИР", new String[][] {
            {"map_streaming", "Загрузка карт", "4000 · автоматически"},
            {"no_render", "No Render", "Скрытие сущностей"},
            {"minimap", "Мини-карта", "Обзор поверхности"},
            {"chunks", "Чанки", "Удержание мира"}}, open);
        group("ОТОБРАЖЕНИЕ", new String[][] {
            {"outline", "Обводка", "Игроки и мобы"},
            {"equipment", "Снаряжение", "Броня и предметы"},
            {"threats", "Угрозы", "Оценка урона"},
            {"schematics", "Схемы / NBT", "Проекции и файлы"}}, open);
        group("ДЕЙСТВИЯ", new String[][] {
            {"platform", "Платформа", "Строительство"},
            {"zip", "Карты ZIP", "Крафт из .qznbt"},
            {"craft", "Автокрафт", "NBT → сундуки"},
            {"deposit", "Разгрузка", "Шалкеры → сундук"},
            {"totem", "Авто-тотем", "Левая рука"},
            {"armor", "Авто-броня", "Лучшая защита"},
            {"area_fill", "Заполнение", "Область по точкам"},
            {"logs", "Журнал", "Диагностика"}}, open);
    }

    private void group(String title, String[][] entries, Consumer<String> open) {
        TextView heading = RelayUi.text(getContext(), title, 10, true);
        heading.setTextColor(RelayUi.MUTED);
        heading.setLetterSpacing(.12f);
        heading.setPadding(dp(2), dp(8), 0, dp(6));
        addView(heading);
        for (int i = 0; i < entries.length; i += 2) {
            LinearLayout row = new LinearLayout(getContext());
            for (int j = i; j < Math.min(i + 2, entries.length); j++) {
                String[] entry = entries[j];
                LinearLayout tile = new LinearLayout(getContext());
                tile.setOrientation(VERTICAL);
                tile.setGravity(Gravity.CENTER_VERTICAL);
                tile.setMinimumHeight(dp(62));
                tile.setPadding(dp(10), dp(10), dp(8), dp(10));
                tile.setTag("overlay-module-" + entry[0]);
                tile.setBackground(RelayUi.action(getContext(), RelayUi.SURFACE, 12, RelayUi.BORDER));
                tile.addView(RelayUi.text(getContext(), entry[1], 13, true));
                TextView hint = RelayUi.text(getContext(), entry[2], 10, false);
                hint.setTextColor(RelayUi.MUTED);
                hint.setPadding(0, dp(4), 0, 0);
                tile.addView(hint);
                tile.setContentDescription(entry[1] + ". " + entry[2]);
                tile.setFocusable(true);
                tile.setOnClickListener(v -> open.accept(entry[0]));
                LayoutParams params = new LayoutParams(0, -1, 1);
                params.setMargins(0, 0, j == i ? dp(6) : 0, dp(6));
                row.addView(tile, params);
            }
            addView(row, new LayoutParams(-1, -2));
        }
    }

    private int dp(int value) { return RelayUi.dp(getContext(), value); }
}
