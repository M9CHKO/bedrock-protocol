using System.Text.Json;

namespace CpeRelay.Windows;

internal sealed class PlatformPanel : FlowLayoutPanel
{
    private readonly RelayBackend backend;
    private readonly AppSettings settings;
    private readonly Label state = new() { AutoSize = true, MaximumSize = new Size(730, 0), ForeColor = Theme.Text };
    private readonly ListBox chests = new() { Width = 730, Height = 120 };
    private readonly Dictionary<string, NumericUpDown> fields = [];
    private bool pending;
    private string lastChests = "";
    internal PlatformPanel(RelayBackend backend, AppSettings settings)
    {
        this.backend = backend; this.settings = settings;
        Width = 750; AutoSize = true; FlowDirection = FlowDirection.TopDown; WrapContents = false;
        AddText("Кварц + светокамень · сундуки и верстаки с одной стороны.\nМеню реле не мешает; камера смотрит по ходу движения. Полосы = 1 — прямая.");
        AddNumber("chunks", "Прямая, чанков", 16, 1, 1024);
        AddNumber("lanes", "Полосы змейки", 1, 1, 32);
        AddNumber("gapChunks", "Боковой переход, чанков", 2, 1, 16);
        AddNumber("turnSide", "Поворот: -1 влево / 1 вправо", -1, -1, 1);
        AddNumber("side", "Сундуки: 1 справа / -1 слева", 1, -1, 1);
        AddNumber("placeMs", "Установка, мс", 50, 50, 2000);
        AddNumber("walkSpeed", "Ходьба, блоков/тик", .12m, .04m, .20m, 2);
        AddNumber("travelSpeed", "За ресурсами / обратно", .30m, .06m, .60m, 2);
        AddNumber("refillMs", "Пауза переноса, мс", 350, 150, 2000);
        AddNumber("refillStacks", "Запас материала, стаков", 3, 1, 6);
        var row = new FlowLayoutPanel { AutoSize = true, Width = 730 };
        foreach (var (title, op) in new[] { ("Старт", "start"), ("Продолжить", "resume"), ("Стоп", "stop"), ("Настройки", "configure"), ("Записать сундук", "record") })
        {
            var button = new Button { Text = title, AutoSize = true, ForeColor = Theme.Text, BackColor = Theme.Sidebar };
            button.Click += async (_, _) => await Command(op); row.Controls.Add(button);
        }
        Controls.Add(row); Controls.Add(state); Controls.Add(chests);
        Controls.SetChildIndex(row,1);
        var remove = new Button { Text = "Удалить выбранную запись", AutoSize = true, ForeColor = Theme.Text, BackColor = Theme.Sidebar };
        remove.Click += async (_, _) => { if (chests.SelectedIndex >= 0) await Command("remove", chests.SelectedIndex + 1); };
        Controls.Add(remove);
    }
    private void AddText(string text) => Controls.Add(new Label { Text = text, AutoSize = true, MaximumSize = new Size(730, 0), ForeColor = Theme.Text });
    private void AddNumber(string key, string label, decimal fallback, decimal min, decimal max, int decimals = 0)
    {
        var row = new FlowLayoutPanel { AutoSize = true, Width = 730 };
        row.Controls.Add(new Label { Text = label, Width = 420, ForeColor = Theme.Text, Padding = new Padding(0, 5, 0, 0) });
        var input = new NumericUpDown { Minimum = min, Maximum = max, DecimalPlaces = decimals, Increment = decimals == 0 ? 1 : .01m, Width = 120 };
        input.Value = Math.Clamp(settings.Platform.GetValueOrDefault(key, fallback), min, max);
        fields[key] = input; row.Controls.Add(input); Controls.Add(row);
    }
    internal async Task Configure()
    {
        foreach (var (key, field) in fields) settings.Platform[key] = field.Value;
        var request = settings.Platform.ToDictionary(x => x.Key, x => (object)x.Value);
        request["action"] = "platform"; request["op"] = "configure";
        await backend.Call(request);
    }
    private async Task Command(string op, int index = 0)
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
        state.Text = value.Text("status", "Подключитесь к Minecraft") + " · Блоков: " + Number(value,"placed");
        if (value.ValueKind != JsonValueKind.Object || !value.TryGetProperty("chests", out var list) || list.GetRawText() == lastChests) return;
        lastChests = list.GetRawText(); int selected = chests.SelectedIndex;
        chests.Items.Clear(); int index = 0;
        foreach (var chest in list.EnumerateArray()) chests.Items.Add($"{++index}: {Number(chest,"x")}, {Number(chest,"y")}, {Number(chest,"z")}");
        if (chests.Items.Count > 0) chests.SelectedIndex = Math.Clamp(selected, 0, chests.Items.Count - 1);
    }
    private static long Number(JsonElement value,string name) => value.ValueKind==JsonValueKind.Object &&
        value.TryGetProperty(name,out var n) && n.TryGetInt64(out var number)?number:0;
}
