using System.Reflection;
using System.Text.Json;
namespace CpeRelay.Windows;

internal static class ModuleUiTests
{
    internal static void Run(string output, Action<bool, string> require)
    {
        var old = JsonSerializer.Deserialize<AppSettings>("{\"HideMaps\":true,\"HideEntities\":true,\"FloatingButton\":true,\"Host\":\"test.example\",\"Deposit\":true}")!;
        old.UpgradeInterface();
        require(!old.HideMaps && !old.FloatingButton && old.HideEntities && old.Deposit && old.Host == "test.example",
            "Legacy UI migration enables automatic maps, hides overlays and preserves unrelated settings");
        old.FloatingButton = true; old.FloatingMaps = false;
        var loaded = JsonSerializer.Deserialize<AppSettings>(JsonSerializer.Serialize(old))!; loaded.UpgradeInterface();
        require(loaded.FloatingButton && !loaded.FloatingMaps && loaded.FloatingDeposit && loaded.FloatingAutoCraft && loaded.FloatingPlatform,
            "New overlay preferences survive reload independently");
        using var form = new MainForm(preview: true);
        require(form.PageCount == 6 && form.Modules.Count == 8, "Dedicated settings page and eight independent feature modules");
        var ids = new HashSet<string>();
        for (int i = 0; i < form.Modules.Count; i++) { form.Modules.SelectModule(i); ids.Add(form.Modules.SelectedId!); }
        require(ids.Count == 8 && ids.Contains("map-streaming") && ids.Contains("zip-maps"), "Server map streaming and ZIP crafting are separate modules");
        form.Modules.ShowCatalog(); require(form.Modules.SelectedId == null, "Module back navigation returns to catalog");
        const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
        AppSettings settings = (AppSettings)typeof(MainForm).GetField("settings", flags)!.GetValue(form)!;
        require(!settings.FloatingButton && !settings.HideMaps, "Default interface does not overlay Minecraft or pause map delivery");
        ((CheckBox)typeof(MainForm).GetField("hideEntities", flags)!.GetValue(form)!).Checked = true;
        ((CheckBox)typeof(MainForm).GetField("floatingPlatformEnabled", flags)!.GetValue(form)!).Checked = false;
        typeof(MainForm).GetMethod("ReadSettings", flags)!.Invoke(form, null);
        require(settings.HideEntities && !settings.HideMaps && !settings.FloatingPlatform, "No Render and construction overlay preferences remain independent");
        var platform = (PlatformPanel)typeof(MainForm).GetField("platformPanel", flags)!.GetValue(form)!;
        var platformButtons = platform.Controls.OfType<FlowLayoutPanel>().SelectMany(row => row.Controls.OfType<Button>()).Select(button => button.Text).ToArray();
        require(!platformButtons.Contains("Настройки") && platformButtons.Contains("Старт") && platformButtons.Contains("Записать сундук"),
            "Construction module removes the redundant Settings command");
        using var floatingPlatform = new FloatingPlatformForm { Location = new Point(-15000, -15000) };
        floatingPlatform.UpdateIndicators(JsonSerializer.SerializeToElement(new { running = true, status = "Строю и иду вперёд", placed = 24, row = 3, stage = "build", recording = false }), true);
        floatingPlatform.UpdateVisibility(true, true);
        var detail = (Label)typeof(FloatingPlatformForm).GetField("detail", flags)!.GetValue(floatingPlatform)!;
        require(floatingPlatform.Visible && floatingPlatform.TopMost && detail.Text.Contains("24") && detail.Text.Contains("Ряд: 3"),
            "Construction has a compact topmost controller with live progress");
        using (var bitmap = new Bitmap(floatingPlatform.Width, floatingPlatform.Height))
        {
            floatingPlatform.DrawToBitmap(bitmap, new Rectangle(Point.Empty, floatingPlatform.Size));
            bitmap.Save(Path.Combine(output, "platform-floating.png"));
        }
        floatingPlatform.UpdateVisibility(true, false);
        require(!floatingPlatform.Visible, "Construction floating controller can be hidden independently");
        using var maps = new MapStreamingModule();
        maps.UpdateConnection(true, true);
        maps.Observe("map_scheduler intercepted=321 sent=320 pending=1 requestsPending=3679");
        var text = (Label)typeof(MapStreamingModule).GetField("progress", flags)!.GetValue(maps)!;
        require(text.Text.Contains("320") && text.Text.Contains("3679"), "Map module shows scheduler progress without payload data");
        maps.UpdateConnection(false, false);
        require(text.Text.Contains("Сеанс завершён"), "Map progress does not claim a completed render after disconnect");
    }
}
