using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Text.Json;

namespace CpeRelay.Windows;

internal static class ShutdownTests
{
    private static RelayBackend? ownerBackend;
    internal static int RunOwner(string output)
    {
        ownerBackend = new RelayBackend(testWorker: true);
        var bound = ownerBackend.Call(new { action = "test.bind" }).GetAwaiter().GetResult();
        File.WriteAllText(output, bound.GetRawText());
        // Intentionally no Dispose/Stop: the Windows job must reap the helper.
        return 0;
    }
    private static T Field<T>(object value, string name) =>
        (T)value.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(value)!;
    private static void Set(object value, string name, object field) =>
        value.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!.SetValue(value, field);
    private static object? Invoke(object value, string name, params object[] args) =>
        value.GetType().GetMethod(name, BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(value, args);
    internal static void Run(string output, Action<bool, string> require)
    {
        var previous = SynchronizationContext.Current;
        using var context = new WindowsFormsSynchronizationContext();
        SynchronizationContext.SetSynchronizationContext(context);
        try
        {
            Task test = RunAsync(output, require);
            var clock = Stopwatch.StartNew();
            while (!test.IsCompleted && clock.Elapsed < TimeSpan.FromSeconds(50))
            { Application.DoEvents(); Thread.Sleep(5); }
            if (!test.IsCompleted) throw new TimeoutException("Shutdown regression test timed out");
            test.GetAwaiter().GetResult();
        }
        finally { SynchronizationContext.SetSynchronizationContext(previous); }
    }
    private static async Task WaitUntil(Func<bool> condition)
    {
        var deadline = Stopwatch.StartNew();
        while (!condition())
        {
            if (deadline.Elapsed > TimeSpan.FromSeconds(6)) throw new TimeoutException("Owned process/window still alive");
            await Task.Delay(10);
        }
    }
    private static bool Exited(int pid)
    {
        try { using var process = Process.GetProcessById(pid); return process.HasExited; }
        catch (ArgumentException) { return true; }
    }
    private static bool PortFree(int port)
    {
        try
        {
            using var socket = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp) { ExclusiveAddressUse = true };
            socket.Bind(new IPEndPoint(IPAddress.Loopback, port)); return true;
        }
        catch (SocketException) { return false; }
    }
    private static async Task RunAsync(string output, Action<bool, string> require)
    {
        // Exercise the real DLL in its child too, without taking the user's port
        // or starting any Microsoft/remote authentication session.
        using (var native = new RelayBackend())
        {
            var versions = await native.Call(new { action = "versions" });
            require(versions.EnumerateArray().Any(v => v.GetString() == "1.21.2"), "Native DLL runs through isolated worker IPC");
            require(!await native.StopAsync(), "Idle native worker shuts down gracefully");
        }
        using var backend = new RelayBackend(testWorker: true);
        var bound = await backend.Call(new { action = "test.bind" });
        int pid = bound.GetProperty("pid").GetInt32(), port = bound.GetProperty("port").GetInt32();
        require(!await backend.StopAsync(), "Responsive helper stops without forced termination");
        await WaitUntil(() => Exited(pid) && PortFree(port));
        require(true, "Graceful stop reaps helper and releases UDP port");

        using var form = new MainForm(preview: true, testBackend: backend);
        form.StartPosition = FormStartPosition.Manual; form.Location = new Point(-15000, -15000); form.Show();
        Set(form, "initialized", true); Set(form, "starting", true);
        Invoke(form, "SetBusy", true);
        require(Field<Button>(form, "stop").Enabled, "Stop button remains enabled during an unfinished start");
        bound = await backend.Call(new { action = "test.bind" });
        pid = bound.GetProperty("pid").GetInt32(); port = bound.GetProperty("port").GetInt32();
        string eventName = @"Local\CpeRelayTest-" + Guid.NewGuid().ToString("N");
        using var entered = new EventWaitHandle(false, EventResetMode.ManualReset, eventName);
        var pending = backend.Call(new { action = "test.hang", readyEvent = eventName });
        require(await Task.Run(() => entered.WaitOne(5000)), "Fault injection reached an indefinitely blocked worker call");
        require(await backend.Poll() == null, "Polling does not queue behind a blocked call");
        var timer = Stopwatch.StartNew();
        await (Task)Invoke(form, "StopRelay")!;
        require(timer.Elapsed < TimeSpan.FromSeconds(5), "Stop cancels a blocked operation within a bounded deadline");
        bool cancelled = false;
        try { await pending; } catch (OperationCanceledException) { cancelled = true; }
        require(cancelled, "Blocked caller receives cancellation, not a stale success");
        await WaitUntil(() => Exited(pid) && PortFree(port));
        require(form.Visible && Field<Button>(form, "start").Enabled, "Window stays usable for restart after forced stop");

        bound = await backend.Call(new { action = "test.bind", hangOnStop = true });
        int restartedPid = bound.GetProperty("pid").GetInt32(); port = bound.GetProperty("port").GetInt32();
        require(restartedPid != pid, "Restart creates a fresh helper after forced stop");
        var floating = Field<FloatingDepositForm>(form, "floating");
        floating.UpdateIndicators(JsonSerializer.SerializeToElement(new { running = true, upstreamReady = true,
            shulkerDeposit = new { supported = true, enabled = true, sent = 4, confirmed = 2, status = "Ожидание подтверждения" } }), true);
        floating.UpdateVisibility(true, true);
        require(floating.Visible && floating.TopMost && Field<RelayButton>(floating, "button").Selected && Field<Label>(floating, "detail").Text.Contains("Подтверждено: 2"),
            "Floating panel uses actual core status and counts, independent of foreground process name");
        form.WindowState = FormWindowState.Minimized;
        floating.UpdateVisibility(true, true);
        require(floating.Visible, "Floating panel remains visible when the main window is minimized");
        floating.UpdateVisibility(true, false);
        require(!floating.Visible, "Floating panel preference hides it immediately");
        floating.Location = new Point(-15000, -15000); floating.Show();
        var floatingAuto2 = Field<FloatingDepositForm>(form, "floatingAuto2");
        floatingAuto2.Location = new Point(-15000, -15000); floatingAuto2.Show();
        form.Close();
        require(!form.Visible && !floating.Visible && !floatingAuto2.Visible, "Close immediately hides main, deposit and Auto 2 floating windows");
        await WaitUntil(() => form.IsDisposed && Exited(restartedPid) && PortFree(port));
        require(true, "Close finishes and releases UDP even when native stop never returns");

        using (var recover = new RelayBackend(testWorker: true))
        {
            bool crashed = false;
            try { await recover.Call(new { action = "test.crash" }); } catch (IOException) { crashed = true; }
            require(crashed, "Worker crash is reported without crashing the UI");
            bound = await recover.Call(new { action = "test.bind" });
            require(bound.Flag("ok"), "Worker can restart after unexpected process exit");
            await recover.StopAsync();
        }

        string marker = Path.Combine(output, "owner-exit.json");
        var start = new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false, CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden };
        start.ArgumentList.Add("--worker-owner-test"); start.ArgumentList.Add(marker);
        using var owner = Process.Start(start)!;
        await owner.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(10));
        using var data = JsonDocument.Parse(File.ReadAllText(marker));
        pid = data.RootElement.GetProperty("pid").GetInt32(); port = data.RootElement.GetProperty("port").GetInt32();
        await WaitUntil(() => Exited(pid) && PortFree(port));
        require(owner.ExitCode == 0, "Windows job terminates orphan helper when its owner exits without cleanup");
    }
}
