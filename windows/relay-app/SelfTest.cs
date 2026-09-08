using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text.Json;

namespace CpeRelay.Windows;

// Explicit local checks only. No Microsoft login, remote server or user settings.
internal static class SelfTest
{
    internal static int Run(string[] args)
    {
        string output = args.Length > 1 ? Path.GetFullPath(args[1]) : Path.Combine(Path.GetTempPath(), "cpe-test-" + Guid.NewGuid());
        Directory.CreateDirectory(output);
        var checks = new List<string>();
        void Require(bool value, string name) { if (!value) throw new InvalidOperationException(name); checks.Add(name); }
        try
        {
            Require(File.Exists(Path.Combine(AppContext.BaseDirectory, "cpe_relay_windows.dll")) &&
                File.Exists(Path.Combine(AppContext.BaseDirectory, "README.txt")) &&
                File.Exists(Path.Combine(AppContext.BaseDirectory, "minecraft-data", "1.21.2", "blockStates.json")) &&
                File.Exists(Path.Combine(AppContext.BaseDirectory, "minecraft-data", "1.21.100", "blockStates.json")),
                "Bundled native library, instructions and both block registries available at runtime");
            Require(AppSettings.ClampInterval(-1) == 30 && AppSettings.ClampInterval(5000) == 3000, "Speed bounds: 30–3000 ms");
            Require(AppSettings.ValidSlot("Weathertop_End_Nested") && !AppSettings.ValidSlot("../test") && !AppSettings.ValidSlot(new string('x', 33)), "Safe NBT slot names");
            AutoCraftUiTests.Run(output, Require);
            MapQueueUiTests.Run(output, Require);
            using (var form = new MainForm(preview: true))
            {
                form.StartPosition = FormStartPosition.Manual; form.Location = new Point(-15000, -15000);
                form.Show(); Application.DoEvents();
                for (int i = 0; i < 5; i++)
                {
                    form.SelectPage(i); form.PerformLayout(); Application.DoEvents();
                    using var bitmap = new Bitmap(form.Width, form.Height);
                    form.DrawToBitmap(bitmap, new Rectangle(Point.Empty, form.Size));
                    bitmap.Save(Path.Combine(output, $"screen-{i + 1}.png"));
                }
                for (int i = 0; i < 200; i++) form.SelectPage(i % 5);
                form.Close();
            }
            checks.Add("All five screens render; 200 navigation switches");
            Require(AuthProfiles.ValidName("Player 123") && !AuthProfiles.ValidName("../Player") && !AuthProfiles.ValidName("CON"), "Safe profile folder names");
            string cacheFixture = Path.Combine(output, "synthetic-cache"), profileRoot = Path.Combine(output, "synthetic-profiles");
            Directory.CreateDirectory(cacheFixture);
            File.WriteAllText(Path.Combine(cacheFixture, "abcdef_live-cache.json"), "{\"test\":\"not-a-real-token\"}");
            File.WriteAllText(Path.Combine(cacheFixture, "abcdef_xbl-cache.json"), "{\"test\":true}");
            Require(AuthProfiles.Import(cacheFixture, "Player 123", profileRoot) == 2, "Import synthetic auth profile");
            Require(File.Exists(Path.Combine(profileRoot, "Player 123", "auth", AuthProfiles.Hash("Player 123") + "_live-cache.json")), "Imported cache filenames use selected profile key");
            bool overwriteRejected = false;
            try { AuthProfiles.Import(cacheFixture, "Player 123", profileRoot); } catch (InvalidOperationException) { overwriteRejected = true; }
            Require(overwriteRejected, "Existing auth profiles cannot be overwritten");
            string exportedRoot = Path.Combine(output, "synthetic-export");
            Require(AuthProfiles.Import(Path.Combine(profileRoot, "Player 123"), "Player 123", exportedRoot) == 2, "Profile export round-trip");
            File.WriteAllText(Path.Combine(cacheFixture, "123456_bed-cache.json"), "{}");
            Require(AuthProfiles.Import(cacheFixture, "Player 123", profileRoot, replaceExisting: true) == 2, "Explicit import into existing profile ignores empty failed-login placeholders");
            var backup = Directory.GetDirectories(Path.Combine(profileRoot, "Player 123"), "auth-backup-*").Single();
            Require(File.ReadAllText(Path.Combine(backup, AuthProfiles.Hash("Player 123") + "_live-cache.json")) == "{\"test\":\"not-a-real-token\"}" && AuthProfiles.HasCache("Player 123", profileRoot), "Previous credentials are preserved and imported nickname key is ready");
            File.WriteAllText(Path.Combine(cacheFixture, "123456_bed-cache.json"), "{\"differentAccount\":true}");
            bool mixedRejected = false;
            try { AuthProfiles.Import(cacheFixture, "Mixed", profileRoot); } catch (InvalidOperationException) { mixedRejected = true; }
            Require(mixedRejected && !Directory.Exists(Path.Combine(profileRoot, "Mixed")), "Mixed-account cache rejected before any destination writes");
            bool failedReplace = false;
            try { AuthProfiles.Import(cacheFixture, "Player 123", profileRoot, replaceExisting: true); } catch (InvalidOperationException) { failedReplace = true; }
            Require(failedReplace && AuthProfiles.HasCache("Player 123", profileRoot) && Directory.GetDirectories(Path.Combine(profileRoot, "Player 123"), "auth-backup-*").Length == 1, "Invalid replacement leaves active profile and backup untouched");
            if (!args.Contains("--preview-only")) ShutdownTests.Run(output, Require);
            if (!args.Contains("--preview-only") && !args.Contains("--shutdown-only"))
            {
                var versions = RelayBackend.Invoke(new { action = "versions" });
                Require(versions.EnumerateArray().Any(v => v.GetString() == "1.21.2"), "Native codec includes 1.21.2");
                Require(versions.EnumerateArray().Any(v => v.GetString() == "1.21.100"), "Native codec includes 1.21.100");
                bool rejected = false;
                try { RelayBackend.Invoke(new { action = "start", host = "bad/host", version = "1.21.2" }); }
                catch (InvalidOperationException) { rejected = true; }
                Require(rejected, "Invalid host rejected without starting");
                string root = Path.Combine(output, "данные"), nbt = Path.Combine(root, "NBT");
                Directory.CreateDirectory(nbt);
                string? fixture = args.Skip(2).FirstOrDefault(x => !x.StartsWith("--"));
                if (fixture != null) File.Copy(fixture, Path.Combine(nbt, "test.qznbt"), true);
                foreach (var version in new[] { "1.21.2", "1.21.100" })
                {
                    RelayBackend.Invoke(new { action = "configure", deposit = true, intervalMs = 30,
                        auto2 = true, craftIntervalMs = 100, windowPauseMs = 300 });
                    string data = Path.Combine(AppContext.BaseDirectory, "minecraft-data", version);
                    var state = RelayBackend.Invoke(new { action = "start", host = "127.0.0.1", port = 19133, version, directory = root, nbtDirectory = nbt,
                        minecraftDataDirectory = Directory.Exists(data) ? data : "", authProfile = "Player 123" });
                    Require(state.Flag("running") && state.Flag("listening"), $"Native relay starts for {version} with Unicode data path");
                    Require(state.Flag("pingOk"), $"Native Winsock loopback verification for {version}");
                    var builder=RelayBackend.Invoke(new { action="platform", op="configure", chunks=10, lanes=3, gapChunks=2, turnSide=-1, placeMs=50 });
                    Require(!builder.Flag("running") && builder.GetProperty("chests").ValueKind==JsonValueKind.Array, $"PlatformBuilder settings and chest list on {version}");
                    builder=RelayBackend.Invoke(new { action="platform", op="start" });
                    Require(!builder.Flag("running"), "Builder refuses to move without a connected player/inventory");
                    builder=RelayBackend.Invoke(new { action="platform", op="record" });
                    Require(builder.Flag("recording"), "Record is armed without decoding inventory");
                    builder=RelayBackend.Invoke(new { action="platform", op="stop" });
                    Require(!builder.Flag("recording") && !builder.Flag("running"), "Stop cancels recording and builder");
                    Require(state.GetProperty("shulkerDeposit").Flag("supported"), $"Deposit compatibility gate for {version}");
                    var craft = state.GetProperty("autoCraftStore");
                    Require(!craft.Flag("busy") && craft.GetProperty("craftIntervalMs").GetInt32() == 100 &&
                        craft.GetProperty("windowPauseMs").GetInt32() == 300, $"Auto 2 settings reach native core without autorun on {version}");
                    craft = RelayBackend.Invoke(new { action = "auto2.toggle" });
                    Require(!craft.Flag("busy") && craft.Text("status").Contains("NBT"), $"Auto 2 rejects missing NBT without opening a window on {version}");
                    Require(RelayBackend.Invoke(new { action = "tick" }).Flag("ok"), "Maintenance tick is callable without an open GUI");
                    RelayBackend.Invoke(new { action = "configure", auto2 = false, craftIntervalMs = -100, windowPauseMs = 50000 });
                    craft = RelayBackend.Invoke(new { action = "snapshot" }).GetProperty("autoCraftStore");
                    Require(craft.GetProperty("craftIntervalMs").GetInt32() == 100 && craft.GetProperty("windowPauseMs").GetInt32() == 3000,
                        "Native API clamps Auto 2 timings");
                    bool auto2Rejected = false;
                    try { RelayBackend.Invoke(new { action = "auto2.toggle" }); } catch (InvalidOperationException) { auto2Rejected = true; }
                    Require(auto2Rejected, "Disabled Auto 2 cannot be started through API");
                    using (var socket = new UdpClient(AddressFamily.InterNetwork))
                    {
                        socket.Client.ReceiveTimeout = 3000;
                        byte[] ping = new byte[33]; ping[0] = 1;
                        Convert.FromHexString("00FFFF00FEFEFEFEFDFDFDFD12345678").CopyTo(ping, 9);
                        socket.Send(ping, ping.Length, "127.0.0.1", 19132);
                        IPEndPoint sender = new(IPAddress.Any, 0);
                        var pong = socket.Receive(ref sender);
                        Require(pong.Length > 35 && pong[0] == 0x1c, $"Local RakNet pong for {version}");
                    }
                    if (fixture != null)
                    {
                        for (int i = 0; i < 3; i++) RelayBackend.Invoke(new { action = "nbt.arm", slot = "test" });
                        state = RelayBackend.Invoke(new { action = "snapshot" });
                        Require(state.Flag("nbtCraftArmed") && state.Text("nbtCraftSlot") == "test", $"PC QZNBTF02 loaded on {version}");
                        RelayBackend.Invoke(new { action = "nbt.off" });
                        Require(!RelayBackend.Invoke(new { action = "snapshot" }).Flag("nbtCraftArmed"), "NBT mode releases loaded selection on off");
                    }
                    for (int i = 0; i < 500; i++) { RelayBackend.Invoke(new { action = "snapshot" }); RelayBackend.Invoke(new { action = "events" }); }
                    checks.Add($"500 snapshot/event allocation/free cycles for {version}");
                    RelayBackend.Invoke(new { action = "stop" });
                    Require(!RelayBackend.Invoke(new { action = "snapshot" }).Flag("running"), $"Stop completes for {version}");
                }
            }
            File.WriteAllText(Path.Combine(output, "result.json"), JsonSerializer.Serialize(new { ok = true, checks }, new JsonSerializerOptions { WriteIndented = true }));
            return 0;
        }
        catch (Exception error)
        {
            File.WriteAllText(Path.Combine(output, "result.json"), JsonSerializer.Serialize(new { ok = false, checks, error = error.ToString() }, new JsonSerializerOptions { WriteIndented = true }));
            return 1;
        }
        finally { if (!args.Contains("--preview-only") && !args.Contains("--shutdown-only")) { try { RelayBackend.Invoke(new { action = "stop" }); } catch { } } }
    }
}

internal static class MapQueueUiTests
{
    private static T Field<T>(object obj,string name)=>(T)obj.GetType().GetField(name,System.Reflection.BindingFlags.NonPublic|System.Reflection.BindingFlags.Instance)!.GetValue(obj)!;
    internal static void Run(string output,Action<bool,string> require)
    {
        var legacy=JsonSerializer.Deserialize<AppSettings>("{}")!;
        require(legacy.MapArchive=="" && legacy.MapTiming.Count==0,"Old settings do not enable maps or load a ZIP");
        using var backend=new RelayBackend();
        using var panel=new MapQueuePanel(backend,new AppSettings{MapTiming=new(){{"transfer",-1},{"timeout",200000},{"hold",0}}});
        var fields=Field<Dictionary<string,NumericUpDown>>(panel,"fields");
        require(fields.Count==11 && fields["transfer"].Value==500 && fields["hold"].Value==300 && fields["timeout"].Value==120000,"Eleven independent map timings with safe bounds");
        using var host=new Form{Size=new Size(850,850),Location=new Point(-15000,-15000),StartPosition=FormStartPosition.Manual,BackColor=Theme.Background,Font=new Font("Segoe UI",10),AutoScroll=true};
        host.Controls.Add(panel);Theme.Inputs(host);host.Show();Application.DoEvents();
        panel.Update(JsonSerializer.SerializeToElement(new{busy=false,loaded=true,status="ZIP готов. Файлов: 200",file="0001.qznbt",completed=0,total=200,maps=0}));
        require(panel.Loaded,"Map panel displays imported archive without starting");
        using(var bitmap=new Bitmap(host.Width,host.Height)){host.DrawToBitmap(bitmap,new Rectangle(Point.Empty,host.Size));bitmap.Save(Path.Combine(output,"maps-settings.png"));}
        using var floating=new FloatingDepositForm(maps:true);floating.Location=new Point(-15000,-15000);
        floating.UpdateIndicators(JsonSerializer.SerializeToElement(new{running=true,upstreamReady=true,mapQueue=new{busy=true,loaded=true,status="Показываю карту 3 / 27",completed=10,total=200,maps=2}}),true);
        floating.UpdateVisibility(true,true);
        require(floating.Visible&&floating.TopMost&&Field<RelayButton>(floating,"button").Text.Contains("СТОП")&&Field<Label>(floating,"detail").Text.Contains("10 / 200"),"Separate floating map button and counters");
        using(var bitmap=new Bitmap(floating.Width,floating.Height)){floating.DrawToBitmap(bitmap,new Rectangle(Point.Empty,floating.Size));bitmap.Save(Path.Combine(output,"maps-floating.png"));}
        floating.ShowError("Закройте текущее окно");
        floating.UpdateIndicators(JsonSerializer.SerializeToElement(new{running=false}),true);
        require(Field<Label>(floating,"detail").Text.Contains("Закройте"),"Map start error remains visible across snapshot refresh");
        host.Close();floating.Close();
    }
}
