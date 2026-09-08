package com.m9chko.bedrockrelay;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.widget.*;
import org.json.*;
import java.util.LinkedHashMap;
import java.util.concurrent.Executors;
import java.util.concurrent.ExecutorService;

/** Same native module in the launcher and CPE overlay; no JNI on the UI thread. */
final class PlatformSettingsControls extends LinearLayout {
    private static final ExecutorService WORKER=Executors.newSingleThreadExecutor();
    private static final String KEY="platform_builder_settings";
    private final SharedPreferences preferences;
    private final Handler handler=new Handler(Looper.getMainLooper());
    private final LinkedHashMap<String,EditText> fields=new LinkedHashMap<>();
    private final TextView state;
    private final Spinner chests;
    private final Runnable refresh=new Runnable(){public void run(){if(attached){call("snapshot",0);handler.postDelayed(this,1500);}}};
    private String lastChests="";
    private boolean pending,attached;
    PlatformSettingsControls(Context context,SharedPreferences preferences) {
        super(context);this.preferences=preferences;setOrientation(VERTICAL);
        setPadding(0,RelayUi.dp(context,12),0,RelayUi.dp(context,12));
        addView(RelayUi.text(context,"PlatformBuilder · строительство",17,true));
        addView(RelayUi.text(context,"Кварц, светокамень, сундуки, верстаки. Направление фиксируется при Старт. " +
            "Меню CPE не мешает работе; камера поворачивается при ходьбе. Полосы = 1: прямо, больше: змейка.",12,false));
        field("chunks","Длина прямой / чанков",16);
        field("lanes","Полосы змейки (1–32)",1);
        field("gapChunks","Боковой переход, чанков (1–16)",2);
        field("turnSide","Поворот: -1 влево, 1 вправо",-1);
        field("side","Сундуки: 1 справа, -1 слева",1);
        field("placeMs","Установка, мс (50–2000)",50);
        field("walkSpeed","Ходьба (0.04–0.20)",.12);
        field("travelSpeed","За ресурсами / обратно (0.06–0.60)",.30);
        field("refillMs","Пауза переноса, мс (150–2000)",350);
        field("refillStacks","Запас материала, стаков (1–6)",3);
        state=RelayUi.text(context,"Запустите реле и войдите в Minecraft",12,false);addView(state);
        buttons(new String[]{"Старт","Продолжить","Стоп"},new String[]{"start","resume","stop"});
        buttons(new String[]{"Сохранить настройки","Записать сундук"},new String[]{"configure","record"});
        chests=new Spinner(context);addView(chests,new LayoutParams(-1,-2));
        Button remove=RelayUi.button(context,"Удалить выбранный сундук из списка",false);
        remove.setOnClickListener(v->call("remove",chests.getSelectedItemPosition()+1));addView(remove);
    }
    private void field(String key,String title,double fallback) {
        addView(RelayUi.text(getContext(),title,12,false));
        EditText input=new EditText(getContext());input.setSingleLine(true);
        input.setInputType(InputType.TYPE_CLASS_NUMBER|InputType.TYPE_NUMBER_FLAG_DECIMAL|InputType.TYPE_NUMBER_FLAG_SIGNED);
        double value=fallback;
        try{value=new JSONObject(preferences.getString(KEY,"{}")).optDouble(key,fallback);}catch(JSONException ignored){}
        input.setText(value==Math.rint(value)?Integer.toString((int)value):Double.toString(value));
        fields.put(key,input);addView(input,new LayoutParams(-1,RelayUi.dp(getContext(),48)));
    }
    private void buttons(String[] labels,String[] ops) {
        LinearLayout row=new LinearLayout(getContext());
        for(int i=0;i<labels.length;i++){final String op=ops[i];Button b=RelayUi.button(getContext(),labels[i],i==0);
            b.setOnClickListener(v->call(op,0));row.addView(b,new LayoutParams(0,-2,1));}
        addView(row);
    }
    static void apply(SharedPreferences preferences) {
        try{JSONObject j=new JSONObject(preferences.getString(KEY,"{}"));j.put("op","configure");NativeBridge.platformCommand(j.toString());}
        catch(Exception ignored){}
    }
    private void call(String op,int index) {
        if(pending)return;
        final JSONObject config=new JSONObject(),request=new JSONObject();
        try {
            request.put("op",op);request.put("index",index);
            if(!op.equals("snapshot") && !op.equals("stop")) {
                for(String key:fields.keySet())config.put(key,Double.parseDouble(fields.get(key).getText().toString()));
                config.put("op","configure");preferences.edit().putString(KEY,config.toString()).apply();
            }
        }catch(Exception e){state.setText("Проверьте числовые настройки");return;}
        pending=true;
        WORKER.execute(()->{
            String result;
            try{if(config.length()>0)NativeBridge.platformCommand(config.toString());result=NativeBridge.platformCommand(request.toString());}
            catch(Exception e){result="{}";}
            final String reply=result;
            handler.post(()->{pending=false;if(!attached)return;
                try{JSONObject j=new JSONObject(reply);state.setText(j.optString("status","Нет подключения")+" · Блоков: "+j.optInt("placed"));
                    JSONArray a=j.optJSONArray("chests");if(a!=null && !lastChests.equals(a.toString())){lastChests=a.toString();String[] rows=new String[a.length()];
                        for(int i=0;i<a.length();i++){JSONObject p=a.getJSONObject(i);rows[i]=(i+1)+": "+p.getInt("x")+", "+p.getInt("y")+", "+p.getInt("z");}
                        int selected=chests.getSelectedItemPosition();
                        chests.setAdapter(new ArrayAdapter<>(getContext(),android.R.layout.simple_spinner_dropdown_item,rows));
                        if(rows.length>0)chests.setSelection(Math.max(0,Math.min(selected,rows.length-1)));
                    }
                }catch(JSONException e){state.setText("Не удалось получить состояние строителя");}
            });
        });
    }
    @Override protected void onAttachedToWindow(){super.onAttachedToWindow();attached=true;handler.post(refresh);}
    @Override protected void onDetachedFromWindow(){attached=false;handler.removeCallbacks(refresh);super.onDetachedFromWindow();}
}
