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
            Require(AppSettings.ClampInterval(-1) == 30 && AppSettings.ClampInterval(5000) == 3000, "Speed bounds: 30–3000 ms");
            Require(AppSettings.ValidSlot("Weathertop_End_Nested") && !AppSettings.ValidSlot("../test") && !AppSettings.ValidSlot(new string('x', 33)), "Safe NBT slot names");
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
            File.WriteAllText(Path.Combine(cacheFixture, "abcdef_xbl-cache.json"), "{}");
            Require(AuthProfiles.Import(cacheFixture, "Player 123", profileRoot) == 2, "Import synthetic auth profile");
            Require(File.Exists(Path.Combine(profileRoot, "Player 123", "auth", AuthProfiles.Hash("Player 123") + "_live-cache.json")), "Imported cache filenames use selected profile key");
            bool overwriteRejected = false;
            try { AuthProfiles.Import(cacheFixture, "Player 123", profileRoot); } catch (InvalidOperationException) { overwriteRejected = true; }
            Require(overwriteRejected, "Existing auth profiles cannot be overwritten");
            string exportedRoot = Path.Combine(output, "synthetic-export");
            Require(AuthProfiles.Import(Path.Combine(profileRoot, "Player 123"), "Player 123", exportedRoot) == 2, "Profile export round-trip");
            File.WriteAllText(Path.Combine(cacheFixture, "123456_bed-cache.json"), "{}");
            bool mixedRejected = false;
            try { AuthProfiles.Import(cacheFixture, "Mixed", profileRoot); } catch (InvalidOperationException) { mixedRejected = true; }
            Require(mixedRejected && !Directory.Exists(Path.Combine(profileRoot, "Mixed")), "Mixed-account cache rejected before any destination writes");
            if (!args.Contains("--preview-only"))
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
                    RelayBackend.Invoke(new { action = "configure", deposit = true, intervalMs = 30 });
                    string data = Path.Combine(AppContext.BaseDirectory, "minecraft-data", version);
                    var state = RelayBackend.Invoke(new { action = "start", host = "127.0.0.1", port = 19133, version, directory = root, nbtDirectory = nbt,
                        minecraftDataDirectory = Directory.Exists(data) ? data : "", authProfile = "Player 123" });
                    Require(state.Flag("running") && state.Flag("listening"), $"Native relay starts for {version} with Unicode data path");
                    Require(state.Flag("pingOk"), $"Native Winsock loopback verification for {version}");
                    Require(state.GetProperty("shulkerDeposit").Flag("supported") == (version == "1.21.100"), $"Deposit compatibility gate for {version}");
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
        finally { if (!args.Contains("--preview-only")) { try { RelayBackend.Invoke(new { action = "stop" }); } catch { } } }
    }
}
