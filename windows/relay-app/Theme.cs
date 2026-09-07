using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;

namespace CpeRelay.Windows;

internal static class Theme
{
    internal static readonly Color Background = Color.FromArgb(14, 17, 25);
    internal static readonly Color Sidebar = Color.FromArgb(19, 23, 33);
    internal static readonly Color Surface = Color.FromArgb(25, 30, 43);
    internal static readonly Color Input = Color.FromArgb(33, 39, 54);
    internal static readonly Color Border = Color.FromArgb(49, 56, 75);
    internal static readonly Color Text = Color.FromArgb(235, 238, 248);
    internal static readonly Color Muted = Color.FromArgb(151, 161, 183);
    internal static readonly Color Accent = Color.FromArgb(166, 143, 255);
    internal static readonly Color Green = Color.FromArgb(101, 219, 186);
    internal static GraphicsPath Round(RectangleF rect, float radius)
    {
        var path = new GraphicsPath(); float d = radius * 2;
        path.AddArc(rect.X, rect.Y, d, d, 180, 90); path.AddArc(rect.Right - d, rect.Y, d, d, 270, 90);
        path.AddArc(rect.Right - d, rect.Bottom - d, d, d, 0, 90); path.AddArc(rect.X, rect.Bottom - d, d, d, 90, 90);
        path.CloseFigure(); return path;
    }
    internal static void Inputs(Control root)
    {
        foreach (Control control in root.Controls)
        {
            if (control is TextBox text && text.Multiline) { text.BorderStyle = BorderStyle.None; text.BackColor = Background; text.ForeColor = Text; }
            else if (control is TextBox or ComboBox or NumericUpDown or ListBox)
            {
                control.BackColor = Input; control.ForeColor = Text;
                if (control is TextBox box) box.BorderStyle = BorderStyle.FixedSingle;
                if (control is ListBox list) { list.BorderStyle = BorderStyle.None; list.ItemHeight = 28; }
                if (control is ComboBox combo)
                {
                    combo.FlatStyle = FlatStyle.Flat; combo.DrawMode = DrawMode.OwnerDrawFixed; combo.ItemHeight = 27;
                    combo.DrawItem += (_, e) =>
                    {
                        using var fill = new SolidBrush(Input); e.Graphics.FillRectangle(fill, e.Bounds);
                        string text = e.Index >= 0 ? combo.GetItemText(combo.Items[e.Index]) ?? "" : combo.Text;
                        TextRenderer.DrawText(e.Graphics, text, combo.Font, Rectangle.Inflate(e.Bounds, -6, 0), Text,
                            TextFormatFlags.VerticalCenter | TextFormatFlags.Left);
                    };
                }
            }
            else if (control is TrackBar) control.BackColor = Surface;
            Inputs(control);
        }
    }
    [DllImport("dwmapi.dll")] private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);
    internal static void DarkTitle(Form form)
    {
        int enabled = 1;
        try { DwmSetWindowAttribute(form.Handle, 20, ref enabled, sizeof(int)); } catch (DllNotFoundException) { }
    }
}

internal sealed class RelayButton : Button
{
    internal bool Primary { get; init; }
    internal bool Selected { get; set; }
    internal bool Navigation { get; init; }
    private bool hover;
    internal RelayButton()
    {
        FlatStyle = FlatStyle.Flat; FlatAppearance.BorderSize = 0;
        SetStyle(ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        Cursor = Cursors.Hand;
    }
    protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnPaint(PaintEventArgs e)
    {
        e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
        e.Graphics.Clear(Parent?.BackColor ?? Theme.Surface);
        using var shape = Theme.Round(new RectangleF(1, 1, Width - 3, Height - 3), 9);
        Color fill = Primary ? Theme.Accent : Selected ? Color.FromArgb(46, 40, 69) : hover ? Color.FromArgb(44, 51, 69) : Navigation ? Theme.Sidebar : Theme.Input;
        if (!Enabled) fill = Color.FromArgb(36, 40, 53);
        using var brush = new SolidBrush(fill); e.Graphics.FillPath(brush, shape);
        if (!Navigation && !Primary) { using var pen = new Pen(Theme.Border); e.Graphics.DrawPath(pen, shape); }
        var textColor = !Enabled ? Theme.Muted : Primary ? Theme.Background : Selected ? Theme.Accent : Theme.Text;
        var bounds = new Rectangle(Navigation ? 17 : 7, 0, Width - (Navigation ? 25 : 14), Height);
        TextRenderer.DrawText(e.Graphics, Text, Font, bounds, textColor,
            TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | (Navigation ? TextFormatFlags.Left : TextFormatFlags.HorizontalCenter));
        if (Focused && ShowFocusCues) ControlPaint.DrawFocusRectangle(e.Graphics, Rectangle.Inflate(ClientRectangle, -5, -5), textColor, fill);
    }
}

internal sealed class RelayComboBox : ComboBox
{
    internal RelayComboBox()
    {
        SetStyle(ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
    }
    protected override void OnPaint(PaintEventArgs e)
    {
        e.Graphics.Clear(Theme.Input);
        using var pen = new Pen(Theme.Border); e.Graphics.DrawRectangle(pen, 0, 0, Width - 1, Height - 1);
        TextRenderer.DrawText(e.Graphics, Text, Font, new Rectangle(9, 0, Width - 34, Height), Enabled ? Theme.Text : Theme.Muted,
            TextFormatFlags.VerticalCenter | TextFormatFlags.Left);
        using var arrow = new Pen(Theme.Muted, 1.5f);
        e.Graphics.DrawLines(arrow, new Point[] { new(Width - 20, Height / 2 - 2), new(Width - 16, Height / 2 + 2), new(Width - 12, Height / 2 - 2) });
        if (Focused && ShowFocusCues) ControlPaint.DrawFocusRectangle(e.Graphics, Rectangle.Inflate(ClientRectangle, -4, -4));
    }
    protected override void OnSelectedIndexChanged(EventArgs e) { Invalidate(); base.OnSelectedIndexChanged(e); }
}

internal sealed class ToggleCheckBox : CheckBox
{
    internal ToggleCheckBox()
    {
        AutoSize = false; Height = 36; Width = 730; Cursor = Cursors.Hand;
        SetStyle(ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer, true);
    }
    protected override void OnPaint(PaintEventArgs e)
    {
        e.Graphics.Clear(Parent?.BackColor ?? Theme.Surface); e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
        int y = (Height - 22) / 2;
        using var path = Theme.Round(new RectangleF(0, y, 40, 22), 11);
        using var background = new SolidBrush(Checked ? Theme.Accent : Theme.Border); e.Graphics.FillPath(background, path);
        using var thumb = new SolidBrush(Checked ? Theme.Background : Theme.Muted); e.Graphics.FillEllipse(thumb, Checked ? 22 : 4, y + 4, 14, 14);
        TextRenderer.DrawText(e.Graphics, Text, Font, new Rectangle(54, 0, Width - 54, Height), Enabled ? Theme.Text : Theme.Muted, TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis);
        if (Focused && ShowFocusCues) ControlPaint.DrawFocusRectangle(e.Graphics, new Rectangle(50, 3, Width - 55, Height - 6));
    }
    protected override void OnCheckedChanged(EventArgs e) { Invalidate(); base.OnCheckedChanged(e); }
}

internal sealed class RelayCard : FlowLayoutPanel
{
    internal RelayCard()
    {
        BackColor = Theme.Surface; Padding = new Padding(22, 16, 22, 16); Margin = new Padding(0, 0, 0, 16);
        Width = 810; AutoSize = true; AutoSizeMode = AutoSizeMode.GrowAndShrink;
        FlowDirection = FlowDirection.TopDown; WrapContents = false;
        SetStyle(ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
    }
    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e); e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
        using var path = Theme.Round(new RectangleF(0.5f, 0.5f, Width - 2, Height - 2), 12);
        using var pen = new Pen(Theme.Border); e.Graphics.DrawPath(pen, path);
    }
}

internal sealed class BrandPanel : Panel
{
    internal BrandPanel() { Height = 136; Dock = DockStyle.Top; DoubleBuffered = true; }
    protected override void OnPaint(PaintEventArgs e)
    {
        e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
        using var pen = new Pen(Theme.Accent, 2.5f);
        Point[] cube = [new(30, 32), new(47, 22), new(64, 32), new(64, 53), new(47, 63), new(30, 53), new(30, 32)];
        e.Graphics.DrawLines(pen, cube); e.Graphics.DrawLine(pen, 30, 32, 47, 42); e.Graphics.DrawLine(pen, 64, 32, 47, 42); e.Graphics.DrawLine(pen, 47, 42, 47, 63);
        using var title = new Font("Segoe UI Semibold", 16); using var subtitle = new Font("Segoe UI", 8.5f);
        TextRenderer.DrawText(e.Graphics, "CPE Relay", title, new Point(78, 25), Theme.Text);
        TextRenderer.DrawText(e.Graphics, "BEDROCK / WINDOWS", subtitle, new Point(29, 91), Theme.Muted);
    }
}
