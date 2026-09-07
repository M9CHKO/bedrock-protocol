using System.Diagnostics;
using System.Runtime.InteropServices;

namespace CpeRelay.Windows;

internal sealed class FloatingDepositForm : Form
{
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")] private static extern bool ReleaseCapture();
    [DllImport("user32.dll")] private static extern IntPtr SendMessage(IntPtr window, int message, IntPtr wparam, IntPtr lparam);
    private readonly RelayButton button = new() { Dock = DockStyle.Fill, Text = "Разгрузка · ВЫКЛ" };
    internal Action? Toggle;
    protected override bool ShowWithoutActivation => true;
    protected override CreateParams CreateParams
    {
        get { var value = base.CreateParams; value.ExStyle |= 0x08000000 | 0x00000080; return value; }
    }
    internal FloatingDepositForm()
    {
        Text = "CPE · Разгрузка";
        FormBorderStyle = FormBorderStyle.None;
        BackColor = Theme.Sidebar;
        ShowInTaskbar = false; TopMost = true;
        StartPosition = FormStartPosition.Manual;
        Size = new Size(190, 68);
        Location = new Point(Screen.PrimaryScreen!.WorkingArea.Right - Width - 30, 120);
        Font = new Font("Segoe UI", 10);
        var handle = new Label { Text = "⋮⋮  CPE RELAY — перетащить", Dock = DockStyle.Top, Height = 24,
            ForeColor = Theme.Muted, BackColor = Theme.Sidebar, TextAlign = ContentAlignment.MiddleCenter };
        handle.MouseDown += (_, e) => { if (e.Button == MouseButtons.Left) { ReleaseCapture(); SendMessage(Handle, 0xA1, (IntPtr)2, IntPtr.Zero); } };
        button.Click += (_, _) => Toggle?.Invoke();
        Controls.Add(button); Controls.Add(handle);
    }
    internal void UpdateState(bool running, bool enabled, bool setting, Form main)
    {
        button.Text = enabled ? "Разгрузка · ВКЛ" : "Разгрузка · ВЫКЛ";
        if (button.Selected != enabled) { button.Selected = enabled; button.Invalidate(); }
        bool foreground = false;
        try
        {
            GetWindowThreadProcessId(GetForegroundWindow(), out var pid);
            using var process = Process.GetProcessById((int)pid);
            foreground = pid == Environment.ProcessId || process.ProcessName.StartsWith("Minecraft", StringComparison.OrdinalIgnoreCase);
        }
        catch (ArgumentException) { }
        catch (InvalidOperationException) { }
        if (running && setting && foreground) { if (!Visible) Show(); }
        else Hide();
    }
}
