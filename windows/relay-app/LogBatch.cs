using System.Text;
using System.Text.Json;

namespace CpeRelay.Windows;

internal static class LogBatch
{
    internal static string NativeEvent(JsonElement item)
    {
        var time = DateTimeOffset.Now;
        if (item.TryGetProperty("timestampMs", out var value) && value.TryGetInt64(out long milliseconds))
        {
            try { time = DateTimeOffset.FromUnixTimeMilliseconds(milliseconds).ToLocalTime(); }
            catch (ArgumentOutOfRangeException) { }
        }
        return $"{time:HH:mm:ss.fff} [{item.Text("level", "INFO")}] {item.Text("type")}: {item.Text("message")}";
    }

    internal static string Build(IReadOnlyList<string> messages, bool timestamped = false)
    {
        static int Priority(string message) => message.Contains("[ERROR]", StringComparison.Ordinal) ? 0 :
            message.Contains("[WARN]", StringComparison.Ordinal) ? 1 :
            message.Contains("[DEBUG]", StringComparison.Ordinal) ? 3 : 2;
        // Reserve the bounded UI/disk budget for errors first. A flight dump
        // appended after the initiating error must never push that error out.
        var selected = new SortedDictionary<int, string>();
        int bytes = 0;
        foreach (int index in Enumerable.Range(0, messages.Count)
                     .OrderBy(i => Priority(messages[i])).ThenByDescending(i => i))
        {
            var text = messages[index];
            if (text.Length > 2000) text = text[..2000] + "…";
            if (!timestamped) text = $"{DateTime.Now:HH:mm:ss.fff} {text}";
            if (selected.Count >= 128 || bytes + text.Length + 2 > 28000) continue;
            selected.Add(index, text); bytes += text.Length + 2;
        }
        var result = new StringBuilder();
        if (selected.Count < messages.Count)
            result.AppendLine($"{DateTime.Now:HH:mm:ss.fff} Скрыто событий: {messages.Count - selected.Count}; ошибки имеют приоритет.");
        foreach (var line in selected.Values) result.AppendLine(line);
        return result.ToString();
    }
}
