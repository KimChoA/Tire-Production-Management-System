namespace TireEquipmentHMI.Theme;

public sealed class ModernCardPanel : Panel
{
    public ModernCardPanel()
    {
        DoubleBuffered = true;
        BackColor = UiTheme.Card;
        Padding = new Padding(20);
        Margin = new Padding(10);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        using var pen = new Pen(UiTheme.Border);
        var rect = ClientRectangle;
        rect.Width -= 1;
        rect.Height -= 1;
        e.Graphics.DrawRectangle(pen, rect);
    }
}

public static class UiFactory
{
    public static Label CardTitle(string text) => new()
    {
        Text = text,
        AutoSize = true,
        Font = new Font("Segoe UI", 10.5F, FontStyle.Bold),
        ForeColor = UiTheme.Text,
        Location = new Point(20, 18)
    };

    public static Label ValueLabel(string text, int x, int y, float size = 12F) => new()
    {
        Text = text,
        AutoSize = true,
        Font = new Font("Segoe UI", size, FontStyle.Regular),
        ForeColor = UiTheme.Text,
        Location = new Point(x, y)
    };

    public static Button ActionButton(string text, int x, int y, int width = 112) => new()
    {
        Text = text,
        Location = new Point(x, y),
        Size = new Size(width, 42),
        FlatStyle = FlatStyle.Flat,
        BackColor = UiTheme.Accent,
        ForeColor = Color.White,
        Font = new Font("Segoe UI", 9.5F, FontStyle.Bold),
        Cursor = Cursors.Hand,
        UseVisualStyleBackColor = false
    };

    public static void StyleGrid(DataGridView grid)
    {
        grid.BorderStyle = BorderStyle.None;
        grid.BackgroundColor = UiTheme.Card;
        grid.GridColor = UiTheme.Border;
        grid.RowHeadersVisible = false;
        grid.EnableHeadersVisualStyles = false;
        grid.ColumnHeadersDefaultCellStyle.BackColor = UiTheme.Sidebar;
        grid.ColumnHeadersDefaultCellStyle.ForeColor = UiTheme.Text;
        grid.ColumnHeadersDefaultCellStyle.Font = new Font("Segoe UI", 9.5F, FontStyle.Bold);
        grid.DefaultCellStyle.BackColor = UiTheme.Card;
        grid.DefaultCellStyle.ForeColor = UiTheme.Text;
        grid.DefaultCellStyle.SelectionBackColor = UiTheme.AccentSoft;
        grid.DefaultCellStyle.SelectionForeColor = UiTheme.AccentDark;
        grid.DefaultCellStyle.Font = new Font("Segoe UI", 9.5F);
        grid.RowTemplate.Height = 34;
        grid.ColumnHeadersHeight = 38;
    }
}
