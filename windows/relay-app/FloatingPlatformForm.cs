using System.Runtime.InteropServices;
using System.Text.Json;

namespace CpeRelay.Windows;

internal sealed class FloatingPlatformForm : Form
{
    [DllImport("user32.dll")] private static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] private static extern bool ReleaseCapture();
    [DllImport("user32.dll")] private static extern IntPtr SendMessage(IntPtr window, int message, IntPtr wparam, IntPtr lparam);

    private readonly RelayButton start = ActionButton("Старт", true);
    private readonly RelayButton resume = ActionButton("Продолжить");
    private readonly RelayButton stop = ActionButton("Стоп");
    private readonly Label detail = new() { Dock = DockStyle.Fill, ForeColor = Theme.Muted, Padding = new Padding(8, 5, 8, 2) };
    private bool commandPending;
    internal Func<string, Task>? Command;

    protected override bool ShowWithoutActivation => true;
    protected override CreateParams CreateParams
    {
        get { var value = base.CreateParams; value.ExStyle |= 0x08000000 | 0x00000080; return value; }
    }

    internal FloatingPlatformForm()
    {
        Text = "CPE · Строительство";
        FormBorderStyle = FormBorderStyle.None;
        BackColor = Theme.Sidebar;
        ShowInTaskbar = false;
        TopMost = true;
        StartPosition = FormStartPosition.Manual;
        Size = new Size(365, 124);
        Location = new Point(Screen.PrimaryScreen!.WorkingArea.Right - Width - 30, 610);
        Font = new Font("Segoe UI", 10);

        var handle = new Label { Text = "⋮⋮  СТРОИТЕЛЬСТВО — перетащить", Dock = DockStyle.Top, Height = 24,
            ForeColor = Theme.Muted, BackColor = Theme.Sidebar, TextAlign = ContentAlignment.MiddleCenter };
        handle.MouseDown += (_, e) => { if (e.Button == MouseButtons.Left) { ReleaseCapture(); SendMessage(Handle, 0xA1, (IntPtr)2, IntPtr.Zero); } };
        var actions = new FlowLayoutPanel { Dock = DockStyle.Top, Height = 46, WrapContents = false,
            Padding = new Padding(7, 3, 0, 3), BackColor = Theme.Sidebar };
        actions.Controls.AddRange([start, resume, stop]);
        start.Click += async (_, _) => await Execute("start");
        resume.Click += async (_, _) => await Execute("resume");
        stop.Click += async (_, _) => await Execute("stop");
        Controls.Add(detail);
        Controls.Add(actions);
        Controls.Add(handle);
        UpdateIndicators(default, false);
    }

    private static RelayButton ActionButton(string text, bool primary = false) => new() {
        Text = text, Primary = primary, Width = 110, Height = 38, Margin = new Padding(0, 0, 5, 0)
    };

    private async Task Execute(string operation)
    {
        if (commandPending || Command is null) return;
        commandPending = true;
        SetButtons(false, false, false);
        try { await Command(operation); }
        finally { commandPending = false; }
    }

    internal void UpdateIndicators(JsonElement value, bool upstreamReady)
    {
        bool present = value.ValueKind == JsonValueKind.Object;
        bool active = present && value.Flag("running");
        string stage = present ? value.Text("stage", "idle") : "idle";
        bool paused = stage == "paused";
        bool recording = present && value.Flag("recording");
        if (!commandPending) SetButtons(upstreamReady && !active, upstreamReady && paused, active || paused || recording);
        if (start.Selected != active) { start.Selected = active; start.Invalidate(); }
        detail.Text = present
            ? value.Text("status", "Строительство остановлено") + "\nБлоков: " + Number(value, "placed") + "  ·  Ряд: " + Number(value, "row")
            : "Ожидание состояния строительства";
    }

    private void SetButtons(bool canStart, bool canResume, bool canStop)
    {
        start.Enabled = canStart; resume.Enabled = canResume; stop.Enabled = canStop;
    }

    internal void UpdateVisibility(bool relayRunning, bool setting)
    {
        if (relayRunning && setting)
        {
            if (!Visible) Show();
            SetWindowPos(Handle, new IntPtr(-1), 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010);
        }
        else Hide();
    }

    private static long Number(JsonElement value, string name) => value.TryGetProperty(name, out var number) && number.TryGetInt64(out var result) ? result : 0;
}
