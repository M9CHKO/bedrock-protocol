using System.Reflection;
using System.Text.Json;

namespace CpeRelay.Windows;

internal static class AutoCraftUiTests
{
    private static T Field<T>(object obj, string name) =>
        (T)obj.GetType().GetField(name, BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(obj)!;
    internal static void Run(string output, Action<bool, string> require)
    {
        require(AppSettings.ClampCraftInterval(-1) == 100 && AppSettings.ClampCraftInterval(9999) == 5000 &&
            AppSettings.ClampWindowPause(-1) == 300 && AppSettings.ClampWindowPause(9999) == 3000, "Auto 2 UI timing bounds");
        var old = JsonSerializer.Deserialize<AppSettings>("{\"Host\":\"localhost\"}")!;
        require(old.Auto2 && old.CraftIntervalMs == 1000 && old.WindowPauseMs == 700, "Old settings receive Auto 2 defaults without autorun");
        var saved = JsonSerializer.Deserialize<AppSettings>(JsonSerializer.Serialize(new AppSettings {
            Auto2 = false, CraftIntervalMs = 350, WindowPauseMs = 1200 }))!;
        require(!saved.Auto2 && saved.CraftIntervalMs == 350 && saved.WindowPauseMs == 1200, "Auto 2 settings serialize independently of runtime state");
        using var main = new MainForm(preview: true);
        var flags = BindingFlags.NonPublic | BindingFlags.Instance;
        main.GetType().GetMethod("UpdateState", flags)!.Invoke(main, [JsonSerializer.SerializeToElement(new { running = false })]);
        require(!Field<Button>(main, "auto2Toggle").Enabled, "Missing snapshot/closed relay disables Auto 2 safely");
        Field<TrackBar>(main, "craftSpeed").Value = 98;
        Field<TrackBar>(main, "windowSpeed").Value = 27;
        main.GetType().GetMethod("ReadSettings", flags)!.Invoke(main, null);
        var settings = Field<AppSettings>(main, "settings");
        require(settings.CraftIntervalMs == 100 && settings.WindowPauseMs == 300, "Rightmost Windows sliders are fastest");

        using var floating = new FloatingDepositForm(autoCraft: true);
        floating.Location = new Point(-15000, -15000);
        floating.UpdateIndicators(JsonSerializer.SerializeToElement(new { running = false }), true);
        require(!Field<RelayButton>(floating, "button").Enabled, "Floating Auto 2 cannot start without Minecraft");
        JsonElement State(bool running, bool busy, string status) => JsonSerializer.SerializeToElement(new {
            running = true, upstreamReady = true,
            autoCraftStore = new { running, busy, status, crafted = 24, stored = 8, template = "End_Nested" }
        });
        var active = State(true, true, "Разгружаю шалкеры");
        floating.UpdateIndicators(active, true);
        require(Field<RelayButton>(floating, "button").Text.Contains("СТОП") && Field<RelayButton>(floating, "button").Selected &&
            Field<Label>(floating, "detail").Text.Contains("Крафт: 24"), "Floating Auto 2 displays native state and counters");
        floating.UpdateIndicators(State(false, true, "Жду закрытия окна Minecraft"), true);
        require(Field<RelayButton>(floating, "button").Text.Contains("ЗАВЕРШЕНИЕ"), "Closing stays busy instead of allowing another start");
        floating.SetCommandPending(true); floating.UpdateIndicators(active, true);
        require(!Field<RelayButton>(floating, "button").Enabled, "Pending toggle suppresses duplicate button commands");
        floating.SetCommandPending(false); floating.UpdateIndicators(State(false, false, "Материалы закончились — пополните ресурсы"), true);
        floating.UpdateVisibility(true, true);
        require(floating.Visible && floating.TopMost && Field<Label>(floating, "detail").Text.Contains("пополните ресурсы"),
            "Floating Auto 2 remains visible with resource-exhaustion reason");
        using var image = new Bitmap(floating.Width, floating.Height);
        floating.DrawToBitmap(image, new Rectangle(Point.Empty, floating.Size));
        image.Save(Path.Combine(output, "auto2-floating.png"));
        floating.UpdateVisibility(true, false);
        require(!floating.Visible, "Floating Auto 2 respects overlay setting");
    }
}
