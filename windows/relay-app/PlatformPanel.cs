using System.Text.Json;

namespace CpeRelay.Windows;

internal sealed class PlatformPanel : FlowLayoutPanel
{
    private readonly RelayBackend backend;
    private readonly AppSettings settings;
    private readonly Label state = new() { AutoSize = true, MaximumSize = new Size(730, 0), ForeColor = Theme.Text };
    private readonly ListBox chests = new() { Width = 730, Height = 120 };
    private readonly Dictionary<string, NumericUpDown> fields = [];
    private readonly ToolTip hints = new() { AutoPopDelay = 12000, InitialDelay = 350, ReshowDelay = 100 };
    private bool pending;
    internal bool IsRunning { get; private set; }
    private string lastChests = "";
    internal PlatformPanel(RelayBackend backend, AppSettings settings)
    {
        this.backend = backend; this.settings = settings;
        Width = 750; AutoSize = true; FlowDirection = FlowDirection.TopDown; WrapContents = false;
        AddText("Кварц + светокамень · сундуки и верстаки с одной стороны.\nПолосы = 1 — прямая. Изменения ниже применяются при старте/продолжении.");
        AddNumber("chunks", "Прямая, чанков", 16, 1, 1024);
        AddNumber("lanes", "Полосы змейки", 1, 1, 32);
        AddNumber("gapChunks", "Боковой переход, чанков", 2, 1, 16);
        AddNumber("turnSide", "Поворот: -1 влево / 1 вправо", -1, -1, 1);
        AddNumber("side", "Сундуки: 1 справа / -1 слева", 1, -1, 1);
        AddNumber("placeMs", "Установка, мс", 50, 50, 2000);
        AddNumber("walkSpeed", "Ходьба, блоков/тик", .12m, .04m, .20m, 2);
        AddNumber("travelSpeed", "За ресурсами / обратно", .30m, .06m, .60m, 2);
        AddNumber("refillMs", "Пауза переноса, мс", 350, 150, 2000);
        AddNumber("refillStacks", "Запас одного материала, стаков", 3, 1, 6);
        var row = new FlowLayoutPanel { AutoSize = true, Width = 730 };
        foreach (var (title, op) in new[] { ("Старт", "start"), ("Продолжить", "resume"), ("Стоп", "stop"), ("Записать сундук", "record") })
        {
            var button = new RelayButton { Text = title, AutoSize = true, Height = 40, MinimumSize = new Size(100,40),
                Padding = new Padding(12,5,12,5), Margin = new Padding(0,3,8,6), Primary = op == "start" };
            button.Click += async (_, _) => await Command(op); row.Controls.Add(button);
        }
        Controls.Add(row); Controls.Add(state); Controls.Add(chests);
        Controls.SetChildIndex(row,1);
        var remove = new RelayButton { Text = "Удалить выбранную запись", AutoSize = true, Height = 40,
            MinimumSize = new Size(220,40), Padding = new Padding(12,5,12,5), Margin = new Padding(0,6,0,6) };
        remove.Click += async (_, _) => { if (chests.SelectedIndex >= 0) await Command("remove", chests.SelectedIndex + 1); };
        Controls.Add(remove);
    }
    private void AddText(string text) => Controls.Add(new Label { Text = text, AutoSize = true, MaximumSize = new Size(730, 0), ForeColor = Theme.Text });
    private void AddNumber(string key, string label, decimal fallback, decimal min, decimal max, int decimals = 0)
    {
        var row = new FlowLayoutPanel { AutoSize = true, Width = 730, Margin = new Padding(0,4,0,4) };
        row.Controls.Add(new Label { Text = label, Width = 420, ForeColor = Theme.Muted, Padding = new Padding(0, 5, 0, 0) });
        var input = new NumericUpDown { Minimum = min, Maximum = max, DecimalPlaces = decimals, Increment = decimals == 0 ? 1 : .01m, Width = 120 };
        var saved = settings.Platform.GetValueOrDefault(key, fallback);
        input.Value = Math.Clamp((key is "side" or "turnSide") && saved == 0 ? fallback : saved, min, max);
        if (key is "side" or "turnSide")
        {
            decimal previous = input.Value;
            input.ValueChanged += (_, _) =>
            {
                if (input.Value == 0) input.Value = previous > 0 ? -1 : 1;
                previous = input.Value;
            };
        }
        string explanation = key switch
        {
            "chunks" => "Длина каждой прямой полосы: 1 чанк = 16 блоков.",
            "lanes" => "Количество параллельных полос. После каждой полосы маршрут разворачивается змейкой.",
            "gapChunks" => "Расстояние между соседними полосами в чанках; при одной полосе не используется.",
            "turnSide" => "Сторона перехода к следующей полосе: −1 влево, +1 вправо относительно первого направления.",
            "side" => "Сторона сундуков и верстаков относительно направления движения: −1 слева, +1 справа.",
            "placeMs" => "Минимальная пауза после отправки установки блока; подтверждение UpdateBlock сервера обязательно.",
            "walkSpeed" => "Шаг строительства в блоках на пакет игрового ввода (тик). Максимум 0,20.",
            "travelSpeed" => "Шаг по пути к записанному сундуку снабжения и обратно в блоках на тик.",
            "refillMs" => "Пауза между переносами предметов из сундука; серверные подтверждения обязательны.",
            "refillStacks" => "Целевой запас запрашиваемого материала: количество полных стаков по 64 предмета.",
            _ => label
        };
        hints.SetToolTip(input, explanation);
        hints.SetToolTip(row.Controls[0], explanation);
        fields[key] = input; row.Controls.Add(input); Controls.Add(row);
    }
    internal void CaptureSettings()
    {
        foreach (var (key, field) in fields) settings.Platform[key] = field.Value;
    }
    internal async Task Configure()
    {
        CaptureSettings();
        var request = settings.Platform.ToDictionary(x => x.Key, x => (object)x.Value);
        request["action"] = "platform"; request["op"] = "configure";
        await backend.Call(request);
    }
    internal async Task Command(string op, int index = 0)
    {
        if (pending) return; pending = true;
        try
        {
            if (op != "stop") { await Configure(); settings.Save(); }
            Update(await backend.Call(new { action = "platform", op, index }));
        }
        catch (Exception e) { if (!IsDisposed) state.Text = e.Message; }
        finally { pending = false; }
    }
    internal void Update(JsonElement value)
    {
        if (IsDisposed) return;
        IsRunning = value.Flag("running");
        state.Text = value.Text("status", "Подключитесь к Minecraft") + " · Блоков: " + Number(value,"placed") + WorldDiagnostics(value);
        if (value.ValueKind != JsonValueKind.Object || !value.TryGetProperty("chests", out var list) || list.GetRawText() == lastChests) return;
        lastChests = list.GetRawText(); int selected = chests.SelectedIndex;
        chests.Items.Clear(); int index = 0;
        foreach (var chest in list.EnumerateArray()) chests.Items.Add($"{++index}: {Number(chest,"x")}, {Number(chest,"y")}, {Number(chest,"z")}");
        if (chests.Items.Count > 0) chests.SelectedIndex = Math.Clamp(selected, 0, chests.Items.Count - 1);
    }
    private static long Number(JsonElement value,string name) => value.ValueKind==JsonValueKind.Object &&
        value.TryGetProperty(name,out var n) && n.TryGetInt64(out var number)?number:0;
    private static string WorldDiagnostics(JsonElement value)
    {
        if (value.ValueKind != JsonValueKind.Object || !value.TryGetProperty("world", out var world) ||
            world.ValueKind != JsonValueKind.Object) return "";
        return $"\nМир: колонок {Number(world,"columns")}, декодировано {Number(world,"decoded")}, ошибок {Number(world,"failures")}, кэш-пропусков {Number(world,"cachedSkipped")}; опора: {world.Text("footing","?")}, место: {world.Text("clearance","?")}, высота: {world.Text("validHeight","?")}";
    }
    protected override void Dispose(bool disposing)
    {
        if (disposing) hints.Dispose();
        base.Dispose(disposing);
    }
}
