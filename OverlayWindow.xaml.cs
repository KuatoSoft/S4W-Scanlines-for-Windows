using System.IO;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media.Imaging;
using S4W.Helpers;
using S4W.Models;
using S4W.Services;

namespace S4W;

public partial class OverlayWindow : Window
{
    // ── Legacy mode (procedural scanlines) ──
    private ScanlineGpuRenderer? _gpuRenderer;
    private bool _gpuInitialized;
    private bool _enableLegacyScanlines;

    /// <summary>
    /// True when the Desktop profile is active and legacy WPF scanlines should render.
    /// False for all game/emulator profiles (hook DLL handles scanlines there).
    /// </summary>
    public bool EnableLegacyScanlines
    {
        get => _enableLegacyScanlines;
        set
        {
            if (_enableLegacyScanlines == value) return;
            _enableLegacyScanlines = value;
            if (value)
            {
                ScanlineHost.Visibility = Visibility.Visible;
            }
            else
            {
                // Hide legacy scanlines and release GPU resources
                ScanlineHost.Visibility = Visibility.Collapsed;
                ScanlineHost.Children.Clear();
                _gpuRenderer?.Dispose();
                _gpuRenderer = null;
                _gpuInitialized = false;
            }
        }
    }

    public OverlayWindow()
    {
        InitializeComponent();
    }

    protected override void OnSourceInitialized(EventArgs e)
    {
        base.OnSourceInitialized(e);
        MakeClickThrough();
    }

    private void MakeClickThrough()
    {
        var hwnd = new WindowInteropHelper(this).Handle;
        int exStyle = NativeMethods.GetWindowLong(hwnd, NativeMethods.GWL_EXSTYLE);
        exStyle |= NativeMethods.WS_EX_TRANSPARENT
                 | NativeMethods.WS_EX_TOOLWINDOW
                 | NativeMethods.WS_EX_NOACTIVATE;  // never steal focus
        NativeMethods.SetWindowLong(hwnd, NativeMethods.GWL_EXSTYLE, exStyle);
    }

    /// <summary>Returns the native HWND once the window is initialized, or Zero beforehand.</summary>
    public IntPtr Handle => new WindowInteropHelper(this).Handle;

    /// <summary>
    /// Periodic maintenance — re-asserts TOPMOST z-order silently (no focus side-effect).
    /// Keeps the overlay visible above fullscreen games.
    /// </summary>
    public void BringToFront()
    {
        var hwnd = new WindowInteropHelper(this).Handle;
        if (hwnd == IntPtr.Zero) return;
        NativeMethods.SetWindowPos(
            hwnd,
            NativeMethods.HWND_TOPMOST,
            0, 0, 0, 0,
            NativeMethods.SWP_NOMOVE | NativeMethods.SWP_NOSIZE | NativeMethods.SWP_NOACTIVATE);
    }

    /// <summary>
    /// Called on fullscreen transition — temporarily strips WS_EX_NOACTIVATE so that
    /// Windows delivers a real activation event, forcing DWM to exit Independent Flip /
    /// MPO mode and re-composite normally.
    /// </summary>
    public void ForceToFrontWithFocusReturn(IntPtr gameHwnd)
    {
        var hwnd = new WindowInteropHelper(this).Handle;
        if (hwnd == IntPtr.Zero) return;

        int exStyle = NativeMethods.GetWindowLong(hwnd, NativeMethods.GWL_EXSTYLE);
        NativeMethods.SetWindowLong(hwnd, NativeMethods.GWL_EXSTYLE,
            exStyle & ~NativeMethods.WS_EX_NOACTIVATE);

        NativeMethods.SetWindowPos(
            hwnd,
            NativeMethods.HWND_TOPMOST,
            0, 0, 0, 0,
            NativeMethods.SWP_NOMOVE | NativeMethods.SWP_NOSIZE);

        if (gameHwnd != IntPtr.Zero)
            NativeMethods.SetForegroundWindow(gameHwnd);

        NativeMethods.SetWindowLong(hwnd, NativeMethods.GWL_EXSTYLE, exStyle);
    }

    public void PositionOnMonitor(int monitorIndex)
    {
        var monitor = MonitorService.GetMonitor(monitorIndex);
        Left = monitor.Left;
        Top = monitor.Top;
        Width = monitor.Width;
        Height = monitor.Height;
    }

    public void UpdateBezel(string path, double opacityPercent)
    {
        if (string.IsNullOrEmpty(path) || path == "None" || path == "Default" || !File.Exists(path))
        {
            BezelImage.Source = null;
            return;
        }

        try
        {
            var bitmap = new BitmapImage();
            bitmap.BeginInit();
            bitmap.UriSource = new Uri(path, UriKind.Absolute);
            bitmap.CacheOption = BitmapCacheOption.OnLoad;
            bitmap.EndInit();
            bitmap.Freeze();
            BezelImage.Source = bitmap;
            BezelImage.Opacity = opacityPercent / 100.0;
        }
        catch
        {
            BezelImage.Source = null;
        }
    }

    /// <summary>
    /// Updates scanline settings.
    /// — Capture mode  : delegates to capture renderer (game/emulator profiles).
    /// — Legacy mode   : delegates to WPF GPU renderer (Desktop profile only).
    /// </summary>
    public void UpdateScanlines(ScanlineSettings settings)
    {
        if (_enableLegacyScanlines)
            UpdateScanlines_Legacy(settings);
    }

    // ══════════════════════════════════════════════════════════════
    //  CAPTURE MODE — Window Capture + Scanline Shader
    // ══════════════════════════════════════════════════════════════

    // ══════════════════════════════════════════════════════════════
    //  LEGACY MODE — Procedural Scanlines (existing behavior)
    // ══════════════════════════════════════════════════════════════

    private void UpdateScanlines_Legacy(ScanlineSettings settings)
    {
        int w = (int)Width;
        int h = (int)Height;
        if (w < 1 || h < 1) return;

        if (!_gpuInitialized)
        {
            _gpuRenderer?.Dispose();
            _gpuRenderer = new ScanlineGpuRenderer();
            var image = _gpuRenderer.Initialize(w, h);
            ScanlineHost.Children.Clear();
            ScanlineHost.Children.Add(image);
            _gpuInitialized = true;
        }

        _gpuRenderer!.Render(settings, w, h);
    }

    protected override void OnClosed(EventArgs e)
    {
        base.OnClosed(e);

        _gpuRenderer?.Dispose();
        _gpuRenderer = null;
        _gpuInitialized = false;
    }
}
