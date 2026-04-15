using System.Windows;
using System.Windows.Input;
using System.Windows.Interop;
using S4W.Helpers;

namespace S4W.Services;

public class HotkeyService : IDisposable
{
    private static int _nextId = 9000;
    private readonly int _hotkeyId;
    private IntPtr _hwnd;
    private HwndSource? _source;
    private Action? _callback;
    private bool _registered;

    public HotkeyService()
    {
        _hotkeyId = Interlocked.Increment(ref _nextId);
    }

    /// <param name="allowRepeat">When true, held-down keys fire repeatedly (for CRT slider adjustments).</param>
    public void Register(Window window, ModifierKeys modifiers, Key key, Action callback,
                         bool allowRepeat = false)
    {
        Unregister();

        _hwnd = new WindowInteropHelper(window).Handle;
        _source = HwndSource.FromHwnd(_hwnd);
        _source?.AddHook(WndProc);
        _callback = callback;

        uint mod = allowRepeat ? 0u : NativeMethods.MOD_NOREPEAT;
        if (modifiers.HasFlag(ModifierKeys.Alt))     mod |= NativeMethods.MOD_ALT;
        if (modifiers.HasFlag(ModifierKeys.Control)) mod |= NativeMethods.MOD_CONTROL;
        if (modifiers.HasFlag(ModifierKeys.Shift))   mod |= NativeMethods.MOD_SHIFT;
        if (modifiers.HasFlag(ModifierKeys.Windows)) mod |= NativeMethods.MOD_WIN;

        uint vk = (uint)KeyInterop.VirtualKeyFromKey(key);
        _registered = NativeMethods.RegisterHotKey(_hwnd, _hotkeyId, mod, vk);
    }

    public bool IsRegistered => _registered;

    public void Unregister()
    {
        if (_registered && _hwnd != IntPtr.Zero)
        {
            NativeMethods.UnregisterHotKey(_hwnd, _hotkeyId);
            _registered = false;
        }
        if (_source != null)
        {
            _source.RemoveHook(WndProc);
            _source = null;
        }
    }

    private IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg == NativeMethods.WM_HOTKEY && wParam.ToInt32() == _hotkeyId)
        {
            _callback?.Invoke();
            handled = true;
        }
        return IntPtr.Zero;
    }

    public void Dispose()
    {
        Unregister();
        GC.SuppressFinalize(this);
    }
}
