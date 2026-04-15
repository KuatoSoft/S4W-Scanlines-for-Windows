using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
using S4W.Models;

namespace S4W.Services;

/// <summary>
/// Shared memory for communicating scanline settings from S4W GUI to the injected hook DLL.
/// Uses a named memory-mapped file that the hook DLL opens with OpenFileMapping.
/// Layout must match SharedMem struct in S4W_Hook.cpp exactly.
/// </summary>
public sealed class HookSharedMemory : IDisposable
{
    private MemoryMappedFile? _mmf;
    private MemoryMappedViewAccessor? _accessor;
    private bool _disposed;

    private const string MAP_NAME = "S4W_ScanlineSettings";
    // 164 (header+ScanlineCB+VHS+TapeNoise) + 4 (osdActive) + 256 (osdText: 128 wchar_t) = 424 → round up to 512
    private const int TOTAL_SIZE = 512;
    private const int OSD_OFFSET        = 164;  // byte offset of OSD block
    private const int OSD_TEXT_MAX_CHARS = 128; // wchar_t count
    private const int BORDERLESS_OFFSET = 424;  // byte offset of borderlessEnabled (int)

    // ── Shared memory layout (must match C++ SharedMem struct) ──
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    private struct SharedMemLayout
    {
        // Offset 0-15: header
        public int Active;          // 1 = apply scanlines, 0 = passthrough
        public int _Unused;         // reserved
        public int Reserved2;
        public int Reserved3;

        // Offset 16-79: ScanlineCB (64 bytes, 16-byte aligned)
        public float ScreenW;
        public float ScreenH;
        public float HThickness;
        public float HGap;

        public float HOpacity;
        public float HStartX;
        public float HWidth;
        public int HEnabled;

        public float VThickness;
        public float VGap;
        public float VOpacity;
        public float VStartY;

        public float VHeight;
        public int VEnabled;
        public float BlurEnabled;
        public float BlurIntensity;

        public float BloomEnabled;
        public float BloomIntensity;
        public float CurvatureEnabled;
        public float CurvatureIntensity;

        public float Brightness;           // -1.0 to +1.0
        public float Contrast;             // -1.0 to +1.0
        public float Saturation;           // -1.0 to +1.0
        public float Temperature;          // -1.0 to +1.0 (negative=cool/blue, positive=warm/orange)
        public float FlickerEnabled;       // 1.0 = on, 0.0 = off
        public float FlickerIntensity;     // 0.0 to 1.0
        public float FlickerRate;          // 0.0–1.0 → LFO freq 1–20 Hz
        public float BlackLevel;           // 0.0 = off, 0.3 = max crush
        public float Gamma;                // 1.0 = neutral, 0.5–2.0
        public float PhosphorEnabled;      // 1.0 = on, 0.0 = off
        public float PhosphorIntensity;    // 0.0 to 1.0

        // Offset 140: VHS + Film Grain + Tape Noise (24 bytes)
        public float VhsEnabled;           // 1.0 = on, 0.0 = off
        public float VhsIntensity;         // 0.0 to 1.0
        public float GrainIntensity;       // 0.0 to 1.0 (0 = grain off)
        public float TapeNoiseEnabled;     // 1.0 = on, 0.0 = off
        public float TapeNoiseIntensity;   // 0.0 to 1.0
        public float VignetteEnabled;      // 1.0 = edge vignette on, 0.0 = off
    }

    /// <summary>
    /// Creates the shared memory region. Must be called before injecting the hook DLL.
    /// </summary>
    public bool Create()
    {
        try
        {
            _mmf = MemoryMappedFile.CreateOrOpen(MAP_NAME, TOTAL_SIZE,
                MemoryMappedFileAccess.ReadWrite);
            _accessor = _mmf.CreateViewAccessor(0, TOTAL_SIZE,
                MemoryMappedFileAccess.ReadWrite);

            // Initialize to inactive
            var data = new SharedMemLayout { Active = 0 };
            _accessor.Write(0, ref data);

            return true;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>
    /// Updates scanline settings in shared memory. Called from UI thread whenever
    /// settings change. The hook DLL reads these on each Present call.
    /// </summary>
    public void UpdateSettings(ScanlineSettings s, int screenW, int screenH)
    {
        if (_accessor == null) return;

        var data = new SharedMemLayout
        {
            Active = 1,
            _Unused = 0,
            ScreenW = screenW,
            ScreenH = screenH,
            HThickness = s.HThickness,
            HGap = s.HGap,
            HOpacity = s.HorizontalEnabled ? (float)(s.HOpacity / 100.0) : 0f,
            // If HWidth=0 (not configured), fill entire screen width
            HStartX = s.HWidth > 0 ? (float)((screenW - s.HWidth) / 2.0) : 0f,
            HWidth  = s.HWidth > 0 ? s.HWidth : screenW,
            HEnabled = (s.HorizontalEnabled && s.HThickness > 0) ? 1 : 0,
            VThickness = s.VThickness,
            VGap = s.VGap,
            VOpacity = s.VerticalEnabled ? (float)(s.VOpacity / 100.0) : 0f,
            // If VHeight=0 (not configured), fill entire screen height
            VStartY = s.VHeight > 0 ? (float)((screenH - s.VHeight) / 2.0) : 0f,
            VHeight = s.VHeight > 0 ? s.VHeight : screenH,
            VEnabled = (s.VerticalEnabled && s.VThickness > 0) ? 1 : 0,
            BlurEnabled = (s.BlurEnabled && s.BlurIntensity > 0) ? 1.0f : 0.0f,
            BlurIntensity = (float)(s.BlurIntensity / 100.0),
            BloomEnabled = (s.BloomEnabled && s.BloomIntensity > 0) ? 1.0f : 0.0f,
            BloomIntensity = (float)(s.BloomIntensity / 100.0),
            CurvatureEnabled = (s.CurvatureEnabled && s.CurvatureIntensity > 0) ? 1.0f : 0.0f,
            CurvatureIntensity = (float)(s.CurvatureIntensity / 100.0),
            FlickerEnabled = (s.FlickerEnabled && s.FlickerIntensity > 0) ? 1.0f : 0.0f,
            FlickerIntensity = (float)(s.FlickerIntensity / 100.0),
            FlickerRate = (float)(s.FlickerRate / 100.0),
            PhosphorEnabled = (s.PhosphorEnabled && s.PhosphorIntensity > 0) ? 1.0f : 0.0f,
            PhosphorIntensity = (float)(s.PhosphorIntensity / 100.0),
            VhsEnabled         = (s.VhsEnabled && s.VhsIntensity > 0) ? 1.0f : 0.0f,
            VhsIntensity       = (float)(s.VhsIntensity / 100.0),
            GrainIntensity     = (s.GrainEnabled && s.GrainIntensity > 0) ? (float)(s.GrainIntensity / 100.0) : 0.0f,
            TapeNoiseEnabled   = (s.TapeNoiseEnabled && s.TapeNoiseIntensity > 0) ? 1.0f : 0.0f,
            TapeNoiseIntensity = (float)(s.TapeNoiseIntensity / 100.0),
            VignetteEnabled    = s.VignetteEnabled ? 1.0f : 0.0f,
            Brightness  = s.LumaEnabled ? (float)(s.BrightnessValue  / 100.0) : 0f,
            Contrast    = s.LumaEnabled ? (float)(s.ContrastValue    / 100.0) : 0f,
            Saturation  = s.LumaEnabled ? (float)(s.SaturationValue  / 100.0) : 0f,
            Temperature = s.LumaEnabled ? (float)(s.TemperatureValue / 100.0) : 0f,
            BlackLevel  = s.LumaEnabled ? (float)s.BlackLevelValue : 0f,
            Gamma       = s.LumaEnabled ? (float)s.GammaValue      : 1f,
        };

        _accessor.Write(0, ref data);

        // Write borderless request flag at fixed offset (outside the main struct)
        _accessor.Write(BORDERLESS_OFFSET, s.BorderlessEnabled ? 1 : 0);
    }

    /// <summary>
    /// Signals the hook DLL to apply or remove SDL2 borderless windowed fullscreen.
    /// Written at offset 424, read by TrySdl2Borderless() in the hook on every swap.
    /// </summary>
    public void SetBorderless(bool enabled)
    {
        _accessor?.Write(BORDERLESS_OFFSET, enabled ? 1 : 0);
    }

    /// <summary>
    /// Deactivates scanlines (sets Active=0). The hook DLL will passthrough
    /// without applying any shader.
    /// </summary>
    public void Deactivate()
    {
        if (_accessor == null) return;
        _accessor.Write(0, 0); // Active = 0 at offset 0
    }

    // ── OSD overlay (text rendered by the hook DLL on the game's back-buffer) ──

    /// <summary>Write text that the hook DLL will render as an on-screen display.</summary>
    public void WriteOsd(string text)
    {
        if (_accessor == null) return;
        _accessor.Write(OSD_OFFSET, 1); // osdActive = 1
        // Write UTF-16 (wchar_t) text + null terminator
        string clamped = text.Length > OSD_TEXT_MAX_CHARS - 1
                         ? text[..(OSD_TEXT_MAX_CHARS - 1)]
                         : text;
        byte[] bytes = System.Text.Encoding.Unicode.GetBytes(clamped + '\0');
        _accessor.WriteArray(OSD_OFFSET + 4, bytes, 0, Math.Min(bytes.Length, OSD_TEXT_MAX_CHARS * 2));
    }

    /// <summary>Hide the in-game OSD.</summary>
    public void ClearOsd()
    {
        if (_accessor == null) return;
        _accessor.Write(OSD_OFFSET, 0); // osdActive = 0
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;

        // Signal inactive before closing
        try { Deactivate(); } catch { }

        _accessor?.Dispose();
        _accessor = null;
        _mmf?.Dispose();
        _mmf = null;
    }
}
