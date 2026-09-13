namespace CpeRelay.Windows;

// A module owns a separate screen. Navigating never starts or stops its feature.
internal abstract class RelayModule : FlowLayoutPanel
{
    internal string Id { get; }
    internal string Title { get; }
    internal string Description { get; }
    protected RelayModule(string id, string title, string description)
    {
        Id = id; Title = title; Description = description;
        Name = "module-" + id; Dock = DockStyle.Fill; AutoScroll = true;
        FlowDirection = FlowDirection.TopDown; WrapContents = false;
        Padding = new Padding(28, 16, 20, 24); BackColor = Theme.Background;
        Controls.Add(new Label { Text = title, AutoSize = true, ForeColor = Theme.Text,
            Font = new Font("Segoe UI Semibold", 25), Margin = new Padding(0, 0, 0, 8) });
        var subtitle = Note(description); subtitle.Margin = new Padding(0, 0, 0, 24); Controls.Add(subtitle);
    }
    protected void Card(params Control[] controls)
    {
        var card = new RelayCard { MinimumSize = new Size(804, 0) };
        card.Controls.AddRange(controls); Controls.Add(card);
    }
    protected static Label Note(string text) => new() { Text = text, AutoSize = true,
        MaximumSize = new Size(744, 0), ForeColor = Theme.Muted, Margin = new Padding(0, 6, 0, 10) };
    protected static Label Caption(string text) => new() { Text = text, AutoSize = true,
        ForeColor = Theme.Accent, Font = new Font("Segoe UI Semibold", 9), Margin = new Padding(0, 0, 0, 12) };
    protected static FlowLayoutPanel Row(params Control[] controls)
    {
        var row = new FlowLayoutPanel { AutoSize = true, WrapContents = false };
        row.Controls.AddRange(controls); return row;
    }
}
