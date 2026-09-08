using System.Text.Json;
namespace CpeRelay.Windows;

internal sealed class MapQueuePanel : FlowLayoutPanel
{
    private readonly RelayBackend backend;
    private readonly AppSettings settings;
    private readonly Label status = new() { AutoSize=true, MaximumSize=new Size(730,0), ForeColor=Theme.Text };
    private readonly Dictionary<string,NumericUpDown> fields=[];
    private bool pending;
    private bool busy;
    internal Action<string>? CommandFailed;
    internal bool Loaded {get;private set;}
    internal MapQueuePanel(RelayBackend backend,AppSettings settings)
    {
        this.backend=backend;this.settings=settings;AutoSize=true;Width=750;WrapContents=false;FlowDirection=FlowDirection.TopDown;
        Controls.Add(new Label { Text="Отдельная очередь .qznbt из ZIP: крафт → показ каждой карты → сундук.\nНужен пустой слот хотбара. Уберите остальные шалкеры. Поиск — 6 блоков; клик — до 4,25.\nПерезапуск или повторный импорт начинает список сначала.",AutoSize=true,MaximumSize=new Size(730,0),ForeColor=Theme.Text });
        var row=new FlowLayoutPanel{AutoSize=true,Width=730};
        foreach(var (label,op) in new[]{("Загрузить ZIP…","zip"),("Старт / стоп","toggle"),("Остановить","stop"),("Сохранить темп","configure")}){
            var b=new Button{Text=label,AutoSize=true,ForeColor=Theme.Text,BackColor=Theme.Sidebar};b.Click+=async(_,_)=>{if(op=="zip")await Import();else await Command(op);};row.Controls.Add(b);}
        Controls.Add(row);Controls.Add(status);
        foreach(var (key,label,value) in new[]{("craft","Крафт",1000),("open","Открытие окна",700),("close","После закрытия",700),("place","После установки",500),
            ("transfer","Перенос карты",600),("hold","Карта в правой руке",1500),("break","Разрушение шалкера",3500),("pickup","Ожидание подбора",1000),
            ("store","Складывание в сундук",700),("next","Между файлами",1000),("timeout","Таймаут ответа сервера",30000)}){
            var r=new FlowLayoutPanel{AutoSize=true,Width=730};r.Controls.Add(new Label{Text=label+", мс",Width=420,ForeColor=Theme.Text});
            int min=key=="break"?3000:key=="timeout"?10000:key=="transfer"?500:key=="hold"?300:100;
            int max=key=="timeout"?120000:key=="break"?15000:10000;
            var f=new NumericUpDown{Minimum=min,Maximum=max,Increment=100,Width=120,Value=Math.Clamp(settings.MapTiming.GetValueOrDefault(key,value),min,max)};
            fields[key]=f;r.Controls.Add(f);Controls.Add(r);}
    }
    private async Task Import(){
        if(pending)return;
        if(busy){status.Text="Остановите карты перед заменой ZIP";return;}
        using var picker=new OpenFileDialog{Filter="ZIP с картами (*.zip)|*.zip",Title="Отдельная библиотека карт"};if(picker.ShowDialog()!=DialogResult.OK)return;
        pending=true;
        try{
            if(new FileInfo(picker.FileName).Length>2L*1024*1024*1024)throw new IOException("ZIP больше 2 ГиБ");
            string directory=Path.Combine(AppSettings.DirectoryPath,"MapQueue");Directory.CreateDirectory(directory);
            string path=Path.Combine(directory,Guid.NewGuid().ToString("N")+".zip");
            await Task.Run(()=>File.Copy(picker.FileName,path,false));
            settings.MapArchive=path;settings.Save();status.Text="ZIP сохранён отдельно. Запустите реле и нажмите Старт.";Loaded=true;
            try{Update(await backend.Call(new{action="maps",op="load",path}));}catch(Exception e){status.Text="ZIP сохранён. "+e.Message;}
        }catch(Exception e){status.Text=e.Message;}finally{pending=false;}
    }
    internal async Task Configure(bool load=false){
        foreach(var (key,f) in fields)settings.MapTiming[key]=(int)f.Value;
        var command=settings.MapTiming.ToDictionary(p=>p.Key,p=>(object)p.Value);command["action"]="maps";command["op"]="configure";
        await backend.Call(command);
        if(load&&!string.IsNullOrEmpty(settings.MapArchive))Update(await backend.Call(new{action="maps",op="load",path=settings.MapArchive}));
    }
    internal async Task Command(string op){if(pending)return;pending=true;
        try{if(op!="stop"){await Configure();settings.Save();}Update(await backend.Call(new{action="maps",op}));}
        catch(Exception e){if(!IsDisposed){status.Text=e.Message;CommandFailed?.Invoke(e.Message);}}finally{pending=false;}}
    internal void Update(JsonElement value){if(IsDisposed||value.ValueKind!=JsonValueKind.Object)return;Loaded=value.Flag("loaded");busy=value.Flag("busy");
        status.Text=value.Text("status")+"\n"+value.Text("file")+" · Завершено: "+value.GetProperty("completed")+" / "+value.GetProperty("total")+" · Карт: "+value.GetProperty("maps");}
}
