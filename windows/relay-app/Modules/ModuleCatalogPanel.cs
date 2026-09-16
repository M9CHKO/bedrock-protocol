namespace CpeRelay.Windows;

internal sealed class ModuleCatalogPanel : Panel
{
    internal sealed record Switch(Func<bool> IsOn, Func<bool, Task> SetOn, Func<bool>? CanChange = null);
    private readonly RelayModule[] modules;
    private readonly IReadOnlyDictionary<string, Switch> bindings;
    private readonly Dictionary<string, ModuleTile> switches = [];
    private readonly Panel catalog = new() { Dock = DockStyle.Fill, BackColor = Theme.Background };
    private readonly Panel detail = new() { Dock = DockStyle.Fill, BackColor = Theme.Background };
    private readonly FlowLayoutPanel tiles = new() { Dock = DockStyle.Fill, AutoScroll = true, Padding = new Padding(28, 4, 8, 24) };
    private readonly Panel content = new() { Dock = DockStyle.Fill };
    internal int Count => modules.Length;
    internal string? SelectedId { get; private set; }
    internal ModuleCatalogPanel(RelayModule[] modules, IReadOnlyDictionary<string, Switch> controls)
    {
        this.modules = modules; bindings = controls;
        var heading = new FlowLayoutPanel { Dock = DockStyle.Top, Height = 133, Padding = new Padding(28, 24, 20, 0),
            FlowDirection = FlowDirection.TopDown, WrapContents = false };
        heading.Controls.Add(new Label { Text = "Модули", AutoSize = true, ForeColor = Theme.Text,
            Font = new Font("Segoe UI Semibold", 27), Margin = new Padding(0, 0, 0, 9) });
        heading.Controls.Add(new Label { Text = "Включайте функции на карточках или откройте раздел для настроек.", AutoSize = true, ForeColor = Theme.Muted });
        for (int i = 0; i < modules.Length; i++)
        {
            int index = i;
            var tile = new ModuleTile(modules[i].Title, modules[i].Description, i == 0);
            if (controls.TryGetValue(modules[i].Id, out var control))
            {
                tile.ConfigureSwitch(async desired =>
                {
                    await control.SetOn(desired);
                    RefreshStates();
                }, RefreshStates);
                switches.Add(modules[i].Id, tile);
            }
            tile.OpenRequested += (_, _) => SelectModule(index); tiles.Controls.Add(tile);
        }
        catalog.Controls.Add(tiles); catalog.Controls.Add(heading);
        var backRow = new Panel { Dock = DockStyle.Top, Height = 64, Padding = new Padding(28, 14, 0, 4) };
        var back = new RelayButton { Text = "←  Все модули", Width = 160, Height = 40, Dock = DockStyle.Left };
        back.Click += (_, _) => ShowCatalog(); backRow.Controls.Add(back);
        detail.Controls.Add(content); detail.Controls.Add(backRow);
        foreach (var module in modules) { module.Visible = false; content.Controls.Add(module); }
        Controls.Add(detail); Controls.Add(catalog); ShowCatalog();
        RefreshStates();
    }
    internal void RefreshStates()
    {
        if (IsDisposed) return;
        foreach (var (id, tile) in switches)
            if (bindings.TryGetValue(id, out var control))
                tile.SetSwitchState(control.IsOn(), control.CanChange?.Invoke() ?? true);
    }
    internal void SelectModule(int index)
    {
        if (index < 0 || index >= modules.Length) throw new ArgumentOutOfRangeException(nameof(index));
        foreach (var module in modules) module.Visible = false;
        modules[index].Visible = true; modules[index].BringToFront();
        SelectedId = modules[index].Id; catalog.Visible = false; detail.Visible = true; detail.BringToFront();
    }
    internal void ShowCatalog()
    {
        SelectedId = null; detail.Visible = false; catalog.Visible = true; catalog.BringToFront();
    }
}

internal sealed class ModuleTile : Panel
{
    private readonly string description;
    private readonly bool automatic;
    private bool hover;
    private readonly ModuleSwitchControl power;
    internal event EventHandler? OpenRequested;
    internal ModuleTile(string title, string description, bool automatic)
    {
        Text = title; this.description = description; this.automatic = automatic;
        AccessibleName = title; AccessibleDescription = description;
        Width = 268; Height = 144; Margin = new Padding(0, 0, 16, 16); BackColor = Theme.Surface;
        Cursor = Cursors.Hand;
        TabStop = true;
        SetStyle(ControlStyles.Selectable | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        power = new ModuleSwitchControl(title) { Location = new Point(164, 8) };
        Controls.Add(power);
    }
    internal void ConfigureSwitch(Func<bool, Task> toggle, Action refresh)
    { power.Toggle = toggle; power.RefreshState = refresh; }
    internal void SetSwitchState(bool on, bool available) => power.SetState(on, available);
    protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnClick(EventArgs e) { OpenRequested?.Invoke(this, EventArgs.Empty); base.OnClick(e); }
    protected override void OnKeyDown(KeyEventArgs e)
    {
        if (e.KeyCode is Keys.Enter or Keys.Space)
        {
            OnClick(EventArgs.Empty);
            e.Handled = e.SuppressKeyPress = true;
            return;
        }
        base.OnKeyDown(e);
    }
    protected override void OnPaint(PaintEventArgs e)
    {
        e.Graphics.Clear(Theme.Background); e.Graphics.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
        using var path = Theme.Round(new RectangleF(1, 1, Width - 3, Height - 3), 14);
        using var fill = new SolidBrush(hover ? Theme.Input : Theme.Surface); e.Graphics.FillPath(fill, path);
        using var border = new Pen(hover ? Theme.Accent : Theme.Border); e.Graphics.DrawPath(border, path);
        using var title = new Font("Segoe UI Semibold", 14);
        using var small = new Font("Segoe UI", 9);
        TextRenderer.DrawText(e.Graphics, automatic ? "КАРТЫ" : "МОДУЛЬ", small,
            new Rectangle(20, 17, 145, 21), Theme.Muted);
        TextRenderer.DrawText(e.Graphics, Text, title, new Rectangle(20, 46, Width - 40, 29), Theme.Text, TextFormatFlags.EndEllipsis);
        TextRenderer.DrawText(e.Graphics, description, Font, new Rectangle(20, 82, Width - 40, 45), Theme.Muted,
            TextFormatFlags.WordBreak | TextFormatFlags.EndEllipsis);
        if (Focused && ShowFocusCues)
            ControlPaint.DrawFocusRectangle(e.Graphics, new Rectangle(5, 5, Width - 10, Height - 10), Theme.Text, Theme.Surface);
    }
}

internal sealed class ModuleSwitchControl : CheckBox
{
    private readonly string module;
    private readonly System.Windows.Forms.Timer animation = new() { Interval = 10 };
    private readonly Font labelFont = new("Segoe UI", 8.5f);
    private long animationStart;
    private float start, progress;
    private bool pending, available = true;
    internal Func<bool, Task>? Toggle;
    internal Action? RefreshState;
    internal ModuleSwitchControl(string module)
    {
        this.module = module;
        Size = new Size(100, 42); AutoCheck = false; Cursor = Cursors.Hand;
        AccessibleRole = AccessibleRole.CheckButton;
        SetStyle(ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer, true);
        animation.Tick += (_, _) =>
        {
            progress = CurrentProgress();
            if (progress == (Checked ? 1f : 0f)) animation.Stop();
            Invalidate();
        };
        SetState(false, true);
    }
    internal void SetState(bool enabled, bool available)
    {
        this.available = available;
        if (!pending) SetVisualState(enabled);
        Text = Checked ? "Вкл" : "Выкл";
        AccessibleName = $"{module}: переключатель";
        AccessibleDescription = !available && !Checked ? "Выключено. Для включения подключитесь к Minecraft и подготовьте модуль." :
            Checked ? "Включено" : "Выключено";
        Invalidate();
    }
    private void SetVisualState(bool enabled)
    {
        if (Checked == enabled) return;
        start = CurrentProgress();
        progress = start;
        Checked = enabled;
        animationStart = System.Diagnostics.Stopwatch.GetTimestamp();
        if (IsHandleCreated && SystemInformation.IsMenuAnimationEnabled) animation.Start();
        else { animation.Stop(); progress = enabled ? 1f : 0f; }
    }
    private float CurrentProgress()
    {
        if (!animation.Enabled) return progress;
        float elapsed = Math.Clamp((float)((System.Diagnostics.Stopwatch.GetTimestamp() - animationStart) /
            (System.Diagnostics.Stopwatch.Frequency * .32)), 0f, 1f);
        float eased = elapsed * elapsed * (3f - 2f * elapsed);
        return start + ((Checked ? 1f : 0f) - start) * eased;
    }
    protected override void OnKeyDown(KeyEventArgs e)
    {
        if (e.KeyCode == Keys.Enter) { OnClick(EventArgs.Empty); e.Handled = e.SuppressKeyPress = true; return; }
        base.OnKeyDown(e);
    }
    protected override async void OnClick(EventArgs e)
    {
        if (pending || Toggle is null) return;
        if (!available)
        {
            MessageBox.Show(FindForm(), module == "Карты из ZIP" ?
                "Подключитесь к Minecraft и загрузите ZIP с картами." :
                "Запустите реле и подключитесь к Minecraft, затем включите модуль.",
                "CPE Relay", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        bool desired = !Checked;
        pending = true;
        SetVisualState(desired);
        Text = desired ? "Вкл" : "Выкл";
        Invalidate();
        Exception? failure = null;
        try { await Toggle(desired); }
        catch (Exception error) { failure = error; }
        finally { pending = false; if (!IsDisposed) RefreshState?.Invoke(); }
        if (failure != null && !IsDisposed)
            MessageBox.Show(FindForm(), failure.Message, "CPE Relay", MessageBoxButtons.OK, MessageBoxIcon.Warning);
    }
    protected override void OnPaint(PaintEventArgs e)
    {
        progress = CurrentProgress();
        e.Graphics.Clear(Theme.Surface);
        e.Graphics.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
        Color TrackColor(float amount) => Color.FromArgb(
            (int)(Theme.Border.R + (Theme.Green.R - Theme.Border.R) * amount),
            (int)(Theme.Border.G + (Theme.Green.G - Theme.Border.G) * amount),
            (int)(Theme.Border.B + (Theme.Green.B - Theme.Border.B) * amount));
        using var track = Theme.Round(new RectangleF(4, 9, 44, 26), 13);
        using var trackFill = new SolidBrush(TrackColor(progress));
        e.Graphics.FillPath(trackFill, track);
        using var thumb = new SolidBrush(Theme.Text);
        e.Graphics.FillEllipse(thumb, 6 + 18 * progress, 11, 22, 22);
        TextRenderer.DrawText(e.Graphics, Text, labelFont, new Rectangle(54, 0, 46, Height),
            available ? Theme.Text : Theme.Muted, TextFormatFlags.VerticalCenter);
        if (Focused && ShowFocusCues)
        {
            using var focus = new Pen(Theme.Accent, 2);
            e.Graphics.DrawRectangle(focus, 1, 1, Width - 3, Height - 3);
        }
    }
    protected override void Dispose(bool disposing)
    {
        if (disposing) { animation.Dispose(); labelFont.Dispose(); }
        base.Dispose(disposing);
    }
}
