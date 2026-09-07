using System.Runtime.InteropServices;
using System.Text.Json;

namespace CpeRelay.Windows;

internal sealed class FloatingDepositForm : Form
{
    [DllImport("user32.dll")] private static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] private static extern bool ReleaseCapture();
    [DllImport("user32.dll")] private static extern IntPtr SendMessage(IntPtr window, int message, IntPtr wparam, IntPtr lparam);
    private readonly RelayButton button = new() { Dock = DockStyle.Fill, Text = "Разгрузка · ВЫКЛ" };
    private readonly Label detail = new() { Dock = DockStyle.Bottom, Height = 51, ForeColor = Theme.Muted, Padding = new Padding(8, 2, 8, 2) };
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
        Size = new Size(340, 121);
        Location = new Point(Screen.PrimaryScreen!.WorkingArea.Right - Width - 30, 120);
        Font = new Font("Segoe UI", 10);
        var handle = new Label { Text = "⋮⋮  CPE RELAY — перетащить", Dock = DockStyle.Top, Height = 24,
            ForeColor = Theme.Muted, BackColor = Theme.Sidebar, TextAlign = ContentAlignment.MiddleCenter };
        handle.MouseDown += (_, e) => { if (e.Button == MouseButtons.Left) { ReleaseCapture(); SendMessage(Handle, 0xA1, (IntPtr)2, IntPtr.Zero); } };
        button.Click += (_, _) => Toggle?.Invoke();
        Controls.Add(button); Controls.Add(detail); Controls.Add(handle);
    }
    internal void UpdateIndicators(JsonElement state, bool requested)
    {
        bool available = state.TryGetProperty("shulkerDeposit", out var value);
        bool enabled = available && value.Flag("enabled");
        bool supported = available && value.Flag("supported");
        button.Text = !state.Flag("upstreamReady") ? "Разгрузка · НЕТ ПОДКЛЮЧЕНИЯ" :
            !supported ? "Разгрузка · ВЕРСИЯ НЕ ПОДДЕРЖАНА" :
            enabled != requested ? "Разгрузка · ПРИМЕНЕНИЕ…" : enabled ? "Разгрузка · ВКЛ" : "Разгрузка · ВЫКЛ";
        if (button.Selected != enabled) { button.Selected = enabled; button.Invalidate(); }
        detail.Text = available ? value.Text("status") + "\nОтправлено: " + value.GetProperty("sent") +
            "  ·  Подтверждено: " + value.GetProperty("confirmed") : "Ожидание состояния реле";
    }
    internal void UpdateVisibility(bool running, bool setting)
    {
        // Independent of worker polling and process names: UWP Minecraft may
        // be hosted by ApplicationFrameHost, including when a chest is open.
        if (running && setting)
        {
            if (!Visible) Show();
            SetWindowPos(Handle, new IntPtr(-1), 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010); // topmost, no activate/move/resize
        }
        else Hide();
    }
}
