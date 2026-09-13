namespace CpeRelay.Windows;

internal sealed class ModuleCatalogPanel : Panel
{
    private readonly RelayModule[] modules;
    private readonly Panel catalog = new() { Dock = DockStyle.Fill, BackColor = Theme.Background };
    private readonly Panel detail = new() { Dock = DockStyle.Fill, BackColor = Theme.Background };
    private readonly FlowLayoutPanel tiles = new() { Dock = DockStyle.Fill, AutoScroll = true, Padding = new Padding(28, 4, 8, 24) };
    private readonly Panel content = new() { Dock = DockStyle.Fill };
    internal int Count => modules.Length;
    internal string? SelectedId { get; private set; }
    internal ModuleCatalogPanel(RelayModule[] modules)
    {
        this.modules = modules;
        var heading = new FlowLayoutPanel { Dock = DockStyle.Top, Height = 133, Padding = new Padding(28, 24, 20, 0),
            FlowDirection = FlowDirection.TopDown, WrapContents = false };
        heading.Controls.Add(new Label { Text = "Модули", AutoSize = true, ForeColor = Theme.Text,
            Font = new Font("Segoe UI Semibold", 27), Margin = new Padding(0, 0, 0, 9) });
        heading.Controls.Add(new Label { Text = "Каждая функция — в своём разделе. Выберите, что настроить.", AutoSize = true, ForeColor = Theme.Muted });
        for (int i = 0; i < modules.Length; i++)
        {
            int index = i;
            var tile = new ModuleTile(modules[i].Title, modules[i].Description, i == 0);
            tile.Click += (_, _) => SelectModule(index); tiles.Controls.Add(tile);
        }
        catalog.Controls.Add(tiles); catalog.Controls.Add(heading);
        var backRow = new Panel { Dock = DockStyle.Top, Height = 64, Padding = new Padding(28, 14, 0, 4) };
        var back = new RelayButton { Text = "←  Все модули", Width = 160, Height = 40, Dock = DockStyle.Left };
        back.Click += (_, _) => ShowCatalog(); backRow.Controls.Add(back);
        detail.Controls.Add(content); detail.Controls.Add(backRow);
        foreach (var module in modules) { module.Visible = false; content.Controls.Add(module); }
        Controls.Add(detail); Controls.Add(catalog); ShowCatalog();
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

internal sealed class ModuleTile : Button
{
    private readonly string description;
    private readonly bool automatic;
    private bool hover;
    internal ModuleTile(string title, string description, bool automatic)
    {
        Text = title; this.description = description; this.automatic = automatic;
        AccessibleName = title; AccessibleDescription = description;
        Width = 268; Height = 164; Margin = new Padding(0, 0, 16, 16);
        Cursor = Cursors.Hand; FlatStyle = FlatStyle.Flat; FlatAppearance.BorderSize = 0;
        SetStyle(ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
    }
    protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnPaint(PaintEventArgs e)
    {
        e.Graphics.Clear(Theme.Background); e.Graphics.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
        using var path = Theme.Round(new RectangleF(1, 1, Width - 3, Height - 3), 14);
        using var fill = new SolidBrush(hover ? Theme.Input : Theme.Surface); e.Graphics.FillPath(fill, path);
        using var border = new Pen(hover ? Theme.Accent : Theme.Border); e.Graphics.DrawPath(border, path);
        using var title = new Font("Segoe UI Semibold", 14);
        using var small = new Font("Segoe UI", 9);
        TextRenderer.DrawText(e.Graphics, automatic ? "АВТОМАТИЧЕСКИ" : "МОДУЛЬ", small,
            new Rectangle(20, 17, Width - 40, 21), automatic ? Theme.Green : Theme.Muted);
        TextRenderer.DrawText(e.Graphics, Text, title, new Rectangle(20, 46, Width - 40, 29), Theme.Text, TextFormatFlags.EndEllipsis);
        TextRenderer.DrawText(e.Graphics, description, Font, new Rectangle(20, 82, Width - 40, 45), Theme.Muted,
            TextFormatFlags.WordBreak | TextFormatFlags.EndEllipsis);
        TextRenderer.DrawText(e.Graphics, "Открыть  →", small, new Rectangle(20, 134, Width - 40, 20), Theme.Accent);
        if (Focused && ShowFocusCues) ControlPaint.DrawFocusRectangle(e.Graphics, Rectangle.Inflate(ClientRectangle, -6, -6));
    }
}
