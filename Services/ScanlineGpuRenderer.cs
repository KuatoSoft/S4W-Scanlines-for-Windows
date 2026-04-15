using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using Vortice.Direct3D;
using Vortice.Direct3D11;
using Vortice.DXGI;
using Vortice.D3DCompiler;
using Vortice.Mathematics;
using S4W.Models;
using WpfImage = System.Windows.Controls.Image;

namespace S4W.Services;

/// <summary>
/// GPU-accelerated scanline renderer using Direct3D 11.
/// Renders a fullscreen quad with a pixel shader that procedurally generates
/// scanlines — no screen capture, no textures, pure math.
/// The result is copied to a WriteableBitmap for WPF display.
/// GPU→CPU copy only happens on parameter change, not every frame.
/// </summary>
public sealed class ScanlineGpuRenderer : IDisposable
{
    // ── D3D11 core objects ──────────────────────────────────────────
    private ID3D11Device? _device;
    private ID3D11DeviceContext? _ctx;
    private ID3D11RenderTargetView? _rtv;
    private ID3D11Texture2D? _renderTarget;
    private ID3D11Texture2D? _stagingTexture;
    private ID3D11VertexShader? _vs;
    private ID3D11PixelShader? _ps;
    private ID3D11Buffer? _cbuffer;
    private ID3D11BlendState? _blendState;

    // ── WPF output ──────────────────────────────────────────────────
    private WriteableBitmap? _bitmap;
    private WpfImage? _wpfImage;
    private int _width;
    private int _height;
    private bool _disposed;

    // ── Constant buffer layout (must be 16-byte aligned) ────────────
    [StructLayout(LayoutKind.Sequential, Pack = 16)]
    private struct ScanlineCB
    {
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
        public float _pad0;  // 16-byte alignment padding
        public float _pad1;
    }

    // ── HLSL source (compiled at runtime via D3DCompiler) ───────────
    private const string VertexShaderSource = @"
        struct VS_OUT {
            float4 pos : SV_Position;
            float2 uv  : TEXCOORD0;
        };
        VS_OUT main(uint id : SV_VertexID) {
            // Fullscreen triangle trick: 3 vertices, no vertex buffer
            VS_OUT o;
            o.uv  = float2((id << 1) & 2, id & 2);
            o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
            return o;
        }
    ";

    private const string PixelShaderSource = @"
        cbuffer CB : register(b0) {
            float screenW;
            float screenH;
            float hThickness;
            float hGap;

            float hOpacity;
            float hStartX;
            float hWidth;
            int   hEnabled;

            float vThickness;
            float vGap;
            float vOpacity;
            float vStartY;

            float vHeight;
            int   vEnabled;
            float pad0;
            float pad1;
        };

        float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
            float px = pos.x;
            float py = pos.y;
            float alpha = 0;

            // Horizontal scanlines
            if (hEnabled && hThickness > 0 && (hThickness + hGap) > 0) {
                float hPeriod = hThickness + hGap;
                float hPos = fmod(py, hPeriod);
                float inH = step(hPos, hThickness - 0.001);
                float inBandX = step(hStartX, px) * step(px, hStartX + hWidth);
                alpha = max(alpha, inH * inBandX * hOpacity);
            }

            // Vertical scanlines
            if (vEnabled && vThickness > 0 && (vThickness + vGap) > 0) {
                float vPeriod = vThickness + vGap;
                float vPos = fmod(px, vPeriod);
                float inV = step(vPos, vThickness - 0.001);
                float inBandY = step(vStartY, py) * step(py, vStartY + vHeight);
                alpha = max(alpha, inV * inBandY * vOpacity);
            }

            return float4(0, 0, 0, alpha);
        }
    ";

    /// <summary>
    /// Initializes the D3D11 device, shaders, and output bitmap.
    /// Returns a WPF Image element to add to the visual tree.
    /// </summary>
    public WpfImage Initialize(int width, int height)
    {
        _width = Math.Max(width, 1);
        _height = Math.Max(height, 1);

        // Create D3D11 device — hardware, feature level 10.0 minimum
        D3D11.D3D11CreateDevice(
            null,
            DriverType.Hardware,
            DeviceCreationFlags.BgraSupport,
            new[] { FeatureLevel.Level_11_0, FeatureLevel.Level_10_1, FeatureLevel.Level_10_0 },
            out _device,
            out _ctx);

        CreateSizeDependentResources();
        CompileShaders();
        CreateBlendState();
        CreateConstantBuffer();

        _wpfImage = new WpfImage
        {
            Stretch = Stretch.None,
            IsHitTestVisible = false
        };

        return _wpfImage;
    }

    /// <summary>
    /// Renders scanlines with the given settings.
    /// Called from OverlayWindow.UpdateScanlines() — same entry point as before.
    /// </summary>
    public void Render(ScanlineSettings s, double canvasWidth, double canvasHeight)
    {
        if (_device == null || _ctx == null || _wpfImage == null) return;

        int w = (int)canvasWidth;
        int h = (int)canvasHeight;
        if (w < 1 || h < 1) return;

        if (w != _width || h != _height)
            Resize(w, h);

        // Build constant buffer data
        var cb = new ScanlineCB
        {
            ScreenW = _width,
            ScreenH = _height,
            HThickness = s.HThickness,
            HGap = s.HGap,
            HOpacity = s.HorizontalEnabled ? (float)(s.HOpacity / 100.0) : 0f,
            HStartX = s.HWidth > 0 ? (float)((_width  - s.HWidth)  / 2.0) : 0f,
            HWidth  = s.HWidth  > 0 ? s.HWidth  : _width,
            HEnabled = (s.HorizontalEnabled && s.HThickness > 0) ? 1 : 0,
            VThickness = s.VThickness,
            VGap = s.VGap,
            VOpacity = s.VerticalEnabled ? (float)(s.VOpacity / 100.0) : 0f,
            VStartY = s.VHeight > 0 ? (float)((_height - s.VHeight) / 2.0) : 0f,
            VHeight = s.VHeight > 0 ? s.VHeight : _height,
            VEnabled = (s.VerticalEnabled && s.VThickness > 0) ? 1 : 0,
            _pad0 = 0f,
            _pad1 = 0f
        };

        // Update constant buffer
        _ctx.UpdateSubresource(in cb, _cbuffer!, 0, 0, 0, null);

        // Bind pipeline
        _ctx.OMSetRenderTargets(_rtv!, (ID3D11DepthStencilView?)null);
        _ctx.OMSetBlendState(_blendState);
        _ctx.RSSetViewport(0, 0, _width, _height, 0f, 1f);
        _ctx.VSSetShader(_vs);
        _ctx.PSSetShader(_ps);
        _ctx.PSSetConstantBuffer(0, _cbuffer);
        _ctx.IASetPrimitiveTopology(PrimitiveTopology.TriangleList);

        // Clear to fully transparent
        var clearColor = new Color4(0, 0, 0, 0);
        _ctx.ClearRenderTargetView(_rtv!, clearColor);

        // Draw fullscreen triangle (3 vertices, no vertex buffer needed)
        _ctx.Draw(3, 0);
        _ctx.Flush();

        // Read back to WriteableBitmap
        CopyToBitmap();
    }

    /// <summary>
    /// Clears the scanline surface to fully transparent.
    /// </summary>
    public void Clear()
    {
        if (_ctx == null || _rtv == null) return;
        var clearColor = new Color4(0, 0, 0, 0);
        _ctx.ClearRenderTargetView(_rtv, clearColor);
        _ctx.Flush();
        CopyToBitmap();
    }

    // ── Private helpers ─────────────────────────────────────────────

    private void Resize(int width, int height)
    {
        _width = Math.Max(width, 1);
        _height = Math.Max(height, 1);

        _rtv?.Dispose();
        _renderTarget?.Dispose();
        _stagingTexture?.Dispose();
        _bitmap = null;

        CreateSizeDependentResources();
    }

    private void CreateSizeDependentResources()
    {
        // Render target (GPU only)
        _renderTarget = _device!.CreateTexture2D(
            Format.B8G8R8A8_UNorm, _width, _height, 1, 1, null,
            BindFlags.RenderTarget, ResourceOptionFlags.None,
            ResourceUsage.Default, CpuAccessFlags.None);

        _rtv = _device.CreateRenderTargetView(_renderTarget, null);

        // Staging texture (CPU readable)
        _stagingTexture = _device.CreateTexture2D(
            Format.B8G8R8A8_UNorm, _width, _height, 1, 1, null,
            BindFlags.None, ResourceOptionFlags.None,
            ResourceUsage.Staging, CpuAccessFlags.Read);

        // WriteableBitmap for WPF display
        _bitmap = new WriteableBitmap(_width, _height, 96, 96, PixelFormats.Bgra32, null);
    }

    private void CompileShaders()
    {
        // Vertex shader
        var vsBytecode = Compiler.Compile(
            VertexShaderSource, "main", "scanline_vs", "vs_4_0",
            ShaderFlags.None, EffectFlags.None);
        _vs = _device!.CreateVertexShader(vsBytecode.ToArray(), null);

        // Pixel shader
        var psBytecode = Compiler.Compile(
            PixelShaderSource, "main", "scanline_ps", "ps_4_0",
            ShaderFlags.None, EffectFlags.None);
        _ps = _device.CreatePixelShader(psBytecode.ToArray(), null);
    }

    private void CreateBlendState()
    {
        var desc = new BlendDescription();
        desc.RenderTarget[0] = new RenderTargetBlendDescription
        {
            BlendEnable = true,
            SourceBlend = Blend.One,
            DestinationBlend = Blend.InverseSourceAlpha,
            BlendOperation = BlendOperation.Add,
            SourceBlendAlpha = Blend.One,
            DestinationBlendAlpha = Blend.InverseSourceAlpha,
            BlendOperationAlpha = BlendOperation.Add,
            RenderTargetWriteMask = ColorWriteEnable.All
        };
        _blendState = _device!.CreateBlendState(desc);
    }

    private void CreateConstantBuffer()
    {
        int size = Marshal.SizeOf<ScanlineCB>();
        // Round up to 16-byte boundary (D3D11 requirement)
        size = (size + 15) & ~15;
        var desc = new BufferDescription(size, BindFlags.ConstantBuffer,
            ResourceUsage.Default, CpuAccessFlags.None,
            ResourceOptionFlags.None, 0);
        _cbuffer = _device!.CreateBuffer(in desc, (IntPtr)0);
    }

    private void CopyToBitmap()
    {
        if (_ctx == null || _renderTarget == null || _stagingTexture == null || _bitmap == null || _wpfImage == null)
            return;

        // GPU → staging texture
        _ctx.CopyResource(_stagingTexture, _renderTarget);

        // Map staging texture for CPU read
        _ctx.Map(_stagingTexture, 0, MapMode.Read, Vortice.Direct3D11.MapFlags.None, out var mapped);

        _bitmap.Lock();
        try
        {
            // Copy row by row (staging pitch may differ from bitmap stride)
            int bitmapStride = _bitmap.BackBufferStride;
            int copyBytes = _width * 4; // BGRA = 4 bytes per pixel
            IntPtr src = mapped.DataPointer;
            IntPtr dst = _bitmap.BackBuffer;
            for (int y = 0; y < _height; y++)
            {
                CopyMemory(dst + y * bitmapStride, src + y * mapped.RowPitch, copyBytes);
            }
            _bitmap.AddDirtyRect(new Int32Rect(0, 0, _width, _height));
        }
        finally
        {
            _bitmap.Unlock();
            _ctx.Unmap(_stagingTexture, 0);
        }

        _wpfImage.Source = _bitmap;
    }

    [DllImport("kernel32.dll", EntryPoint = "RtlMoveMemory")]
    private static extern void CopyMemory(IntPtr dest, IntPtr src, int size);

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _blendState?.Dispose();
        _cbuffer?.Dispose();
        _ps?.Dispose();
        _vs?.Dispose();
        _rtv?.Dispose();
        _renderTarget?.Dispose();
        _stagingTexture?.Dispose();
        _ctx?.Dispose();
        _device?.Dispose();
    }
}
