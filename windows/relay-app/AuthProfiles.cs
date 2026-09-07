using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace CpeRelay.Windows;

internal static class AuthProfiles
{
    internal static string Root => Path.Combine(AppSettings.DirectoryPath, "profiles");
    private static readonly Regex CacheName = new(@"\A([a-f0-9]{6})_(live|xbl|bed|mcs|pfb)-cache\.json\z", RegexOptions.IgnoreCase);
    internal static bool ValidName(string? name) => name != null &&
        Regex.IsMatch(name, @"\A[A-Za-z0-9_-][A-Za-z0-9 _-]{0,31}\z") && !name.EndsWith(' ') &&
        !Regex.IsMatch(name, @"\A(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])\z", RegexOptions.IgnoreCase);
    internal static string Folder(string name, string? root = null)
    {
        if (!ValidName(name)) throw new InvalidOperationException("Имя профиля: до 32 английских букв, цифр, пробел, _ или -. Без пробела в конце.");
        return Path.Combine(root ?? Root, name);
    }
    internal static string Hash(string name) => Convert.ToHexString(SHA1.HashData(Encoding.ASCII.GetBytes(name)))[..6].ToLowerInvariant();
    internal static void CreatePrivateDirectory(string path)
    {
        // Credentials remain portable, but only this Windows user and SYSTEM receive access.
        if (Directory.Exists(path)) return;
        using var identity = WindowsIdentity.GetCurrent();
        var security = new DirectorySecurity();
        security.SetAccessRuleProtection(true, false);
        foreach (var sid in new[] { identity.User!, new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null) })
            security.AddAccessRule(new FileSystemAccessRule(sid, FileSystemRights.FullControl,
                InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit, PropagationFlags.None, AccessControlType.Allow));
        new DirectoryInfo(path).Create(security);
    }
    internal static string Ensure(string name)
    {
        var folder = Folder(name); CreatePrivateDirectory(Root); CreatePrivateDirectory(folder);
        string auth = Path.Combine(folder, "auth"); CreatePrivateDirectory(auth); return auth;
    }
    private static List<(string Suffix, byte[] Bytes)> ReadCache(string selected)
    {
        string source = Directory.Exists(Path.Combine(selected, "auth")) ? Path.Combine(selected, "auth") : selected;
        if ((File.GetAttributes(source) & FileAttributes.ReparsePoint) != 0)
            throw new InvalidOperationException("Выберите обычную папку токенов, не ссылку.");
        var files = Directory.EnumerateFiles(source, "*-cache.json").Take(33).ToArray();
        if (files.Length == 0 || files.Length > 32) throw new InvalidOperationException("Нужна папка одного профиля реле с файлами *-cache.json.");
        var result = new List<(string Suffix, byte[] Bytes)>();
        string? prefix = null; long total = 0;
        foreach (string file in files)
        {
            var match = CacheName.Match(Path.GetFileName(file));
            if (!match.Success) continue;
            if (prefix != null && prefix != match.Groups[1].Value.ToLowerInvariant())
                throw new InvalidOperationException("В папке токены нескольких профилей. Выберите папку только одного аккаунта.");
            prefix = match.Groups[1].Value.ToLowerInvariant();
            var info = new FileInfo(file); total += info.Length;
            if ((info.Attributes & FileAttributes.ReparsePoint) != 0 || info.Length > 4 * 1024 * 1024 || total > 8 * 1024 * 1024)
                throw new InvalidOperationException("Недопустимый файл токенов или превышен размер 8 МиБ.");
            using var stream = File.OpenRead(file);
            if (stream.Length != info.Length) throw new InvalidOperationException("Папка токенов изменяется. Остановите исходное реле.");
            byte[] bytes = new byte[(int)stream.Length]; stream.ReadExactly(bytes);
            try
            {
                using var json = JsonDocument.Parse(bytes, new JsonDocumentOptions { MaxDepth = 64 });
                if (json.RootElement.ValueKind != JsonValueKind.Object) throw new JsonException();
            }
            catch (JsonException) { throw new InvalidOperationException("Повреждён JSON токенов. Содержимое скрыто из соображений безопасности."); }
            result.Add((match.Groups[2].Value.ToLowerInvariant() + "-cache.json", bytes));
        }
        if (!result.Any(x => x.Suffix == "live-cache.json"))
            throw new InvalidOperationException("Не найден live-cache.json. Нужен полный кэш авторизации Live из реле, не одиночный access token.");
        return result;
    }
    internal static int Import(string source, string name, string? root = null)
    {
        string parent = root ?? Root;
        string target = Folder(name, parent);
        if (Directory.Exists(target)) throw new InvalidOperationException("Профиль уже существует. Укажите новое имя, чтобы сохранить прежние токены.");
        var cache = ReadCache(source); // Validate all input before creating the destination.
        CreatePrivateDirectory(parent); CreatePrivateDirectory(target);
        string auth = Path.Combine(target, "auth"); CreatePrivateDirectory(auth);
        try
        {
            foreach (var file in cache)
            {
                string path = Path.Combine(auth, Hash(name) + "_" + file.Suffix);
                using var output = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None);
                output.Write(file.Bytes); output.Flush(true);
            }
        }
        catch (IOException) { throw new InvalidOperationException("Не удалось записать профиль целиком. Не используйте его; повторите импорт под новым именем."); }
        finally { foreach (var file in cache) CryptographicOperations.ZeroMemory(file.Bytes); }
        return cache.Count;
    }
}
