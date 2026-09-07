namespace CpeRelay.Windows;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length == 2 && (args[0] == "--relay-worker" || args[0] == "--relay-worker-test"))
            return RelayWorker.Run(args[1], args[0] == "--relay-worker-test").GetAwaiter().GetResult();
        if (args.Length == 2 && args[0] == "--worker-owner-test") return ShutdownTests.RunOwner(args[1]);
        ApplicationConfiguration.Initialize();
        if (args.Length >= 1 && args[0] == "--self-test") return SelfTest.Run(args);
        using var mutex = new Mutex(true, @"Local\CpeRelayWindowsDesktop", out var created);
        if (!created) { MessageBox.Show("CPE Relay уже запущен.", "CPE Relay"); return 1; }
        Application.ThreadException += (_, e) => MessageBox.Show(e.Exception.Message, "CPE Relay — ошибка");
        using var form = new MainForm();
        Application.Run(form);
        return 0;
    }
}
