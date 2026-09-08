package com.m9chko.bedrockrelay;

import android.content.*;
import android.os.*;
import android.widget.*;
import android.text.InputType;
import org.json.*;
import java.io.*;
import java.util.*;
import java.util.concurrent.*;

/** Separate ZIP library; native work and copies never run on the UI thread. */
final class MapQueueControls extends LinearLayout {
    static final String ARCHIVE="map_queue_archive",TIMING="map_queue_timing";
    static final ExecutorService WORKER=Executors.newSingleThreadExecutor();
    private final SharedPreferences preferences;
    private final LinkedHashMap<String,EditText> fields=new LinkedHashMap<>();
    private final TextView status;
    private boolean pending,attached;
    private final Handler handler=new Handler(Looper.getMainLooper());
    private final Runnable refresh=new Runnable(){public void run(){if(attached){command("snapshot");handler.postDelayed(this,1500);}}};
    MapQueueControls(Context context,SharedPreferences preferences){super(context);this.preferences=preferences;setOrientation(VERTICAL);
        addView(RelayUi.text(context,"Карты из ZIP · отдельный модуль",17,true));
        addView(RelayUi.text(context,"ZIP с .qznbt загружается отдельно. Один шалкер за цикл. Одна карта в ячейке, непосредственно внутри шалкера. Освободите слот хотбара и уберите остальные шалкеры. Поиск — 6 блоков, клик — 4,25. После перезапуска или импорта очередь начинается сначала.",12,false));
        Button zip=RelayUi.button(context,"Загрузить отдельный ZIP с картами",true);
        zip.setOnClickListener(v->{Intent intent=new Intent(context,MainActivity.class).setAction(MainActivity.ACTION_IMPORT_MAP_ZIP).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK|Intent.FLAG_ACTIVITY_SINGLE_TOP);context.startActivity(intent);});addView(zip);
        status=RelayUi.text(context,"Сначала загрузите ZIP",12,false);addView(status);
        for(String op:new String[]{"toggle","stop","configure"}){Button b=RelayUi.button(context,op.equals("toggle")?"Старт / стоп карт":op.equals("stop")?"Остановить":"Сохранить темп",false);b.setOnClickListener(v->command(op));addView(b);}
        String[] keys={"craft","open","close","place","transfer","hold","break","pickup","store","next","timeout"};
        String[] titles={"Крафт","Открытие окна","После закрытия","После установки","Перенос карты (от 500)","Карта в правой руке (от 300)","Разрушение (от 3000)","Подбор","В сундук","Между файлами","Таймаут (10000–120000)"};
        int[] defaults={1000,700,700,500,600,1500,3500,1000,700,1000,30000};JSONObject saved=new JSONObject();try{saved=new JSONObject(preferences.getString(TIMING,"{}"));}catch(Exception ignored){}
        for(int i=0;i<keys.length;i++){addView(RelayUi.text(context,titles[i]+", мс",12,false));EditText f=new EditText(context);f.setInputType(InputType.TYPE_CLASS_NUMBER);f.setSingleLine(true);f.setText(Integer.toString(clamp(keys[i],saved.optInt(keys[i],defaults[i]))));fields.put(keys[i],f);addView(f);}
    }
    static int clamp(String key,int value){int min=key.equals("break")?3000:key.equals("timeout")?10000:key.equals("transfer")?500:key.equals("hold")?300:100;
        int max=key.equals("timeout")?120000:key.equals("break")?15000:10000;return Math.max(min,Math.min(max,value));}
    static void apply(SharedPreferences preferences,boolean load){try{
        JSONObject config=new JSONObject(preferences.getString(TIMING,"{}"));config.put("op","configure");NativeBridge.mapQueueCommand(config.toString());
        String path=preferences.getString(ARCHIVE,"");if(load&&!path.isEmpty())NativeBridge.mapQueueCommand(new JSONObject().put("op","load").put("path",path).toString());
    }catch(Exception ignored){}}
    static void importZip(Context context,SharedPreferences preferences,android.net.Uri source){
        Context app=context.getApplicationContext();WORKER.execute(()->{String message;
            try{
                if(new JSONObject(NativeBridge.mapQueueCommand("{\"op\":\"snapshot\"}")).optBoolean("busy"))throw new IOException("Остановите карты перед заменой ZIP");
                File directory=new File(app.getFilesDir(),"MapQueue");if(!directory.isDirectory()&&!directory.mkdirs())throw new IOException("Нет папки карт");
                File file=new File(directory,UUID.randomUUID()+".zip");
                try(InputStream in=app.getContentResolver().openInputStream(source);OutputStream out=new FileOutputStream(file)){
                    if(in==null)throw new IOException("Не удалось открыть ZIP");byte[] buffer=new byte[65536];long count=0;int n;
                    while((n=in.read(buffer))!=-1){count+=n;if(count>2L*1024*1024*1024)throw new IOException("ZIP больше 2 ГиБ");out.write(buffer,0,n);}
                }catch(Exception e){file.delete();throw e;}
                preferences.edit().putString(ARCHIVE,file.getAbsolutePath()).apply();
                JSONObject reply=new JSONObject(NativeBridge.mapQueueCommand(new JSONObject().put("op","load").put("path",file.getAbsolutePath()).toString()));
                message=reply.has("error")?"ZIP сохранён. "+reply.optString("error"):reply.optString("status","ZIP загружен");
            }catch(Exception e){message="ZIP: "+e.getMessage();}
            final String text=message;new Handler(Looper.getMainLooper()).post(()->Toast.makeText(app,text,Toast.LENGTH_LONG).show());
        });
    }
    private void command(String op){if(pending)return;JSONObject config=new JSONObject();
        try{if(!op.equals("snapshot")&&!op.equals("stop")){for(String key:fields.keySet()){int value=clamp(key,Integer.parseInt(fields.get(key).getText().toString()));config.put(key,value);fields.get(key).setText(Integer.toString(value));}config.put("op","configure");preferences.edit().putString(TIMING,config.toString()).apply();}}
        catch(Exception e){status.setText("Проверьте задержки в миллисекундах");return;}
        pending=true;WORKER.execute(()->{String response;try{if(config.length()>0)NativeBridge.mapQueueCommand(config.toString());response=NativeBridge.mapQueueCommand(new JSONObject().put("op",op).toString());}catch(Exception e){response="{}";}
            final String reply=response;handler.post(()->{pending=false;if(!attached)return;try{JSONObject q=new JSONObject(reply);status.setText(q.optString("error",q.optString("status","Нет реле"))+"\n"+q.optString("file")+" · "+q.optInt("completed")+" / "+q.optInt("total")+" · Карт: "+q.optInt("maps"));}catch(Exception ignored){}});
        });}
    @Override protected void onAttachedToWindow(){super.onAttachedToWindow();attached=true;handler.post(refresh);}
    @Override protected void onDetachedFromWindow(){attached=false;handler.removeCallbacks(refresh);super.onDetachedFromWindow();}
}
