using System.Reflection;
using System.Text.Json;
namespace CpeRelay.Windows;

internal static class ModuleUiTests
{
    private sealed class TestModule() : RelayModule("test", "Тест", "Проверка переключателя") { }
    internal static void Run(string output, Action<bool, string> require)
    {
        var old = JsonSerializer.Deserialize<AppSettings>("{\"InterfaceVersion\":2,\"HideMaps\":true,\"HideEntities\":true,\"FloatingButton\":true,\"FloatingDeposit\":true,\"FloatingAutoCraft\":true,\"FloatingMaps\":true,\"FloatingPlatform\":true,\"Auto2\":true,\"Host\":\"test.example\",\"Deposit\":true}")!;
        old.UpgradeInterface();
        require(old.InterfaceVersion == 4 && !old.HideMaps && !old.FloatingButton && !old.FloatingDeposit &&
            !old.FloatingAutoCraft && !old.FloatingMaps && !old.FloatingPlatform && !old.HideEntities &&
            !old.Deposit && !old.Auto2 && !old.PlatformEnabled && !old.ZipMapsEnabled && old.Host == "test.example",
            "Existing profiles turn off modules and topmost panels without losing server settings");
        old.FloatingButton = true; old.FloatingMaps = true;
        var loaded = JsonSerializer.Deserialize<AppSettings>(JsonSerializer.Serialize(old))!; loaded.UpgradeInterface();
        require(loaded.FloatingButton && loaded.FloatingMaps && !loaded.FloatingDeposit && !loaded.FloatingAutoCraft && !loaded.FloatingPlatform,
            "Explicit overlay choices made after migration survive reload");
        var previous = JsonSerializer.Deserialize<AppSettings>("{\"InterfaceVersion\":3,\"FloatingPlatform\":true,\"FloatingMaps\":true}")!;
        previous.UpgradeInterface();
        require(previous.InterfaceVersion == 4 && previous.PlatformEnabled && previous.ZipMapsEnabled,
            "Existing construction and ZIP switches migrate without losing their enabled state");
        using var form = new MainForm(preview: true);
        require(form.PageCount == 6 && form.Modules.Count == 8, "Dedicated settings page and eight independent feature modules");
        var ids = new HashSet<string>();
        for (int i = 0; i < form.Modules.Count; i++) { form.Modules.SelectModule(i); ids.Add(form.Modules.SelectedId!); }
        require(ids.Count == 8 && ids.Contains("map-streaming") && ids.Contains("zip-maps"), "Server map streaming and ZIP crafting are separate modules");
        form.Modules.ShowCatalog(); require(form.Modules.SelectedId == null, "Module back navigation returns to catalog");
        const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
        AppSettings settings = (AppSettings)typeof(MainForm).GetField("settings", flags)!.GetValue(form)!;
        require(!settings.FloatingButton && !settings.MapStreaming && !settings.Auto2 && !settings.Deposit &&
            !settings.Armor && !settings.Totem && !settings.HideEntities,
            "New profiles leave all modules off until explicitly enabled");
        ((CheckBox)typeof(MainForm).GetField("hideEntities", flags)!.GetValue(form)!).Checked = true;
        ((CheckBox)typeof(MainForm).GetField("floatingPlatformEnabled", flags)!.GetValue(form)!).Checked = false;
        typeof(MainForm).GetMethod("ReadSettings", flags)!.Invoke(form, null);
        require(settings.HideEntities && !settings.HideMaps && !settings.FloatingPlatform, "No Render and construction overlay preferences remain independent");
        var platform = (PlatformPanel)typeof(MainForm).GetField("platformPanel", flags)!.GetValue(form)!;
        var fields = (Dictionary<string, NumericUpDown>)typeof(PlatformPanel).GetField("fields", flags)!.GetValue(platform)!;
        require(fields.Keys.ToHashSet().SetEquals(["chunks", "lanes", "gapChunks", "turnSide", "side", "placeMs",
                "walkSpeed", "travelSpeed", "refillMs", "refillStacks"]) &&
            fields["chunks"].Value == 16 && fields["lanes"].Value == 1 && fields["walkSpeed"].Value == .12m &&
            fields["refillStacks"].Value == 3,
            "All ten construction settings have matching native keys and defaults");
        fields["side"].Value = -1; fields["side"].UpButton();
        require(fields["side"].Value == 1, "Construction side selector skips invalid zero");
        fields["turnSide"].Value = 1; fields["turnSide"].DownButton();
        require(fields["turnSide"].Value == -1, "Construction turn selector skips invalid zero");
        var tiles = (Dictionary<string, ModuleTile>)typeof(ModuleCatalogPanel).GetField("switches", flags)!.GetValue(form.Modules)!;
        require(tiles.Count == 8, "Every module has a catalog power control");
        var power = tiles["platform"].Controls.OfType<ModuleSwitchControl>().Single();
        require(!power.Checked && power.Text == "Выкл" && power.AccessibleRole == AccessibleRole.CheckButton && power.Enabled,
            "Construction can be enabled before connecting to Minecraft");
        typeof(ModuleSwitchControl).GetMethod("OnClick", flags)!.Invoke(power, [EventArgs.Empty]);
        require(power.Checked && ((CheckBox)typeof(MainForm).GetField("floatingPlatformEnabled", flags)!.GetValue(form)!).Checked &&
            ((CheckBox)typeof(MainForm).GetField("floatingEnabled", flags)!.GetValue(form)!).Checked &&
            form.Modules.SelectedId == null && !platform.IsRunning,
            "Construction switch enables its floating controller without starting the bot or opening detail");
        typeof(MainForm).GetMethod("SaveConfiguration", flags)!.Invoke(form, null);
        require(settings.PlatformEnabled && settings.FloatingPlatform && settings.FloatingButton && settings.Platform.ContainsKey("chunks") &&
            settings.MapTiming.ContainsKey("craft"), "Top Save captures module, construction and ZIP settings");
        var saved = JsonSerializer.Deserialize<AppSettings>(JsonSerializer.Serialize(settings))!; saved.UpgradeInterface();
        require(saved.PlatformEnabled && saved.FloatingPlatform && saved.FloatingButton && saved.Platform.ContainsKey("chunks"),
            "Enabled modules and construction values survive settings serialization");
        var noRenderPower = tiles["no-render"].Controls.OfType<ModuleSwitchControl>().Single();
        require(noRenderPower.Enabled && noRenderPower.Checked && noRenderPower.Text == "Вкл", "Detail module state appears immediately in catalog");
        ((CheckBox)typeof(MainForm).GetField("hideEntities", flags)!.GetValue(form)!).Checked = false;
        form.Modules.RefreshStates();
        require(!noRenderPower.Checked && noRenderPower.Text == "Выкл" && noRenderPower.AccessibleDescription == "Выключено",
            "Catalog switch follows changes made in module detail");
        bool testActive = false;
        using (var isolated = new ModuleCatalogPanel([new TestModule()], new Dictionary<string, ModuleCatalogPanel.Switch>
            { ["test"] = new(() => testActive, on => { testActive = on; return Task.CompletedTask; }) }))
        {
            using var testHost = new Form { StartPosition = FormStartPosition.Manual, Location = new Point(-15000, -15000), Size = new Size(900, 600) };
            isolated.Dock = DockStyle.Fill; testHost.Controls.Add(isolated); testHost.Show(); Application.DoEvents();
            var isolatedTile = isolated.Controls.OfType<Panel>().SelectMany(p => p.Controls.OfType<FlowLayoutPanel>())
                .SelectMany(p => p.Controls.OfType<ModuleTile>()).Single();
            var switchControl = isolatedTile.Controls.OfType<ModuleSwitchControl>().Single();
            typeof(ModuleSwitchControl).GetMethod("OnClick", flags)!.Invoke(switchControl, [EventArgs.Empty]);
            require(testActive && switchControl.Checked && isolated.SelectedId == null,
                "Activating a switch changes the module without opening its detail page");
            if (SystemInformation.IsMenuAnimationEnabled)
            {
                Thread.Sleep(90);
                float midpoint = (float)typeof(ModuleSwitchControl).GetMethod("CurrentProgress", flags)!.Invoke(switchControl, null)!;
                require(midpoint > 0f && midpoint < 1f, "Switch thumb interpolates between states instead of jumping");
            }
            require(!isolatedTile.Controls.OfType<Button>().Any(), "Catalog cards have no redundant Open button");
            typeof(ModuleTile).GetMethod("OnClick", flags)!.Invoke(isolatedTile, [EventArgs.Empty]);
            require(isolated.SelectedId == "test", "The card body opens module detail separately from its switch");
            testHost.Hide();
        }
        var overlayOption = (CheckBox)typeof(MainForm).GetField("floatingDepositEnabled", flags)!.GetValue(form)!;
        typeof(MainForm).GetMethod("EnableModuleOverlay", flags)!.Invoke(form, [overlayOption]);
        require(overlayOption.Checked && ((CheckBox)typeof(MainForm).GetField("floatingEnabled", flags)!.GetValue(form)!).Checked,
            "Enabling a module also enables its topmost controller and the master overlay setting");
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
        using var maps = new MapStreamingModule(new ToggleCheckBox());
        maps.UpdateConnection(true, true);
        maps.Observe("map_scheduler intercepted=321 sent=320 pending=1 requestsPending=3679");
        var text = (Label)typeof(MapStreamingModule).GetField("progress", flags)!.GetValue(maps)!;
        require(text.Text.Contains("320") && text.Text.Contains("3679"), "Map module shows scheduler progress without payload data");
        maps.UpdateConnection(false, false);
        require(text.Text.Contains("Сеанс завершён"), "Map progress does not claim a completed render after disconnect");
    }
}
