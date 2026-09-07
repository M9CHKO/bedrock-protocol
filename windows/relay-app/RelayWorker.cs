using System.Buffers.Binary;
using System.Diagnostics;
using System.IO.Pipes;
using System.Net;
using System.Net.Sockets;
using System.Text.Json;
using System.Runtime.InteropServices;

namespace CpeRelay.Windows;

internal static class WorkerWire
{
    private const int MaximumMessage = 4 * 1024 * 1024;
    internal static async Task Write(Stream stream, object value, CancellationToken cancellation)
    {
        byte[] bytes = JsonSerializer.SerializeToUtf8Bytes(value);
        if (bytes.Length > MaximumMessage) throw new IOException("Слишком большой ответ реле.");
        byte[] header = new byte[4]; BinaryPrimitives.WriteInt32LittleEndian(header, bytes.Length);
        await stream.WriteAsync(header, cancellation).ConfigureAwait(false);
        await stream.WriteAsync(bytes, cancellation).ConfigureAwait(false);
        await stream.FlushAsync(cancellation).ConfigureAwait(false);
    }
    internal static async Task<JsonElement> Read(Stream stream, CancellationToken cancellation)
    {
        byte[] header = new byte[4];
        await stream.ReadExactlyAsync(header, cancellation).ConfigureAwait(false);
        int count = BinaryPrimitives.ReadInt32LittleEndian(header);
        if (count < 1 || count > MaximumMessage) throw new IOException("Недопустимый ответ реле.");
        byte[] bytes = new byte[count];
        await stream.ReadExactlyAsync(bytes, cancellation).ConfigureAwait(false);
        using var json = JsonDocument.Parse(bytes);
        return json.RootElement.Clone();
    }
}

// A fresh process owns every native thread, socket and DLL allocation. The UI
// never loads the DLL; timeout recovery cannot leave native threads in the UI.
internal sealed class WorkerSession : IDisposable
{
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetNamedPipeClientProcessId(Microsoft.Win32.SafeHandles.SafePipeHandle pipe, out uint pid);
    private readonly NamedPipeServerStream pipe;
    private readonly Process process;
    private readonly WorkerJob job;
    private readonly SemaphoreSlim gate = new(1, 1);
    private readonly CancellationTokenSource lifetime = new();
    private readonly Task ready;
    private int disposed, stopping;
    internal bool IsDisposed => Volatile.Read(ref disposed) != 0;

    internal WorkerSession(bool testWorker)
    {
        string name = "CpeRelay-" + Guid.NewGuid().ToString("N");
        pipe = new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
        job = new WorkerJob();
        var start = new ProcessStartInfo(Environment.ProcessPath!)
        {
            UseShellExecute = false, CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden,
            WorkingDirectory = AppContext.BaseDirectory
        };
        start.ArgumentList.Add(testWorker ? "--relay-worker-test" : "--relay-worker");
        start.ArgumentList.Add(name);
        try
        {
            process = Process.Start(start) ?? throw new IOException("Не удалось запустить ядро реле.");
            job.Attach(process);
        }
        catch
        {
            if (process != null) { try { process.Kill(); } catch (InvalidOperationException) { } process.Dispose(); }
            job.Dispose(); pipe.Dispose(); throw;
        }
        ready = Connect();
    }
    private async Task Connect()
    {
        try
        {
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token);
            timeout.CancelAfter(TimeSpan.FromSeconds(10));
            await pipe.WaitForConnectionAsync(timeout.Token).ConfigureAwait(false);
            if (!GetNamedPipeClientProcessId(pipe.SafePipeHandle, out uint pid) || pid != process.Id)
                throw new IOException("Не удалось проверить процесс реле.");
        }
        catch { Dispose(); throw; }
    }
    private async Task<JsonElement> Exchange(object request, CancellationToken cancellation)
    {
        await ready.WaitAsync(cancellation).ConfigureAwait(false);
        await WorkerWire.Write(pipe, request, cancellation).ConfigureAwait(false);
        var result = await WorkerWire.Read(pipe, cancellation).ConfigureAwait(false);
        if (result.ValueKind == JsonValueKind.Object && !result.TryGetProperty("running", out _) &&
            result.Text("error") is { Length: > 0 } error) throw new InvalidOperationException(error);
        return result;
    }
    internal async Task<JsonElement> Call(object request)
    {
        await gate.WaitAsync(lifetime.Token).ConfigureAwait(false);
        try
        {
            if (Volatile.Read(ref stopping) != 0 || IsDisposed) throw new OperationCanceledException();
            return await Exchange(request, lifetime.Token).ConfigureAwait(false);
        }
        catch (Exception error) when (error is IOException or ObjectDisposedException)
        {
            Dispose();
            if (Volatile.Read(ref stopping) != 0) throw new OperationCanceledException();
            throw new IOException("Связь с ядром реле прервана. Можно запустить его снова.");
        }
        finally { gate.Release(); }
    }
    internal async Task<(JsonElement State, JsonElement Events)?> Poll()
    {
        if (IsDisposed || Volatile.Read(ref stopping) != 0 || !await gate.WaitAsync(0).ConfigureAwait(false)) return null;
        try
        {
            var state = await Exchange(new { action = "snapshot" }, lifetime.Token).ConfigureAwait(false);
            var events = await Exchange(new { action = "events" }, lifetime.Token).ConfigureAwait(false);
            return (state, events);
        }
        catch (Exception error) when (error is IOException or ObjectDisposedException or OperationCanceledException)
        { Dispose(); return null; }
        finally { gate.Release(); }
    }
    internal async Task<bool> StopAsync()
    {
        Interlocked.Exchange(ref stopping, 1);
        bool locked = false, forced = false;
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
        try
        {
            await gate.WaitAsync(timeout.Token).ConfigureAwait(false); locked = true;
            if (!IsDisposed)
            {
                await Exchange(new { action = "stop" }, timeout.Token).ConfigureAwait(false);
                await process.WaitForExitAsync(timeout.Token).ConfigureAwait(false);
            }
        }
        catch (Exception error) when (error is OperationCanceledException or IOException or InvalidOperationException)
        { forced = true; }
        finally
        {
            Dispose(); // Closing the job also kills an unresponsive helper, never Minecraft.
            if (locked) gate.Release();
        }
        return forced;
    }
    public void Dispose()
    {
        if (Interlocked.Exchange(ref disposed, 1) != 0) return;
        job.Dispose();
        lifetime.Cancel(); pipe.Dispose();
        // Process handles are cheap; closing this handle never waits for a native thread.
        process.Dispose();
    }
}

internal static class RelayWorker
{
    // One coalesced worker loop, not a UI timer. Restoration also runs while
    // Minecraft is in a workbench and the main app is minimized.
    private static async Task Maintain(CancellationToken cancellation)
    {
        using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(100));
        try
        {
            while (await timer.WaitForNextTickAsync(cancellation).ConfigureAwait(false))
                RelayBackend.Invoke(new { action = "tick" });
        }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { }
    }
    internal static async Task<int> Run(string name, bool test)
    {
        using var lifetime = new CancellationTokenSource();
        Task maintenance = Task.CompletedTask;
        try
        {
            using var pipe = new NamedPipeClientStream(".", name, PipeDirection.InOut, PipeOptions.Asynchronous);
            await pipe.ConnectAsync(10000).ConfigureAwait(false);
            if (!test) maintenance = Task.Run(() => Maintain(lifetime.Token));
            using var testSocket = test ? new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)) : null;
            bool hangOnStop = false;
            while (true)
            {
                var request = await WorkerWire.Read(pipe, CancellationToken.None).ConfigureAwait(false);
                string action = request.Text("action");
                object response;
                if (test)
                {
                    if (action == "test.bind") hangOnStop = request.Flag("hangOnStop");
                    if (action == "test.crash") Environment.Exit(3);
                    if (action == "test.hang" || (action == "stop" && hangOnStop))
                    {
                        if (request.Text("readyEvent").StartsWith(@"Local\CpeRelayTest-", StringComparison.Ordinal))
                        { using var signal = EventWaitHandle.OpenExisting(request.Text("readyEvent")); signal.Set(); }
                        await Task.Delay(Timeout.Infinite).ConfigureAwait(false);
                    }
                    response = new { ok = true, pid = Environment.ProcessId, port = ((IPEndPoint)testSocket!.Client.LocalEndPoint!).Port };
                }
                else
                {
                    try { response = RelayBackend.Invoke(request); }
                    catch (Exception error) { response = new { error = error.Message }; }
                }
                await WorkerWire.Write(pipe, response, CancellationToken.None).ConfigureAwait(false);
                if (action == "stop") return 0;
            }
        }
        catch { return 1; } // Never print IPC payloads, device codes or token contents.
        finally
        {
            lifetime.Cancel();
            // Process-owned cleanup stays bounded, including native failures.
            try { await maintenance.WaitAsync(TimeSpan.FromSeconds(1)).ConfigureAwait(false); }
            catch (Exception) { }
        }
    }
}
