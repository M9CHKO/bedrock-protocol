using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading.Channels;

namespace CpeRelay.Windows;

internal sealed class AppSettings
{
    internal static readonly string DirectoryPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CPE Relay Windows");
    internal static string SettingsFile => Path.Combine(DirectoryPath, "settings.json");
    public string Host { get; set; } = "cpe.ign.gg";
    public int Port { get; set; } = 19132;
    public string Version { get; set; } = "1.21.100";
    public string AuthProfile { get; set; } = "default";
    public string NbtDirectory { get; set; } = Path.Combine(DirectoryPath, "NBT");
    public bool Deposit { get; set; }
    public bool Hotbar { get; set; }
    public bool Armor { get; set; }
    public bool Totem { get; set; }
    public bool Logging { get; set; }
    public bool FloatingButton { get; set; } = true;
    public bool Auto2 { get; set; } = true;
    public int CraftIntervalMs { get; set; } = 1000;
    public int WindowPauseMs { get; set; } = 700;
    public int IntervalMs { get; set; } = 1000;
    internal static bool ValidSlot(string name) => Regex.IsMatch(name, @"\A[A-Za-z0-9_-]{1,32}\z");
    internal static int ClampInterval(int value) => Math.Clamp(value, 30, 3000);
    internal static int ClampCraftInterval(int value) => Math.Clamp(value, 100, 5000);
    internal static int ClampWindowPause(int value) => Math.Clamp(value, 300, 3000);
    internal static AppSettings Load()
    {
        try
        {
            var value = JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(SettingsFile)) ?? new();
            value.IntervalMs = ClampInterval(value.IntervalMs);
            value.CraftIntervalMs = ClampCraftInterval(value.CraftIntervalMs);
            value.WindowPauseMs = ClampWindowPause(value.WindowPauseMs);
            value.Port = Math.Clamp(value.Port, 1, 65535);
            if (!AuthProfiles.ValidName(value.AuthProfile)) value.AuthProfile = "default";
            if (string.IsNullOrWhiteSpace(value.NbtDirectory)) value.NbtDirectory = new AppSettings().NbtDirectory;
            return value;
        }
        catch (Exception error) when (error is IOException or JsonException or UnauthorizedAccessException) { return new(); }
    }
    internal void Save()
    {
        Directory.CreateDirectory(DirectoryPath);
        var temporary = SettingsFile + ".tmp";
        File.WriteAllText(temporary, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
        File.Move(temporary, SettingsFile, true);
    }
}

internal static class LogStore
{
    private const int MaximumBytes = 256 * 1024;
    private static readonly Channel<string> Queue = Channel.CreateBounded<string>(new BoundedChannelOptions(256)
    { SingleReader = true, FullMode = BoundedChannelFullMode.DropOldest });
    private static readonly Task Worker = Task.Run(async () =>
    {
        await foreach (var line in Queue.Reader.ReadAllAsync()) Write(line);
    });
    internal static void Append(string line) => Queue.Writer.TryWrite(line.Length > 32000 ? line[..32000] : line);
    internal static async Task Finish()
    {
        Queue.Writer.TryComplete();
        await Worker;
    }
    private static void Write(string line)
    {
        // Only sanitized native events/status text are passed here, never device codes or tokens.
        try
        {
            Directory.CreateDirectory(AppSettings.DirectoryPath);
            var path = Path.Combine(AppSettings.DirectoryPath, "relay.log");
            if (File.Exists(path) && new FileInfo(path).Length > MaximumBytes)
                File.Move(path, path + ".previous", true);
            File.AppendAllText(path, line + Environment.NewLine);
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }
}
