using System.Windows;
using System.Windows.Media;
using S4W.Models;

namespace S4W.Services;

public class ScanlineVisual : FrameworkElement
{
    private DrawingVisual? _visual;

    public ScanlineVisual()
    {
        IsHitTestVisible = false;
    }

    public void Render(ScanlineSettings s, double canvasWidth, double canvasHeight)
    {
        if (_visual != null)
            RemoveVisualChild(_visual);

        _visual = new DrawingVisual();

        using (var dc = _visual.RenderOpen())
        {
            DrawHorizontalLines(dc, s, canvasWidth, canvasHeight);
            DrawVerticalLines(dc, s, canvasWidth, canvasHeight);
        }

        AddVisualChild(_visual);
        InvalidateMeasure();
        InvalidateVisual();
    }

    private static void DrawHorizontalLines(DrawingContext dc, ScanlineSettings s,
        double canvasWidth, double canvasHeight)
    {
        if (!s.HorizontalEnabled || s.HThickness <= 0)
            return;

        var brush = new SolidColorBrush(Color.FromArgb(
            (byte)(s.HOpacity / 100.0 * 255), 0, 0, 0));
        brush.Freeze();

        double lineWidth = s.HWidth;
        double thickness = s.HThickness;
        double gap = s.HGap;
        double startX = (canvasWidth - lineWidth) / 2.0;

        for (double y = 0; y < canvasHeight; y += thickness + gap)
        {
            if (y + thickness > canvasHeight) break;
            dc.DrawRectangle(brush, null,
                new Rect(startX, y, lineWidth, thickness));
        }
    }

    private static void DrawVerticalLines(DrawingContext dc, ScanlineSettings s,
        double canvasWidth, double canvasHeight)
    {
        if (!s.VerticalEnabled || s.VThickness <= 0)
            return;

        var brush = new SolidColorBrush(Color.FromArgb(
            (byte)(s.VOpacity / 100.0 * 255), 0, 0, 0));
        brush.Freeze();

        double lineHeight = s.VHeight;
        double thickness = s.VThickness;
        double gap = s.VGap;
        double startY = (canvasHeight - lineHeight) / 2.0;

        for (double x = 0; x < canvasWidth; x += thickness + gap)
        {
            if (x + thickness > canvasWidth) break;
            dc.DrawRectangle(brush, null,
                new Rect(x, startY, thickness, lineHeight));
        }
    }

    protected override int VisualChildrenCount => _visual != null ? 1 : 0;

    protected override Visual GetVisualChild(int index)
    {
        if (_visual == null || index != 0)
            throw new ArgumentOutOfRangeException(nameof(index));
        return _visual;
    }

    protected override Size MeasureOverride(Size availableSize) => availableSize;
    protected override Size ArrangeOverride(Size finalSize) => finalSize;
}
