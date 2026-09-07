using System.Runtime.InteropServices;
using System.Text.Json;

namespace CpeRelay.Windows;

internal sealed class RelayBackend : IDisposable
{
    [DllImport("cpe_relay_windows.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr cpe_call([MarshalAs(UnmanagedType.LPUTF8Str)] string json);
    [DllImport("cpe_relay_windows.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern void cpe_free(IntPtr value);
    private readonly object sync = new();
    private readonly bool testWorker;
    private WorkerSession? worker;
    private Task<bool>? stopping;
    private bool disposed;

    internal RelayBackend(bool testWorker = false) => this.testWorker = testWorker;
    internal bool HasWorker { get { lock (sync) return worker is { IsDisposed: false }; } }

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
        WorkerSession selected;
        lock (sync)
        {
            if (disposed || stopping is { IsCompleted: false }) throw new OperationCanceledException();
            if (worker is null || worker.IsDisposed) worker = new WorkerSession(testWorker);
            selected = worker;
        }
        return await selected.Call(request);
    }

    // Stop bypasses the command queue. A blocked native call must not prevent
    // its own cancellation. Only this backend's owned helper can be terminated.
    internal Task<bool> StopAsync()
    {
        lock (sync)
        {
            if (stopping is { IsCompleted: false }) return stopping;
            var previous = worker; worker = null;
            return stopping = previous is null ? Task.FromResult(false) : previous.StopAsync();
        }
    }

    // Polling never queues behind slow starts, file loads or stops.
    internal async Task<(JsonElement State, JsonElement Events)?> Poll()
    {
        WorkerSession? selected;
        lock (sync)
        {
            if (disposed || stopping is { IsCompleted: false }) return null;
            selected = worker;
        }
        if (selected is null || selected.IsDisposed)
            return (JsonSerializer.SerializeToElement(new { running = false }), JsonSerializer.SerializeToElement(Array.Empty<object>()));
        return await selected.Poll();
    }

    public void Dispose()
    {
        lock (sync)
        {
            disposed = true;
            worker?.Dispose(); worker = null;
        }
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
