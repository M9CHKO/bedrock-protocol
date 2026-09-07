using System.Runtime.InteropServices;
using System.Text.Json;

namespace CpeRelay.Windows;

internal sealed class RelayBackend
{
    [DllImport("cpe_relay_windows.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr cpe_call([MarshalAs(UnmanagedType.LPUTF8Str)] string json);
    [DllImport("cpe_relay_windows.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern void cpe_free(IntPtr value);
    private readonly SemaphoreSlim gate = new(1, 1);

    internal static JsonElement Invoke(object request)
    {
        var value = cpe_call(JsonSerializer.Serialize(request));
        if (value == IntPtr.Zero) throw new InvalidOperationException("Не удалось выделить память для ответа реле");
        try
        {
            using var document = JsonDocument.Parse(Marshal.PtrToStringUTF8(value)!);
            var result = document.RootElement.Clone();
            if (result.ValueKind == JsonValueKind.Object && result.TryGetProperty("error", out var error) &&
                !result.TryGetProperty("running", out _) && error.ValueKind == JsonValueKind.String && !string.IsNullOrEmpty(error.GetString()))
                throw new InvalidOperationException(error.GetString());
            return result;
        }
        finally { cpe_free(value); }
    }

    internal async Task<JsonElement> Call(object request)
    {
        await gate.WaitAsync();
        try { return await Task.Run(() => Invoke(request)); }
        finally { gate.Release(); }
    }

    // Polling never queues behind slow starts, file loads or stops.
    internal async Task<(JsonElement State, JsonElement Events)?> Poll()
    {
        if (!await gate.WaitAsync(0)) return null;
        try { return await Task.Run(() => (Invoke(new { action = "snapshot" }), Invoke(new { action = "events" }))); }
        finally { gate.Release(); }
    }
}

internal static class JsonExtensions
{
    internal static bool Flag(this JsonElement value, string key) => value.ValueKind == JsonValueKind.Object &&
        value.TryGetProperty(key, out var item) && item.ValueKind == JsonValueKind.True;
    internal static string Text(this JsonElement value, string key, string fallback = "") =>
        value.ValueKind == JsonValueKind.Object && value.TryGetProperty(key, out var item) &&
        item.ValueKind == JsonValueKind.String ? item.GetString()! : fallback;
}
