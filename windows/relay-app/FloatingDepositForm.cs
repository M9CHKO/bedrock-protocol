using System.Runtime.InteropServices;
using System.Text.Json;

namespace CpeRelay.Windows;

internal sealed class FloatingDepositForm : Form
{
    private DateTime errorUntil;
    internal void ShowError(string message){detail.Text=message;errorUntil=DateTime.UtcNow.AddSeconds(6);}
    [DllImport("user32.dll")] private static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] private static extern bool ReleaseCapture();
    [DllImport("user32.dll")] private static extern IntPtr SendMessage(IntPtr window, int message, IntPtr wparam, IntPtr lparam);
    private readonly RelayButton button = new() { Dock = DockStyle.Fill, Text = "Разгрузка · ВЫКЛ" };
    private readonly Label detail = new() { Dock = DockStyle.Bottom, Height = 51, ForeColor = Theme.Muted, Padding = new Padding(8, 2, 8, 2) };
    private readonly bool autoCraft;
    private readonly bool maps;
    private bool commandPending;
    internal Action? Toggle;
    protected override bool ShowWithoutActivation => true;
    protected override CreateParams CreateParams
    {
        get { var value = base.CreateParams; value.ExStyle |= 0x08000000 | 0x00000080; return value; }
    }
    internal FloatingDepositForm(bool autoCraft = false, bool maps = false)
    {
        this.autoCraft = autoCraft;
        this.maps = maps;
        Text = autoCraft ? "CPE · Авто 2" : "CPE · Разгрузка";
        FormBorderStyle = FormBorderStyle.None;
        BackColor = Theme.Sidebar;
        ShowInTaskbar = false; TopMost = true;
        StartPosition = FormStartPosition.Manual;
        Size = new Size(340, 121);
        Location = new Point(Screen.PrimaryScreen!.WorkingArea.Right - Width - 30, 120);
        if (autoCraft) { Top = 270; Height = 151; detail.Height = 81; button.Text = "Авто 2 · СТАРТ"; }
        if(maps){Top=440;Height=151;detail.Height=81;button.Text="Карты ZIP · СТАРТ";Text="CPE · Карты ZIP";}
        Font = new Font("Segoe UI", 10);
        var handle = new Label { Text = "⋮⋮  CPE RELAY — перетащить", Dock = DockStyle.Top, Height = 24,
            ForeColor = Theme.Muted, BackColor = Theme.Sidebar, TextAlign = ContentAlignment.MiddleCenter };
        handle.MouseDown += (_, e) => { if (e.Button == MouseButtons.Left) { ReleaseCapture(); SendMessage(Handle, 0xA1, (IntPtr)2, IntPtr.Zero); } };
        button.Click += (_, _) => Toggle?.Invoke();
        Controls.Add(button); Controls.Add(detail); Controls.Add(handle);
    }
    internal void UpdateIndicators(JsonElement state, bool requested)
    {
        if(maps){bool present=state.TryGetProperty("mapQueue",out var q);bool active=present&&q.Flag("busy");
            button.Enabled=!commandPending&&(active||(present&&q.Flag("loaded")&&state.Flag("upstreamReady")));
            button.Text=active?"Карты ZIP · СТОП":"Карты ZIP · СТАРТ";
            if(DateTime.UtcNow>=errorUntil)detail.Text=present?q.Text("status")+"\nФайлов: "+q.GetProperty("completed")+" / "+q.GetProperty("total")+" · Карт: "+q.GetProperty("maps"):"Загрузите ZIP в модуле карт";return;}
        if (autoCraft)
        {
            bool present = state.TryGetProperty("autoCraftStore", out var craft);
            bool active = present && craft.Flag("busy");
            button.Enabled = !commandPending && (active || (requested && state.Flag("upstreamReady")));
            button.Text = commandPending ? "Авто 2 · ПРИМЕНЕНИЕ…" : active ?
                (craft.Flag("running") ? "Авто 2 · СТОП" : "Авто 2 · ЗАВЕРШЕНИЕ…") : "Авто 2 · СТАРТ";
            bool selected = present && craft.Flag("running");
            if (button.Selected != selected) { button.Selected = selected; button.Invalidate(); }
            detail.Text = present ? craft.Text("status") + "\nКрафт: " + craft.GetProperty("crafted") +
                "  ·  В сундук: " + craft.GetProperty("stored") : "Ожидание Minecraft и выбранного NBT";
            return;
        }
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
    internal void SetCommandPending(bool pending)
    {
        commandPending = pending;
        if (pending) { button.Enabled = false; button.Text = "Авто 2 · ПРИМЕНЕНИЕ…"; }
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
