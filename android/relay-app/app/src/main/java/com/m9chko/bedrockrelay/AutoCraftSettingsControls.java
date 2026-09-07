package com.m9chko.bedrockrelay;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;

/** Same controls in the launcher and the in-game CPE menu. No native work on the UI thread. */
final class AutoCraftSettingsControls extends LinearLayout {
    private final SharedPreferences preferences;
    private final Runnable changed;
    private final SeekBar[] sliders = new SeekBar[AutoCraftSettings.ALL.length];
    private final TextView[] labels = new TextView[AutoCraftSettings.ALL.length];

    AutoCraftSettingsControls(Context context, SharedPreferences preferences, Runnable changed) {
        super(context);
        this.preferences=preferences; this.changed=changed;
        setOrientation(VERTICAL);
        addView(RelayUi.text(context, "Настройки крафта · Авто 2", 15, true));
        for (int i=0; i<AutoCraftSettings.ALL.length; ++i) {
            final AutoCraftSettings.Spec spec=AutoCraftSettings.ALL[i];
            TextView label=RelayUi.text(context, "", 12, false);
            label.setPadding(0, RelayUi.dp(context,12), 0, 0);
            labels[i]=label; addView(label);
            SeekBar slider=new SeekBar(context);
            sliders[i]=slider;
            slider.setTag(spec.key);
            slider.setMax(spec.maxProgress());
            slider.setMinimumHeight(RelayUi.dp(context,48));
            slider.setProgressTintList(ColorStateList.valueOf(RelayUi.ACCENT));
            slider.setThumbTintList(ColorStateList.valueOf(RelayUi.ACCENT));
            slider.setContentDescription(spec.title + ": вправо быстрее");
            slider.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
                private boolean dragging;
                @Override public void onProgressChanged(SeekBar bar, int progress, boolean fromUser) {
                    label.setText(spec.label(spec.interval(progress)));
                    if (fromUser && !dragging) save(spec,spec.interval(progress));
                }
                @Override public void onStartTrackingTouch(SeekBar bar) { dragging=true; }
                @Override public void onStopTrackingTouch(SeekBar bar) {
                    dragging=false; save(spec,spec.interval(bar.getProgress()));
                }
            });
            addView(slider, new LayoutParams(-1,RelayUi.dp(context,48)));
        }
        TextView note=RelayUi.text(context,
            "Крафт: 100–5000 мс. Переходы: 300–3000 мс после подтверждения закрытия и перед первой операцией. " +
            "Для тяжёлых NBT начните с 1000 мс на крафт. Скорость разгрузки настраивается отдельно. " +
            "Изменения сохраняются после отпускания ползунка. Ожидание клиента/сервера не отключается.",12,false);
        note.setTextColor(RelayUi.MUTED); addView(note);
        Button reset=RelayUi.button(context,"Сбросить паузы крафта",false);
        reset.setOnClickListener(view -> {
            SharedPreferences.Editor edit=preferences.edit();
            for (AutoCraftSettings.Spec spec:AutoCraftSettings.ALL) edit.putInt(spec.key,spec.defaultMs);
            edit.apply(); refresh(); changed.run();
        });
        LayoutParams resetParams=new LayoutParams(-1,-2);
        resetParams.topMargin=RelayUi.dp(context,8);
        addView(reset,resetParams);
        refresh();
    }

    void refresh() {
        for (int i=0; i<sliders.length; ++i) {
            AutoCraftSettings.Spec spec=AutoCraftSettings.ALL[i];
            int value=spec.clamp(preferences.getInt(spec.key,spec.defaultMs));
            sliders[i].setProgress(spec.progress(value));
            labels[i].setText(spec.label(value));
        }
    }
    private void save(AutoCraftSettings.Spec spec, int value) {
        if (preferences.getInt(spec.key,spec.defaultMs)==value) return;
        preferences.edit().putInt(spec.key,value).apply(); changed.run();
    }
}
