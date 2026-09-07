namespace CpeRelay.Windows;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        ApplicationConfiguration.Initialize();
        if (args.Length >= 1 && args[0] == "--self-test") return SelfTest.Run(args);
        using var mutex = new Mutex(true, @"Local\CpeRelayWindowsDesktop", out var created);
        if (!created) { MessageBox.Show("CPE Relay уже запущен.", "CPE Relay"); return 1; }
        Application.ThreadException += (_, e) => MessageBox.Show(e.Exception.Message, "CPE Relay — ошибка");
        Application.Run(new MainForm());
        return 0;
    }
}
