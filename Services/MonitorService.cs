using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace S4W.Services;

public record MonitorInfo(int Index, double Left, double Top, double Width, double Height);

/// <summary>
/// Enumerates physical monitors via EnumDisplayMonitors (Win32 direct P/Invoke).
/// Bypasses the WinForms Screen.AllScreens internal cache which can return stale
/// results in some DPI / multi-GPU configurations.
/// </summary>
public static class MonitorService
{
    // ── Win32 P/Invoke ──────────────────────────────────────────────────────────
    private delegate bool MonitorEnumProc(
        IntPtr hMonitor, IntPtr hdcMonitor, ref RECT lprcMonitor, IntPtr dwData);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool EnumDisplayMonitors(
        IntPtr hdc, IntPtr lprcClip, MonitorEnumProc lpfnEnum, IntPtr dwData);

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int Left, Top, Right, Bottom; }

    // ── DisplaySettingsChanged hook ─────────────────────────────────────────────
    /// <summary>
    /// Subscribe once from MainWindow.OnLoaded so monitor count refreshes
    /// automatically when the user plugs/unplugs a display.
    /// Callback receives the refreshed count.
    /// </summary>
    public static event Action<int>? MonitorCountChanged;

    /// <summary>Call once from OnLoaded to wire the OS display-settings event.</summary>
    public static void StartMonitoring()
    {
        SystemEvents.DisplaySettingsChanged += OnDisplaySettingsChanged;
    }

    public static void StopMonitoring()
    {
        SystemEvents.DisplaySettingsChanged -= OnDisplaySettingsChanged;
    }

    private static void OnDisplaySettingsChanged(object? sender, EventArgs e)
    {
        MonitorCountChanged?.Invoke(Count);
    }

    // ── Public API ──────────────────────────────────────────────────────────────
    public static List<MonitorInfo> GetMonitors()
    {
        var result = new List<MonitorInfo>();

        bool Callback(IntPtr hMonitor, IntPtr hdcMonitor, ref RECT rc, IntPtr dwData)
        {
            result.Add(new MonitorInfo(
                result.Count,
                rc.Left,
                rc.Top,
                rc.Right  - rc.Left,
                rc.Bottom - rc.Top));
            return true;
        }

        EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, Callback, IntPtr.Zero);

        // Fallback: if P/Invoke returned nothing use WinForms
        if (result.Count == 0)
        {
            var screens = System.Windows.Forms.Screen.AllScreens;
            for (int i = 0; i < screens.Length; i++)
            {
                var b = screens[i].Bounds;
                result.Add(new MonitorInfo(i, b.Left, b.Top, b.Width, b.Height));
            }
        }

        return result;
    }

    public static MonitorInfo GetMonitor(int index)
    {
        var monitors = GetMonitors();
        return (index >= 0 && index < monitors.Count) ? monitors[index] : monitors[0];
    }

    /// <summary>Live count — calls EnumDisplayMonitors on every access (no cache).</summary>
    public static int Count => GetMonitors().Count;
}
