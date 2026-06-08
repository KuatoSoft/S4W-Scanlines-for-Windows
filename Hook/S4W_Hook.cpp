// ═══════════════════════════════════════════════════════════════════════════
//  S4W_Hook.dll — D3D11 + OpenGL Present Hook for scanline injection
//
//  Injected into the game/emulator process by S4W.
//  Hooks:
//    - IDXGISwapChain::Present (D3D11) via vtable patching
//    - wglSwapBuffers (OpenGL) via IAT/trampoline patching
//  Applies a scanline pixel shader DIRECTLY on the game's backbuffer
//  BEFORE Present, so scanlines are part of the game's rendered frame.
//  Settings are received via shared memory from the S4W GUI.
//
//  Shader features:
//    - sin()-based anti-shimmer scanlines (smooth bell curve falloff)
//    - smoothstep option (toggled via shared memory flag)
//    - Hard step mode for classic CRT look
// ═══════════════════════════════════════════════════════════════════════════

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <d3d11.h>
#include <d3d11on12.h>   // D3D11-on-D3D12 interop (D3D12 game support)
#include <d3d12.h>       // ID3D12Device / ID3D12CommandQueue (queue capture)
#include <dxgi1_4.h>     // IDXGISwapChain3::GetCurrentBackBufferIndex (D3D12)
#include <d3d9.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <gl/GL.h>
#include <d2d1.h>
#include <d2d1_1.h>     // ID2D1DeviceContext — format-robust OSD on D3D12 backbuffer
#include <dwrite.h>
#include <wincodec.h>   // WIC for bezel PNG decoding

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

// ── Shared memory layout (must match C# HookSharedMemory.cs) ────────────
#pragma pack(push, 4)
struct SharedMem {
    int    active;                                // offset 0: 1=apply scanlines
    int    _unused;                               // offset 4: reserved
    int    reserved[2];                           // offset 8-15: padding to 16B
    // ── ScanlineCB starts at offset 16, 64 bytes, 16-byte aligned ──
    float  screenW, screenH, hThickness, hGap;    // row 0
    float  hOpacity, hStartX, hWidth;             // row 1
    int    hEnabled;
    float  vThickness, vGap, vOpacity, vStartY;   // row 2
    float  vHeight;                               // row 3
    int    vEnabled;
    float  blurEnabled;                            // 1.0 = blur on, 0.0 = off
    float  blurIntensity;                          // 0.0 to 1.0
    float  bloomEnabled;                           // 1.0 = bloom on, 0.0 = off
    float  bloomIntensity;                         // 0.0 to 1.0
    float  curvatureEnabled;                       // 1.0 = curvature on (reserved)
    float  curvatureIntensity;                     // 0.0 to 1.0 (reserved)
    float  brightness;                             // -1.0 to +1.0, 0 = neutral
    float  contrast;                               // -1.0 to +1.0, 0 = neutral
    float  saturation;                             // -1.0 to +1.0, 0 = neutral
    float  temperature;                            // -1.0 to +1.0 (neg=cool, pos=warm)
    float  flickerEnabled;                         // 1.0 = flicker on, 0.0 = off
    float  flickerIntensity;                       // 0.0 to 1.0
    float  flickerRate;                            // 0.0–1.0 → LFO freq 1–20 Hz
    float  blackLevel;                             // 0.0 = off, 0.3 = max crush
    float  gamma;                                  // 1.0 = neutral, 0.5–2.0
    float  phosphorEnabled;                        // 1.0 = on, 0.0 = off
    float  phosphorIntensity;                      // 0.0 to 1.0
    // VHS + Film Grain (offset 140, 16 bytes)
    float  vhsEnabled;                             // 1.0 = on, 0.0 = off
    float  vhsIntensity;                           // 0.0 to 1.0
    float  grainIntensity;                         // 0.0 to 1.0 (0 = grain off)
    float  tapeNoiseEnabled;                       // 1.0 = on, 0.0 = off
    float  tapeNoiseIntensity;                     // 0.0 to 1.0
    float  vignetteEnabled;                        // 1.0 = edge vignette on, 0.0 = off
    // ── OSD overlay (offset 164) ──
    int    osdActive;                              // 1 = render OSD text
    wchar_t osdText[128];                          // UTF-16 text (null-terminated)
    // ── Borderless fullscreen request (offset 424) ──
    int    borderlessEnabled;                      // 1 = request SDL2 borderless windowed fullscreen
    // ── MegaBezel reflection (offset 428) ──
    // Mirror reflection of game edges into the border zone surrounding the image.
    // Independent of bezel-art toggle. Zero-cost when megaBezelEnabled = 0.
    float  megaBezelEnabled;                       // 1.0 = on, 0.0 = off
    float  megaBezelThickness;                     // 0.0–1.0 → reflection zone width (max 10% per side)
    float  megaBezelOpacity;                       // 0.0–1.0 → reflection brightness/visibility
    float  megaBezelBlur;                          // 0.0–1.0 → blur strength applied to reflection samples
    float  megaBezelRadius;                        // 0.0–1.0 → corner radius of inner viewport (0 = sharp 45° miter)
    // ── Bezel PNG render-in-hook block (offset 448) ──
    // When set, the hook loads the PNG itself and composites it under the
    // reflection so the reflection appears ON TOP of the bezel art.
    // The C# overlay window hides its bezel image while this is active.
    float  bezelHookActive;                        // 1.0 = hook renders bezel, 0 = overlay does
    float  bezelHookOpacity;                       // 0.0–1.0 → bezel image alpha multiplier
    wchar_t bezelHookPath[260];                    // UTF-16 path (520 bytes) — empty = unload
    // ── Reflection width (offset 976) ──
    // Independent of megaBezelThickness (which sizes the bezel zone itself).
    // 0.0–1.0 = fraction of the bezel zone the reflection actually fills.
    float  megaBezelReflectionWidth;               // 0.0–1.0 → reflection extent within bezel zone
};
#pragma pack(pop)

// ── GPU constant buffer (matches cbuffer CB in HLSL) ─────────────────────
// 128 bytes = 8 x float4 rows, 16-byte aligned
struct ScanlineCBData {
    float screenW, screenH, hThickness, hGap;       // row 0
    float hOpacity, hStartX, hWidth;                 // row 1
    int   hEnabled;
    float vThickness, vGap, vOpacity, vStartY;       // row 2
    float vHeight;                                   // row 3
    int   vEnabled;
    float blurEnabled;
    float blurIntensity;
    float bloomEnabled;                              // row 4
    float bloomIntensity;
    float curvatureEnabled;
    float curvatureIntensity;
    float brightness;                                // row 5
    float contrast;
    float saturation;
    float temperature;
    float flickerEnabled;                            // row 6
    float flickerIntensity;
    float flickerRate;
    float time;
    float blackLevel;                                // row 7
    float gamma;
    float phosphorEnabled;
    float phosphorIntensity;
    float vhsEnabled;                               // row 8
    float vhsIntensity;
    float grainIntensity;
    float tapeNoiseEnabled;
    float tapeNoiseIntensity;                       // row 9
    float vignetteEnabled;
    float megaBezelEnabled;
    float megaBezelThickness;
    float megaBezelOpacity;                          // row 10
    float megaBezelBlur;
    float bezelHookActive;
    float bezelHookOpacity;
    float megaBezelRadius;                           // row 11
    float megaBezelReflectionWidth;
    float megaBezelStartFade;                        // 0.0–1.0 — startup fade-in to mask any
                                                     // initial clear-color/splash visible in the
                                                     // bezel reflection during the first ~1.5s.
    float _cbpad3;
};

// ══════════════════════════════════════════════════════════════════════════
//  D3D11 HOOK
// ══════════════════════════════════════════════════════════════════════════

// ── Present function pointer types ───────────────────────────────────────
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

// ── Forward declarations for cross-API mutual exclusion ──────────────────
// (Defined later in the OpenGL section — declared here for HookedPresent)
static bool g_GLInited = false;
static bool g_Sdl2BorderlessApplied = false;
static void TrySdl2Borderless(bool enable);

// ── Globals (D3D11) ──────────────────────────────────────────────────────
static PFN_Present       g_OrigPresent  = nullptr;
static PFN_Present1      g_OrigPresent1 = nullptr;
// Inline hook state — fallback for NVIDIA per-instance vtables (vtable patching
// only patches the dummy device's shared vtable; games with per-instance vtables
// have their own vtable copy and never hit the vtable patch).
static BYTE* g_D3D11PresentAddr   = nullptr;
static BYTE  g_D3D11PresentOrig[14]  = {};
static BYTE  g_D3D11PresentJmp[14]   = {};
static BYTE* g_D3D11Present1Addr  = nullptr;
static BYTE  g_D3D11Present1Orig[14] = {};
static BYTE  g_D3D11Present1Jmp[14]  = {};
// Hook JMP size — used by all inline hooks (D3D11, D3D9, OpenGL, GDI)
#ifdef _WIN64
static const int HOOK_JMP_SIZE = 14; // x64: FF 25 00 00 00 00 [8-byte addr]
#else
static const int HOOK_JMP_SIZE = 5;  // x86: E9 [4-byte relative offset]
#endif
static ID3D11Device*     g_Device       = nullptr;
static ID3D11DeviceContext* g_Ctx       = nullptr;
static ID3D11VertexShader*  g_VS        = nullptr;
static ID3D11PixelShader*   g_PS        = nullptr;
static ID3D11Buffer*        g_CB        = nullptr;
static ID3D11BlendState*    g_Blend     = nullptr;
static ID3D11BlendState*    g_BlendOver = nullptr;   // overwrite blend for blur mode
static ID3D11RasterizerState* g_Raster  = nullptr;
static ID3D11Texture2D*     g_BBCopy    = nullptr;   // backbuffer copy for blur sampling
static ID3D11ShaderResourceView* g_BBSRV = nullptr;
static ID3D11SamplerState*  g_Sampler   = nullptr;
// Bezel PNG cached texture (loaded by hook from path in shared mem)
static ID3D11Texture2D*           g_BezelTex  = nullptr;
static ID3D11ShaderResourceView*  g_BezelSRV  = nullptr;
static ID3D11SamplerState*        g_BezelSamp = nullptr;
static wchar_t                    g_BezelPathCached[260] = {};
static UINT                 g_LastBBW = 0, g_LastBBH = 0;
static bool              g_Inited       = false;
static HANDLE            g_MapFile      = nullptr;
static SharedMem*        g_Shared       = nullptr;
static HMODULE           g_Module       = nullptr;

// ══════════════════════════════════════════════════════════════════════════
//  D3D12 PARALLEL PATH (via D3D11On12) — ISOLATED, ZERO IMPACT ON D3D11/9/GL
// ──────────────────────────────────────────────────────────────────────────
//  D3D12 games (e.g. Mina the Hollower) share the DXGI swap-chain Present with
//  D3D11, so our existing Present hook FIRES — but sc->GetDevice(ID3D11Device)
//  returns E_NOINTERFACE because the real device is D3D12. We detect this and
//  switch to a parallel pipeline: create a D3D11 device GRAFTED onto the game's
//  D3D12 device (D3D11On12CreateDevice), wrap the D3D12 back buffer as a D3D11
//  texture, and run the EXACT SAME HLSL shader pipeline as the native D3D11
//  path. No shader rewrite. All entry points are loaded dynamically so the DLL
//  gains no static dependency on d3d12.dll — non-D3D12 games are untouched.
// ──────────────────────────────────────────────────────────────────────────
// g_GfxApi: 0 = unknown (probe), 11 = native D3D11, 12 = D3D12-on-11.
static int               g_GfxApi       = 0;
static bool              g_Inited12     = false;
// Dynamically-loaded creation entry points (no d3d12.lib import).
typedef HRESULT (WINAPI *PFN_D3D12_CREATE_DEVICE)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
typedef HRESULT (WINAPI *PFN_D3D11ON12_CREATE_DEVICE)(IUnknown*, UINT,
    const D3D_FEATURE_LEVEL*, UINT, IUnknown* const*, UINT, UINT,
    ID3D11Device**, ID3D11DeviceContext**, D3D_FEATURE_LEVEL*);
// Game objects we attach to.
static ID3D12Device*        g_D3D12Device  = nullptr;   // game's D3D12 device (from swapchain)
static ID3D12CommandQueue*  g_GameCmdQueue = nullptr;   // captured via ExecuteCommandLists hook
static IDXGISwapChain3*     g_SC3          = nullptr;   // for GetCurrentBackBufferIndex
// D3D11On12 device + our shader resources (mirror of the native-D3D11 set).
static ID3D11On12Device*    g_On12         = nullptr;
static ID3D11Device*        g_Dev11on12    = nullptr;
static ID3D11DeviceContext* g_Ctx11on12    = nullptr;
static ID3D11VertexShader*  g_VS12         = nullptr;
static ID3D11PixelShader*   g_PS12         = nullptr;
static ID3D11Buffer*        g_CB12         = nullptr;
static ID3D11BlendState*    g_Blend12      = nullptr;
static ID3D11BlendState*    g_BlendOver12  = nullptr;
static ID3D11SamplerState*  g_Sampler12    = nullptr;
static ID3D11SamplerState*  g_BezelSamp12  = nullptr;
static ID3D11RasterizerState* g_Raster12   = nullptr;
static ID3D11Texture2D*     g_BBCopy12     = nullptr;   // backbuffer copy for blur/bloom/etc.
static ID3D11ShaderResourceView* g_BBSRV12 = nullptr;
static UINT                 g_LastBBW12 = 0, g_LastBBH12 = 0;
static ID3D11Texture2D*           g_BezelTex12 = nullptr;
static ID3D11ShaderResourceView*  g_BezelSRV12 = nullptr;
static wchar_t                    g_BezelPathCached12[260] = {};
// ExecuteCommandLists vtable hook (captures the game's DIRECT command queue).
typedef void (STDMETHODCALLTYPE *PFN_ExecuteCommandLists)(
    ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
static PFN_ExecuteCommandLists g_OrigECL       = nullptr;
static void**                  g_ECLVtableSlot = nullptr;   // &vtable[10], for auto-unhook
static bool                    g_D3D12QueueHookInstalled = false;
// D2D 1.1 device context for OSD text on the D3D12 back buffer (format-robust).
// Non-fatal: if any of these fail to create, effects still work, OSD is skipped.
static ID2D1Factory1*      g_D2DFactory12 = nullptr;
static ID2D1Device*        g_D2DDevice12  = nullptr;
static ID2D1DeviceContext* g_D2DCtx12     = nullptr;

// ── GOPHER PARALLEL PATH FLAG ───────────────────────────────────────────
// Set at DllMain (before HookThread starts) when the host exe name is in the
// Gopher64 family. When TRUE, HookThread TAKES A COMPLETELY DIFFERENT CODE
// PATH — none of the D3D11/D3D9/OpenGL/DDraw/GDI main hooks are installed.
// When FALSE, HookThread is byte-for-byte identical to v1.2 behavior. This
// flag is the ONLY point where the Gopher code touches anything in this file
// outside the dedicated Gopher block.
static bool              g_IsGopher     = false;

// ── MAME PARALLEL PATH FLAG ─────────────────────────────────────────────
// Set at DllMain when the host exe is a MAME binary (mame.exe, mame64.exe,
// mame0270.exe, etc.).  When TRUE:
//   - HookedD3D9CreateDevice/Ex become PERMANENT (restore-call-repatch) and
//     call D3D9ReleaseOurResources() before the real call so MAME can create
//     its new exclusive-fullscreen device without hitting D3DERR_INVALIDCALL.
//   - The permanent hooks are also installed in the D3D9 SUCCESS path (not
//     just the fallback path).
// When FALSE: CreateDevice hooks remain one-shot deferred fallbacks — v1.2
// behavior, every other D3D9 game is completely unaffected.
static bool              g_IsMame       = false;

// ── MMF2 (CLICKTEAM) FLAG ──────────────────────────────────────────────
// Set at DllMain when the process has mmfs2.dll loaded (Clickteam Multimedia
// Fusion 2 runtime).  MMF2 games call IDirect3DDevice9::Reset multiple times
// per level transition, causing redundant shader recompilation (~2.5s each).
// When TRUE: the compiled ps_3_0 bytecode is cached in g_D3D9PSCachedBlob
// and reused across Resets instead of recompiling from source every time.
// When FALSE: every Reset recompiles from source — v1.2 behavior, every
// other D3D9 game is completely unaffected.
static bool              g_IsMMF2       = false;
static ID3DBlob*         g_D3D9PSCachedBlob = nullptr;

// ── D2D1 / DirectWrite for OSD text overlay ─────────────────────────────
static ID2D1Factory*         g_D2DFactory   = nullptr;
static IDWriteFactory*       g_DWFactory    = nullptr;
static IDWriteTextFormat*    g_OsdFormat    = nullptr;

// ── High-resolution timer for flicker animation ─────────────────────────
static LARGE_INTEGER     g_PerfFreq    = {};
static LARGE_INTEGER     g_PerfStart   = {};
static bool              g_TimeInited  = false;

static float GetTimeSeconds() {
    if (!g_TimeInited) {
        QueryPerformanceFrequency(&g_PerfFreq);
        QueryPerformanceCounter(&g_PerfStart);
        g_TimeInited = true;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (float)((double)(now.QuadPart - g_PerfStart.QuadPart) / (double)g_PerfFreq.QuadPart);
}

// MegaBezel startup fade-in (0 → 1 over 1.5s after megabezel first turns on).
// Masks any initial clear-color flash (vivid blue splash, skybox, etc.) the
// game might render in its first frames before real content appears — without
// this, the reflection mirrors the splash and looks unprofessional. Toggling
// off then on again after game launch returns to full opacity immediately
// (firstActive remains set, so saturation kicks in).
static float GetMegaBezelStartFade(bool active) {
    static float firstActive = -1.0f;
    if (!active) return 0.0f;
    if (firstActive < 0.0f) firstActive = GetTimeSeconds();
    float t = (GetTimeSeconds() - firstActive) / 1.5f;
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

// ══════════════════════════════════════════════════════════════════════════
//  DIAGNOSTIC LOGGING
//  Output: S4W_Hook_Log.txt  (same folder as S4W_Hook.dll / S4W.exe)
// ══════════════════════════════════════════════════════════════════════════

static FILE*             g_Log          = nullptr;
static CRITICAL_SECTION  g_LogCS;

static void LogInit() {
    InitializeCriticalSection(&g_LogCS);

    // Build per-process log filename: S4W_Hook_<processname>_Log.txt in
    // {DllDir}\debug\  (next to S4W.exe / S4W_Hook.dll). Each injected process
    // gets its own file — no overwriting between Ares, BatBoy, etc.
    wchar_t path[MAX_PATH] = {};
    wchar_t procName[MAX_PATH] = {};
    wchar_t baseName[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, procName, MAX_PATH)) {
        wchar_t* slash = wcsrchr(procName, L'\\');
        wchar_t* dot   = wcsrchr(procName, L'.');
        if (slash && dot && dot > slash)
            wcsncpy_s(baseName, MAX_PATH, slash + 1, dot - slash - 1);
        else if (slash)
            wcscpy_s(baseName, MAX_PATH, slash + 1);
        else
            wcscpy_s(baseName, MAX_PATH, L"unknown");
    } else {
        wcscpy_s(baseName, MAX_PATH, L"unknown");
    }

    // Locate DLL directory: {DllDir}\debug\S4W_Hook_<proc>_Log.txt
    wchar_t dllDir[MAX_PATH] = {};
    if (g_Module && GetModuleFileNameW(g_Module, dllDir, MAX_PATH)) {
        wchar_t* slash = wcsrchr(dllDir, L'\\');
        if (slash) *(slash + 1) = L'\0';
        wcscpy_s(path, MAX_PATH, dllDir);
        wcscat_s(path, MAX_PATH, L"debug");
        CreateDirectoryW(path, nullptr);
        wcscat_s(path, MAX_PATH, L"\\S4W_Hook_");
        wcscat_s(path, MAX_PATH, baseName);
        // Gopher64: launcher + child renderer share the same exe name.
        // fopen("a") holds a write-lock on Windows — the second process (child)
        // would get access denied and produce no logs. Add PID to the filename
        // so every Gopher process gets its own unique log file.
        if (_wcsicmp(baseName, L"gopher64-windows-x86_64") == 0 ||
            _wcsicmp(baseName, L"gopher64") == 0) {
            wchar_t pidBuf[32] = {};
            swprintf_s(pidBuf, 32, L"_%lu", GetCurrentProcessId());
            wcscat_s(path, MAX_PATH, pidBuf);
        }
        wcscat_s(path, MAX_PATH, L"_Log.txt");
    }
    if (!path[0])
        wcscpy_s(path, MAX_PATH, L"C:\\S4W_Hook_Log.txt");

    // Append mode — preserve previous sessions in the same file
    _wfopen_s(&g_Log, path, L"a");

    // Fallback: try next to the DLL
    if (!g_Log) {
        wchar_t dllPath[MAX_PATH];
        if (GetModuleFileNameW(g_Module, dllPath, MAX_PATH)) {
            wchar_t* ext = wcsrchr(dllPath, L'.');
            if (ext) wcscpy_s(ext, MAX_PATH - (ext - dllPath), L"_Log.txt");
            _wfopen_s(&g_Log, dllPath, L"a");
        }
    }

    if (!g_Log) return;

    // Header
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_Log,
        "============================================================\n"
        "  S4W_Hook Diagnostic Log\n"
        "  Date: %04d-%02d-%02d  %02d:%02d:%02d\n"
        "  Log file: %ls\n"
        "============================================================\n\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        path);

    // Log target process name
    wchar_t procPath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, procPath, MAX_PATH)) {
        wchar_t* slash = wcsrchr(procPath, L'\\');
        fprintf(g_Log, "[INIT] Injected into process: %ls\n", slash ? slash + 1 : procPath);
    }

    // Log DLL path
    if (GetModuleFileNameW(g_Module, path, MAX_PATH))
        fprintf(g_Log, "[INIT] DLL path: %ls\n", path);

    fprintf(g_Log, "\n");
    fflush(g_Log);
}

static void Log(const char* fmt, ...) {
    if (!g_Log) return;
    EnterCriticalSection(&g_LogCS);

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_Log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    vfprintf(g_Log, fmt, args);
    va_end(args);

    fprintf(g_Log, "\n");
    fflush(g_Log);

    LeaveCriticalSection(&g_LogCS);
}

static void LogCleanup() {
    if (g_Log) { fclose(g_Log); g_Log = nullptr; }
    DeleteCriticalSection(&g_LogCS);
}

// Log D3D9/D3D11 shader compilation error blob
static void LogShaderError(const char* stage, ID3DBlob* errBlob) {
    if (!errBlob) {
        Log("[SHADER-ERROR] %s: compile failed (no error blob)", stage);
        return;
    }
    Log("[SHADER-ERROR] %s: %s", stage,
        static_cast<const char*>(errBlob->GetBufferPointer()));
}

// ── HLSL Vertex Shader (fullscreen triangle from SV_VertexID) ────────────
static const char* VS_SRC = R"(
struct VS_OUT {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};
VS_OUT main(uint id : SV_VertexID) {
    float2 tc = float2((id << 1) & 2, id & 2);
    VS_OUT o;
    o.pos = float4(tc * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv  = tc;
    return o;
}
)";

// ── HLSL Pixel Shader — CRT scanline mask + optional Gaussian blur ───────
//
// MEGABEZEL_REFLECTION_FADE: 1 = smooth quadratic falloff of the reflection
//   from the game edge to the configured reflection width, 0 = hard cut at
//   the configured width (no fade). Recompile the hook DLL after changing.
static const char* PS_SRC = R"(
#define MEGABEZEL_REFLECTION_FADE 1
Texture2D    g_Tex      : register(t0);
SamplerState g_Samp     : register(s0);
Texture2D    g_BezelTex : register(t1);
SamplerState g_BezelSamp: register(s1);

cbuffer CB : register(b0) {
    float screenW, screenH, hThickness, hGap;
    float hOpacity, hStartX, hWidth;
    int   hEnabled;
    float vThickness, vGap, vOpacity, vStartY;
    float vHeight;
    int   vEnabled;
    float blurEnabled;
    float blurIntensity;
    float bloomEnabled;
    float bloomIntensity;
    float curvatureEnabled;
    float curvatureIntensity;
    float brightness;
    float contrast;
    float saturation;
    float temperature;
    float flickerEnabled;
    float flickerIntensity;
    float flickerRate;
    float time;
    float blackLevel;
    float gamma;
    float phosphorEnabled;
    float phosphorIntensity;
    float vhsEnabled;
    float vhsIntensity;
    float grainIntensity;
    float tapeNoiseEnabled;
    float tapeNoiseIntensity;
    float vignetteEnabled;
    float megaBezelEnabled;
    float megaBezelThickness;
    float megaBezelOpacity;
    float megaBezelBlur;
    float bezelHookActive;
    float bezelHookOpacity;
    float megaBezelRadius;
    float megaBezelReflectionWidth;
    float megaBezelStartFade;
    float _cbpad3;
};

// Tape Noise helper functions (ported from libretro static.glsl)
float tnHash(float n) { return frac(sin(n) * 43758.5453123); }
float tnN3d(float3 x) {
    float3 p = floor(x);
    float3 f = frac(x);
    f = f*f*(3.0-2.0*f);
    float n = p.x + p.y*57.0 + 113.0*p.z;
    return lerp(lerp(lerp(tnHash(n),       tnHash(n+1.0),   f.x),
                     lerp(tnHash(n+57.0),  tnHash(n+58.0),  f.x), f.y),
               lerp(lerp(tnHash(n+113.0),  tnHash(n+114.0), f.x),
                     lerp(tnHash(n+170.0), tnHash(n+171.0), f.x), f.y), f.z);
}
float tnNn(float2 p, float fc) {
    float y = p.y;
    float s = fmod(fc * 0.15, 4837.0);
    float v = tnN3d(float3(y*0.01  + s,        1.0, 1.0))
            * tnN3d(float3(y*0.011 + 1000.0+s, 1.0, 1.0))
            * tnN3d(float3(y*0.51  + 421.0+s,  1.0, 1.0));
    v *= tnHash(p.x + fc * 0.01) + 0.3;
    v = pow(v + 0.3, 1.0);
    if (v < 0.99) v = 0.0;
    return v;
}

// fw = fwidth(pos): pixel footprint in scanline-space.
// When the barrel warp stretches scanPos near the edges, fw grows.
// Once fw >= period/2 (below Nyquist) the scanlines fade to transparent
// instead of aliasing into concentric moire rings.
float scanlineIntensity(float pos, float thickness, float gap, float opacity, float fw) {
    float period = thickness + gap;
    if (period <= 0.0) return 1.0;

    // ── Anti-moiré: snap period to integer pixel boundary ──
    // When the period is non-integer (e.g. 2.7px), fmod(pos, period) drifts
    // across successive pixels, creating visible beat-frequency bands — wide
    // darker/brighter stripes across the screen.  Rounding to the nearest
    // integer pixel ensures fmod() returns the same phase pattern for every
    // repeat, eliminating the beat artifact.  Thickness is scaled
    // proportionally so the dark/bright ratio stays the same.
    float snapped = max(round(period), 1.0);
    float scale   = snapped / period;       // ≈1 when period was near-integer
    thickness    *= scale;
    period        = snapped;

    float t = fmod(pos, period);
    // Smooth edge instead of hard step — width proportional to pixel footprint
    float edge  = max(fw, 0.5);
    float inLine = 1.0 - smoothstep(thickness - edge, thickness + edge, t);
    // Fade out entirely when footprint >= half period (can't resolve the frequency)
    float vis = 1.0 - smoothstep(period * 0.3, period * 0.5, fw);
    return 1.0 - inLine * opacity * vis;
}

// Barrel distortion — simulates CRT curved glass
// r² = x² + y² (uniform spherical bowl) is the only formula that produces
// equal arc ratios on all four edges regardless of aspect ratio:
//   Δr²_right = β = 1  (y goes 0→1 along right edge)
//   Δr²_top   = α = 1  (x goes 0→1 along top  edge)  ← identical ✓
// Any α≠β skews one pair of edges, making them appear flatter or more curved.
float2 CurveUV(float2 uv, float strength) {
    float2 cc = uv * 2.0 - 1.0;
    cc *= 1.0 + strength * dot(cc, cc);
    return cc * 0.5 + 0.5;
}
)";

// ── HLSL Pixel Shader part 2 (main function) — split to stay within MSVC
//    string-literal size limit (65535 bytes per token).
static const char* PS_SRC2 = R"(
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    // ── Curvature + MegaBezel: barrel-distort UV, reflection in border ──
    // Scanlines stay in screen-space (pos.xy) — never warp them with barrel
    // distortion, otherwise the periodic grid creates Moiré interference rings.
    float2 sampleUV = uv;
    bool curved = curvatureEnabled >= 0.5 && curvatureIntensity > 0.0;
    bool megaBz = megaBezelEnabled >= 0.5;
    // MegaBezel ON: ALWAYS shrink first so the resize slider always controls
    // the visible game viewport size, even when curvature is active. Curvature
    // (if on) is then applied INSIDE the shrunken viewport.
    // MegaBezel OFF + curvature ON: classic full-screen curvature (no shrink).
    if (megaBz) {
        float margin = megaBezelThickness * 0.10;  // max 10% per side
        if (margin < 0.001) margin = 0.001;
        sampleUV.x = (uv.x - margin) / (1.0 - 2.0 * margin);
        sampleUV.y = (uv.y - margin) / (1.0 - 2.0 * margin);
        if (curved) {
            sampleUV = CurveUV(sampleUV, curvatureIntensity * 0.25);
        }
    } else if (curved) {
        sampleUV = CurveUV(uv, curvatureIntensity * 0.25);
    }

    // Rounded-rect SDF for the inner game viewport — computed in sampleUV space
    // so it works identically for non-curvature mode (sampleUV from margin shrink)
    // AND curvature mode (sampleUV from barrel distortion). The radius rounds
    // the visible corners of the game in BOTH modes.
    // REFL. RADIUS (megaBezelRadius) — rounds the reflection's INNER corner so it
    // hugs a rounded CRT bezel PNG. 0 = square inner corner (original look);
    // higher rounds the corner and lets the reflection overflow INWARD over the
    // game corner (wanted, to match a rounded bezel). Fully INDEPENDENT of GAME
    // CORNERS (vignetteEnabled), which only rounds the in-game black mask below.
    float  mbR        = megaBezelRadius * 0.025;
    float2 mbCV       = sampleUV - 0.5;
    float2 mbHalfExt  = float2(0.5, 0.5);              // game = [0,1] in sampleUV
    float2 mbQ        = abs(mbCV) - mbHalfExt + mbR;
    float  mbSDF      = length(max(mbQ, 0.0)) + min(max(mbQ.x, mbQ.y), 0.0) - mbR;

    // Reflection boundary = SQUARE viewport → clean 45° miter corners, NO radial
    // fan, NO overflow. The GAME's corners are rounded separately below (REFL.
    // RADIUS hard-mask) so the reflection stays clean and the game tucks under the
    // bezel — the only artifact-free way to match a rounded CRT bezel.
    bool outsideGame = megaBz ? (mbSDF > 0.0)
                    : (sampleUV.x < 0.0 || sampleUV.x > 1.0
                    || sampleUV.y < 0.0 || sampleUV.y > 1.0);

    if (outsideGame) {
        if (megaBz) {
            // ── Picture-frame mirror reflection ──
            // Sides: 45 deg miter (clean diagonal at corners — like a wood frame).
            // Rounded corners: radial mirror across the arc, smooth and seamless.
            float2 mUV;
            float bezelDepth;
            // ── Compute side (45° miter) reflection ──
            float depthX = sampleUV.x < 0.0 ? -sampleUV.x
                         : sampleUV.x > 1.0 ? sampleUV.x - 1.0 : 0.0;
            float depthY = sampleUV.y < 0.0 ? -sampleUV.y
                         : sampleUV.y > 1.0 ? sampleUV.y - 1.0 : 0.0;
            bool sInSide = depthX > 0.0;
            bool sInTopBot = depthY > 0.0;
            float2 mUV_side;
            if (sInSide && sInTopBot) {
                if (depthX >= depthY) {
                    mUV_side.x = sampleUV.x < 0.0 ? -sampleUV.x : 2.0 - sampleUV.x;
                    mUV_side.y = saturate(sampleUV.y);
                } else {
                    mUV_side.x = saturate(sampleUV.x);
                    mUV_side.y = sampleUV.y < 0.5 ? -sampleUV.y : 2.0 - sampleUV.y;
                }
            } else if (sInSide) {
                mUV_side.x = sampleUV.x < 0.0 ? -sampleUV.x : 2.0 - sampleUV.x;
                mUV_side.y = saturate(sampleUV.y);
            } else {
                mUV_side.x = saturate(sampleUV.x);
                mUV_side.y = sampleUV.y < 0.5 ? -sampleUV.y : 2.0 - sampleUV.y;
            }
            float bezelDepth_side = max(depthX, depthY);

            // ── Compute corner (radial through arc) reflection ──
            float2 arcCenter = sign(mbCV) * (mbHalfExt - mbR);
            float2 u         = mbCV - arcCenter;
            float  distU     = max(length(u), 1e-5);
            float2 mCV       = arcCenter + u * (2.0 * mbR - distU) / distU;
            float2 mUV_corner = mCV + 0.5;
            float bezelDepth_corner = max(distU - mbR, 0.0);

            // Smoothly blend between side and corner reflections to avoid the
            // visible seam at the boundary (mbQ.x=0 or mbQ.y=0). The blend region
            // is mbR wide; deep in the corner we fully use the radial method,
            // along the sides we fully use the miter, with a smooth transition.
            // Pure 45° miter (radial corner mirror disabled — it caused the
            // "peacock fan" artifact on bright game-corner content).
            float blendCorner = 0.0;
            bezelDepth = lerp(bezelDepth_side, bezelDepth_corner, blendCorner);
            // ── Per-axis mirror across the ROUNDED game edge (radius mbR = REFL. RADIUS) ──
            // Mirror each axis across the rounded-edge position (arc in the corner,
            // straight elsewhere). The 45-deg split fills the rounded-off corner with the
            // two side reflections meeting at the diagonal — axis-aligned, NO radial fan.
            float2 cR    = sampleUV - 0.5;
            float2 sgnR  = sign(cR);
            float2 aR    = abs(cR);
            float  arcCo = 0.5 - mbR;
            float  xEdge = (aR.y > arcCo) ? (arcCo + sqrt(max(mbR*mbR - (aR.y-arcCo)*(aR.y-arcCo), 0.0))) : 0.5;
            float  yEdge = (aR.x > arcCo) ? (arcCo + sqrt(max(mbR*mbR - (aR.x-arcCo)*(aR.x-arcCo), 0.0))) : 0.5;
            float  depthXr = aR.x - xEdge;
            float  depthYr = aR.y - yEdge;
            float2 aM_X   = float2(2.0*xEdge - aR.x, aR.y);
            float2 aM_Y   = float2(aR.x, 2.0*yEdge - aR.y);
            float  blendW = max(mbR * 0.6, 1e-4);          // soft diagonal blend (tunable: bigger = smoother)
            float2 aM     = lerp(aM_Y, aM_X, smoothstep(-blendW, blendW, depthXr - depthYr));
            mUV = saturate(0.5 + sgnR * aM);
            // Letterbox inset: crops intrinsic top/bottom black bars from the
            // reflection, but ONLY on top/bottom reflections. Side (L/R) reflections
            // keep mUV.y = sampleUV.y so content stays vertically aligned with the
            // game. insetW blends side(0)<->top/bottom(1), no corner seam.
            const float yLetterboxInset = 0.02;
            float insetW = 1.0 - smoothstep(-blendW, blendW, depthXr - depthYr);
            mUV.y = lerp(mUV.y, yLetterboxInset + mUV.y * (1.0 - 2.0 * yLetterboxInset), insetW);
            // 7x7 Gaussian blur (49 taps), texel-spaced — wide smooth blur
            // without ghosting. Sigma scales BOTH spread and weight falloff.
            float sigma = megaBezelBlur * 5.0 + 0.0001;
            float2 texel = 1.0 / float2(screenW, screenH);
            float3 reflColor;
            if (sigma > 0.05) {
                float3 sum = float3(0, 0, 0);
                float  wSum = 0.0;
                [unroll] for (int dy = -3; dy <= 3; dy++) {
                    [unroll] for (int dx = -3; dx <= 3; dx++) {
                        float d2 = float(dx*dx + dy*dy);
                        float w  = exp(-d2 / (2.0 * sigma * sigma));
                        sum  += g_Tex.Sample(g_Samp, saturate(mUV + float2(dx, dy) * texel * sigma)).rgb * w;
                        wSum += w;
                    }
                }
                reflColor = sum / wSum;
            } else {
                reflColor = g_Tex.Sample(g_Samp, mUV).rgb;
            }
            // ── Reflection width + fade (CURVED-DEPTH per-axis, miter-aligned) ──
            //
            // Both the mirror miter (the sharp 45° seam where side and top/bot
            // mirrors meet at corners) and the fade contour use the SAME depth
            // metric: per-axis distance OUTSIDE [0,1]² in the curved sampleUV
            // space. They share the same comparison (depthX vs depthY in curved
            // sampleUV), so the fade boundary cleanly follows the miter line on
            // all 4 corners.
            //
            // Per-axis normalization uses the curved depth at the SCREEN EDGE
            // for the current pixel's row/column — computed by re-running
            // CurveUV on synthetic edge positions (uv.x=0, uv.x=1, uv.y=0,
            // uv.y=1, same other axis). This gives:
            //
            //   • Slider 100% reaches the physical screen edge on all 4 sides,
            //     curvature ON or OFF.
            //   • Without curvature, CurveUV() is the identity → math is
            //     mathematically identical to the original (pre-changes)
            //     behavior — clean 45° miter restored.
            //   • With curvature, the fade covers the FULL visible bezel zone
            //     (from the curved game edge to the screen edge), so no
            //     full-opacity "plateau" between game edge and uv=margin.
            //   • Top↔bottom symmetric (CurveUV is symmetric around 0.5).
            //   • Miter and fade contour both pass through uv.x == uv.y at
            //     corners (by curvature's diagonal symmetry).
            float marginRef     = max(megaBezelThickness * 0.10, 0.001);
            float gameWpx       = max(screenW * (1.0 - 2.0 * marginRef), 1.0);
            float gameHpx       = max(screenH * (1.0 - 2.0 * marginRef), 1.0);
            float reflW         = max(megaBezelReflectionWidth, 0.001);
            float invShrink     = 1.0 / max(1.0 - 2.0 * marginRef, 1e-5);
            float curvStrength  = curved ? curvatureIntensity * 0.25 : 0.0;

            // Synthetic pre-margin sampleUV at each screen edge for current row/col.
            float2 sUV_left  = float2((0.0 - marginRef) * invShrink,
                                       (uv.y - marginRef) * invShrink);
            float2 sUV_right = float2((1.0 - marginRef) * invShrink,
                                       (uv.y - marginRef) * invShrink);
            float2 sUV_top   = float2((uv.x - marginRef) * invShrink,
                                       (0.0 - marginRef) * invShrink);
            float2 sUV_bot   = float2((uv.x - marginRef) * invShrink,
                                       (1.0 - marginRef) * invShrink);
            // Apply curvature (identity if curved=false → CurveUV w/ strength=0).
            float2 csu_left  = curved ? CurveUV(sUV_left,  curvStrength) : sUV_left;
            float2 csu_right = curved ? CurveUV(sUV_right, curvStrength) : sUV_right;
            float2 csu_top   = curved ? CurveUV(sUV_top,   curvStrength) : sUV_top;
            float2 csu_bot   = curved ? CurveUV(sUV_bot,   curvStrength) : sUV_bot;

            // Current pixel's curved per-axis depth from game edge.
            float depthXc = sampleUV.x < 0.0 ? -sampleUV.x
                          : sampleUV.x > 1.0 ? sampleUV.x - 1.0 : 0.0;
            float depthYc = sampleUV.y < 0.0 ? -sampleUV.y
                          : sampleUV.y > 1.0 ? sampleUV.y - 1.0 : 0.0;

            // Per-axis curved depth at the screen edge in the same row/col.
            // Branch on which side the current pixel is (left vs right, top vs bot)
            // so each axis normalizes against its own physical screen edge.
            float maxDepthX = sampleUV.x < 0.0 ? -csu_left.x
                            : sampleUV.x > 1.0 ? csu_right.x - 1.0 : 1.0;
            float maxDepthY = sampleUV.y < 0.0 ? -csu_top.y
                            : sampleUV.y > 1.0 ? csu_bot.y - 1.0 : 1.0;

            // Aspect-correction scale on the SHORTER bezel-zone axis so the
            // fade rate (in screen pixels per unit fade) matches the LONGER
            // axis. Without this, on a 16:9 screen the top/bottom 54-px bezel
            // fades over 54 px while the 96-px sides fade over 96 px — the
            // user perceives top/bottom as "more abrupt / cut short".
            // With this, both axes fade at the same pixel rate; on the shorter
            // axis the fade gradient extends past the screen edge and gets
            // naturally clipped, leaving a brighter residual near the screen
            // edge — exactly the user's "augmenter la distance T/B" request.
            // Side mirror region (where X depth dominates) is unaffected
            // because dxN there is unchanged → L/R look stays identical.
            float aspect = screenW / max(screenH, 1.0);
            float xFadeScale = max(1.0 / aspect, 1.0);  // > 1 only on portrait
            float yFadeScale = max(aspect, 1.0);        // > 1 only on landscape

            // Per-axis fade (miter-style) — the radius only shapes the game
            // boundary (outsideGame) and the sampling coordinate (mUV blend);
            // the fade gradient always follows 45° diagonals regardless of
            // radius, keeping the reflection unified with no corner seams.
            // Corner reflection fade follows the ARC (radial distance from the
            // rounded game corner), mirrored from the D3D9 path so the reflection
            // hugs the rounded corner instead of the square viewport. Sides keep
            // the 45° miter fade; blendCorner cross-fades between them.
            float2 dirCorn  = u / max(distU, 1e-5);
            float corMaxX   = (maxDepthX + mbR) / max(abs(dirCorn.x), 0.01);
            float corMaxY   = (maxDepthY + mbR) / max(abs(dirCorn.y), 0.01);
            float corMaxRef = max(min(corMaxX, corMaxY) - mbR, 1e-5);
            float normDepth_corner = saturate(bezelDepth_corner / max(corMaxRef * reflW, 1e-5));
            float dxN = depthXc / max(maxDepthX * xFadeScale, 1e-5);
            float dyN = depthYc / max(maxDepthY * yFadeScale, 1e-5);
            float normDepth_side = saturate(max(dxN, dyN) / reflW);
            float normDepth = lerp(normDepth_side, normDepth_corner, blendCorner);
        #if MEGABEZEL_REFLECTION_FADE
            // Smooth quadratic falloff: 1.0 at the game edge → 0.0 at reflPx.
            float fade = 1.0 - normDepth;
            fade = fade * fade;
        #else
            // Hard cut: full reflection inside reflPx, nothing beyond.
            float fade = step(normDepth, 1.0);
        #endif
            // Startup fade-in masks any initial clear-color flash (e.g. a vivid
            // blue splash or skybox) the game shows in its first frames — the
            // reflection ramps up from 0 over ~1.5s instead of mirroring it
            // straight away.
            float startFade = saturate(megaBezelStartFade);
            float3 reflected = reflColor * megaBezelOpacity * fade * startFade;
            // Composite reflection ON TOP of bezel PNG when hook owns the
            // bezel rendering. Standard "over" operator: bezel is the
            // background, reflection is the foreground at megaBezelOpacity*fade.
            if (bezelHookActive >= 0.5) {
                float4 bz = g_BezelTex.Sample(g_BezelSamp, uv);
                bz.rgb *= bezelHookOpacity;
                float ra = megaBezelOpacity * fade * startFade;
                return float4(reflected + bz.rgb * bz.a * (1.0 - ra), 1.0);
            }
            return float4(reflected, 1.0);
        }
        // Curvature with no MegaBezel: black outside (matches v1.2 behavior).
        if (curved) return float4(0, 0, 0, 1);
    }
)";

// PS_SRC2b: remainder of pixel shader main() — split to stay under MSVC limit.
static const char* PS_SRC2b = R"(
    float mask = 1.0;
    // Pixel footprint in screen-space — constant ~1.0, used for smooth scanline edges
    float fwY = fwidth(pos.y);
    float fwX = fwidth(pos.x);

    if (hEnabled && hThickness > 0 && (hThickness + hGap) > 0) {
        float inBandX = step(hStartX, pos.x) * step(pos.x, hStartX + hWidth);
        if (inBandX > 0.5)
            mask *= scanlineIntensity(pos.y, hThickness, hGap, hOpacity, fwY);
    }

    if (vEnabled && vThickness > 0 && (vThickness + vGap) > 0) {
        float inBandY = step(vStartY, pos.y) * step(pos.y, vStartY + vHeight);
        if (inBandY > 0.5)
            mask *= scanlineIntensity(pos.x, vThickness, vGap, vOpacity, fwX);
    }

    bool needTex = blurEnabled >= 0.5 || bloomEnabled >= 0.5 || curved || megaBz
                || flickerEnabled >= 0.5 || phosphorEnabled >= 0.5
                || abs(brightness) > 0.001 || abs(contrast) > 0.001
                || abs(saturation) > 0.001 || abs(temperature) > 0.001
                || blackLevel > 0.001 || abs(gamma - 1.0) > 0.001
                || vhsEnabled >= 0.5 || grainIntensity > 0.001
                || tapeNoiseEnabled >= 0.5 || vignetteEnabled > 0.0
                || bezelHookActive >= 0.5;
    if (!needTex) {
        if (vignetteEnabled > 0.0) {
            float  r    = max(vignetteEnabled * 0.10, 0.022);  // radius floored at fade width so the fade hugs the edge at all settings (gradient unchanged)
            float2 qv   = abs(uv - 0.5) - 0.5 + r;
            float  rSDF = length(max(qv, 0.0)) + min(max(qv.x, qv.y), 0.0) - r;
            float2 outN;
            if (qv.x > 0.0 && qv.y > 0.0) outN = normalize(qv);
            else if (qv.x > 0.0)          outN = float2(1.0, 0.0);
            else                           outN = float2(0.0, 1.0);
            float fadeW = length(float2(outN.x * 0.008, outN.y * 0.020));
            mask *= smoothstep(0.0, fadeW, -rSDF);
        }
        return float4(mask, mask, mask, 1.0);
    }

    float2 texel = 1.0 / float2(screenW, screenH);

    // NOTE: previous versions cropped 2% off the game's top/bottom here to hide
    // the game's intrinsic letterbox. That hardcoded crop also stretched games
    // WITHOUT letterbox, eating HUD pixels. Removed: the game is now sampled
    // unstretched at its original aspect ratio. The reflection mirror still
    // uses its own 2% inset (mUV remap) so it samples real game art.

    // Sample game frame at (distorted) UV
    float4 color = g_Tex.Sample(g_Samp, sampleUV);
    // Raw source luminance — saved BEFORE any processing so it reflects the
    // original game pixel value.  Used as a content mask: perfectly-black
    // letterbox bars have rawSourceLuma≈0 and get tape-noise/VHS suppressed,
    // while actual game pixels (even dark ones) have enough luma to pass through.
    float rawSourceLuma = dot(color.rgb, float3(0.299, 0.587, 0.114));

    // Gaussian blur (must run before B/C/S/T — blur re-samples original texture)
    if (blurEnabled >= 0.5) {
        float sigma = blurIntensity * 2.0 + 0.0001;
        float4 blurred = float4(0, 0, 0, 0);
        float totalW = 0;
        [unroll] for (int y = -2; y <= 2; y++) {
            [unroll] for (int x = -2; x <= 2; x++) {
                float d2 = float(x*x + y*y);
                float w = exp(-d2 / (2.0 * sigma * sigma));
                blurred += g_Tex.Sample(g_Samp, sampleUV + float2(x, y) * texel * sigma) * w;
                totalW += w;
            }
        }
        color = blurred / totalW;
    }

    // Bloom: extract bright pixels, add glow (re-samples original texture)
    if (bloomEnabled >= 0.5) {
        float sigma = bloomIntensity * 3.0 + 0.5;
        float4 glow = float4(0, 0, 0, 0);
        float wTotal = 0;
        [unroll] for (int by = -3; by <= 3; by++) {
            [unroll] for (int bx = -3; bx <= 3; bx++) {
                float4 s = g_Tex.Sample(g_Samp, sampleUV + float2(bx, by) * texel * sigma);
                float lum = dot(s.rgb, float3(0.299, 0.587, 0.114));
                float bright = saturate((lum - 0.4) * 2.5);
                glow += s * bright;
                wTotal += bright;
            }
        }
        if (wTotal > 0.001) glow /= wTotal;
        color.rgb += glow.rgb * bloomIntensity * 0.6;
        color = saturate(color);
    }

    // Brightness/Contrast/Saturation/Temperature — applied after blur/bloom
    color.rgb *= 1.0 + brightness;
    color.rgb = (color.rgb - 0.5) * (1.0 + contrast) + 0.5;
    float luma = dot(color.rgb, float3(0.299, 0.587, 0.114));
    color.rgb = lerp(float3(luma, luma, luma), color.rgb, 1.0 + saturation);
    // Temperature: shift warm (positive) = boost red, cut blue; cool (negative) = boost blue, cut red
    color.r += temperature * 0.1;
    color.b -= temperature * 0.1;
    color = saturate(color);

    // Phosphor Glow — tight micro-halo around bright pixels (CRT phosphor persistence)
    if (phosphorEnabled >= 0.5) {
        float4 glow = float4(0, 0, 0, 0);
        float wTotal = 0;
        [unroll] for (int gy = -2; gy <= 2; gy++) {
            [unroll] for (int gx = -2; gx <= 2; gx++) {
                float d2 = float(gx*gx + gy*gy);
                float w = exp(-d2 / 1.28); // sigma = 0.8 px — tight halo
                float4 s = g_Tex.Sample(g_Samp, sampleUV + float2(gx, gy) * texel);
                float lp = dot(s.rgb, float3(0.2126, 0.7152, 0.0722));
                float bright = saturate((lp - 0.3) * 3.0);
                glow += s * (w * bright);
                wTotal += w * bright;
            }
        }
        if (wTotal > 0.001)
            color.rgb += (glow.rgb / wTotal) * phosphorIntensity * 0.3;
        color = saturate(color);
    }

    // Vignette: subtle edge darkening that follows the curvature
    if (curved) {
        float2 vigUV = sampleUV * 2.0 - 1.0;
        float vig = 1.0 - dot(vigUV, vigUV) * 0.3 * curvatureIntensity;
        color.rgb *= saturate(vig);
    }

    // Sub-pixel dither — breaks 8-bit quantization banding on gradients/vignette.
    // Uses screen-space position (pos.xy) so the noise is always pixel-aligned,
    // never warped by the barrel distortion.
    // IGN (Interleaved Gradient Noise) — less structured than sin-hash.
    float dither = frac(52.9829189 * frac(dot(pos.xy, float2(0.06711056, 0.00583715))));
    color.rgb += (dither - 0.5) * (1.0 / 255.0);

    // Apply scanline mask
    float4 output = color * mask;

    // Final grade: Black Level + Gamma — applied last as fine-tuning
    // Black Level: luminance-masked crush — only affects shadows below ~30% luma
    if (blackLevel > 0.001) {
        float lum = dot(output.rgb, float3(0.2126, 0.7152, 0.0722));
        float darkMask = 1.0 - smoothstep(0.0, 0.3, lum);
        output.rgb = max(output.rgb - blackLevel * darkMask, 0.0);
    }
    // Gamma: power curve on midtones only (R=G=B factor → no hue drift)
    if (abs(gamma - 1.0) > 0.001)
        output.rgb = pow(max(output.rgb, 0.0001), 1.0 / gamma);

    // CRT Flicker — smooth sine LFO with vertical phase offset
    if (flickerEnabled >= 0.5) {
        float freq = 1.0 + flickerRate * 19.0; // slider 0→1 = 1→20 Hz
        float phase = uv.y * 0.15;
        float flicker = sin(time * 6.28318 * freq + phase) * 0.05 * flickerIntensity;
        output.rgb *= 1.0 + flicker;
    }

    // VHS tape effect — realistic analog tape degradation + NTSC composite artifacts
    if (vhsEnabled >= 0.5 && uv.y <= 0.96) {
        float inten = vhsIntensity;
        float t     = time;
        // outsideGame follows curvature + MegaBezel resize boundary exactly.
        // rawSourceLuma is a secondary gate for in-game letterbox bars.
        float contentMask = outsideGame ? 0.0 : smoothstep(0.0, 0.015, rawSourceLuma);

        // — 1. LINE JITTER: sparse spike lines only (no global sinusoidal shift) —
        float lineIdx = floor(uv.y * 720.0);
        float sH    = frac(sin(lineIdx * 127.1 + floor(t * 10.0) * 311.7) * 43758.5);
        float spike = (sH > 0.97) ? (sH - 0.97) / 0.03 * 0.016 - 0.008 : 0.0;
        float2 jUV  = sampleUV + float2(spike * inten, 0.0);

        float4 s0   = g_Tex.Sample(g_Samp, jUV);

        // — 3. NTSC DOT CRAWL: animated color shimmer on chroma edges —
        float luma0  = dot(s0.rgb, float3(0.299, 0.587, 0.114));
        float chrMag = saturate(length(s0.rgb - luma0) * 2.5);
        float dotPhi = (uv.x * 240.0 + floor(t * 29.97) * 0.5) * 3.14159265;
        output.r += sin(dotPhi)         * inten * 0.008 * chrMag * contentMask;
        output.b += sin(dotPhi + 1.047) * inten * 0.006 * chrMag * contentMask;

        // — 4. LUMA NOISE: horizontal tape hiss streaks —
        float ny     = floor(uv.y * 200.0);
        float nx     = floor(uv.x * 15.0);
        float nt     = floor(t * 25.0);
        float streak = frac(sin(dot(float2(nx + ny * 200.0, nt), float2(127.1, 311.7))) * 43758.5) - 0.5;
        output.rgb  += streak * inten * 0.022 * contentMask;

        // — 5. HEAD-SWITCHING BAND: bottom ~3% of screen, mechanical artifact —
        float headZone = smoothstep(0.97, 1.00, uv.y) * contentMask;
        float headH    = frac(sin(floor(uv.y * 300.0) * 127.1 + floor(t * 30.0) * 311.7) * 43758.5);
        float headOff  = (headH - 0.5) * inten * 0.030;
        float4 headS   = g_Tex.Sample(g_Samp, jUV + float2(headOff, 0.0));
        output.rgb     = lerp(output.rgb, headS.rgb * mask + headH * inten * 0.35, headZone);

        // — 6. COLOR GRADING: desaturation + luma lift + warm shadows —
        float vLuma = dot(output.rgb, float3(0.299, 0.587, 0.114));
        output.rgb  = lerp(output.rgb, float3(vLuma, vLuma, vLuma), inten * 0.25 * contentMask);
        output.rgb *= lerp(1.0, 1.06, inten * contentMask);               // analog luma lift
        output.r   += inten * 0.020 * (1.0 - vLuma) * contentMask;        // warm shadows
        output.b   -= inten * 0.014 * (1.0 - vLuma) * contentMask;        // cool shadows
        output      = saturate(output);
    }

    // Film Grain — true per-pixel noise, hash without sine (no periodic banding)
    if (grainIntensity > 0.001) {
        float contentMask = outsideGame ? 0.0 : smoothstep(0.0, 0.015, rawSourceLuma);
        float lum   = dot(output.rgb, float3(0.299, 0.587, 0.114));
        float3 p3   = frac(float3(pos.xy, floor(time * 24.0) + 1.0)
                         * float3(0.1031, 0.1030, 0.0973));
        p3         += dot(p3, p3.yzx + 33.33);
        float grain = frac((p3.x + p3.y) * p3.z) * 2.0 - 1.0;
        float amp   = 1.0 - (2.0 * lum - 1.0) * (2.0 * lum - 1.0);
        output.rgb += grain * grainIntensity * 0.18 * amp * contentMask;
        output      = saturate(output);
    }

    // Tape Noise — libretro-style analog tape interference spikes
    // contentMask suppresses the effect on letterbox black bars (rawSourceLuma≈0).
    // The smoothstep range [0, 0.015] is narrow enough that even dim game content
    // gets full-strength noise; only perfectly-black pixels are excluded.
    if (tapeNoiseEnabled >= 0.5) {
        float contentMask = outsideGame ? 0.0 : smoothstep(0.0, 0.015, rawSourceLuma);
        float fc     = time * 24.0;
        float2 tnUV  = uv * screenH * 4.0;
        float col    = tnNn(tnUV, fc) * contentMask;
        output.rgb  += clamp(float3(col, col, col), 0.0, 0.5) * tapeNoiseIntensity;
        output       = saturate(output);
    }

    // Edge vignette with rounded corners.
    // SDF drives the fade entirely: gradient follows arc in corners, sides get
    // per-axis fade widths (X=0.012, Y=0.033). outN = outward normal at the
    // nearest boundary point — interpolates between axes around the arc.
    if (vignetteEnabled > 0.0) {
        float  r    = max(vignetteEnabled * 0.10, 0.022);  // radius floored at fade width so the fade hugs the edge at all settings (gradient unchanged)
        float2 qv   = abs(sampleUV - 0.5) - 0.5 + r;
        float  rSDF = length(max(qv, 0.0)) + min(max(qv.x, qv.y), 0.0) - r;
        float2 outN;
        if (qv.x > 0.0 && qv.y > 0.0) outN = normalize(qv);
        else if (qv.x > 0.0)          outN = float2(1.0, 0.0);
        else                           outN = float2(0.0, 1.0);
        float fadeW = length(float2(outN.x * 0.008, outN.y * 0.020));
        output.rgb *= smoothstep(0.0, fadeW, -rSDF);
    }

    // REFL. RADIUS: round the game window's corners FOR REAL — hard-mask the
    // rounded-off corner to black so the game stops showing square corners. The
    // bezel composited just below covers it (as on a real CRT). mbSDF = rounded-
    // rect SDF with radius mbR (= REFL. RADIUS); >0 only in the corner triangle,
    // 0 on straight edges. No-op when REFL. RADIUS=0.
    if (megaBz && mbSDF > 0.0) output.rgb = float3(0.0, 0.0, 0.0);

    // Bezel PNG overlay (inside-game pixels). Standard alpha "over" composite
    // so the bezel art layers ON TOP of the rendered game frame. Reflection
    // pixels in the border zone already returned earlier with bezel composited.
    if (bezelHookActive >= 0.5) {
        float4 bz = g_BezelTex.Sample(g_BezelSamp, uv);
        float a = bz.a * bezelHookOpacity;
        output.rgb = output.rgb * (1.0 - a) + bz.rgb * a;
    }

    return output;
}
)";

// ── Combined PS source (PS_SRC + PS_SRC2 + PS_SRC2b) — built once ────────
// Each variable is a SEPARATE static const char* declaration (NOT inline
// )" R"( splits within one declaration — those merge before C2026 check).
static char* g_PS_SRC_FULL = nullptr;
static const char* GetPSSrc() {
    if (!g_PS_SRC_FULL) {
        size_t l1 = strlen(PS_SRC), l2 = strlen(PS_SRC2), l3 = strlen(PS_SRC2b);
        g_PS_SRC_FULL = (char*)malloc(l1 + l2 + l3 + 1);
        if (g_PS_SRC_FULL) {
            memcpy(g_PS_SRC_FULL,           PS_SRC,   l1);
            memcpy(g_PS_SRC_FULL + l1,      PS_SRC2,  l2);
            memcpy(g_PS_SRC_FULL + l1 + l2, PS_SRC2b, l3 + 1);
        } else {
            return PS_SRC;
        }
    }
    return g_PS_SRC_FULL;
}

// ── D3D11 State save/restore (minimal set for fullscreen draw) ──────────
struct SavedState {
    D3D11_VIEWPORT           vp;
    UINT                     numVP;
    ID3D11RenderTargetView*  rtv;
    ID3D11DepthStencilView*  dsv;
    ID3D11BlendState*        blend;
    FLOAT                    blendFactor[4];
    UINT                     sampleMask;
    ID3D11RasterizerState*   raster;
    ID3D11VertexShader*      vs;
    ID3D11PixelShader*       ps;
    ID3D11Buffer*            psCB;
    D3D_PRIMITIVE_TOPOLOGY   topo;
    ID3D11InputLayout*       layout;
    ID3D11ShaderResourceView* psSRV;
    ID3D11SamplerState*      psSampler;
};

static void SaveState(ID3D11DeviceContext* ctx, SavedState& s) {
    s.numVP = 1;
    ctx->RSGetViewports(&s.numVP, &s.vp);
    ctx->OMGetRenderTargets(1, &s.rtv, &s.dsv);
    ctx->OMGetBlendState(&s.blend, s.blendFactor, &s.sampleMask);
    ctx->RSGetState(&s.raster);
    ctx->VSGetShader(&s.vs, nullptr, nullptr);
    ctx->PSGetShader(&s.ps, nullptr, nullptr);
    ctx->PSGetConstantBuffers(0, 1, &s.psCB);
    ctx->IAGetPrimitiveTopology(&s.topo);
    ctx->IAGetInputLayout(&s.layout);
    ctx->PSGetShaderResources(0, 1, &s.psSRV);
    ctx->PSGetSamplers(0, 1, &s.psSampler);
}

static void RestoreState(ID3D11DeviceContext* ctx, SavedState& s) {
    ctx->RSSetViewports(s.numVP, &s.vp);
    ctx->OMSetRenderTargets(1, &s.rtv, s.dsv);
    ctx->OMSetBlendState(s.blend, s.blendFactor, s.sampleMask);
    ctx->RSSetState(s.raster);
    ctx->VSSetShader(s.vs, nullptr, 0);
    ctx->PSSetShader(s.ps, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &s.psCB);
    ctx->IASetPrimitiveTopology(s.topo);
    ctx->IASetInputLayout(s.layout);
    ctx->PSSetShaderResources(0, 1, &s.psSRV);
    ctx->PSSetSamplers(0, 1, &s.psSampler);
    if (s.psSRV) s.psSRV->Release();
    if (s.psSampler) s.psSampler->Release();
    if (s.rtv)    s.rtv->Release();
    if (s.dsv)    s.dsv->Release();
    if (s.blend)  s.blend->Release();
    if (s.raster) s.raster->Release();
    if (s.vs)     s.vs->Release();
    if (s.ps)     s.ps->Release();
    if (s.psCB)   s.psCB->Release();
    if (s.layout) s.layout->Release();
}

// ── Open shared memory ───────────────────────────────────────────────────
static bool OpenSharedMem() {
    if (g_Shared) return true;
    g_MapFile = OpenFileMappingW(FILE_MAP_READ, FALSE, L"S4W_ScanlineSettings");
    if (g_MapFile) {
        g_Shared = (SharedMem*)MapViewOfFile(g_MapFile, FILE_MAP_READ, 0, 0, 0);
    }
    if (g_Shared) {
        Log("[SHMEM] Connected. active=%d screen=%.0fx%.0f hEnabled=%d hThick=%.1f hGap=%.1f hOpacity=%.2f hStartX=%.0f hWidth=%.0f",
            g_Shared->active, g_Shared->screenW, g_Shared->screenH,
            g_Shared->hEnabled, g_Shared->hThickness, g_Shared->hGap, g_Shared->hOpacity,
            g_Shared->hStartX, g_Shared->hWidth);
        Log("[SHMEM] vEnabled=%d vThick=%.1f vGap=%.1f vOpacity=%.2f vStartY=%.0f vHeight=%.0f",
            g_Shared->vEnabled, g_Shared->vThickness, g_Shared->vGap, g_Shared->vOpacity,
            g_Shared->vStartY, g_Shared->vHeight);
    } else {
        Log("[SHMEM] FAILED — S4W shared memory not found (is S4W.exe running?)");
    }
    return g_Shared != nullptr;
}

// ── Initialize D3D11 resources using the game's device ──────────────────
static bool InitResources(IDXGISwapChain* sc) {
    Log("[D3D11] InitResources — acquiring device from SwapChain");
    HRESULT hr = sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_Device);
    if (FAILED(hr) || !g_Device) { Log("[D3D11] FAIL: GetDevice hr=0x%08X", hr); return false; }
    g_Device->GetImmediateContext(&g_Ctx);
    if (!g_Ctx) { Log("[D3D11] FAIL: GetImmediateContext returned null"); return false; }

    // Get swap chain dimensions for log
    DXGI_SWAP_CHAIN_DESC scDesc = {};
    sc->GetDesc(&scDesc);
    Log("[D3D11] SwapChain: %ux%u fmt=%u bufs=%u windowed=%d",
        scDesc.BufferDesc.Width, scDesc.BufferDesc.Height,
        scDesc.BufferDesc.Format, scDesc.BufferCount, scDesc.Windowed);

    // Compile vertex shader
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* errBlob = nullptr;
    hr = D3DCompile(VS_SRC, strlen(VS_SRC), "vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr) || !vsBlob) { LogShaderError("D3D11 VS", errBlob); if (errBlob) errBlob->Release(); return false; }
    if (errBlob) errBlob->Release();
    hr = g_Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_VS);
    vsBlob->Release();
    if (FAILED(hr)) { Log("[D3D11] FAIL: CreateVertexShader hr=0x%08X", hr); return false; }

    // Compile pixel shader
    ID3DBlob* psBlob = nullptr;
    const char* psSrc = GetPSSrc();
    hr = D3DCompile(psSrc, strlen(psSrc), "ps", nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr) || !psBlob) { LogShaderError("D3D11 PS", errBlob); if (errBlob) errBlob->Release(); return false; }
    if (errBlob) errBlob->Release();
    hr = g_Device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_PS);
    psBlob->Release();
    if (FAILED(hr)) { Log("[D3D11] FAIL: CreatePixelShader hr=0x%08X", hr); return false; }

    // Constant buffer — 13 x float4 = 208 bytes
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 192; // 12 x float4 (rows 0-11)
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = g_Device->CreateBuffer(&cbd, nullptr, &g_CB);
    if (FAILED(hr)) return false;

    // Multiplicative blend state: Output = Backbuffer * ShaderMask
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend  = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
    bd.RenderTarget[0].BlendOp   = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = g_Device->CreateBlendState(&bd, &g_Blend);
    if (FAILED(hr)) return false;

    // Overwrite blend state: Output = ShaderOutput (for blur mode — shader writes final color)
    D3D11_BLEND_DESC bdOver = {};
    bdOver.RenderTarget[0].BlendEnable = FALSE;
    bdOver.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = g_Device->CreateBlendState(&bdOver, &g_BlendOver);
    if (FAILED(hr)) return false;

    // Linear sampler for blur texture sampling
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = g_Device->CreateSamplerState(&sd, &g_Sampler);
    if (FAILED(hr)) return false;
    // Bezel sampler — same params; separate object so we can switch independently
    hr = g_Device->CreateSamplerState(&sd, &g_BezelSamp);
    if (FAILED(hr)) return false;

    // Rasterizer: no culling, no scissor
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = FALSE;
    rd.DepthClipEnable = TRUE;
    hr = g_Device->CreateRasterizerState(&rd, &g_Raster);
    if (FAILED(hr)) return false;

    OpenSharedMem();

    // ── D2D1 / DirectWrite for OSD text rendering ──
    if (!g_D2DFactory) {
        HRESULT hrd = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_D2DFactory);
        if (FAILED(hrd)) { Log("[OSD] D2D1CreateFactory FAILED hr=0x%08X", hrd); g_D2DFactory = nullptr; }
    }
    if (!g_DWFactory) {
        HRESULT hrd = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_DWFactory));
        if (FAILED(hrd)) { Log("[OSD] DWriteCreateFactory FAILED hr=0x%08X", hrd); g_DWFactory = nullptr; }
    }
    if (g_DWFactory && !g_OsdFormat) {
        g_DWFactory->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            22.0f, L"en-us", &g_OsdFormat);
        if (g_OsdFormat) {
            g_OsdFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            g_OsdFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            Log("[OSD] DirectWrite text format created OK");
        }
    }

    g_Inited = true;
    Log("[D3D11] Resources initialized OK");
    return true;
}

// ── Apply scanlines to the game's backbuffer ─────────────────────────────
// ── Bezel PNG → D3D11 texture (WIC decode, cached) ───────────────────────
static void ReleaseBezelTexture() {
    if (g_BezelSRV) { g_BezelSRV->Release(); g_BezelSRV = nullptr; }
    if (g_BezelTex) { g_BezelTex->Release(); g_BezelTex = nullptr; }
    g_BezelPathCached[0] = 0;
}

static bool LoadBezelTexture(ID3D11Device* dev, const wchar_t* path) {
    ReleaseBezelTexture();
    if (!path || !path[0]) {
        Log("[BEZEL] LoadBezelTexture skipped: path=%p path[0]=%d",
            path, (path ? (int)path[0] : -1));
        return false;
    }

    // CoInitialize for this thread (safe to call repeatedly)
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IWICImagingFactory*    wic       = nullptr;
    IWICBitmapDecoder*     decoder   = nullptr;
    IWICBitmapFrameDecode* frame     = nullptr;
    IWICFormatConverter*   converter = nullptr;
    BYTE*                  pixels    = nullptr;
    bool ok = false;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(hr)) { Log("[BEZEL] CoCreateInstance(WIC) hr=0x%08X", hr); goto cleanup; }

    hr = wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) { Log("[BEZEL] decode '%ls' hr=0x%08X", path, hr); goto cleanup; }

    if (FAILED(decoder->GetFrame(0, &frame))) goto cleanup;
    if (FAILED(wic->CreateFormatConverter(&converter))) goto cleanup;
    if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) goto cleanup;

    UINT w = 0, h = 0;
    converter->GetSize(&w, &h);
    if (w == 0 || h == 0) goto cleanup;

    UINT stride = w * 4;
    UINT imgSize = stride * h;
    pixels = new BYTE[imgSize];
    if (FAILED(converter->CopyPixels(nullptr, stride, imgSize, pixels))) goto cleanup;

    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w; td.Height = h;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc = { 1, 0 };
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd = { pixels, stride, 0 };
        if (FAILED(dev->CreateTexture2D(&td, &sd, &g_BezelTex))) goto cleanup;
        if (FAILED(dev->CreateShaderResourceView(g_BezelTex, nullptr, &g_BezelSRV))) goto cleanup;
    }

    wcscpy_s(g_BezelPathCached, 260, path);
    Log("[BEZEL] Loaded '%ls' (%ux%u) into D3D11 texture", path, w, h);
    ok = true;

cleanup:
    if (pixels)    delete[] pixels;
    if (converter) converter->Release();
    if (frame)     frame->Release();
    if (decoder)   decoder->Release();
    if (wic)       wic->Release();
    if (!ok)       ReleaseBezelTexture();
    return ok;
}

static bool g_D3D11FirstFrame = true;
static void ApplyScanlines(IDXGISwapChain* sc) {
    if (!g_Shared) { OpenSharedMem(); }
    if (!g_Shared) {
        if (g_D3D11FirstFrame) { Log("[D3D11] ApplyScanlines: no shared memory, skipping"); g_D3D11FirstFrame = false; }
        return;
    }
    if (!g_Shared->active) {
        if (g_D3D11FirstFrame) { Log("[D3D11] ApplyScanlines: active=0 (scanlines disabled from S4W UI)"); g_D3D11FirstFrame = false; }
        return;
    }
    // Fast-path: active=1 but every individual effect is off → nothing to render, skip GPU work entirely.
    // This avoids shader pipeline overhead on games where the profile is "attached" but all effects are at 0.
    {
        bool anyEffect =
            g_Shared->hEnabled ||
            g_Shared->vEnabled ||
            (g_Shared->blurEnabled      > 0.0f && g_Shared->blurIntensity      > 0.0f) ||
            (g_Shared->bloomEnabled     > 0.0f && g_Shared->bloomIntensity     > 0.0f) ||
            (g_Shared->curvatureEnabled > 0.0f && g_Shared->curvatureIntensity > 0.0f) ||
            (g_Shared->flickerEnabled   > 0.0f && g_Shared->flickerIntensity   > 0.0f) ||
            (g_Shared->phosphorEnabled  > 0.0f && g_Shared->phosphorIntensity  > 0.0f) ||
            (g_Shared->vhsEnabled       > 0.0f) ||
            (g_Shared->grainIntensity   > 0.0f) ||
            (g_Shared->tapeNoiseEnabled > 0.0f && g_Shared->tapeNoiseIntensity > 0.0f) ||
            (g_Shared->vignetteEnabled  > 0.0f) ||
            (g_Shared->megaBezelEnabled > 0.0f) ||
            (g_Shared->bezelHookActive  > 0.0f) ||
            g_Shared->brightness  != 0.0f || g_Shared->contrast    != 0.0f ||
            g_Shared->saturation  != 0.0f || g_Shared->temperature != 0.0f ||
            g_Shared->blackLevel  > 0.0f  ||
            (g_Shared->gamma != 1.0f && g_Shared->gamma != 0.0f) ||
            g_Shared->osdActive;
        if (!anyEffect) return;
    }
    if (!g_Ctx || !g_Device) return;

    if (g_D3D11FirstFrame) {
        Log("[D3D11] First scanline draw: hEnabled=%d hThick=%.1f hGap=%.1f hOpacity=%.2f ",
            g_Shared->hEnabled, g_Shared->hThickness, g_Shared->hGap, g_Shared->hOpacity);
        g_D3D11FirstFrame = false;
    }

    // Get backbuffer
    ID3D11Texture2D* backbuffer = nullptr;
    HRESULT hr = sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer);
    if (FAILED(hr) || !backbuffer) return;

    // Copy backbuffer when blur, bloom, curvature, brightness/contrast/saturation or flicker is active
    bool blurOn    = g_Shared->blurEnabled       > 0.0f && g_Shared->blurIntensity       > 0.0f;
    bool bloomOn   = g_Shared->bloomEnabled      > 0.0f && g_Shared->bloomIntensity      > 0.0f;
    bool curvOn    = g_Shared->curvatureEnabled  > 0.0f && g_Shared->curvatureIntensity  > 0.0f;
    bool bcOn      = g_Shared->brightness != 0.0f || g_Shared->contrast != 0.0f || g_Shared->saturation != 0.0f || g_Shared->temperature != 0.0f
                  || g_Shared->blackLevel > 0.0f || (g_Shared->gamma != 1.0f && g_Shared->gamma != 0.0f);
    bool flickOn   = g_Shared->flickerEnabled    > 0.0f && g_Shared->flickerIntensity    > 0.0f;
    bool phosphOn  = g_Shared->phosphorEnabled  > 0.0f && g_Shared->phosphorIntensity  > 0.0f;
    bool vhsOn        = g_Shared->vhsEnabled        > 0.0f;
    bool grainOn      = g_Shared->grainIntensity    > 0.0f;
    bool tapeNoiseOn  = g_Shared->tapeNoiseEnabled  > 0.0f && g_Shared->tapeNoiseIntensity > 0.0f;
    bool megaBzOn     = g_Shared->megaBezelEnabled  > 0.0f;
    bool bezelHookOn  = g_Shared->bezelHookActive   > 0.0f;

    // ── Startup blackout (megabezel only) ──
    // For the first ~1.5s after megabezel first becomes active, blank the
    // entire backbuffer to BLACK and skip our shader. Hides the game's launch
    // splash / loading clear-color (vivid blue, skybox, etc.) which otherwise
    // is what gets reflected into the bezel zone AND shines through as random
    // color flashes inside the game viewport during the first few frames.
    // After blackout ends, normal rendering kicks in and the megaBezelStartFade
    // uniform smoothly ramps the reflection up over its own 1.5s window
    // → total reveal time at game launch is ~3s of clean black-to-game.
    if (megaBzOn) {
        static float blackoutStart = -1.0f;
        const float BLACKOUT_SECONDS = 1.5f;
        if (blackoutStart < 0.0f) blackoutStart = GetTimeSeconds();
        if ((GetTimeSeconds() - blackoutStart) < BLACKOUT_SECONDS) {
            ID3D11RenderTargetView* rtvBlackout = nullptr;
            if (SUCCEEDED(g_Device->CreateRenderTargetView(backbuffer, nullptr, &rtvBlackout))) {
                float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                g_Ctx->ClearRenderTargetView(rtvBlackout, black);
                rtvBlackout->Release();
            }
            backbuffer->Release();
            return;
        }
    }

    // megaBzOn / bezelHookOn force needCopy: reflection + bezel composite both
    // require overwrite blend (alpha compositing in the shader) and a source
    // texture bound at slot t0 (g_Tex for game), so we must copy the backbuffer.
    bool needCopy  = blurOn || bloomOn || curvOn || bcOn || flickOn || phosphOn || vhsOn || grainOn || tapeNoiseOn || megaBzOn || bezelHookOn;
    if (needCopy) {
        D3D11_TEXTURE2D_DESC bbDesc;
        backbuffer->GetDesc(&bbDesc);
        UINT bbW2 = bbDesc.Width, bbH2 = bbDesc.Height;
        // Recreate copy texture if dimensions changed
        if (!g_BBCopy || g_LastBBW != bbW2 || g_LastBBH != bbH2) {
            if (g_BBSRV) { g_BBSRV->Release(); g_BBSRV = nullptr; }
            if (g_BBCopy) { g_BBCopy->Release(); g_BBCopy = nullptr; }
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = bbW2; td.Height = bbH2;
            td.MipLevels = 1; td.ArraySize = 1;
            td.Format = bbDesc.Format;
            td.SampleDesc = { 1, 0 };
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (SUCCEEDED(g_Device->CreateTexture2D(&td, nullptr, &g_BBCopy))) {
                g_Device->CreateShaderResourceView(g_BBCopy, nullptr, &g_BBSRV);
                g_LastBBW = bbW2; g_LastBBH = bbH2;
                Log("[D3D11] Blur: created backbuffer copy %ux%u fmt=%u", bbW2, bbH2, bbDesc.Format);
            }
        }
        if (g_BBCopy) {
            // Handle MSAA: if backbuffer is multisampled, resolve to our non-MSAA copy
            if (bbDesc.SampleDesc.Count > 1)
                g_Ctx->ResolveSubresource(g_BBCopy, 0, backbuffer, 0, bbDesc.Format);
            else
                g_Ctx->CopyResource(g_BBCopy, backbuffer);
        }
    }

    // Create RTV for backbuffer
    ID3D11RenderTargetView* rtv = nullptr;
    hr = g_Device->CreateRenderTargetView(backbuffer, nullptr, &rtv);
    backbuffer->Release();
    if (FAILED(hr) || !rtv) return;

    // Get backbuffer dimensions for viewport
    DXGI_SWAP_CHAIN_DESC scDesc;
    sc->GetDesc(&scDesc);
    D3D11_VIEWPORT vp = { 0, 0,
        (float)scDesc.BufferDesc.Width, (float)scDesc.BufferDesc.Height,
        0.0f, 1.0f };

    // Update constant buffer — INJECTION MODE: use full backbuffer dimensions
    // The game's backbuffer IS the content, no band offset needed.
    float bbW = (float)scDesc.BufferDesc.Width;
    float bbH = (float)scDesc.BufferDesc.Height;

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = g_Ctx->Map(g_CB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        ScanlineCBData cb;
        cb.screenW    = bbW;
        cb.screenH    = bbH;
        cb.hThickness = g_Shared->hThickness;
        cb.hGap       = g_Shared->hGap;
        cb.hOpacity   = g_Shared->hOpacity;
        cb.hStartX    = 0.0f;     // full backbuffer — no band offset
        cb.hWidth     = bbW;      // full backbuffer width
        cb.hEnabled   = g_Shared->hEnabled;
        cb.vThickness = g_Shared->vThickness;
        cb.vGap       = g_Shared->vGap;
        cb.vOpacity   = g_Shared->vOpacity;
        cb.vStartY    = 0.0f;     // full backbuffer — no band offset
        cb.vHeight    = bbH;      // full backbuffer height
        cb.vEnabled   = g_Shared->vEnabled;
        cb.blurEnabled       = g_Shared->blurEnabled;
        cb.blurIntensity     = g_Shared->blurIntensity;
        cb.bloomEnabled      = g_Shared->bloomEnabled;
        cb.bloomIntensity    = g_Shared->bloomIntensity;
        cb.curvatureEnabled  = g_Shared->curvatureEnabled;
        cb.curvatureIntensity = g_Shared->curvatureIntensity;
        cb.brightness        = g_Shared->brightness;
        cb.contrast          = g_Shared->contrast;
        cb.saturation        = g_Shared->saturation;
        cb.temperature       = g_Shared->temperature;
        cb.flickerEnabled    = g_Shared->flickerEnabled;
        cb.flickerIntensity  = g_Shared->flickerIntensity;
        cb.flickerRate       = g_Shared->flickerRate;
        cb.time              = GetTimeSeconds();
        cb.blackLevel        = g_Shared->blackLevel;
        cb.gamma             = g_Shared->gamma;
        cb.phosphorEnabled   = g_Shared->phosphorEnabled;
        cb.phosphorIntensity = g_Shared->phosphorIntensity;
        cb.vhsEnabled        = g_Shared->vhsEnabled;
        cb.vhsIntensity      = g_Shared->vhsIntensity;
        cb.grainIntensity    = g_Shared->grainIntensity;
        cb.tapeNoiseEnabled  = tapeNoiseOn ? 1.0f : 0.0f;
        cb.tapeNoiseIntensity = g_Shared->tapeNoiseIntensity;
        cb.vignetteEnabled    = g_Shared->vignetteEnabled;
        cb.megaBezelEnabled   = g_Shared->megaBezelEnabled;
        cb.megaBezelThickness = g_Shared->megaBezelThickness;
        cb.megaBezelOpacity   = g_Shared->megaBezelOpacity;
        cb.megaBezelBlur      = g_Shared->megaBezelBlur;
        cb.bezelHookActive    = (bezelHookOn && g_BezelSRV) ? g_Shared->bezelHookActive : 0.0f;
        cb.bezelHookOpacity   = g_Shared->bezelHookOpacity;
        cb.megaBezelRadius    = g_Shared->megaBezelRadius;
        cb.megaBezelReflectionWidth = g_Shared->megaBezelReflectionWidth;
        cb.megaBezelStartFade = GetMegaBezelStartFade(g_Shared->megaBezelEnabled > 0.0f);
        cb._cbpad3 = 0.0f;
        memcpy(mapped.pData, &cb, sizeof(ScanlineCBData));
        g_Ctx->Unmap(g_CB, 0);
    }

    // Save game's D3D11 state
    SavedState saved;
    memset(&saved, 0, sizeof(saved));
    SaveState(g_Ctx, saved);

    // Set our scanline render state
    float blendFactor[4] = { 1, 1, 1, 1 };
    g_Ctx->OMSetRenderTargets(1, &rtv, nullptr);
    // Blur on: overwrite blend (shader writes final blurred+masked color)
    // Blur off: multiplicative blend (dest * mask)
    g_Ctx->OMSetBlendState(needCopy ? g_BlendOver : g_Blend, blendFactor, 0xFFFFFFFF);
    g_Ctx->RSSetViewports(1, &vp);
    g_Ctx->RSSetState(g_Raster);
    g_Ctx->VSSetShader(g_VS, nullptr, 0);
    g_Ctx->PSSetShader(g_PS, nullptr, 0);
    g_Ctx->PSSetConstantBuffers(0, 1, &g_CB);
    if (needCopy && g_BBSRV && g_Sampler) {
        g_Ctx->PSSetShaderResources(0, 1, &g_BBSRV);
        g_Ctx->PSSetSamplers(0, 1, &g_Sampler);
    }
    // Bezel texture (slot 1). Reload when shared-mem path changes.
    // Also retry if texture is null but path is non-empty (race condition).
    if (bezelHookOn) {
        bool pathChanged = wcscmp(g_Shared->bezelHookPath, g_BezelPathCached) != 0;
        if (pathChanged || (!g_BezelSRV && g_Shared->bezelHookPath[0]))
            LoadBezelTexture(g_Device, g_Shared->bezelHookPath);
        if (g_BezelSRV && g_BezelSamp) {
            g_Ctx->PSSetShaderResources(1, 1, &g_BezelSRV);
            g_Ctx->PSSetSamplers(1, 1, &g_BezelSamp);
        }
    } else if (g_BezelTex) {
        // bezel turned off in shared mem → drop cached texture
        ReleaseBezelTexture();
    }
    g_Ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_Ctx->IASetInputLayout(nullptr);

    // Draw fullscreen triangle — scanlines baked into backbuffer via multiplicative blend
    g_Ctx->Draw(3, 0);

    // Restore game's state
    RestoreState(g_Ctx, saved);

    rtv->Release();
}

// ── OSD text overlay (D2D1 — rendered independently of scanlines) ────────
static void RenderOsd(IDXGISwapChain* sc) {
    if (!g_Shared) { OpenSharedMem(); }
    if (!g_Shared || !g_Shared->osdActive || !g_Shared->osdText[0]) return;
    if (!g_D2DFactory || !g_DWFactory || !g_OsdFormat) return;

    // Get backbuffer dimensions
    DXGI_SWAP_CHAIN_DESC scDesc;
    if (FAILED(sc->GetDesc(&scDesc))) return;
    float bbW = (float)scDesc.BufferDesc.Width;
    float bbH = (float)scDesc.BufferDesc.Height;

    IDXGISurface* surface = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(IDXGISurface), (void**)&surface))) return;

    // Try premultiplied alpha first, fall back to ignore
    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ID2D1RenderTarget* d2dRT = nullptr;
    HRESULT hr = g_D2DFactory->CreateDxgiSurfaceRenderTarget(surface, &rtProps, &d2dRT);
    if (FAILED(hr)) {
        rtProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
        hr = g_D2DFactory->CreateDxgiSurfaceRenderTarget(surface, &rtProps, &d2dRT);
    }
    if (FAILED(hr) || !d2dRT) { surface->Release(); return; }

    // Scale font with resolution (base 22px at 1080p)
    float scale = bbH / 1080.0f;
    if (scale < 0.8f) scale = 0.8f;

    UINT len = (UINT)wcsnlen(g_Shared->osdText, 127);
    IDWriteTextLayout* layout = nullptr;
    g_DWFactory->CreateTextLayout(g_Shared->osdText, len, g_OsdFormat,
        bbW * 0.8f, 100.0f, &layout);

    if (layout) {
        layout->SetFontSize(22.0f * scale, { 0, len });

        DWRITE_TEXT_METRICS tm;
        layout->GetMetrics(&tm);

        float padX = 32.0f * scale;
        float padY = 14.0f * scale;
        float boxW = tm.width + padX * 2;
        float boxH = tm.height + padY * 2;
        float x = (bbW - boxW) / 2.0f;      // centred horizontally
        float y = bbH * 0.05f;               // 5% from top

        ID2D1SolidColorBrush* bgBrush = nullptr;
        ID2D1SolidColorBrush* borderBrush = nullptr;
        ID2D1SolidColorBrush* txtBrush = nullptr;
        // Dark background matching WPF: ARGB(210, 14, 17, 38)
        d2dRT->CreateSolidColorBrush(D2D1::ColorF(14.0f/255, 17.0f/255, 38.0f/255, 210.0f/255), &bgBrush);
        // Pink border: ARGB(140, 255, 42, 255)
        d2dRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.165f, 1.0f, 140.0f/255), &borderBrush);
        // White text
        d2dRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &txtBrush);

        d2dRT->BeginDraw();
        float cr = 10.0f * scale;
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF(x, y, x + boxW, y + boxH), cr, cr);
        d2dRT->FillRoundedRectangle(rr, bgBrush);
        d2dRT->DrawRoundedRectangle(rr, borderBrush, 1.0f * scale);
        d2dRT->DrawTextLayout(D2D1::Point2F(x + padX, y + padY), layout, txtBrush);
        d2dRT->EndDraw();

        if (txtBrush) txtBrush->Release();
        if (borderBrush) borderBrush->Release();
        if (bgBrush) bgBrush->Release();
        layout->Release();
    }
    d2dRT->Release();
    surface->Release();
}

// ══════════════════════════════════════════════════════════════════════════
//  D3D12 PIPELINE (D3D11On12) — mirrors the native-D3D11 functions above but
//  renders onto the game's D3D12 back buffer through a grafted D3D11 device.
// ══════════════════════════════════════════════════════════════════════════

// ── Bezel PNG → D3D11On12 texture (isolated copy of LoadBezelTexture) ─────
static void ReleaseBezelTexture12() {
    if (g_BezelSRV12) { g_BezelSRV12->Release(); g_BezelSRV12 = nullptr; }
    if (g_BezelTex12) { g_BezelTex12->Release(); g_BezelTex12 = nullptr; }
    g_BezelPathCached12[0] = 0;
}
static bool LoadBezelTexture12(ID3D11Device* dev, const wchar_t* path) {
    ReleaseBezelTexture12();
    if (!dev || !path || !path[0]) return false;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory*    wic       = nullptr;
    IWICBitmapDecoder*     decoder   = nullptr;
    IWICBitmapFrameDecode* frame     = nullptr;
    IWICFormatConverter*   converter = nullptr;
    BYTE*                  pixels    = nullptr;
    bool ok = false;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(hr)) goto cleanup;
    hr = wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) { Log("[D3D12-BEZEL] decode '%ls' hr=0x%08X", path, hr); goto cleanup; }
    if (FAILED(decoder->GetFrame(0, &frame))) goto cleanup;
    if (FAILED(wic->CreateFormatConverter(&converter))) goto cleanup;
    if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) goto cleanup;
    {
        UINT w = 0, h = 0;
        converter->GetSize(&w, &h);
        if (w == 0 || h == 0) goto cleanup;
        UINT stride = w * 4;
        UINT imgSize = stride * h;
        pixels = new BYTE[imgSize];
        if (FAILED(converter->CopyPixels(nullptr, stride, imgSize, pixels))) goto cleanup;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w; td.Height = h;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc = { 1, 0 };
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd = { pixels, stride, 0 };
        if (FAILED(dev->CreateTexture2D(&td, &sd, &g_BezelTex12))) goto cleanup;
        if (FAILED(dev->CreateShaderResourceView(g_BezelTex12, nullptr, &g_BezelSRV12))) goto cleanup;
        wcscpy_s(g_BezelPathCached12, 260, path);
        Log("[D3D12-BEZEL] Loaded '%ls' (%ux%u) into D3D11On12 texture", path, w, h);
        ok = true;
    }
cleanup:
    if (pixels)    delete[] pixels;
    if (converter) converter->Release();
    if (frame)     frame->Release();
    if (decoder)   decoder->Release();
    if (wic)       wic->Release();
    if (!ok)       ReleaseBezelTexture12();
    return ok;
}

// ── Detect whether a swapchain belongs to a D3D12 device ──────────────────
static bool IsD3D12SwapChain(IDXGISwapChain* sc) {
    if (g_D3D12Device) return true;
    ID3D12Device* dev = nullptr;
    HRESULT hr = sc->GetDevice(__uuidof(ID3D12Device), (void**)&dev);
    if (SUCCEEDED(hr) && dev) { g_D3D12Device = dev; return true; }
    if (dev) dev->Release();
    return false;
}

// ── Initialise the D3D11On12 device + shader resources ────────────────────
static bool InitResources12(IDXGISwapChain* sc) {
    if (g_Inited12) return true;
    // The command queue is captured asynchronously by HookedExecuteCommandLists.
    if (!g_GameCmdQueue) {
        static bool logged = false;
        if (!logged) { logged = true; Log("[D3D12] InitResources12: awaiting DIRECT command-queue capture..."); }
        return false;
    }
    if (!g_D3D12Device && !IsD3D12SwapChain(sc)) {
        Log("[D3D12] InitResources12: GetDevice(ID3D12Device) failed");
        return false;
    }
    // D3D11On12CreateDevice lives in d3d11.dll — load dynamically (no import).
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    PFN_D3D11ON12_CREATE_DEVICE pOn12 = d3d11
        ? (PFN_D3D11ON12_CREATE_DEVICE)GetProcAddress(d3d11, "D3D11On12CreateDevice") : nullptr;
    if (!pOn12) { Log("[D3D12] D3D11On12CreateDevice not found in d3d11.dll"); return false; }

    IUnknown* queues[1] = { g_GameCmdQueue };
    // BGRA_SUPPORT enables Direct2D interop for the OSD text overlay.
    HRESULT hr = pOn12((IUnknown*)g_D3D12Device, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                       queues, 1, 0, &g_Dev11on12, &g_Ctx11on12, nullptr);
    if (FAILED(hr) || !g_Dev11on12 || !g_Ctx11on12) {
        Log("[D3D12] D3D11On12CreateDevice hr=0x%08X", hr); return false;
    }
    hr = g_Dev11on12->QueryInterface(__uuidof(ID3D11On12Device), (void**)&g_On12);
    if (FAILED(hr) || !g_On12) { Log("[D3D12] QI ID3D11On12Device hr=0x%08X", hr); return false; }

    // ── Compile shaders + create states on the grafted device (mirror InitResources) ──
    ID3DBlob* vsBlob = nullptr; ID3DBlob* errBlob = nullptr;
    hr = D3DCompile(VS_SRC, strlen(VS_SRC), "vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr) || !vsBlob) { LogShaderError("D3D12 VS", errBlob); if (errBlob) errBlob->Release(); return false; }
    if (errBlob) { errBlob->Release(); errBlob = nullptr; }
    hr = g_Dev11on12->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_VS12);
    vsBlob->Release();
    if (FAILED(hr)) { Log("[D3D12] CreateVertexShader hr=0x%08X", hr); return false; }

    ID3DBlob* psBlob = nullptr;
    const char* psSrc = GetPSSrc();
    hr = D3DCompile(psSrc, strlen(psSrc), "ps", nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr) || !psBlob) { LogShaderError("D3D12 PS", errBlob); if (errBlob) errBlob->Release(); return false; }
    if (errBlob) { errBlob->Release(); errBlob = nullptr; }
    hr = g_Dev11on12->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_PS12);
    psBlob->Release();
    if (FAILED(hr)) { Log("[D3D12] CreatePixelShader hr=0x%08X", hr); return false; }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 192;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_Dev11on12->CreateBuffer(&cbd, nullptr, &g_CB12))) return false;

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend  = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
    bd.RenderTarget[0].BlendOp   = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(g_Dev11on12->CreateBlendState(&bd, &g_Blend12))) return false;

    D3D11_BLEND_DESC bdOver = {};
    bdOver.RenderTarget[0].BlendEnable = FALSE;
    bdOver.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(g_Dev11on12->CreateBlendState(&bdOver, &g_BlendOver12))) return false;

    D3D11_SAMPLER_DESC smd = {};
    smd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    smd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(g_Dev11on12->CreateSamplerState(&smd, &g_Sampler12))) return false;
    if (FAILED(g_Dev11on12->CreateSamplerState(&smd, &g_BezelSamp12))) return false;

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = FALSE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(g_Dev11on12->CreateRasterizerState(&rd, &g_Raster12))) return false;

    OpenSharedMem();

    // ── D2D 1.1 device context for OSD text (NON-FATAL) ──────────────────
    // We use a D2D device context (not CreateDxgiSurfaceRenderTarget) so OSD
    // works regardless of the D3D12 swapchain format (BGRA or RGBA). Any
    // failure here only disables OSD — the scanline/CRT effects still render.
    {
        IDXGIDevice* dxgiDev = nullptr;
        if (SUCCEEDED(g_Dev11on12->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)) && dxgiDev) {
            if (!g_D2DFactory12) {
                D2D1_FACTORY_OPTIONS fo = {};
                HRESULT hrd = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                    __uuidof(ID2D1Factory1), &fo, (void**)&g_D2DFactory12);
                if (FAILED(hrd)) { Log("[D3D12-OSD] D2D1CreateFactory hr=0x%08X", hrd); g_D2DFactory12 = nullptr; }
            }
            if (g_D2DFactory12 && !g_D2DDevice12) {
                HRESULT hrd = g_D2DFactory12->CreateDevice(dxgiDev, &g_D2DDevice12);
                if (FAILED(hrd)) { Log("[D3D12-OSD] CreateDevice hr=0x%08X", hrd); g_D2DDevice12 = nullptr; }
            }
            if (g_D2DDevice12 && !g_D2DCtx12) {
                HRESULT hrd = g_D2DDevice12->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_D2DCtx12);
                if (FAILED(hrd)) { Log("[D3D12-OSD] CreateDeviceContext hr=0x%08X", hrd); g_D2DCtx12 = nullptr; }
            }
            dxgiDev->Release();
        }
        if (!g_DWFactory) {
            HRESULT hrd = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_DWFactory));
            if (FAILED(hrd)) { Log("[D3D12-OSD] DWriteCreateFactory hr=0x%08X", hrd); g_DWFactory = nullptr; }
        }
        if (g_DWFactory && !g_OsdFormat) {
            g_DWFactory->CreateTextFormat(L"Segoe UI", nullptr,
                DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                22.0f, L"en-us", &g_OsdFormat);
            if (g_OsdFormat) {
                g_OsdFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                g_OsdFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }
        }
        Log("[D3D12-OSD] D2D status: ctx=%s dwrite=%s format=%s",
            g_D2DCtx12 ? "OK" : "no", g_DWFactory ? "OK" : "no", g_OsdFormat ? "OK" : "no");
    }

    g_Inited12 = true;
    Log("[D3D12] D3D11On12 resources initialised OK (queue=0x%p)", (void*)g_GameCmdQueue);
    return true;
}

// ── OSD text overlay on the D3D12 back buffer (via D3D11On12 + D2D 1.1) ───
// Format-robust (works on BGRA and RGBA swapchains). Mirrors RenderOsd's
// visuals. Fully self-contained acquire/draw/release; any failure → skip.
static void RenderOsd12(IDXGISwapChain* sc) {
    if (!g_Shared) { OpenSharedMem(); }
    if (!g_Shared || !g_Shared->osdActive || !g_Shared->osdText[0]) return;
    if (!g_D2DCtx12 || !g_DWFactory || !g_OsdFormat || !g_On12 || !g_Ctx11on12) return;

    static IDXGISwapChain* s_osdSc3Src = nullptr;
    if (!g_SC3 || s_osdSc3Src != sc) {
        if (g_SC3) { g_SC3->Release(); g_SC3 = nullptr; }
        if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&g_SC3)) || !g_SC3) {
            s_osdSc3Src = nullptr; return;
        }
        s_osdSc3Src = sc;
    }
    UINT idx = g_SC3->GetCurrentBackBufferIndex();

    ID3D12Resource* bb12 = nullptr;
    if (FAILED(sc->GetBuffer(idx, __uuidof(ID3D12Resource), (void**)&bb12)) || !bb12) return;
    D3D11_RESOURCE_FLAGS rf = {}; rf.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Resource* wrapped = nullptr;
    HRESULT hr = g_On12->CreateWrappedResource(
        bb12, &rf, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT,
        __uuidof(ID3D11Resource), (void**)&wrapped);
    bb12->Release();
    if (FAILED(hr) || !wrapped) return;
    g_On12->AcquireWrappedResources(&wrapped, 1);

    IDXGISurface* surface = nullptr;
    if (SUCCEEDED(wrapped->QueryInterface(__uuidof(IDXGISurface), (void**)&surface)) && surface) {
        DXGI_SURFACE_DESC sdsc = {}; surface->GetDesc(&sdsc);
        D2D1_BITMAP_PROPERTIES1 bp = {};
        bp.pixelFormat.format    = sdsc.Format;
        bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
        bp.dpiX = 96.0f; bp.dpiY = 96.0f;
        bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        ID2D1Bitmap1* bmp = nullptr;
        HRESULT hb = g_D2DCtx12->CreateBitmapFromDxgiSurface(surface, &bp, &bmp);
        if (FAILED(hb)) {
            // Some formats need premultiplied alpha — retry once.
            bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
            hb = g_D2DCtx12->CreateBitmapFromDxgiSurface(surface, &bp, &bmp);
        }
        if (SUCCEEDED(hb) && bmp) {
            float bbW = (float)sdsc.Width, bbH = (float)sdsc.Height;
            g_D2DCtx12->SetTarget(bmp);
            float scale = bbH / 1080.0f; if (scale < 0.8f) scale = 0.8f;
            UINT len = (UINT)wcsnlen(g_Shared->osdText, 127);
            IDWriteTextLayout* layout = nullptr;
            g_DWFactory->CreateTextLayout(g_Shared->osdText, len, g_OsdFormat, bbW * 0.8f, 100.0f, &layout);
            if (layout) {
                layout->SetFontSize(22.0f * scale, { 0, len });
                DWRITE_TEXT_METRICS tm; layout->GetMetrics(&tm);
                float padX = 32.0f * scale, padY = 14.0f * scale;
                float boxW = tm.width + padX * 2, boxH = tm.height + padY * 2;
                float x = (bbW - boxW) / 2.0f, y = bbH * 0.05f;
                ID2D1SolidColorBrush* bgBrush = nullptr;
                ID2D1SolidColorBrush* borderBrush = nullptr;
                ID2D1SolidColorBrush* txtBrush = nullptr;
                g_D2DCtx12->CreateSolidColorBrush(D2D1::ColorF(14.0f/255, 17.0f/255, 38.0f/255, 210.0f/255), &bgBrush);
                g_D2DCtx12->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.165f, 1.0f, 140.0f/255), &borderBrush);
                g_D2DCtx12->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &txtBrush);
                g_D2DCtx12->BeginDraw();
                float cr = 10.0f * scale;
                D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(x, y, x + boxW, y + boxH), cr, cr);
                if (bgBrush)     g_D2DCtx12->FillRoundedRectangle(rr, bgBrush);
                if (borderBrush) g_D2DCtx12->DrawRoundedRectangle(rr, borderBrush, 1.0f * scale);
                if (txtBrush)    g_D2DCtx12->DrawTextLayout(D2D1::Point2F(x + padX, y + padY), layout, txtBrush);
                g_D2DCtx12->EndDraw();
                if (txtBrush)    txtBrush->Release();
                if (borderBrush) borderBrush->Release();
                if (bgBrush)     bgBrush->Release();
                layout->Release();
            }
            g_D2DCtx12->SetTarget(nullptr);
            bmp->Release();
        } else {
            static bool logged = false;
            if (!logged) { logged = true; Log("[D3D12-OSD] CreateBitmapFromDxgiSurface hr=0x%08X fmt=%u", hb, sdsc.Format); }
        }
        surface->Release();
    }

    g_On12->ReleaseWrappedResources(&wrapped, 1);
    wrapped->Release();
    g_Ctx11on12->Flush();
}

// ── Apply scanlines to the D3D12 back buffer (via D3D11On12 wrap) ─────────
static bool g_D3D12FirstFrame = true;
static void ApplyScanlines12(IDXGISwapChain* sc) {
    if (!g_Shared) { OpenSharedMem(); }
    if (!g_Shared || !g_Shared->active) return;
    if (!g_Ctx11on12 || !g_On12 || !g_Dev11on12) return;

    // Fast-path: nothing enabled → skip all GPU work (same gate as ApplyScanlines).
    {
        bool anyEffect =
            g_Shared->hEnabled || g_Shared->vEnabled ||
            (g_Shared->blurEnabled      > 0.0f && g_Shared->blurIntensity      > 0.0f) ||
            (g_Shared->bloomEnabled     > 0.0f && g_Shared->bloomIntensity     > 0.0f) ||
            (g_Shared->curvatureEnabled > 0.0f && g_Shared->curvatureIntensity > 0.0f) ||
            (g_Shared->flickerEnabled   > 0.0f && g_Shared->flickerIntensity   > 0.0f) ||
            (g_Shared->phosphorEnabled  > 0.0f && g_Shared->phosphorIntensity  > 0.0f) ||
            (g_Shared->vhsEnabled       > 0.0f) || (g_Shared->grainIntensity   > 0.0f) ||
            (g_Shared->tapeNoiseEnabled > 0.0f && g_Shared->tapeNoiseIntensity > 0.0f) ||
            (g_Shared->vignetteEnabled  > 0.0f) || (g_Shared->megaBezelEnabled > 0.0f) ||
            (g_Shared->bezelHookActive  > 0.0f) ||
            g_Shared->brightness  != 0.0f || g_Shared->contrast    != 0.0f ||
            g_Shared->saturation  != 0.0f || g_Shared->temperature != 0.0f ||
            g_Shared->blackLevel  > 0.0f  ||
            (g_Shared->gamma != 1.0f && g_Shared->gamma != 0.0f);
        if (!anyEffect) return;
    }

    // Current back-buffer index (flip-model rotates the buffer each frame).
    // Re-query when the swapchain pointer changes — a fullscreen/borderless
    // toggle can make the game recreate its swapchain, invalidating g_SC3.
    static IDXGISwapChain* s_sc3Src = nullptr;
    if (!g_SC3 || s_sc3Src != sc) {
        if (g_SC3) { g_SC3->Release(); g_SC3 = nullptr; }
        if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&g_SC3)) || !g_SC3) {
            static bool logged = false;
            if (!logged) { logged = true; Log("[D3D12] QI IDXGISwapChain3 failed — cannot resolve back buffer"); }
            s_sc3Src = nullptr;
            return;
        }
        s_sc3Src = sc;
    }
    UINT idx = g_SC3->GetCurrentBackBufferIndex();

    // Wrap the D3D12 back buffer as a D3D11 texture for THIS frame only. We do
    // NOT cache the wrapper across frames: holding a back-buffer reference would
    // make the game's ResizeBuffers() fail on resolution / fullscreen changes.
    ID3D12Resource* bb12 = nullptr;
    if (FAILED(sc->GetBuffer(idx, __uuidof(ID3D12Resource), (void**)&bb12)) || !bb12) return;
    D3D11_RESOURCE_FLAGS rf = {};
    rf.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Resource* wrapped = nullptr;
    HRESULT hr = g_On12->CreateWrappedResource(
        bb12, &rf,
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT,
        __uuidof(ID3D11Resource), (void**)&wrapped);
    bb12->Release();
    if (FAILED(hr) || !wrapped) {
        static bool logged = false;
        if (!logged) { logged = true; Log("[D3D12] CreateWrappedResource hr=0x%08X", hr); }
        return;
    }
    g_On12->AcquireWrappedResources(&wrapped, 1);

    ID3D11Texture2D* backbuffer = nullptr;
    if (SUCCEEDED(wrapped->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&backbuffer)) && backbuffer) {
        if (g_D3D12FirstFrame) {
            DXGI_SWAP_CHAIN_DESC scd0 = {}; sc->GetDesc(&scd0);
            Log("[D3D12] First scanline draw: %ux%u fmt=%u bufIdx=%u",
                scd0.BufferDesc.Width, scd0.BufferDesc.Height, scd0.BufferDesc.Format, idx);
            g_D3D12FirstFrame = false;
        }

        bool blurOn   = g_Shared->blurEnabled      > 0.0f && g_Shared->blurIntensity      > 0.0f;
        bool bloomOn  = g_Shared->bloomEnabled     > 0.0f && g_Shared->bloomIntensity     > 0.0f;
        bool curvOn   = g_Shared->curvatureEnabled > 0.0f && g_Shared->curvatureIntensity > 0.0f;
        bool bcOn     = g_Shared->brightness != 0.0f || g_Shared->contrast != 0.0f || g_Shared->saturation != 0.0f || g_Shared->temperature != 0.0f
                      || g_Shared->blackLevel > 0.0f || (g_Shared->gamma != 1.0f && g_Shared->gamma != 0.0f);
        bool flickOn  = g_Shared->flickerEnabled   > 0.0f && g_Shared->flickerIntensity   > 0.0f;
        bool phosphOn = g_Shared->phosphorEnabled  > 0.0f && g_Shared->phosphorIntensity  > 0.0f;
        bool vhsOn       = g_Shared->vhsEnabled       > 0.0f;
        bool grainOn     = g_Shared->grainIntensity   > 0.0f;
        bool tapeNoiseOn = g_Shared->tapeNoiseEnabled > 0.0f && g_Shared->tapeNoiseIntensity > 0.0f;
        bool megaBzOn    = g_Shared->megaBezelEnabled > 0.0f;
        bool bezelHookOn = g_Shared->bezelHookActive  > 0.0f;

        // Startup blackout for MegaBezel (mirror native path — hides launch splash).
        bool blackedOut = false;
        if (megaBzOn) {
            static float blackoutStart = -1.0f;
            const float BLACKOUT_SECONDS = 1.5f;
            if (blackoutStart < 0.0f) blackoutStart = GetTimeSeconds();
            if ((GetTimeSeconds() - blackoutStart) < BLACKOUT_SECONDS) {
                ID3D11RenderTargetView* rtvB = nullptr;
                if (SUCCEEDED(g_Dev11on12->CreateRenderTargetView(backbuffer, nullptr, &rtvB))) {
                    float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                    g_Ctx11on12->ClearRenderTargetView(rtvB, black);
                    rtvB->Release();
                }
                blackedOut = true;
            }
        }

        bool needCopy = blurOn || bloomOn || curvOn || bcOn || flickOn || phosphOn || vhsOn || grainOn || tapeNoiseOn || megaBzOn || bezelHookOn;

        if (!blackedOut) {
            D3D11_TEXTURE2D_DESC bbDesc; backbuffer->GetDesc(&bbDesc);
            UINT bbW2 = bbDesc.Width, bbH2 = bbDesc.Height;

            if (needCopy) {
                if (!g_BBCopy12 || g_LastBBW12 != bbW2 || g_LastBBH12 != bbH2) {
                    if (g_BBSRV12) { g_BBSRV12->Release(); g_BBSRV12 = nullptr; }
                    if (g_BBCopy12) { g_BBCopy12->Release(); g_BBCopy12 = nullptr; }
                    D3D11_TEXTURE2D_DESC td = {};
                    td.Width = bbW2; td.Height = bbH2;
                    td.MipLevels = 1; td.ArraySize = 1;
                    td.Format = bbDesc.Format;
                    td.SampleDesc = { 1, 0 };
                    td.Usage = D3D11_USAGE_DEFAULT;
                    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    if (SUCCEEDED(g_Dev11on12->CreateTexture2D(&td, nullptr, &g_BBCopy12))) {
                        g_Dev11on12->CreateShaderResourceView(g_BBCopy12, nullptr, &g_BBSRV12);
                        g_LastBBW12 = bbW2; g_LastBBH12 = bbH2;
                        Log("[D3D12] Created backbuffer copy %ux%u fmt=%u", bbW2, bbH2, bbDesc.Format);
                    }
                }
                if (g_BBCopy12) {
                    if (bbDesc.SampleDesc.Count > 1)
                        g_Ctx11on12->ResolveSubresource(g_BBCopy12, 0, backbuffer, 0, bbDesc.Format);
                    else
                        g_Ctx11on12->CopyResource(g_BBCopy12, backbuffer);
                }
            }

            ID3D11RenderTargetView* rtv = nullptr;
            if (SUCCEEDED(g_Dev11on12->CreateRenderTargetView(backbuffer, nullptr, &rtv)) && rtv) {
                D3D11_VIEWPORT vp = { 0, 0, (float)bbW2, (float)bbH2, 0.0f, 1.0f };

                D3D11_MAPPED_SUBRESOURCE mapped;
                if (SUCCEEDED(g_Ctx11on12->Map(g_CB12, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                    ScanlineCBData cb;
                    cb.screenW    = (float)bbW2;
                    cb.screenH    = (float)bbH2;
                    cb.hThickness = g_Shared->hThickness;
                    cb.hGap       = g_Shared->hGap;
                    cb.hOpacity   = g_Shared->hOpacity;
                    cb.hStartX    = 0.0f;
                    cb.hWidth     = (float)bbW2;
                    cb.hEnabled   = g_Shared->hEnabled;
                    cb.vThickness = g_Shared->vThickness;
                    cb.vGap       = g_Shared->vGap;
                    cb.vOpacity   = g_Shared->vOpacity;
                    cb.vStartY    = 0.0f;
                    cb.vHeight    = (float)bbH2;
                    cb.vEnabled   = g_Shared->vEnabled;
                    cb.blurEnabled       = g_Shared->blurEnabled;
                    cb.blurIntensity     = g_Shared->blurIntensity;
                    cb.bloomEnabled      = g_Shared->bloomEnabled;
                    cb.bloomIntensity    = g_Shared->bloomIntensity;
                    cb.curvatureEnabled  = g_Shared->curvatureEnabled;
                    cb.curvatureIntensity = g_Shared->curvatureIntensity;
                    cb.brightness        = g_Shared->brightness;
                    cb.contrast          = g_Shared->contrast;
                    cb.saturation        = g_Shared->saturation;
                    cb.temperature       = g_Shared->temperature;
                    cb.flickerEnabled    = g_Shared->flickerEnabled;
                    cb.flickerIntensity  = g_Shared->flickerIntensity;
                    cb.flickerRate       = g_Shared->flickerRate;
                    cb.time              = GetTimeSeconds();
                    cb.blackLevel        = g_Shared->blackLevel;
                    cb.gamma             = g_Shared->gamma;
                    cb.phosphorEnabled   = g_Shared->phosphorEnabled;
                    cb.phosphorIntensity = g_Shared->phosphorIntensity;
                    cb.vhsEnabled        = g_Shared->vhsEnabled;
                    cb.vhsIntensity      = g_Shared->vhsIntensity;
                    cb.grainIntensity    = g_Shared->grainIntensity;
                    cb.tapeNoiseEnabled  = tapeNoiseOn ? 1.0f : 0.0f;
                    cb.tapeNoiseIntensity = g_Shared->tapeNoiseIntensity;
                    cb.vignetteEnabled    = g_Shared->vignetteEnabled;
                    cb.megaBezelEnabled   = g_Shared->megaBezelEnabled;
                    cb.megaBezelThickness = g_Shared->megaBezelThickness;
                    cb.megaBezelOpacity   = g_Shared->megaBezelOpacity;
                    cb.megaBezelBlur      = g_Shared->megaBezelBlur;
                    cb.bezelHookActive    = (bezelHookOn && g_BezelSRV12) ? g_Shared->bezelHookActive : 0.0f;
                    cb.bezelHookOpacity   = g_Shared->bezelHookOpacity;
                    cb.megaBezelRadius    = g_Shared->megaBezelRadius;
                    cb.megaBezelReflectionWidth = g_Shared->megaBezelReflectionWidth;
                    cb.megaBezelStartFade = GetMegaBezelStartFade(g_Shared->megaBezelEnabled > 0.0f);
                    cb._cbpad3 = 0.0f;
                    memcpy(mapped.pData, &cb, sizeof(ScanlineCBData));
                    g_Ctx11on12->Unmap(g_CB12, 0);
                }

                SavedState saved; memset(&saved, 0, sizeof(saved));
                SaveState(g_Ctx11on12, saved);

                float blendFactor[4] = { 1, 1, 1, 1 };
                g_Ctx11on12->OMSetRenderTargets(1, &rtv, nullptr);
                g_Ctx11on12->OMSetBlendState(needCopy ? g_BlendOver12 : g_Blend12, blendFactor, 0xFFFFFFFF);
                g_Ctx11on12->RSSetViewports(1, &vp);
                g_Ctx11on12->RSSetState(g_Raster12);
                g_Ctx11on12->VSSetShader(g_VS12, nullptr, 0);
                g_Ctx11on12->PSSetShader(g_PS12, nullptr, 0);
                g_Ctx11on12->PSSetConstantBuffers(0, 1, &g_CB12);
                if (needCopy && g_BBSRV12 && g_Sampler12) {
                    g_Ctx11on12->PSSetShaderResources(0, 1, &g_BBSRV12);
                    g_Ctx11on12->PSSetSamplers(0, 1, &g_Sampler12);
                }
                if (bezelHookOn) {
                    bool pathChanged = wcscmp(g_Shared->bezelHookPath, g_BezelPathCached12) != 0;
                    if (pathChanged || (!g_BezelSRV12 && g_Shared->bezelHookPath[0]))
                        LoadBezelTexture12(g_Dev11on12, g_Shared->bezelHookPath);
                    if (g_BezelSRV12 && g_BezelSamp12) {
                        g_Ctx11on12->PSSetShaderResources(1, 1, &g_BezelSRV12);
                        g_Ctx11on12->PSSetSamplers(1, 1, &g_BezelSamp12);
                    }
                } else if (g_BezelTex12) {
                    ReleaseBezelTexture12();
                }
                g_Ctx11on12->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                g_Ctx11on12->IASetInputLayout(nullptr);
                g_Ctx11on12->Draw(3, 0);

                RestoreState(g_Ctx11on12, saved);
                rtv->Release();
            }
        }
        backbuffer->Release();
    }

    g_On12->ReleaseWrappedResources(&wrapped, 1);
    wrapped->Release();
    g_Ctx11on12->Flush();   // submit our work to the game's command queue
}

// ── Unified DXGI frame dispatcher (auto-detects native-D3D11 vs D3D12) ─────
// Replaces the inlined init/apply block in HookedPresent / HookedPresent1.
// For native D3D11 games the behaviour is identical to before; for D3D12 games
// it engages the D3D11On12 path. OSD text is not yet drawn on D3D12.
static void ApplyDXGIFrame(IDXGISwapChain* sc) {
    if (g_GLInited) return;   // OpenGL active — never touch DXGI (NVIDIA GL uses DXGI internally)

    if (g_GfxApi == 0) {
        // First-time API probe. Try native D3D11; if the swapchain is D3D12, switch.
        if (InitResources(sc)) {
            g_GfxApi = 11;
        } else if (IsD3D12SwapChain(sc)) {
            g_GfxApi = 12;
            Log("[D3D12] Swapchain is D3D12 — engaging D3D11On12 parallel path");
        } else {
            return;  // neither ready yet — retry next frame
        }
    }

    if (g_GfxApi == 12) {
        if (!g_Inited12) { if (!InitResources12(sc)) return; }
        ApplyScanlines12(sc);
        RenderOsd12(sc);
        return;
    }

    if (g_Inited) {
        ApplyScanlines(sc);
        RenderOsd(sc);
    }
}

// ── Hooked Present ───────────────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(
    IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
    static bool s_firstCall = true;
    if (s_firstCall) { s_firstCall = false; Log("[D3D11] HookedPresent FIRED — pSC=0x%p", (void*)pSwapChain); }
    __try {
        // If OpenGL hook is already active, don't touch DXGI.
        // NVIDIA OpenGL uses DXGI internally — hooking it would double-apply
        // scanlines and crash the driver's internal swap chain.
        // ApplyDXGIFrame auto-detects native D3D11 vs D3D12 (D3D11On12 path).
        ApplyDXGIFrame(pSwapChain);
        // SDL2 borderless check (works for SDL2+D3D11 games like FNA/Axiom Verge)
        if (g_Shared) {
            bool want = (g_Shared->borderlessEnabled != 0);
            if (want != g_Sdl2BorderlessApplied)
                TrySdl2Borderless(want);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("[D3D11] EXCEPTION 0x%08X in HookedPresent — scanlines disabled", GetExceptionCode());
        g_Inited = false; g_Inited12 = false; // prevent further attempts that might crash
    }
    // Restore-call-repatch: remove inline hook before calling original so that
    // g_OrigPresent (= function body) doesn't recurse back into HookedPresent.
    // When only the vtable hook is active (no inline hook), g_D3D11PresentAddr is
    // null and we fall through to the direct call.
    if (g_D3D11PresentAddr) {
        DWORD op;
        VirtualProtect(g_D3D11PresentAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
        memcpy(g_D3D11PresentAddr, g_D3D11PresentOrig, HOOK_JMP_SIZE);
        FlushInstructionCache(GetCurrentProcess(), g_D3D11PresentAddr, HOOK_JMP_SIZE);
        VirtualProtect(g_D3D11PresentAddr, HOOK_JMP_SIZE, op, &op);
        HRESULT hr = g_OrigPresent(pSwapChain, SyncInterval, Flags);
        VirtualProtect(g_D3D11PresentAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
        memcpy(g_D3D11PresentAddr, g_D3D11PresentJmp, HOOK_JMP_SIZE);
        FlushInstructionCache(GetCurrentProcess(), g_D3D11PresentAddr, HOOK_JMP_SIZE);
        VirtualProtect(g_D3D11PresentAddr, HOOK_JMP_SIZE, op, &op);
        return hr;
    }
    return g_OrigPresent(pSwapChain, SyncInterval, Flags);
}

// ── Hooked Present1 (IDXGISwapChain1) — used by FNA D3D11 mode ──────────
static HRESULT STDMETHODCALLTYPE HookedPresent1(
    IDXGISwapChain1* pSwapChain, UINT SyncInterval, UINT Flags,
    const DXGI_PRESENT_PARAMETERS* pPresentParams)
{
    static bool s_firstCall = true;
    if (s_firstCall) { s_firstCall = false; Log("[D3D11] HookedPresent1 FIRED — pSC=0x%p", (void*)pSwapChain); }
    __try {
        // ApplyDXGIFrame auto-detects native D3D11 vs D3D12 (D3D11On12 path).
        ApplyDXGIFrame((IDXGISwapChain*)pSwapChain);
        // SDL2 borderless check (works for SDL2+D3D11 games like FNA/Axiom Verge)
        if (g_Shared) {
            bool want = (g_Shared->borderlessEnabled != 0);
            if (want != g_Sdl2BorderlessApplied)
                TrySdl2Borderless(want);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("[D3D11] EXCEPTION 0x%08X in HookedPresent1 — scanlines disabled", GetExceptionCode());
        g_Inited = false; g_Inited12 = false;
    }
    if (g_D3D11Present1Addr) {
        DWORD op;
        VirtualProtect(g_D3D11Present1Addr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
        memcpy(g_D3D11Present1Addr, g_D3D11Present1Orig, HOOK_JMP_SIZE);
        FlushInstructionCache(GetCurrentProcess(), g_D3D11Present1Addr, HOOK_JMP_SIZE);
        VirtualProtect(g_D3D11Present1Addr, HOOK_JMP_SIZE, op, &op);
        HRESULT hr = g_OrigPresent1(pSwapChain, SyncInterval, Flags, pPresentParams);
        VirtualProtect(g_D3D11Present1Addr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
        memcpy(g_D3D11Present1Addr, g_D3D11Present1Jmp, HOOK_JMP_SIZE);
        FlushInstructionCache(GetCurrentProcess(), g_D3D11Present1Addr, HOOK_JMP_SIZE);
        VirtualProtect(g_D3D11Present1Addr, HOOK_JMP_SIZE, op, &op);
        return hr;
    }
    return g_OrigPresent1(pSwapChain, SyncInterval, Flags, pPresentParams);
}

// ══════════════════════════════════════════════════════════════════════════
//  OPENGL HOOK (wglSwapBuffers)
// ══════════════════════════════════════════════════════════════════════════

// OpenGL extension function pointers
typedef GLuint (APIENTRY *PFNGLCREATESHADERPROC)(GLenum type);
typedef void   (APIENTRY *PFNGLSHADERSOURCEPROC)(GLuint shader, GLsizei count, const char** string, const GLint* length);
typedef void   (APIENTRY *PFNGLCOMPILESHADERPROC)(GLuint shader);
typedef GLuint (APIENTRY *PFNGLCREATEPROGRAMPROC)(void);
typedef void   (APIENTRY *PFNGLATTACHSHADERPROC)(GLuint program, GLuint shader);
typedef void   (APIENTRY *PFNGLLINKPROGRAMPROC)(GLuint program);
typedef void   (APIENTRY *PFNGLUSEPROGRAMPROC)(GLuint program);
typedef void   (APIENTRY *PFNGLDELETESHADERPROC)(GLuint shader);
typedef GLint  (APIENTRY *PFNGLGETUNIFORMLOCATIONPROC)(GLuint program, const char* name);
typedef void   (APIENTRY *PFNGLUNIFORM1FPROC)(GLint location, GLfloat v0);
typedef void   (APIENTRY *PFNGLUNIFORM1IPROC)(GLint location, GLint v0);
typedef void   (APIENTRY *PFNGLUNIFORM4FPROC)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
typedef void   (APIENTRY *PFNGLGETSHADERIVPROC)(GLuint shader, GLenum pname, GLint* params);
// VAO — required in OpenGL 3.2+ core profile for glDrawArrays
typedef void   (APIENTRY *PFNGLGENVERTEXARRAYSPROC)(GLsizei n, GLuint* arrays);
typedef void   (APIENTRY *PFNGLBINDVERTEXARRAYPROC)(GLuint array);
// FBO — bind FB0 to ensure scanlines go to the displayed framebuffer
typedef void   (APIENTRY *PFNGLBINDFRAMEBUFFERPROC)(GLenum target, GLuint framebuffer);
// Shader diagnostics
typedef void   (APIENTRY *PFNGLGETSHADERINFOLOGPROC)(GLuint, GLsizei, GLsizei*, char*);
typedef void   (APIENTRY *PFNGLGETPROGRAMIVPROC)(GLuint, GLenum, GLint*);
typedef void   (APIENTRY *PFNGLGETPROGRAMINFOLOGPROC)(GLuint, GLsizei, GLsizei*, char*);

#define GL_FRAGMENT_SHADER   0x8B30
#define GL_VERTEX_SHADER     0x8B31
#define GL_COMPILE_STATUS    0x8B81
#define GL_LINK_STATUS       0x8B82

// Globals (OpenGL)
typedef BOOL (WINAPI* PFN_wglSwapBuffers)(HDC);
// g_GLInited forward-declared at top of file

// Restore-call-repatch globals (replaces trampoline — no instruction corruption)
static BYTE* g_SwapBuffersAddr = nullptr;     // address of wglSwapBuffers (opengl32)
static BYTE* g_GdiSwapBuffersAddr = nullptr;  // address of SwapBuffers (gdi32, SDL2/FNA)
static BYTE  g_SwapBufOrigBytes[14]    = {};  // saved original bytes — wglSwapBuffers
static BYTE  g_SwapBufHookJmp[14]      = {};  // our JMP — wglSwapBuffers
static BYTE  g_GdiSwapBufOrigBytes[14] = {};  // saved original bytes — gdi32::SwapBuffers
static BYTE  g_GdiSwapBufHookJmp[14]   = {};  // our JMP — gdi32::SwapBuffers
// Re-entrancy guard: wglSwapBuffers may call gdi32::SwapBuffers internally.
// We only apply scanlines on the FIRST entry, not on re-entrant calls.
static bool  g_SwapHookActive = false;
static GLuint g_GLProgram = 0;
static GLint g_uScreenW, g_uScreenH;
static GLint g_uHThickness, g_uHGap, g_uHOpacity, g_uHStartX, g_uHWidth, g_uHEnabled;
static GLint g_uVThickness, g_uVGap, g_uVOpacity, g_uVStartY, g_uVHeight, g_uVEnabled;
static GLint g_uBlurEnabled, g_uBlurIntensity, g_uBackBuf;
static GLint g_uBloomEnabled, g_uBloomIntensity;
static GLint g_uCurvatureEnabled, g_uCurvatureIntensity;
static GLint g_uBrightness, g_uContrast, g_uSaturation, g_uTemperature;
static GLint g_uBlackLevel, g_uGamma;
static GLint g_uFlickerEnabled, g_uFlickerIntensity, g_uFlickerRate, g_uTime;
static GLint g_uPhosphorEnabled, g_uPhosphorIntensity;
static GLint g_uVhsEnabled, g_uVhsIntensity, g_uGrainIntensity;
static GLint g_uTapeNoiseEnabled, g_uTapeNoiseIntensity;
static GLint g_uVignetteEnabled;
static GLint g_uMegaBezelEnabled, g_uMegaBezelThickness, g_uMegaBezelOpacity;
static GLint g_uMegaBezelBlur, g_uMegaBezelRadius, g_uMegaBezelReflectionWidth;
static GLint g_uMegaBezelStartFade;
static GLint g_uGameRect;
static GLuint g_GLBlurTex = 0;
static int g_GLBlurTexW = 0, g_GLBlurTexH = 0;

// GL bezel PNG texture — loaded via WIC, mirrors g_BezelTex (D3D11) / g_D3D9BezelTex
static GLuint  g_GLBezelTex = 0;
static wchar_t g_GLBezelPathCached[260] = {};
static GLint   g_uBezelHookActive  = -1;
static GLint   g_uBezelHookOpacity = -1;
static GLint   g_uBezelTex         = -1;

// glActiveTexture — GL 1.3+, need wglGetProcAddress on Windows
typedef void (APIENTRY *PFNGLACTIVETEXTUREPROC_)(GLenum);
static PFNGLACTIVETEXTUREPROC_ glActiveTexture_fn = nullptr;

// glUniform4f — needed for OSD quad position
static PFNGLUNIFORM4FPROC glUniform4f_fn = nullptr;

// ── GL OSD overlay ──────────────────────────────────────────────────────
static GLuint  g_OsdGLProgram  = 0;
static GLint   g_OsdU_Rect     = -1;
static GLint   g_OsdU_Tex      = -1;
static GLuint  g_OsdGLTexture  = 0;
static int     g_OsdGLTexW     = 0, g_OsdGLTexH = 0;
static wchar_t g_OsdGLLastText[128] = {};

// GL extension function pointers
static PFNGLCREATESHADERPROC      glCreateShader_fn      = nullptr;
static PFNGLSHADERSOURCEPROC      glShaderSource_fn      = nullptr;
static PFNGLCOMPILESHADERPROC     glCompileShader_fn     = nullptr;
static PFNGLCREATEPROGRAMPROC     glCreateProgram_fn     = nullptr;
static PFNGLATTACHSHADERPROC      glAttachShader_fn      = nullptr;
static PFNGLLINKPROGRAMPROC       glLinkProgram_fn       = nullptr;
static PFNGLUSEPROGRAMPROC        glUseProgram_fn        = nullptr;
static PFNGLDELETESHADERPROC      glDeleteShader_fn      = nullptr;
static PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation_fn = nullptr;
static PFNGLUNIFORM1FPROC         glUniform1f_fn         = nullptr;
static PFNGLUNIFORM1IPROC         glUniform1i_fn         = nullptr;
static PFNGLGETSHADERIVPROC       glGetShaderiv_fn       = nullptr;
static PFNGLGENVERTEXARRAYSPROC   glGenVertexArrays_fn   = nullptr;
static PFNGLBINDVERTEXARRAYPROC   glBindVertexArray_fn   = nullptr;
static PFNGLBINDFRAMEBUFFERPROC   glBindFramebuffer_fn   = nullptr;
static PFNGLGETSHADERINFOLOGPROC  glGetShaderInfoLog_fn  = nullptr;
static PFNGLGETPROGRAMIVPROC      glGetProgramiv_fn      = nullptr;
static PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog_fn = nullptr;
static GLuint                     g_GLVao                = 0;
static HGLRC                      g_hCurrentGlrc         = NULL; // tracks active GL context for change detection
// g_Sdl2BorderlessApplied declared in the forward-declarations block above

// glBlendEquation — MUST be set to GL_FUNC_ADD before our draw!
typedef void (APIENTRY *PFNGLBLENDEQUATIONPROC_)(GLenum);
static PFNGLBLENDEQUATIONPROC_ glBlendEquation_fn = nullptr;

// glBindFragDataLocation — explicit output binding for core profile
typedef void (APIENTRY *PFNGLBINDFRAGDATALOCATIONPROC)(GLuint, GLuint, const char*);
static PFNGLBINDFRAGDATALOCATIONPROC glBindFragDataLocation_fn = nullptr;

// glReadPixels is in opengl32 already (no extension needed)



// GLSL Vertex Shader — fullscreen quad via gl_VertexID (no VBO needed)
// Uses #version 150 (GLSL 1.50 = OpenGL 3.2) for core profile compatibility
static const char* GL_VS_SRC =
    "#version 150\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    vec2 pos;\n"
    "    vec2 uv;\n"
    "    if      (gl_VertexID == 0) { pos = vec2(-1.0, -1.0); uv = vec2(0.0, 0.0); }\n"
    "    else if (gl_VertexID == 1) { pos = vec2( 1.0, -1.0); uv = vec2(1.0, 0.0); }\n"
    "    else if (gl_VertexID == 2) { pos = vec2(-1.0,  1.0); uv = vec2(0.0, 1.0); }\n"
    "    else                       { pos = vec2( 1.0,  1.0); uv = vec2(1.0, 1.0); }\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "    vUV = uv;\n"
    "}\n";

// GLSL Fragment Shader — hard-step CRT scanline mask
static const char* GL_FS_SRC =
    "#version 150\n"
    "in vec2 vUV;\n"
    "out vec4 outColor;\n"
    "uniform float screenW, screenH;\n"
    "uniform float hThickness, hGap, hOpacity, hStartX, hWidth;\n"
    "uniform int hEnabled;\n"
    "uniform float vThickness, vGap, vOpacity, vStartY, vHeight;\n"
    "uniform int vEnabled;\n"
    "uniform float blurEnabled, blurIntensity;\n"
    "uniform float bloomEnabled, bloomIntensity;\n"
    "uniform float curvatureEnabled, curvatureIntensity;\n"
    "uniform float brightness, contrast, saturation, temperature;\n"
    "uniform float flickerEnabled, flickerIntensity, flickerRate, time;\n"
    "uniform float phosphorEnabled, phosphorIntensity;\n"
    "uniform float blackLevel, gamma;\n"
    "uniform float vhsEnabled, vhsIntensity, grainIntensity;\n"
    "uniform float tapeNoiseEnabled, tapeNoiseIntensity;\n"
    "uniform float vignetteEnabled;\n"
    "uniform float megaBezelEnabled, megaBezelThickness, megaBezelOpacity;\n"
    "uniform float megaBezelBlur, megaBezelRadius, megaBezelReflectionWidth;\n"
    "uniform float megaBezelStartFade;\n"
    "uniform vec4 gameRect;\n"   // (u0, v0, u1, v1) — game viewport in UV [0,1]
    "uniform float bezelHookActive, bezelHookOpacity;\n"
    "uniform sampler2D backBuf;\n"
    "uniform sampler2D bezelTex;\n"
    "\n"
    "float tnHash(float n) { return fract(sin(n) * 43758.5453123); }\n"
    "float tnN3d(vec3 x) {\n"
    "    vec3 p = floor(x);\n"
    "    vec3 f = fract(x);\n"
    "    f = f*f*(3.0-2.0*f);\n"
    "    float n = p.x + p.y*57.0 + 113.0*p.z;\n"
    "    return mix(mix(mix(tnHash(n),       tnHash(n+1.0),   f.x),\n"
    "                   mix(tnHash(n+57.0),  tnHash(n+58.0),  f.x), f.y),\n"
    "              mix(mix(tnHash(n+113.0),  tnHash(n+114.0), f.x),\n"
    "                   mix(tnHash(n+170.0), tnHash(n+171.0), f.x), f.y), f.z);\n"
    "}\n"
    "float tnNn(vec2 p, float fc) {\n"
    "    float y = p.y;\n"
    "    float s = mod(fc * 0.15, 4837.0);\n"
    "    float v = tnN3d(vec3(y*0.01  + s,        1.0, 1.0))\n"
    "            * tnN3d(vec3(y*0.011 + 1000.0+s, 1.0, 1.0))\n"
    "            * tnN3d(vec3(y*0.51  + 421.0+s,  1.0, 1.0));\n"
    "    v *= tnHash(p.x + fc * 0.01) + 0.3;\n"
    "    v = pow(v + 0.3, 1.0);\n"
    "    if (v < 0.99) v = 0.0;\n"
    "    return v;\n"
    "}\n"
    "\n"
    "float scanlineIntensity(float pos, float thick, float gap, float opacity, float fw) {\n"
    "    float period = thick + gap;\n"
    "    if (period <= 0.0) return 1.0;\n"
    "    float snapped = max(round(period), 1.0);\n"
    "    float scale   = snapped / period;\n"
    "    thick        *= scale;\n"
    "    period        = snapped;\n"
    "    float t = mod(pos, period);\n"
    "    float edge  = max(fw, 0.5);\n"
    "    float inLine = 1.0 - smoothstep(thick - edge, thick + edge, t);\n"
    "    float vis = 1.0 - smoothstep(period * 0.3, period * 0.5, fw);\n"
    "    return 1.0 - inLine * opacity * vis;\n"
    "}\n"
    "\n"
    "vec2 CurveUV(vec2 uv, float strength) {\n"
    "    vec2 cc = uv * 2.0 - 1.0;\n"
    "    cc *= 1.0 + strength * dot(cc, cc);\n"
    "    return cc * 0.5 + 0.5;\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    // ── Curvature + MegaBezel: barrel-distort UV, reflection in border ──\n"
    "    vec2 sampleUV = vUV;\n"
    "    bool curved = curvatureEnabled >= 0.5 && curvatureIntensity > 0.0;\n"
    "    bool megaBz = megaBezelEnabled >= 0.5;\n"
    "    float px = gl_FragCoord.x;\n"
    "    float py = screenH - gl_FragCoord.y;\n"
    "\n"
    "    // MegaBezel: margin shrink in SCREEN space (vUV [0,1] = full window).\n"
    "    // Same approach as D3D11 — the game image is 'stretched' to fill the\n"
    "    // whole window with a uniform border. gameRect is only used later to\n"
    "    // remap texture coordinates so backBuf sampling reads game pixels.\n"
    "    if (megaBz) {\n"
    "        float margin = megaBezelThickness * 0.10;\n"
    "        if (margin < 0.001) margin = 0.001;\n"
    "        sampleUV.x = (vUV.x - margin) / (1.0 - 2.0 * margin);\n"
    "        sampleUV.y = (vUV.y - margin) / (1.0 - 2.0 * margin);\n"
    "        if (curved) {\n"
    "            sampleUV = CurveUV(sampleUV, curvatureIntensity * 0.25);\n"
    "        }\n"
    "    } else if (curved) {\n"
    "        sampleUV = CurveUV(vUV, curvatureIntensity * 0.25);\n"
    "    }\n"
    "\n"
    "    // REFL. RADIUS (megaBezelRadius) — rounds the reflection inner corner so it\n"
    "    // hugs a rounded CRT bezel PNG. 0 = square (original); higher rounds and lets\n"
    "    // the reflection overflow inward over the game corner. Independent of GAME\n"
    "    // CORNERS (vignetteEnabled), which only rounds the in-game black mask.\n"
    "    float  mbR       = megaBezelRadius * 0.025;\n"
    "    vec2   mbCV      = sampleUV - 0.5;\n"
    "    vec2   mbHalfExt = vec2(0.5, 0.5);\n"
    "    vec2   mbQ       = abs(mbCV) - mbHalfExt + mbR;\n"
    "    float  mbSDF     = length(max(mbQ, 0.0)) + min(max(mbQ.x, mbQ.y), 0.0) - mbR;\n"
    "\n"
    "    // Reflection boundary = SQUARE (clean miter, no fan). Game corners are\n"
    "    // rounded separately below (REFL. RADIUS hard mask).\n"
    "    bool outsideGame = megaBz ? (mbSDF > 0.0) : (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0);\n"
    "\n"
    "    if (outsideGame) {\n"
    "        if (megaBz) {\n"
    "            // ── Picture-frame mirror reflection ──\n"
    "            float depthX = sampleUV.x < 0.0 ? -sampleUV.x\n"
    "                         : sampleUV.x > 1.0 ? sampleUV.x - 1.0 : 0.0;\n"
    "            float depthY = sampleUV.y < 0.0 ? -sampleUV.y\n"
    "                         : sampleUV.y > 1.0 ? sampleUV.y - 1.0 : 0.0;\n"
    "            bool sInSide   = depthX > 0.0;\n"
    "            bool sInTopBot = depthY > 0.0;\n"
    "            vec2 mUV_side;\n"
    "            if (sInSide && sInTopBot) {\n"
    "                if (depthX >= depthY) {\n"
    "                    mUV_side.x = sampleUV.x < 0.0 ? -sampleUV.x : 2.0 - sampleUV.x;\n"
    "                    mUV_side.y = clamp(sampleUV.y, 0.0, 1.0);\n"
    "                } else {\n"
    "                    mUV_side.x = clamp(sampleUV.x, 0.0, 1.0);\n"
    "                    mUV_side.y = sampleUV.y < 0.5 ? -sampleUV.y : 2.0 - sampleUV.y;\n"
    "                }\n"
    "            } else if (sInSide) {\n"
    "                mUV_side.x = sampleUV.x < 0.0 ? -sampleUV.x : 2.0 - sampleUV.x;\n"
    "                mUV_side.y = clamp(sampleUV.y, 0.0, 1.0);\n"
    "            } else {\n"
    "                mUV_side.x = clamp(sampleUV.x, 0.0, 1.0);\n"
    "                mUV_side.y = sampleUV.y < 0.5 ? -sampleUV.y : 2.0 - sampleUV.y;\n"
    "            }\n"
    "            float bezelDepth_side = max(depthX, depthY);\n"
    "\n"
    "            // Corner (radial through arc) reflection\n"
    "            vec2  arcCenter = sign(mbCV) * (mbHalfExt - mbR);\n"
    "            vec2  u         = mbCV - arcCenter;\n"
    "            float distU     = max(length(u), 1e-5);\n"
    "            vec2  mCV       = arcCenter + u * (2.0 * mbR - distU) / distU;\n"
    "            vec2  mUV_corner = mCV + 0.5;\n"
    "            float bezelDepth_corner = max(distU - mbR, 0.0);\n"
    "\n"
    "            // Blend side ↔ corner reflections\n"
    "            // Pure 45 deg miter (radial corner mirror disabled — peacock-fan artifact).\n"
    "            float blendCorner = 0.0;\n"
    "            float bezelDepth = mix(bezelDepth_side, bezelDepth_corner, blendCorner);\n"
    "            // Per-axis mirror across the ROUNDED game edge — fills the corner, no fan.\n"
    "            vec2  cR    = sampleUV - 0.5;\n"
    "            vec2  sgnR  = sign(cR);\n"
    "            vec2  aR    = abs(cR);\n"
    "            float arcCo = 0.5 - mbR;\n"
    "            float xEdge = (aR.y > arcCo) ? (arcCo + sqrt(max(mbR*mbR - (aR.y-arcCo)*(aR.y-arcCo), 0.0))) : 0.5;\n"
    "            float yEdge = (aR.x > arcCo) ? (arcCo + sqrt(max(mbR*mbR - (aR.x-arcCo)*(aR.x-arcCo), 0.0))) : 0.5;\n"
    "            float depthXr = aR.x - xEdge;\n"
    "            float depthYr = aR.y - yEdge;\n"
    "            vec2  aM_X = vec2(2.0*xEdge - aR.x, aR.y);\n"
    "            vec2  aM_Y = vec2(aR.x, 2.0*yEdge - aR.y);\n"
    "            float blendW = max(mbR * 0.6, 1e-4);\n"
    "            vec2  aM = mix(aM_Y, aM_X, smoothstep(-blendW, blendW, depthXr - depthYr));\n"
    "            vec2  mUV = clamp(0.5 + sgnR * aM, 0.0, 1.0);\n"
    "\n"
    "            // Letterbox inset only on top/bottom reflections (sides keep vertical\n"
    "            // alignment with the game — no 4% Y squish). Smooth side<->top/bottom blend.\n"
    "            float yLetterboxInset = 0.02;\n"
    "            float insetW = 1.0 - smoothstep(-blendW, blendW, depthXr - depthYr);\n"
    "            mUV.y = mix(mUV.y, yLetterboxInset + mUV.y * (1.0 - 2.0 * yLetterboxInset), insetW);\n"
    "\n"
    "            // Remap mUV [0,1] to texture-space via gameRect so reflection\n"
    "            // samples actual game pixels (not black bars in the backBuf).\n"
    "            vec2 texMUV = gameRect.xy + mUV * (gameRect.zw - gameRect.xy);\n"
    "\n"
    "            // 7x7 Gaussian blur on reflection samples\n"
    "            float sigma = megaBezelBlur * 5.0 + 0.0001;\n"
    "            vec2  texel = 1.0 / vec2(screenW, screenH);\n"
    "            vec3  reflColor;\n"
    "            if (sigma > 0.05) {\n"
    "                vec3  sum  = vec3(0.0);\n"
    "                float wSum = 0.0;\n"
    "                for (int dy = -3; dy <= 3; dy++) {\n"
    "                    for (int dx = -3; dx <= 3; dx++) {\n"
    "                        float d2 = float(dx*dx + dy*dy);\n"
    "                        float w  = exp(-d2 / (2.0 * sigma * sigma));\n"
    "                        sum  += texture(backBuf, clamp(texMUV + vec2(dx, dy) * texel * sigma, 0.0, 1.0)).rgb * w;\n"
    "                        wSum += w;\n"
    "                    }\n"
    "                }\n"
    "                reflColor = sum / wSum;\n"
    "            } else {\n"
    "                reflColor = texture(backBuf, texMUV).rgb;\n"
    "            }\n"
    "\n"
    "            // ── Reflection width + fade (same as D3D11 — screen-space) ──\n"
    "            float marginRef    = max(megaBezelThickness * 0.10, 0.001);\n"
    "            float reflW        = max(megaBezelReflectionWidth, 0.001);\n"
    "            float invShrink    = 1.0 / max(1.0 - 2.0 * marginRef, 1e-5);\n"
    "            float curvStrength = curved ? curvatureIntensity * 0.25 : 0.0;\n"
    "\n"
    "            // Synthetic sampleUV at screen edges for current row/col\n"
    "            vec2 sUV_left  = vec2((0.0 - marginRef) * invShrink, (vUV.y - marginRef) * invShrink);\n"
    "            vec2 sUV_right = vec2((1.0 - marginRef) * invShrink, (vUV.y - marginRef) * invShrink);\n"
    "            vec2 sUV_top   = vec2((vUV.x - marginRef) * invShrink, (0.0 - marginRef) * invShrink);\n"
    "            vec2 sUV_bot   = vec2((vUV.x - marginRef) * invShrink, (1.0 - marginRef) * invShrink);\n"
    "            vec2 csu_left  = curved ? CurveUV(sUV_left,  curvStrength) : sUV_left;\n"
    "            vec2 csu_right = curved ? CurveUV(sUV_right, curvStrength) : sUV_right;\n"
    "            vec2 csu_top   = curved ? CurveUV(sUV_top,   curvStrength) : sUV_top;\n"
    "            vec2 csu_bot   = curved ? CurveUV(sUV_bot,   curvStrength) : sUV_bot;\n"
    "\n"
    "            float depthXc = sampleUV.x < 0.0 ? -sampleUV.x\n"
    "                          : sampleUV.x > 1.0 ? sampleUV.x - 1.0 : 0.0;\n"
    "            float depthYc = sampleUV.y < 0.0 ? -sampleUV.y\n"
    "                          : sampleUV.y > 1.0 ? sampleUV.y - 1.0 : 0.0;\n"
    "            float maxDepthX = sampleUV.x < 0.0 ? -csu_left.x\n"
    "                            : sampleUV.x > 1.0 ? csu_right.x - 1.0 : 1.0;\n"
    "            float maxDepthY = sampleUV.y < 0.0 ? -csu_top.y\n"
    "                            : sampleUV.y > 1.0 ? csu_bot.y - 1.0 : 1.0;\n"
    "\n"
    "            float aspect     = screenW / max(screenH, 1.0);\n"
    "            float xFadeScale = max(1.0 / aspect, 1.0);\n"
    "            float yFadeScale = max(aspect, 1.0);\n"
    "            // Corner fade follows the ARC (mirror of D3D9) so the reflection hugs the rounded corner.\n"
    "            vec2  dirCorn   = u / max(distU, 1e-5);\n"
    "            float corMaxX   = (maxDepthX + mbR) / max(abs(dirCorn.x), 0.01);\n"
    "            float corMaxY   = (maxDepthY + mbR) / max(abs(dirCorn.y), 0.01);\n"
    "            float corMaxRef = max(min(corMaxX, corMaxY) - mbR, 1e-5);\n"
    "            float normDepth_corner = clamp(bezelDepth_corner / max(corMaxRef * reflW, 1e-5), 0.0, 1.0);\n"
    "            float dxN = depthXc / max(maxDepthX * xFadeScale, 1e-5);\n"
    "            float dyN = depthYc / max(maxDepthY * yFadeScale, 1e-5);\n"
    "            float normDepth_side = clamp(max(dxN, dyN) / reflW, 0.0, 1.0);\n"
    "            float normDepth = mix(normDepth_side, normDepth_corner, blendCorner);\n"
    "            // Smooth quadratic falloff\n"
    "            float fade = 1.0 - normDepth;\n"
    "            fade = fade * fade;\n"
    "\n"
    "            float startFade = clamp(megaBezelStartFade, 0.0, 1.0);\n"
    "            vec3  reflected = reflColor * megaBezelOpacity * fade * startFade;\n"
    "            // Composite reflection ON TOP of bezel PNG (same layer order as D3D11/D3D9)\n"
    "            if (bezelHookActive >= 0.5) {\n"
    "                vec4 bz = texture(bezelTex, vUV);\n"
    "                bz.rgb *= bezelHookOpacity;\n"
    "                float ra = megaBezelOpacity * fade * startFade;\n"
    "                outColor = vec4(reflected + bz.rgb * bz.a * (1.0 - ra), 1.0);\n"
    "                return;\n"
    "            }\n"
    "            outColor = vec4(reflected, 1.0);\n"
    "            return;\n"
    "        }\n"
    "        // Curvature with no MegaBezel: black outside\n"
    "        if (curved) { outColor = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
    "    }\n"
    "\n"
    "    float mask = 1.0;\n"
    "    float fwY = fwidth(py);\n"
    "    float fwX = fwidth(px);\n"
    "\n"
    "    if (hEnabled != 0 && hThickness > 0.0 && (hThickness + hGap) > 0.0) {\n"
    "        float inBandX = step(hStartX, px) * step(px, hStartX + hWidth);\n"
    "        if (inBandX > 0.5)\n"
    "            mask *= scanlineIntensity(py, hThickness, hGap, hOpacity, fwY);\n"
    "    }\n"
    "\n"
    "    if (vEnabled != 0 && vThickness > 0.0 && (vThickness + vGap) > 0.0) {\n"
    "        float inBandY = step(vStartY, py) * step(py, vStartY + vHeight);\n"
    "        if (inBandY > 0.5)\n"
    "            mask *= scanlineIntensity(px, vThickness, vGap, vOpacity, fwX);\n"
    "    }\n"
    "\n"
    "    bool needTex = blurEnabled >= 0.5 || bloomEnabled >= 0.5 || curved || megaBz\n"
    "                || flickerEnabled >= 0.5 || phosphorEnabled >= 0.5\n"
    "                || abs(brightness) > 0.001 || abs(contrast) > 0.001\n"
    "                || abs(saturation) > 0.001 || abs(temperature) > 0.001\n"
    "                || blackLevel > 0.001 || abs(gamma - 1.0) > 0.001\n"
    "                || vhsEnabled >= 0.5 || grainIntensity > 0.001\n"
    "                || tapeNoiseEnabled >= 0.5 || vignetteEnabled > 0.0\n"
    "                || bezelHookActive >= 0.5;\n"
    "    if (!needTex) {\n"
    "        if (vignetteEnabled > 0.0) {\n"
    "            float r    = max(vignetteEnabled * 0.10, 0.022);\n"
    "            vec2  qv   = abs(vUV - 0.5) - 0.5 + r;\n"
    "            float rSDF = length(max(qv, 0.0)) + min(max(qv.x, qv.y), 0.0) - r;\n"
    "            vec2  outN;\n"
    "            if (qv.x > 0.0 && qv.y > 0.0) outN = normalize(qv);\n"
    "            else if (qv.x > 0.0)          outN = vec2(1.0, 0.0);\n"
    "            else                           outN = vec2(0.0, 1.0);\n"
    "            float fadeW = length(vec2(outN.x * 0.008, outN.y * 0.020));\n"
    "            mask *= smoothstep(0.0, fadeW, -rSDF);\n"
    "        }\n"
    "        outColor = vec4(mask, mask, mask, 1.0);\n"
    "        return;\n"
    "    }\n"
    "\n"
    "    // Remap sampleUV from game-space [0,1] to texture-space for backBuf sampling\n"
    "    vec2 texSampleUV = sampleUV;\n"
    "    if (megaBz && gameRect.z > gameRect.x && gameRect.w > gameRect.y) {\n"
    "        texSampleUV = gameRect.xy + sampleUV * (gameRect.zw - gameRect.xy);\n"
    "    }\n"
    "    vec2 texel = 1.0 / vec2(screenW, screenH);\n"
    "    vec4 color = texture(backBuf, texSampleUV);\n"
    "\n"
    "    // Gaussian blur (must run before B/C/S/T — blur re-samples original texture)\n"
    "    if (blurEnabled >= 0.5) {\n"
    "        float sigma = blurIntensity * 2.0 + 0.0001;\n"
    "        vec4 blurred = vec4(0.0);\n"
    "        float totalW = 0.0;\n"
    "        for (int y = -2; y <= 2; y++) {\n"
    "            for (int x = -2; x <= 2; x++) {\n"
    "                float d2 = float(x*x + y*y);\n"
    "                float w = exp(-d2 / (2.0 * sigma * sigma));\n"
    "                blurred += texture(backBuf, texSampleUV + vec2(x, y) * texel * sigma) * w;\n"
    "                totalW += w;\n"
    "            }\n"
    "        }\n"
    "        color = blurred / totalW;\n"
    "    }\n"
    "\n"
    "    // Bloom (re-samples original texture)\n"
    "    if (bloomEnabled >= 0.5) {\n"
    "        float sigma = bloomIntensity * 3.0 + 0.5;\n"
    "        vec4 glow = vec4(0.0);\n"
    "        float wTotal = 0.0;\n"
    "        for (int by = -3; by <= 3; by++) {\n"
    "            for (int bx = -3; bx <= 3; bx++) {\n"
    "                vec4 s = texture(backBuf, texSampleUV + vec2(bx, by) * texel * sigma);\n"
    "                float lum = dot(s.rgb, vec3(0.299, 0.587, 0.114));\n"
    "                float bright = clamp((lum - 0.4) * 2.5, 0.0, 1.0);\n"
    "                glow += s * bright;\n"
    "                wTotal += bright;\n"
    "            }\n"
    "        }\n"
    "        if (wTotal > 0.001) glow /= wTotal;\n"
    "        color.rgb += glow.rgb * bloomIntensity * 0.6;\n"
    "        color = clamp(color, 0.0, 1.0);\n"
    "    }\n"
    "\n"
    "    // Brightness/Contrast/Saturation/Temperature — applied after blur/bloom\n"
    "    color.rgb *= 1.0 + brightness;\n"
    "    color.rgb = (color.rgb - 0.5) * (1.0 + contrast) + 0.5;\n"
    "    float luma = dot(color.rgb, vec3(0.299, 0.587, 0.114));\n"
    "    color.rgb = mix(vec3(luma), color.rgb, 1.0 + saturation);\n"
    "    color.r += temperature * 0.1;\n"
    "    color.b -= temperature * 0.1;\n"
    "    color = clamp(color, 0.0, 1.0);\n"
    "\n"
    "    // Phosphor Glow — tight micro-halo around bright pixels (CRT phosphor persistence)\n"
    "    if (phosphorEnabled >= 0.5) {\n"
    "        vec4 glow = vec4(0.0);\n"
    "        float wTotal = 0.0;\n"
    "        for (int gy = -2; gy <= 2; gy++) {\n"
    "            for (int gx = -2; gx <= 2; gx++) {\n"
    "                float d2 = float(gx*gx + gy*gy);\n"
    "                float w = exp(-d2 / 1.28);\n"
    "                vec4 s = texture(backBuf, texSampleUV + vec2(gx, gy) * texel);\n"
    "                float lp = dot(s.rgb, vec3(0.2126, 0.7152, 0.0722));\n"
    "                float bright = clamp((lp - 0.3) * 3.0, 0.0, 1.0);\n"
    "                glow += s * (w * bright);\n"
    "                wTotal += w * bright;\n"
    "            }\n"
    "        }\n"
    "        if (wTotal > 0.001)\n"
    "            color.rgb += (glow.rgb / wTotal) * phosphorIntensity * 0.3;\n"
    "        color = clamp(color, 0.0, 1.0);\n"
    "    }\n"
    "\n"
    "    if (curved) {\n"
    "        vec2 vigUV = sampleUV * 2.0 - 1.0;\n"
    "        float vig = 1.0 - dot(vigUV, vigUV) * 0.3 * curvatureIntensity;\n"
    "        color.rgb *= clamp(vig, 0.0, 1.0);\n"
    "    }\n"
    "\n"
    "    // Sub-pixel dither — breaks 8-bit banding on vignette/gradients.\n"
    "    // gl_FragCoord.xy is always screen-aligned, never warped by barrel distortion.\n"
    "    float dither = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n"
    "    color.rgb += (dither - 0.5) * (1.0 / 255.0);\n"
    "\n"
    "    // Apply scanline mask\n"
    "    vec4 output = color * mask;\n"
    "\n"
    "    // Final grade: Black Level + Gamma — applied last as fine-tuning\n"
    "    // Black Level: luminance-masked crush — only affects shadows below ~30% luma\n"
    "    if (blackLevel > 0.001) {\n"
    "        float lum = dot(output.rgb, vec3(0.2126, 0.7152, 0.0722));\n"
    "        float darkMask = 1.0 - smoothstep(0.0, 0.3, lum);\n"
    "        output.rgb = max(output.rgb - blackLevel * darkMask, vec3(0.0));\n"
    "    }\n"
    "    if (abs(gamma - 1.0) > 0.001)\n"
    "        output.rgb = pow(max(output.rgb, vec3(0.0001)), vec3(1.0 / gamma));\n"
    "\n"
    "    // CRT Flicker — smooth sine LFO with vertical phase offset\n"
    "    if (flickerEnabled >= 0.5) {\n"
    "        float freq = 1.0 + flickerRate * 19.0;\n"
    "        float phase = vUV.y * 0.15;\n"
    "        float flicker = sin(time * 6.28318 * freq + phase) * 0.05 * flickerIntensity;\n"
    "        output.rgb *= 1.0 + flicker;\n"
    "    }\n"
    "\n"
    "    // VHS tape effect — realistic analog tape degradation + NTSC composite artifacts\n"
    "    if (vhsEnabled >= 0.5 && vUV.y <= 0.96) {\n"
    "        float inten = vhsIntensity;\n"
    "        float t     = time;\n"
    "        float rawSrcLuma  = dot(texture(backBuf, texSampleUV).rgb, vec3(0.299, 0.587, 0.114));\n"
    "        float contentMask = smoothstep(0.0, 0.015, rawSrcLuma);\n"
    "        // — 1. LINE JITTER: sparse spike lines only (no global sinusoidal shift) —\n"
    "        float lineIdx = floor(vUV.y * 720.0);\n"
    "        float sH    = fract(sin(lineIdx * 127.1 + floor(t * 10.0) * 311.7) * 43758.5);\n"
    "        float spike = (sH > 0.97) ? (sH - 0.97) / 0.03 * 0.016 - 0.008 : 0.0;\n"
    "        vec2  jUV   = texSampleUV + vec2(spike * inten, 0.0);\n"
    "        vec4  s0    = texture(backBuf, jUV);\n"
    "        // — 3. NTSC DOT CRAWL: animated color shimmer on chroma edges —\n"
    "        float luma0  = dot(s0.rgb, vec3(0.299, 0.587, 0.114));\n"
    "        float chrMag = clamp(length(s0.rgb - luma0) * 2.5, 0.0, 1.0);\n"
    "        float dotPhi = (vUV.x * 240.0 + floor(t * 29.97) * 0.5) * 3.14159265;\n"
    "        output.r += sin(dotPhi)         * inten * 0.008 * chrMag * contentMask;\n"
    "        output.b += sin(dotPhi + 1.047) * inten * 0.006 * chrMag * contentMask;\n"
    "        // — 4. LUMA NOISE: horizontal tape hiss streaks —\n"
    "        float ny     = floor(vUV.y * 200.0);\n"
    "        float nx     = floor(vUV.x * 15.0);\n"
    "        float nt     = floor(t * 25.0);\n"
    "        float streak = fract(sin(dot(vec2(nx + ny * 200.0, nt), vec2(127.1, 311.7))) * 43758.5) - 0.5;\n"
    "        output.rgb  += streak * inten * 0.022 * contentMask;\n"
    "        // — 5. HEAD-SWITCHING BAND: bottom ~3% of screen, mechanical artifact —\n"
    "        float headZone = smoothstep(0.97, 1.00, vUV.y) * contentMask;\n"
    "        float headH    = fract(sin(floor(vUV.y * 300.0) * 127.1 + floor(t * 30.0) * 311.7) * 43758.5);\n"
    "        float headOff  = (headH - 0.5) * inten * 0.030;\n"
    "        vec4  headS    = texture(backBuf, jUV + vec2(headOff, 0.0));\n"
    "        output.rgb     = mix(output.rgb, headS.rgb * mask + headH * inten * 0.35, headZone);\n"
    "        // — 6. COLOR GRADING: desaturation + luma lift + warm shadows —\n"
    "        float vLuma = dot(output.rgb, vec3(0.299, 0.587, 0.114));\n"
    "        output.rgb  = mix(output.rgb, vec3(vLuma), inten * 0.25 * contentMask);\n"
    "        output.rgb *= mix(1.0, 1.06, inten * contentMask);\n"
    "        output.r   += inten * 0.020 * (1.0 - vLuma) * contentMask;\n"
    "        output.b   -= inten * 0.014 * (1.0 - vLuma) * contentMask;\n"
    "        output      = clamp(output, 0.0, 1.0);\n"
    "    }\n"
    "\n"
    "    // Film Grain — true per-pixel noise, hash without sine (no periodic banding)\n"
    "    if (grainIntensity > 0.001) {\n"
    "        float lum   = dot(output.rgb, vec3(0.299, 0.587, 0.114));\n"
    "        vec3  p3    = fract(vec3(gl_FragCoord.xy, floor(time * 24.0) + 1.0)\n"
    "                          * vec3(0.1031, 0.1030, 0.0973));\n"
    "        p3         += dot(p3, p3.yzx + 33.33);\n"
    "        float grain = fract((p3.x + p3.y) * p3.z) * 2.0 - 1.0;\n"
    "        float amp   = 1.0 - (2.0 * lum - 1.0) * (2.0 * lum - 1.0);\n"
    "        output.rgb += grain * grainIntensity * 0.18 * amp;\n"
    "        output = clamp(output, 0.0, 1.0);\n"
    "    }\n"
    "\n"
    "    // Tape Noise — libretro-style analog tape interference spikes\n"
    "    if (tapeNoiseEnabled >= 0.5) {\n"
    "        float fc     = time * 24.0;\n"
    "        vec2  tnUV   = vUV * screenH * 4.0;\n"
    "        float col    = tnNn(tnUV, fc);\n"
    "        output.rgb  += clamp(vec3(col), 0.0, 0.5) * tapeNoiseIntensity;\n"
    "        output       = clamp(output, 0.0, 1.0);\n"
    "    }\n"
    "\n"
    "    if (vignetteEnabled > 0.0) {\n"
    "        float r    = max(vignetteEnabled * 0.10, 0.022);\n"
    "        vec2  qv   = abs(sampleUV - 0.5) - 0.5 + r;\n"
    "        float rSDF = length(max(qv, 0.0)) + min(max(qv.x, qv.y), 0.0) - r;\n"
    "        vec2  outN;\n"
    "        if (qv.x > 0.0 && qv.y > 0.0) outN = normalize(qv);\n"
    "        else if (qv.x > 0.0)          outN = vec2(1.0, 0.0);\n"
    "        else                           outN = vec2(0.0, 1.0);\n"
    "        float fadeW = length(vec2(outN.x * 0.008, outN.y * 0.020));\n"
    "        output.rgb *= smoothstep(0.0, fadeW, -rSDF);\n"
    "    }\n"
    "\n"
    "    // REFL. RADIUS: hard-mask the rounded-off game corner to black (bezel covers).\n"
    "    if (megaBz && mbSDF > 0.0) output.rgb = vec3(0.0, 0.0, 0.0);\n"
    "    // Bezel PNG overlay (inside-game pixels) — same layer order as D3D11/D3D9\n"
    "    if (bezelHookActive >= 0.5) {\n"
    "        vec4 bz = texture(bezelTex, vUV);\n"
    "        float a = bz.a * bezelHookOpacity;\n"
    "        output.rgb = output.rgb * (1.0 - a) + bz.rgb * a;\n"
    "    }\n"
    "\n"
    "    outColor = output;\n"
    "}\n";

// ── OSD overlay shaders (GL) ────────────────────────────────────────────
static const char* GL_OSD_VS =
    "#version 150\n"
    "out vec2 vUV;\n"
    "uniform vec4 uRect;\n"  // (x, y, w, h) in NDC
    "void main() {\n"
    "    vec2 c;\n"
    "    if      (gl_VertexID == 0) c = vec2(0.0, 0.0);\n"
    "    else if (gl_VertexID == 1) c = vec2(1.0, 0.0);\n"
    "    else if (gl_VertexID == 2) c = vec2(0.0, 1.0);\n"
    "    else                       c = vec2(1.0, 1.0);\n"
    "    gl_Position = vec4(uRect.xy + c * uRect.zw, 0.0, 1.0);\n"
    "    vUV = vec2(c.x, 1.0 - c.y);\n"
    "}\n";

static const char* GL_OSD_FS =
    "#version 150\n"
    "in vec2 vUV;\n"
    "out vec4 outColor;\n"
    "uniform sampler2D uTex;\n"
    "void main() {\n"
    "    outColor = texture(uTex, vUV);\n"
    "}\n";

static bool LoadGLExtensions() {
    HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    if (!gl) return false;

    // wglGetProcAddress for shader functions
    typedef void* (WINAPI* PFN_wglGetProcAddress)(const char*);
    auto wglGetProc = (PFN_wglGetProcAddress)GetProcAddress(gl, "wglGetProcAddress");
    if (!wglGetProc) return false;

    glCreateShader_fn = (PFNGLCREATESHADERPROC)wglGetProc("glCreateShader");
    glShaderSource_fn = (PFNGLSHADERSOURCEPROC)wglGetProc("glShaderSource");
    glCompileShader_fn = (PFNGLCOMPILESHADERPROC)wglGetProc("glCompileShader");
    glCreateProgram_fn = (PFNGLCREATEPROGRAMPROC)wglGetProc("glCreateProgram");
    glAttachShader_fn = (PFNGLATTACHSHADERPROC)wglGetProc("glAttachShader");
    glLinkProgram_fn = (PFNGLLINKPROGRAMPROC)wglGetProc("glLinkProgram");
    glUseProgram_fn = (PFNGLUSEPROGRAMPROC)wglGetProc("glUseProgram");
    glDeleteShader_fn = (PFNGLDELETESHADERPROC)wglGetProc("glDeleteShader");
    glGetUniformLocation_fn = (PFNGLGETUNIFORMLOCATIONPROC)wglGetProc("glGetUniformLocation");
    glUniform1f_fn = (PFNGLUNIFORM1FPROC)wglGetProc("glUniform1f");
    glUniform1i_fn = (PFNGLUNIFORM1IPROC)wglGetProc("glUniform1i");
    glUniform4f_fn = (PFNGLUNIFORM4FPROC)wglGetProc("glUniform4f");
    glGetShaderiv_fn = (PFNGLGETSHADERIVPROC)wglGetProc("glGetShaderiv");
    // VAO (required in OpenGL 3.2+ core profile, optional in compat)
    glGenVertexArrays_fn   = (PFNGLGENVERTEXARRAYSPROC)wglGetProc("glGenVertexArrays");
    glBindVertexArray_fn   = (PFNGLBINDVERTEXARRAYPROC)wglGetProc("glBindVertexArray");
    // FBO
    glBindFramebuffer_fn   = (PFNGLBINDFRAMEBUFFERPROC)wglGetProc("glBindFramebuffer");
    // Shader diagnostics
    glGetShaderInfoLog_fn  = (PFNGLGETSHADERINFOLOGPROC)wglGetProc("glGetShaderInfoLog");
    glGetProgramiv_fn      = (PFNGLGETPROGRAMIVPROC)wglGetProc("glGetProgramiv");
    glGetProgramInfoLog_fn = (PFNGLGETPROGRAMINFOLOGPROC)wglGetProc("glGetProgramInfoLog");
    // Blend equation — CRITICAL for correct multiplicative blending
    glBlendEquation_fn     = (PFNGLBLENDEQUATIONPROC_)wglGetProc("glBlendEquation");
    // Fragment output binding — explicit location for core profile
    glBindFragDataLocation_fn = (PFNGLBINDFRAGDATALOCATIONPROC)wglGetProc("glBindFragDataLocation");
    // glActiveTexture — needed for blur texture binding
    glActiveTexture_fn = (PFNGLACTIVETEXTUREPROC_)wglGetProc("glActiveTexture");

    Log("[GL] Extensions: vao=%s fbo=%s shaderLog=%s blendEq=%s fragDataLoc=%s",
        glBindVertexArray_fn ? "OK" : "missing",
        glBindFramebuffer_fn ? "OK" : "missing",
        glGetShaderInfoLog_fn ? "OK" : "missing",
        glBlendEquation_fn ? "OK" : "missing",
        glBindFragDataLocation_fn ? "OK" : "missing");

    return glCreateShader_fn && glShaderSource_fn && glCompileShader_fn &&
           glCreateProgram_fn && glAttachShader_fn && glLinkProgram_fn &&
           glUseProgram_fn && glGetUniformLocation_fn && glUniform1f_fn && glUniform1i_fn;
}

static bool InitGLResources() {
    Log("[GL] InitGLResources — first wglSwapBuffers call");
    if (!LoadGLExtensions()) { Log("[GL] FAIL: LoadGLExtensions (wglGetProcAddress not found)"); return false; }

    // Log GL version for diagnosis
    const char* glVer = (const char*)glGetString(GL_VERSION);
    const char* glRenderer = (const char*)glGetString(GL_RENDERER);
    Log("[GL] Version: %s | Renderer: %s", glVer ? glVer : "?", glRenderer ? glRenderer : "?");

    // Helper: log shader/program compile/link errors
    auto LogShaderStatus = [&](const char* stage, GLuint obj, bool isProgram) {
        GLint status = 0;
        if (!isProgram && glGetShaderiv_fn)
            glGetShaderiv_fn(obj, GL_COMPILE_STATUS, &status);
        else if (isProgram && glGetProgramiv_fn)
            glGetProgramiv_fn(obj, 0x8B82 /*GL_LINK_STATUS*/, &status);
        else
            status = 1; // unknown — assume OK if no query fn

        char buf[1024] = {};
        if (!status) {
            if (!isProgram && glGetShaderInfoLog_fn)
                glGetShaderInfoLog_fn(obj, 1023, nullptr, buf);
            else if (isProgram && glGetProgramInfoLog_fn)
                glGetProgramInfoLog_fn(obj, 1023, nullptr, buf);
            Log("[GL] FAIL: %s — %s", stage, buf[0] ? buf : "(no info log)");
        } else {
            Log("[GL] OK: %s compiled/linked", stage);
        }
        return (bool)status;
    };

    // Compile vertex shader (#version 150 — GL 3.2 core profile)
    GLuint vs = glCreateShader_fn(GL_VERTEX_SHADER);
    glShaderSource_fn(vs, 1, &GL_VS_SRC, nullptr);
    glCompileShader_fn(vs);
    if (!LogShaderStatus("vertex shader", vs, false)) {
        glDeleteShader_fn(vs);
        return false;
    }

    // Compile fragment shader (#version 150 with 'out vec4 outColor')
    GLuint fs = glCreateShader_fn(GL_FRAGMENT_SHADER);
    glShaderSource_fn(fs, 1, &GL_FS_SRC, nullptr);
    glCompileShader_fn(fs);
    if (!LogShaderStatus("fragment shader", fs, false)) {
        glDeleteShader_fn(vs); glDeleteShader_fn(fs);
        return false;
    }

    // Link program
    g_GLProgram = glCreateProgram_fn();
    glAttachShader_fn(g_GLProgram, vs);
    glAttachShader_fn(g_GLProgram, fs);
    // CRITICAL: Bind fragment output BEFORE linking — ensures outColor -> location 0
    if (glBindFragDataLocation_fn) {
        glBindFragDataLocation_fn(g_GLProgram, 0, "outColor");
        Log("[GL] glBindFragDataLocation(outColor -> 0) OK");
    }
    glLinkProgram_fn(g_GLProgram);
    glDeleteShader_fn(vs);
    glDeleteShader_fn(fs);
    if (!LogShaderStatus("program link", g_GLProgram, true)) return false;

    // Get uniform locations — log ALL of them (if any is -1, that uniform is dead)
    g_uScreenW = glGetUniformLocation_fn(g_GLProgram, "screenW");
    g_uScreenH = glGetUniformLocation_fn(g_GLProgram, "screenH");
    g_uHThickness = glGetUniformLocation_fn(g_GLProgram, "hThickness");
    g_uHGap = glGetUniformLocation_fn(g_GLProgram, "hGap");
    g_uHOpacity = glGetUniformLocation_fn(g_GLProgram, "hOpacity");
    g_uHStartX = glGetUniformLocation_fn(g_GLProgram, "hStartX");
    g_uHWidth = glGetUniformLocation_fn(g_GLProgram, "hWidth");
    g_uHEnabled = glGetUniformLocation_fn(g_GLProgram, "hEnabled");
    g_uVThickness = glGetUniformLocation_fn(g_GLProgram, "vThickness");
    g_uVGap = glGetUniformLocation_fn(g_GLProgram, "vGap");
    g_uVOpacity = glGetUniformLocation_fn(g_GLProgram, "vOpacity");
    g_uVStartY = glGetUniformLocation_fn(g_GLProgram, "vStartY");
    g_uVHeight = glGetUniformLocation_fn(g_GLProgram, "vHeight");
    g_uVEnabled = glGetUniformLocation_fn(g_GLProgram, "vEnabled");
    Log("[GL] Uniform locations: screenW=%d screenH=%d hThick=%d hGap=%d hOpacity=%d hStartX=%d hWidth=%d hEnabled=%d",
        g_uScreenW, g_uScreenH, g_uHThickness, g_uHGap, g_uHOpacity, g_uHStartX, g_uHWidth, g_uHEnabled);
    Log("[GL] Uniform locations: vThick=%d vGap=%d vOpacity=%d vStartY=%d vHeight=%d vEnabled=%d",
        g_uVThickness, g_uVGap, g_uVOpacity, g_uVStartY, g_uVHeight, g_uVEnabled);
    g_uBlurEnabled    = glGetUniformLocation_fn(g_GLProgram, "blurEnabled");
    g_uBlurIntensity  = glGetUniformLocation_fn(g_GLProgram, "blurIntensity");
    g_uBloomEnabled       = glGetUniformLocation_fn(g_GLProgram, "bloomEnabled");
    g_uBloomIntensity     = glGetUniformLocation_fn(g_GLProgram, "bloomIntensity");
    g_uCurvatureEnabled   = glGetUniformLocation_fn(g_GLProgram, "curvatureEnabled");
    g_uCurvatureIntensity = glGetUniformLocation_fn(g_GLProgram, "curvatureIntensity");
    g_uBrightness         = glGetUniformLocation_fn(g_GLProgram, "brightness");
    g_uContrast           = glGetUniformLocation_fn(g_GLProgram, "contrast");
    g_uSaturation         = glGetUniformLocation_fn(g_GLProgram, "saturation");
    g_uTemperature        = glGetUniformLocation_fn(g_GLProgram, "temperature");
    g_uBlackLevel         = glGetUniformLocation_fn(g_GLProgram, "blackLevel");
    g_uGamma              = glGetUniformLocation_fn(g_GLProgram, "gamma");
    g_uFlickerEnabled     = glGetUniformLocation_fn(g_GLProgram, "flickerEnabled");
    g_uFlickerIntensity   = glGetUniformLocation_fn(g_GLProgram, "flickerIntensity");
    g_uFlickerRate        = glGetUniformLocation_fn(g_GLProgram, "flickerRate");
    g_uTime               = glGetUniformLocation_fn(g_GLProgram, "time");
    g_uPhosphorEnabled    = glGetUniformLocation_fn(g_GLProgram, "phosphorEnabled");
    g_uPhosphorIntensity  = glGetUniformLocation_fn(g_GLProgram, "phosphorIntensity");
    g_uVhsEnabled         = glGetUniformLocation_fn(g_GLProgram, "vhsEnabled");
    g_uVhsIntensity       = glGetUniformLocation_fn(g_GLProgram, "vhsIntensity");
    g_uGrainIntensity     = glGetUniformLocation_fn(g_GLProgram, "grainIntensity");
    g_uTapeNoiseEnabled   = glGetUniformLocation_fn(g_GLProgram, "tapeNoiseEnabled");
    g_uTapeNoiseIntensity = glGetUniformLocation_fn(g_GLProgram, "tapeNoiseIntensity");
    g_uVignetteEnabled    = glGetUniformLocation_fn(g_GLProgram, "vignetteEnabled");
    g_uMegaBezelEnabled         = glGetUniformLocation_fn(g_GLProgram, "megaBezelEnabled");
    g_uMegaBezelThickness       = glGetUniformLocation_fn(g_GLProgram, "megaBezelThickness");
    g_uMegaBezelOpacity         = glGetUniformLocation_fn(g_GLProgram, "megaBezelOpacity");
    g_uMegaBezelBlur            = glGetUniformLocation_fn(g_GLProgram, "megaBezelBlur");
    g_uMegaBezelRadius          = glGetUniformLocation_fn(g_GLProgram, "megaBezelRadius");
    g_uMegaBezelReflectionWidth = glGetUniformLocation_fn(g_GLProgram, "megaBezelReflectionWidth");
    g_uMegaBezelStartFade       = glGetUniformLocation_fn(g_GLProgram, "megaBezelStartFade");
    g_uGameRect                 = glGetUniformLocation_fn(g_GLProgram, "gameRect");
    g_uBezelHookActive  = glGetUniformLocation_fn(g_GLProgram, "bezelHookActive");
    g_uBezelHookOpacity = glGetUniformLocation_fn(g_GLProgram, "bezelHookOpacity");
    g_uBezelTex         = glGetUniformLocation_fn(g_GLProgram, "bezelTex");
    g_uBackBuf       = glGetUniformLocation_fn(g_GLProgram, "backBuf");
    Log("[GL] Blur uniform locations: blurEnabled=%d blurIntensity=%d backBuf=%d",
        g_uBlurEnabled, g_uBlurIntensity, g_uBackBuf);

    // Create a VAO — required in OpenGL 3.2+ core profile for glDrawArrays
    if (glGenVertexArrays_fn && glBindVertexArray_fn) {
        glGenVertexArrays_fn(1, &g_GLVao);
        Log("[GL] VAO created (id=%u)", g_GLVao);
    } else {
        Log("[GL] WARNING: glGenVertexArrays not available — may crash in core profile");
    }

    // ── Build OSD shader program ──────────────────────────────────────
    {
        GLuint ovs = glCreateShader_fn(GL_VERTEX_SHADER);
        glShaderSource_fn(ovs, 1, &GL_OSD_VS, nullptr);
        glCompileShader_fn(ovs);

        GLuint ofs = glCreateShader_fn(GL_FRAGMENT_SHADER);
        glShaderSource_fn(ofs, 1, &GL_OSD_FS, nullptr);
        glCompileShader_fn(ofs);

        g_OsdGLProgram = glCreateProgram_fn();
        glAttachShader_fn(g_OsdGLProgram, ovs);
        glAttachShader_fn(g_OsdGLProgram, ofs);
        if (glBindFragDataLocation_fn)
            glBindFragDataLocation_fn(g_OsdGLProgram, 0, "outColor");
        glLinkProgram_fn(g_OsdGLProgram);
        glDeleteShader_fn(ovs);
        glDeleteShader_fn(ofs);

        g_OsdU_Rect = glGetUniformLocation_fn(g_OsdGLProgram, "uRect");
        g_OsdU_Tex  = glGetUniformLocation_fn(g_OsdGLProgram, "uTex");
        Log("[OSD-GL] OSD shader: program=%u uRect=%d uTex=%d", g_OsdGLProgram, g_OsdU_Rect, g_OsdU_Tex);
    }

    OpenSharedMem();

    g_GLInited = true;
    Log("[GL] Resources initialized OK — scanlines active");
    return true;
}

// GL constants not always in gl.h
#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM 0x8B8D
#endif
#ifndef GL_BLEND_SRC
#define GL_BLEND_SRC 0x0BE1
#endif
#ifndef GL_BLEND_DST
#define GL_BLEND_DST 0x0BE0
#endif

// ── GL bezel PNG loader (WIC → OpenGL texture) ─────────────────────────────
// Mirrors LoadBezelTexture (D3D11) and LoadBezelTextureD3D9.
// Decodes PNG via WIC to RGBA pixels and uploads to a GL_TEXTURE_2D on unit 1.
static void ReleaseBezelTextureGL() {
    if (g_GLBezelTex) { glDeleteTextures(1, &g_GLBezelTex); g_GLBezelTex = 0; }
    g_GLBezelPathCached[0] = L'\0';
}

static bool LoadBezelTextureGL(const wchar_t* path) {
    ReleaseBezelTextureGL();
    if (!path || !path[0]) return false;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IWICImagingFactory*    wic       = nullptr;
    IWICBitmapDecoder*     decoder   = nullptr;
    IWICBitmapFrameDecode* frame     = nullptr;
    IWICFormatConverter*   converter = nullptr;
    BYTE*                  pixels    = nullptr;
    bool ok = false;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(hr)) { Log("[GL-BEZEL] CoCreateInstance(WIC) hr=0x%08X", hr); goto cleanup; }

    hr = wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) { Log("[GL-BEZEL] decode '%ls' hr=0x%08X", path, hr); goto cleanup; }

    if (FAILED(decoder->GetFrame(0, &frame))) goto cleanup;
    if (FAILED(wic->CreateFormatConverter(&converter))) goto cleanup;
    if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) goto cleanup;

    {
        UINT w = 0, h = 0;
        converter->GetSize(&w, &h);
        if (w == 0 || h == 0) goto cleanup;

        UINT stride = w * 4;
        UINT imgSize = stride * h;
        pixels = new BYTE[imgSize];
        if (FAILED(converter->CopyPixels(nullptr, stride, imgSize, pixels))) goto cleanup;

        // Upload to GL texture
        glGenTextures(1, &g_GLBezelTex);
        glBindTexture(GL_TEXTURE_2D, g_GLBezelTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F /*GL_CLAMP_TO_EDGE*/);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F /*GL_CLAMP_TO_EDGE*/);

        wcscpy_s(g_GLBezelPathCached, 260, path);
        Log("[GL-BEZEL] Loaded '%ls' (%ux%u) into GL texture %u", path, w, h, g_GLBezelTex);
        ok = true;
    }

cleanup:
    if (pixels)    delete[] pixels;
    if (converter) converter->Release();
    if (frame)     frame->Release();
    if (decoder)   decoder->Release();
    if (wic)       wic->Release();
    if (!ok)       ReleaseBezelTextureGL();
    return ok;
}

static bool g_GLFirstFrame = true;
static void ApplyGLScanlines(HDC hdc) {
    if (!g_Shared) { OpenSharedMem(); }
    if (!g_Shared) {
        if (g_GLFirstFrame) { Log("[GL] ApplyGLScanlines: no shared memory"); g_GLFirstFrame = false; }
        return;
    }
    if (!g_Shared->active) {
        if (g_GLFirstFrame) { Log("[GL] ApplyGLScanlines: active=0 (disabled from S4W UI)"); g_GLFirstFrame = false; }
        return;
    }

    // ══════════════════════════════════════════════════════════════════════
    // SAVE COMPLETE OpenGL STATE — bullet-proof approach for any game/driver
    // ══════════════════════════════════════════════════════════════════════
    GLint prevProgram = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    GLboolean prevBlend = glIsEnabled(GL_BLEND);
    GLint prevBlendSrc = GL_ONE, prevBlendDst = GL_ZERO;
    glGetIntegerv(GL_BLEND_SRC, &prevBlendSrc);
    glGetIntegerv(GL_BLEND_DST, &prevBlendDst);
    GLint prevBlendEqRGB = 0x8006; // GL_FUNC_ADD default
    glGetIntegerv(0x8009 /*GL_BLEND_EQUATION_RGB*/, &prevBlendEqRGB);
    GLboolean prevDepth   = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prevCull    = glIsEnabled(GL_CULL_FACE);
    GLboolean prevScissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean prevStencil = glIsEnabled(GL_STENCIL_TEST);
    // COLOR WRITE MASK — if game disabled color writes, our draw is invisible!
    GLboolean prevColorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    glGetBooleanv(0x0C23 /*GL_COLOR_WRITEMASK*/, prevColorMask);
    // VIEWPORT — save and override to cover entire framebuffer
    GLint prevViewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    // VAO — critical for OpenGL 3.2+ core profile
    GLint prevVAO = 0;
    if (glBindVertexArray_fn)
        glGetIntegerv(0x85B5 /*GL_VERTEX_ARRAY_BINDING*/, &prevVAO);
    // FBO — save current draw framebuffer
    GLint prevDrawFB = 0;
    if (glBindFramebuffer_fn)
        glGetIntegerv(0x8CA6 /*GL_DRAW_FRAMEBUFFER_BINDING*/, &prevDrawFB);

    // ── Full window size (computed before log and glViewport) ─────────────
    // Use the FULL WINDOW size for scanline coverage, not the game's sub-viewport.
    // The game may render 4:3 content in a sub-viewport within a 16:9 window
    // (e.g., Ares renders 1456x1080 inside a 1920x1080 window). wglSwapBuffers
    // presents the ENTIRE window, so we override with the real client rect.
    int vpW = 0, vpH = 0;
    {
        HWND hwnd = WindowFromDC(hdc);
        if (hwnd) {
            RECT rc;
            if (GetClientRect(hwnd, &rc)) {
                vpW = rc.right;
                vpH = rc.bottom;
            }
        }
    }
    // Fallback: game's GL viewport, then shared-mem screen size
    if (vpW <= 0) vpW = prevViewport[2] > 0 ? prevViewport[2] : (int)g_Shared->screenW;
    if (vpH <= 0) vpH = prevViewport[3] > 0 ? prevViewport[3] : (int)g_Shared->screenH;

    // ── Log diagnostics on first scanline draw ────────────────────────────
    if (g_GLFirstFrame) {
        Log("[GL] First scanline draw: gameViewport=(%d,%d,%d,%d) fullWindow=%dx%d hEnabled=%d hThick=%.1f hGap=%.1f hOpacity=%.2f ",
            prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3], vpW, vpH,
            g_Shared->hEnabled, g_Shared->hThickness, g_Shared->hGap, g_Shared->hOpacity);
        Log("[GL] State at draw: FBO=%d VAO=%d program=%d blend=%d colorMask=(%d,%d,%d,%d) stencil=%d scissor=%d",
            prevDrawFB, prevVAO, prevProgram, (int)prevBlend,
            (int)prevColorMask[0], (int)prevColorMask[1], (int)prevColorMask[2], (int)prevColorMask[3],
            (int)prevStencil, (int)prevScissor);
        Log("[GL] screenW=%.0f screenH=%.0f hStartX=%.0f hWidth=%.0f (shared mem)",
            g_Shared->screenW, g_Shared->screenH, g_Shared->hStartX, g_Shared->hWidth);
        g_GLFirstFrame = false;
    }

    // ══════════════════════════════════════════════════════════════════════
    // SET OUR RENDER STATE — override EVERYTHING the game might have left
    // ══════════════════════════════════════════════════════════════════════

    // Force FB0 (the actual displayed backbuffer)
    if (prevDrawFB != 0 && glBindFramebuffer_fn) {
        Log("[GL] FBO %d was bound — switching to FB0 for scanline draw", prevDrawFB);
        glBindFramebuffer_fn(0x8D40 /*GL_DRAW_FRAMEBUFFER*/, 0);
    }

    // CRITICAL: Force color writes ON
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // Copy framebuffer when blur, bloom or curvature is active
    bool glBlurOn   = g_Shared->blurEnabled      > 0.0f && g_Shared->blurIntensity      > 0.0f;
    bool glBloomOn  = g_Shared->bloomEnabled     > 0.0f && g_Shared->bloomIntensity     > 0.0f;
    bool glCurvOn   = g_Shared->curvatureEnabled > 0.0f && g_Shared->curvatureIntensity > 0.0f;
    bool glBcOn     = g_Shared->brightness != 0.0f || g_Shared->contrast != 0.0f || g_Shared->saturation != 0.0f || g_Shared->temperature != 0.0f
                   || g_Shared->blackLevel > 0.0f || (g_Shared->gamma != 1.0f && g_Shared->gamma != 0.0f);
    bool glFlickOn    = g_Shared->flickerEnabled  > 0.0f && g_Shared->flickerIntensity  > 0.0f;
    bool glPhosphOn   = g_Shared->phosphorEnabled > 0.0f && g_Shared->phosphorIntensity > 0.0f;
    bool glVhsOn         = g_Shared->vhsEnabled        > 0.0f;
    bool glGrainOn       = g_Shared->grainIntensity    > 0.0f;
    bool glTapeNoiseOn   = g_Shared->tapeNoiseEnabled  > 0.0f && g_Shared->tapeNoiseIntensity > 0.0f;
    bool glMegaBzOn      = g_Shared->megaBezelEnabled  > 0.0f;
    bool glBezelHookOn   = g_Shared->bezelHookActive   > 0.0f;
    bool glNeedCopy = (glBlurOn || glBloomOn || glCurvOn || glBcOn || glFlickOn || glPhosphOn || glVhsOn || glGrainOn || glTapeNoiseOn || glMegaBzOn || glBezelHookOn) && glActiveTexture_fn;
    GLint prevTexBinding = 0;
    GLint prevActiveTexUnit = 0;
    if (glNeedCopy) {
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexBinding);
        glGetIntegerv(0x84E0 /*GL_ACTIVE_TEXTURE*/, &prevActiveTexUnit);
        glActiveTexture_fn(0x84C0 /*GL_TEXTURE0*/);
        // Create or resize blur copy texture
        if (!g_GLBlurTex) glGenTextures(1, &g_GLBlurTex);
        glBindTexture(GL_TEXTURE_2D, g_GLBlurTex);
        if (g_GLBlurTexW != vpW || g_GLBlurTexH != vpH) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vpW, vpH, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F /*GL_CLAMP_TO_EDGE*/);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F /*GL_CLAMP_TO_EDGE*/);
            g_GLBlurTexW = vpW;
            g_GLBlurTexH = vpH;
            Log("[GL] Blur: created copy texture %dx%d", vpW, vpH);
        }
        // Copy current framebuffer into our texture
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, vpW, vpH);
    }

    // ── Load bezel PNG on texture unit 1 (mirrors D3D11/D3D9 bezel binding) ──
    if (glBezelHookOn && glActiveTexture_fn) {
        bool pathChanged = wcscmp(g_Shared->bezelHookPath, g_GLBezelPathCached) != 0;
        if (pathChanged || (!g_GLBezelTex && g_Shared->bezelHookPath[0])) {
            // Must bind unit 1 for the texture upload, then restore
            glActiveTexture_fn(0x84C1 /*GL_TEXTURE1*/);
            LoadBezelTextureGL(g_Shared->bezelHookPath);
        }
        if (g_GLBezelTex) {
            glActiveTexture_fn(0x84C1 /*GL_TEXTURE1*/);
            glBindTexture(GL_TEXTURE_2D, g_GLBezelTex);
            glActiveTexture_fn(0x84C0 /*GL_TEXTURE0*/);
        }
    }

    // Blur on: overwrite blend (shader writes final blurred+masked color)
    // Blur off: multiplicative blend (dest * mask)
    glEnable(GL_BLEND);
    if (glNeedCopy) {
        glBlendFunc(GL_ONE, GL_ZERO); // overwrite
    } else {
        glBlendFunc(GL_ZERO, GL_SRC_COLOR); // multiplicative
    }
    if (glBlendEquation_fn) glBlendEquation_fn(0x8006 /*GL_FUNC_ADD*/);

    // Disable all tests that could reject our fragments
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);

    // Full window viewport for scanline rendering
    glViewport(0, 0, vpW, vpH);

    // Bind our VAO (required in core profile — empty VAO is sufficient)
    if (glBindVertexArray_fn && g_GLVao)
        glBindVertexArray_fn(g_GLVao);

    // Use our scanline shader
    glUseProgram_fn(g_GLProgram);

    // ── INJECTION MODE: always use full viewport ──
    // In hook injection, the game's framebuffer IS the content (no black bars).
    // hStartX/hWidth from shared memory are screen-relative and don't apply here.
    // Scanline thickness/gap/opacity still come from settings (per-system profiles).
    float fVpW = (float)vpW;
    float fVpH = (float)vpH;

    glUniform1f_fn(g_uScreenW, fVpW);
    glUniform1f_fn(g_uScreenH, fVpH);
    glUniform1f_fn(g_uHThickness, g_Shared->hThickness);
    glUniform1f_fn(g_uHGap, g_Shared->hGap);
    glUniform1f_fn(g_uHOpacity, g_Shared->hOpacity);
    glUniform1f_fn(g_uHStartX, 0.0f);        // full viewport — no band offset
    glUniform1f_fn(g_uHWidth, fVpW);          // full viewport width
    glUniform1i_fn(g_uHEnabled, g_Shared->hEnabled);
    glUniform1f_fn(g_uVThickness, g_Shared->vThickness);
    glUniform1f_fn(g_uVGap, g_Shared->vGap);
    glUniform1f_fn(g_uVOpacity, g_Shared->vOpacity);
    glUniform1f_fn(g_uVStartY, 0.0f);        // full viewport — no band offset
    glUniform1f_fn(g_uVHeight, fVpH);         // full viewport height
    glUniform1i_fn(g_uVEnabled, g_Shared->vEnabled);
    glUniform1f_fn(g_uBlurEnabled,    glBlurOn  ? 1.0f : 0.0f);
    glUniform1f_fn(g_uBlurIntensity,  g_Shared->blurIntensity);
    if (g_uBloomEnabled       >= 0) glUniform1f_fn(g_uBloomEnabled,       glBloomOn ? 1.0f : 0.0f);
    if (g_uBloomIntensity     >= 0) glUniform1f_fn(g_uBloomIntensity,     g_Shared->bloomIntensity);
    if (g_uCurvatureEnabled   >= 0) glUniform1f_fn(g_uCurvatureEnabled,   glCurvOn  ? 1.0f : 0.0f);
    if (g_uCurvatureIntensity >= 0) glUniform1f_fn(g_uCurvatureIntensity, g_Shared->curvatureIntensity);
    if (g_uBrightness       >= 0) glUniform1f_fn(g_uBrightness,       g_Shared->brightness);
    if (g_uContrast         >= 0) glUniform1f_fn(g_uContrast,         g_Shared->contrast);
    if (g_uSaturation       >= 0) glUniform1f_fn(g_uSaturation,       g_Shared->saturation);
    if (g_uTemperature      >= 0) glUniform1f_fn(g_uTemperature,      g_Shared->temperature);
    if (g_uBlackLevel       >= 0) glUniform1f_fn(g_uBlackLevel,       g_Shared->blackLevel);
    if (g_uGamma            >= 0) glUniform1f_fn(g_uGamma,            g_Shared->gamma);
    if (g_uFlickerEnabled   >= 0) glUniform1f_fn(g_uFlickerEnabled,   glFlickOn ? 1.0f : 0.0f);
    if (g_uFlickerIntensity >= 0) glUniform1f_fn(g_uFlickerIntensity, g_Shared->flickerIntensity);
    if (g_uFlickerRate      >= 0) glUniform1f_fn(g_uFlickerRate,      g_Shared->flickerRate);
    if (g_uTime             >= 0) glUniform1f_fn(g_uTime,             GetTimeSeconds());
    bool glPhosphorOn = g_Shared->phosphorEnabled > 0.0f && g_Shared->phosphorIntensity > 0.0f;
    if (g_uPhosphorEnabled  >= 0) glUniform1f_fn(g_uPhosphorEnabled,  glPhosphorOn ? 1.0f : 0.0f);
    if (g_uPhosphorIntensity>= 0) glUniform1f_fn(g_uPhosphorIntensity,g_Shared->phosphorIntensity);
    if (g_uVhsEnabled        >= 0) glUniform1f_fn(g_uVhsEnabled,        glVhsOn       ? 1.0f : 0.0f);
    if (g_uVhsIntensity      >= 0) glUniform1f_fn(g_uVhsIntensity,      g_Shared->vhsIntensity);
    if (g_uGrainIntensity    >= 0) glUniform1f_fn(g_uGrainIntensity,    g_Shared->grainIntensity);
    if (g_uTapeNoiseEnabled  >= 0) glUniform1f_fn(g_uTapeNoiseEnabled,  glTapeNoiseOn ? 1.0f : 0.0f);
    if (g_uTapeNoiseIntensity>= 0) glUniform1f_fn(g_uTapeNoiseIntensity,g_Shared->tapeNoiseIntensity);
    if (g_uVignetteEnabled   >= 0) glUniform1f_fn(g_uVignetteEnabled,   g_Shared->vignetteEnabled);
    if (g_uMegaBezelEnabled         >= 0) glUniform1f_fn(g_uMegaBezelEnabled,         glMegaBzOn ? 1.0f : 0.0f);
    if (g_uMegaBezelThickness       >= 0) glUniform1f_fn(g_uMegaBezelThickness,       g_Shared->megaBezelThickness);
    if (g_uMegaBezelOpacity         >= 0) glUniform1f_fn(g_uMegaBezelOpacity,         g_Shared->megaBezelOpacity);
    if (g_uMegaBezelBlur            >= 0) glUniform1f_fn(g_uMegaBezelBlur,            g_Shared->megaBezelBlur);
    if (g_uMegaBezelRadius          >= 0) glUniform1f_fn(g_uMegaBezelRadius,          g_Shared->megaBezelRadius);
    if (g_uMegaBezelReflectionWidth >= 0) glUniform1f_fn(g_uMegaBezelReflectionWidth, g_Shared->megaBezelReflectionWidth);
    if (g_uMegaBezelStartFade       >= 0) glUniform1f_fn(g_uMegaBezelStartFade,       GetMegaBezelStartFade(g_Shared->megaBezelEnabled > 0.0f));
    // gameRect: game viewport in UV [0,1] within the full window.
    // Needed so MegaBezel reflection samples actual game pixels, not letterbox bars.
    if (g_uGameRect >= 0 && glUniform4f_fn) {
        float gx0 = (float)prevViewport[0] / (float)vpW;
        float gy0 = (float)prevViewport[1] / (float)vpH;
        float gx1 = (float)(prevViewport[0] + prevViewport[2]) / (float)vpW;
        float gy1 = (float)(prevViewport[1] + prevViewport[3]) / (float)vpH;
        // If the game viewport matches the full window, gameRect is (0,0,1,1) — no remap.
        glUniform4f_fn(g_uGameRect, gx0, gy0, gx1, gy1);
    }
    if (glNeedCopy && g_uBackBuf >= 0) {
        glUniform1i_fn(g_uBackBuf, 0); // texture unit 0
    }
    // Bezel hook uniforms — active/opacity + texture on unit 1
    if (g_uBezelHookActive  >= 0) glUniform1f_fn(g_uBezelHookActive,  (glBezelHookOn && g_GLBezelTex) ? 1.0f : 0.0f);
    if (g_uBezelHookOpacity >= 0) glUniform1f_fn(g_uBezelHookOpacity, g_Shared->bezelHookOpacity);
    if (g_uBezelTex         >= 0) glUniform1i_fn(g_uBezelTex, 1); // texture unit 1

    // Draw fullscreen quad (4 vertices as triangle strip)
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Check for GL errors after draw (logged once per session)
    static bool s_glErrorLogged = false;
    if (!s_glErrorLogged) {
        GLenum err = glGetError();
        if (err != 0) {
            Log("[GL] ERROR after glDrawArrays: 0x%04X", err);
        } else {
            Log("[GL] glDrawArrays OK (no GL error) — viewport=%dx%d (full viewport mode)", vpW, vpH);
        }
        s_glErrorLogged = true;
    }

    // ══════════════════════════════════════════════════════════════════════
    // RESTORE COMPLETE OpenGL STATE — leave the game exactly as we found it
    // ══════════════════════════════════════════════════════════════════════
    glUseProgram_fn(prevProgram);
    if (glBindVertexArray_fn)  glBindVertexArray_fn(prevVAO);
    // Restore FBO
    if (prevDrawFB != 0 && glBindFramebuffer_fn)
        glBindFramebuffer_fn(0x8D40 /*GL_DRAW_FRAMEBUFFER*/, prevDrawFB);
    // Restore viewport
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    // Restore color write mask
    glColorMask(prevColorMask[0], prevColorMask[1], prevColorMask[2], prevColorMask[3]);
    // Restore texture state
    if (glNeedCopy && glActiveTexture_fn) {
        // Unbind bezel texture from unit 1
        if (glBezelHookOn && g_GLBezelTex) {
            glActiveTexture_fn(0x84C1 /*GL_TEXTURE1*/);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        glActiveTexture_fn(0x84C0 /*GL_TEXTURE0*/);
        glBindTexture(GL_TEXTURE_2D, prevTexBinding);
        glActiveTexture_fn(prevActiveTexUnit);
    }
    // Restore blend
    if (!prevBlend) glDisable(GL_BLEND);
    else { glEnable(GL_BLEND); glBlendFunc(prevBlendSrc, prevBlendDst); }
    if (glBlendEquation_fn) glBlendEquation_fn(prevBlendEqRGB);
    // Restore tests
    if (prevDepth)   glEnable(GL_DEPTH_TEST);   else glDisable(GL_DEPTH_TEST);
    if (prevCull)    glEnable(GL_CULL_FACE);     else glDisable(GL_CULL_FACE);
    if (prevScissor) glEnable(GL_SCISSOR_TEST);  else glDisable(GL_SCISSOR_TEST);
    if (prevStencil) glEnable(GL_STENCIL_TEST);  else glDisable(GL_STENCIL_TEST);
}

// ── GL OSD text overlay (GDI text → GL texture → quad) ──────────────────
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif

static void RenderOsdGL() {
    if (!g_Shared) { OpenSharedMem(); }
    if (!g_Shared || !g_Shared->osdActive || !g_Shared->osdText[0]) return;
    if (!g_OsdGLProgram || !glUniform4f_fn) return;

    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    int vpW = vp[2], vpH = vp[3];
    if (vpW <= 0 || vpH <= 0) return;

    // Rebuild texture when text changes
    bool textChanged = (wcscmp(g_OsdGLLastText, g_Shared->osdText) != 0);
    if (textChanged || g_OsdGLTexture == 0) {
        wcscpy_s(g_OsdGLLastText, 128, g_Shared->osdText);

        // ── GDI text → RGBA bitmap (matching WPF OSD look) ──
        HDC memDC = CreateCompatibleDC(NULL);
        int fontSize = (int)(22.0f * vpH / 1080.0f);
        if (fontSize < 16) fontSize = 16;
        HFONT hFont = CreateFontW(-fontSize, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HFONT oldFont = (HFONT)SelectObject(memDC, hFont);

        // Measure text with GetTextExtentPoint32W — reliable, no rect ambiguity
        int textLen = (int)wcsnlen(g_OsdGLLastText, 127);
        SIZE textSize = {};
        GetTextExtentPoint32W(memDC, g_OsdGLLastText, textLen, &textSize);

        int padX = (int)(32.0f * vpH / 1080.0f);
        int padY = (int)(14.0f * vpH / 1080.0f);
        if (padX < 16) padX = 16;
        if (padY < 8)  padY = 8;
        int borderW = max(1, (int)(1.0f * vpH / 1080.0f));
        int texW = textSize.cx + padX * 2 + borderW * 2;
        int texH = textSize.cy + padY * 2 + borderW * 2;
        texW = (texW + 3) & ~3; // align to 4

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = texW;
        bmi.bmiHeader.biHeight = -texH; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        BYTE* bits = nullptr;
        HBITMAP hBmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, hBmp);
        memset(bits, 0, texW * texH * 4);

        // Background: ARGB(210, 14, 17, 38) + pink border ARGB(140, 255, 42, 255) + rounded corners
        int cornerR = (int)(10.0f * vpH / 1080.0f);
        if (cornerR < 4) cornerR = 4;

        auto isInsideRoundedRect = [](int px, int py, int w, int h, int r) -> bool {
            if (px < r && py < r) { int dx = r - px, dy = r - py; return dx*dx + dy*dy <= r*r; }
            if (px >= w - r && py < r) { int dx = px - w + r + 1, dy = r - py; return dx*dx + dy*dy <= r*r; }
            if (px < r && py >= h - r) { int dx = r - px, dy = py - h + r + 1; return dx*dx + dy*dy <= r*r; }
            if (px >= w - r && py >= h - r) { int dx = px - w + r + 1, dy = py - h + r + 1; return dx*dx + dy*dy <= r*r; }
            return true;
        };

        for (int py = 0; py < texH; py++) {
            for (int px = 0; px < texW; px++) {
                if (!isInsideRoundedRect(px, py, texW, texH, cornerR)) continue;
                BYTE* p = bits + (py * texW + px) * 4;
                // Check if this pixel is on the border (within borderW of the edge)
                bool onBorder = (px < borderW || px >= texW - borderW || py < borderW || py >= texH - borderW);
                // Also check border near rounded corners
                if (!onBorder && (px < cornerR || px >= texW - cornerR || py < cornerR || py >= texH - cornerR)) {
                    // Check if the inner rounded rect excludes this pixel
                    int inX = px - borderW, inY = py - borderW, inW = texW - borderW * 2, inH = texH - borderW * 2;
                    int inR = cornerR - borderW; if (inR < 0) inR = 0;
                    if (!isInsideRoundedRect(inX, inY, inW, inH, inR)) onBorder = true;
                }
                if (onBorder) {
                    // Pink border: BGRA = (255, 42, 255, 140)
                    p[0] = 255; p[1] = 42; p[2] = 255; p[3] = 140;
                } else {
                    // Dark background: BGRA = (38, 17, 14, 210)
                    p[0] = 38; p[1] = 17; p[2] = 14; p[3] = 210;
                }
            }
        }

        // Draw white text: perfectly centred both ways using computed coords
        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, RGB(255, 255, 255));
        SetTextAlign(memDC, TA_LEFT | TA_TOP | TA_NOUPDATECP);
        int textX = (texW - textSize.cx) / 2;          // horizontal centre
        int textY = (texH - textSize.cy) / 2;          // vertical centre
        TextOutW(memDC, textX, textY, g_OsdGLLastText, textLen);

        // Post-process: GDI wrote RGB text but alpha is 0. Fix it, then BGRA → RGBA.
        for (int i = 0; i < texW * texH; i++) {
            BYTE* p = bits + i * 4;
            // Any non-background pixel that GDI touched has its RGB set but alpha zeroed.
            // Our dark background is BGRA(38,17,14,210). If alpha is 0 and any RGB channel
            // is non-zero, it's a text pixel — make it solid white.
            if (p[3] == 0 && (p[0] | p[1] | p[2])) {
                p[0] = 255; p[1] = 255; p[2] = 255; p[3] = 255;
            }
            // Swap B ↔ R so layout becomes RGBA for OpenGL
            BYTE tmp = p[0]; p[0] = p[2]; p[2] = tmp;
        }

        // Upload to GL texture
        if (!g_OsdGLTexture) glGenTextures(1, &g_OsdGLTexture);
        glBindTexture(GL_TEXTURE_2D, g_OsdGLTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texW, texH, 0, GL_RGBA, GL_UNSIGNED_BYTE, bits);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F /*GL_CLAMP_TO_EDGE*/);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);

        g_OsdGLTexW = texW;
        g_OsdGLTexH = texH;

        SelectObject(memDC, oldBmp);
        SelectObject(memDC, oldFont);
        DeleteObject(hBmp);
        DeleteObject(hFont);
        DeleteDC(memDC);
    }

    if (!g_OsdGLTexture || g_OsdGLTexW <= 0) return;

    // ── Save GL state ──
    GLint prevProgram = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    GLboolean prevBlend = glIsEnabled(GL_BLEND);
    GLint prevBlendSrc = GL_ONE, prevBlendDst = GL_ZERO;
    glGetIntegerv(0x0BE1 /*GL_BLEND_SRC*/, &prevBlendSrc);
    glGetIntegerv(0x0BE0 /*GL_BLEND_DST*/, &prevBlendDst);
    GLboolean prevDepth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prevScissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint prevTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    GLint prevVAO = 0;
    if (glBindVertexArray_fn) glGetIntegerv(0x85B5 /*GL_VERTEX_ARRAY_BINDING*/, &prevVAO);

    // ── Draw OSD quad ──
    glUseProgram_fn(g_OsdGLProgram);

    // Compute NDC position: centred horizontally, ~5% from top
    float quadW_ndc = (float)g_OsdGLTexW / vpW * 2.0f;
    float quadH_ndc = (float)g_OsdGLTexH / vpH * 2.0f;
    float osd_x = -quadW_ndc / 2.0f;       // centred
    float osd_y = 1.0f - 0.10f - quadH_ndc; // 5% from top in NDC (0.10 = 5% * 2.0)

    glUniform4f_fn(g_OsdU_Rect, osd_x, osd_y, quadW_ndc, quadH_ndc);
    if (g_OsdU_Tex >= 0) glUniform1i_fn(g_OsdU_Tex, 0);

    glBindTexture(GL_TEXTURE_2D, g_OsdGLTexture);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);

    if (glBindVertexArray_fn && g_GLVao)
        glBindVertexArray_fn(g_GLVao);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // ── Restore GL state ──
    glUseProgram_fn(prevProgram);
    if (glBindVertexArray_fn) glBindVertexArray_fn(prevVAO);
    glBindTexture(GL_TEXTURE_2D, prevTex);
    if (!prevBlend) glDisable(GL_BLEND);
    else glBlendFunc(prevBlendSrc, prevBlendDst);
    if (prevDepth) glEnable(GL_DEPTH_TEST);
    if (prevScissor) glEnable(GL_SCISSOR_TEST);
}

// ── SDL2 borderless windowed fullscreen helper ───────────────────────────
// Called from any hooked Present/SwapBuffers when g_Shared->borderlessEnabled changes.
// Uses SDL2's own API so it bypasses SDL2's WM_WINDOWPOSCHANGING handler
// which would otherwise revert any external Win32 SetWindowPos size change.
// Works for both SDL2+OpenGL and SDL2+D3D11 (FNA) games.
static void TrySdl2Borderless(bool enable) {
    typedef struct { int unused; } SDL_Window_Opaque;
    typedef SDL_Window_Opaque* (*PFN_SDL_GL_GetCurrentWindow)(void);
    typedef SDL_Window_Opaque* (*PFN_SDL_GetKeyboardFocus)(void);
    typedef int                (*PFN_SDL_SetWindowFullscreen)(SDL_Window_Opaque*, unsigned int);

    static PFN_SDL_GL_GetCurrentWindow pfnGLGetWindow   = nullptr;
    static PFN_SDL_GetKeyboardFocus    pfnKbFocus        = nullptr;
    static PFN_SDL_SetWindowFullscreen pfnSetFullscr     = nullptr;
    static bool                        s_lookupDone      = false;

    if (!s_lookupDone) {
        s_lookupDone = true;
        HMODULE sdl2 = GetModuleHandleW(L"SDL2.dll");
        if (sdl2) {
            pfnGLGetWindow = (PFN_SDL_GL_GetCurrentWindow)GetProcAddress(sdl2, "SDL_GL_GetCurrentWindow");
            pfnKbFocus     = (PFN_SDL_GetKeyboardFocus)   GetProcAddress(sdl2, "SDL_GetKeyboardFocus");
            pfnSetFullscr  = (PFN_SDL_SetWindowFullscreen)GetProcAddress(sdl2, "SDL_SetWindowFullscreen");
            if (pfnSetFullscr && (pfnGLGetWindow || pfnKbFocus))
                Log("[SDL2] SDL2.dll found — borderless mode available (GL=%s KB=%s)",
                    pfnGLGetWindow ? "yes" : "no", pfnKbFocus ? "yes" : "no");
            else
                Log("[SDL2] SDL2.dll found but required exports missing (SetFullscreen=%s)",
                    pfnSetFullscr ? "yes" : "no");
        }
    }

    if (!pfnSetFullscr) return;

    // Try GL context window first, then keyboard-focus window (works for D3D11/Vulkan backends)
    SDL_Window_Opaque* win = nullptr;
    if (pfnGLGetWindow) win = pfnGLGetWindow();
    if (!win && pfnKbFocus) win = pfnKbFocus();
    if (!win) { Log("[SDL2] TrySdl2Borderless: no SDL window found (GL=null, KB=null)"); return; }

    // SDL_WINDOW_FULLSCREEN_DESKTOP = 0x00001001
    unsigned int flag = enable ? 0x00001001u : 0u;
    int result = pfnSetFullscr(win, flag);
    Log("[SDL2] borderless: SDL_SetWindowFullscreen(%s) = %d",
        enable ? "FULLSCREEN_DESKTOP" : "WINDOWED", result);
    if (result == 0)
        g_Sdl2BorderlessApplied = enable;
}

// ── Hooked wglSwapBuffers (opengl32.dll) ─────────────────────────────────
static BOOL WINAPI HookedSwapBuffers(HDC hdc) {
    static bool s_firstCall = true;
    if (s_firstCall) { s_firstCall = false; Log("[GL] HookedSwapBuffers FIRED — hdc=0x%p", (void*)hdc); }
    bool doScanlines = !g_SwapHookActive;
    if (doScanlines) {
        g_SwapHookActive = true;
        __try {
            if (!g_GLInited) InitGLResources();
            if (g_GLInited) {
                ApplyGLScanlines(hdc);
                RenderOsdGL();
            }
            // ── SDL2 borderless: apply/restore when shared-mem flag changes ──
            if (g_Shared) {
                bool want = (g_Shared->borderlessEnabled != 0);
                if (want != g_Sdl2BorderlessApplied)
                    TrySdl2Borderless(want);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            Log("[GL] EXCEPTION 0x%08X in HookedSwapBuffers — scanlines disabled", GetExceptionCode());
            g_GLInited = false;
        }
    }

    DWORD oldProt;
    VirtualProtect(g_SwapBuffersAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_SwapBuffersAddr, g_SwapBufOrigBytes, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_SwapBuffersAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_SwapBuffersAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    BOOL result = ((PFN_wglSwapBuffers)g_SwapBuffersAddr)(hdc);

    VirtualProtect(g_SwapBuffersAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_SwapBuffersAddr, g_SwapBufHookJmp, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_SwapBuffersAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_SwapBuffersAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    if (doScanlines) g_SwapHookActive = false;
    return result;
}

// ── Hooked SwapBuffers (gdi32.dll) — SDL2/FNA games call this directly ───
static BOOL WINAPI HookedGdiSwapBuffers(HDC hdc) {
    static bool s_firstCall = true;
    if (s_firstCall) { s_firstCall = false; Log("[GL] HookedGdiSwapBuffers FIRED — hdc=0x%p", (void*)hdc); }
    bool doScanlines = !g_SwapHookActive;
    if (doScanlines) {
        g_SwapHookActive = true;
        __try {
            if (!g_GLInited) InitGLResources();
            if (g_GLInited) {
                ApplyGLScanlines(hdc);
                RenderOsdGL();
            }
            // ── SDL2 borderless: apply/restore when shared-mem flag changes ──
            if (g_Shared) {
                bool want = (g_Shared->borderlessEnabled != 0);
                if (want != g_Sdl2BorderlessApplied)
                    TrySdl2Borderless(want);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            Log("[GL] EXCEPTION 0x%08X in HookedGdiSwapBuffers — scanlines disabled", GetExceptionCode());
            g_GLInited = false;
        }
    }

    DWORD oldProt;
    VirtualProtect(g_GdiSwapBuffersAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_GdiSwapBuffersAddr, g_GdiSwapBufOrigBytes, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_GdiSwapBuffersAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_GdiSwapBuffersAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    BOOL result = ((PFN_wglSwapBuffers)g_GdiSwapBuffersAddr)(hdc);

    VirtualProtect(g_GdiSwapBuffersAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_GdiSwapBuffersAddr, g_GdiSwapBufHookJmp, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_GdiSwapBuffersAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_GdiSwapBuffersAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    if (doScanlines) g_SwapHookActive = false;
    return result;
}

// ══════════════════════════════════════════════════════════════════════════
//  HOOK INSTALLATION
// ══════════════════════════════════════════════════════════════════════════

// Restore-call-repatch hook for wglSwapBuffers
// Unlike a trampoline (which copies the first 14 bytes of the function and
// can corrupt RIP-relative instructions), this approach:
//   1. Overwrites the first 14 bytes with a JMP to our hook
//   2. In the hook: restores original bytes → calls real function → re-hooks
// Safe because OpenGL is single-threaded per context.
static bool InstallHookAt(BYTE* pFunc, BYTE* origBytes, BYTE* hookJmp, void* hookFn) {
#ifdef _WIN64
    hookJmp[0] = 0xFF; hookJmp[1] = 0x25;
    *(DWORD*)(hookJmp + 2) = 0;
    *(UINT_PTR*)(hookJmp + 6) = (UINT_PTR)hookFn;
#else
    hookJmp[0] = 0xE9;
    *(DWORD*)(hookJmp + 1) = (DWORD)((BYTE*)hookFn - pFunc - 5);
#endif
    memcpy(origBytes, pFunc, HOOK_JMP_SIZE);
    DWORD oldProt;
    if (!VirtualProtect(pFunc, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt)) return false;
    memcpy(pFunc, hookJmp, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), pFunc, HOOK_JMP_SIZE);
    VirtualProtect(pFunc, HOOK_JMP_SIZE, oldProt, &oldProt);
    return true;
}

// ── wglMakeCurrent diagnostic hook ───────────────────────────────────────
typedef BOOL (WINAPI *PFN_wglMakeCurrent)(HDC, HGLRC);
static PFN_wglMakeCurrent g_OrigMakeCurrent = nullptr;
static BYTE* g_MakeCurrentAddr = nullptr;
static BYTE  g_MakeCurrentOrig[14] = {};
static BYTE  g_MakeCurrentJmp[14]  = {};
static BOOL WINAPI HookedMakeCurrent(HDC hdc, HGLRC hglrc) {
    static bool s_first = true;
    if (s_first) {
        s_first = false;
        Log("[GL] wglMakeCurrent FIRED — hdc=0x%p hglrc=0x%p  (OpenGL IS active on this thread)", (void*)hdc, (void*)hglrc);
        // Log the full path of opengl32.dll to detect game-local override
        wchar_t glPath[MAX_PATH] = {};
        HMODULE glMod = GetModuleHandleW(L"opengl32.dll");
        if (glMod) GetModuleFileNameW(glMod, glPath, MAX_PATH);
        char glPathA[MAX_PATH] = {};
        WideCharToMultiByte(CP_ACP, 0, glPath, -1, glPathA, MAX_PATH, nullptr, nullptr);
        Log("[GL] opengl32.dll full path: %s", glPathA);
    }
    DWORD op;
    VirtualProtect(g_MakeCurrentAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
    memcpy(g_MakeCurrentAddr, g_MakeCurrentOrig, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_MakeCurrentAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_MakeCurrentAddr, HOOK_JMP_SIZE, op, &op);
    BOOL r = g_OrigMakeCurrent(hdc, hglrc);
    VirtualProtect(g_MakeCurrentAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
    memcpy(g_MakeCurrentAddr, g_MakeCurrentJmp, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_MakeCurrentAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_MakeCurrentAddr, HOOK_JMP_SIZE, op, &op);

    // ── Context change detection ──────────────────────────────────────────
    // If a new non-null HGLRC is made current, our GL objects (VAO, programs,
    // textures) from the old context are now invalid. Zero them out so the
    // next SwapBuffers call triggers a full reinitialisation in the new context.
    if (r && hglrc != NULL && hglrc != g_hCurrentGlrc) {
        Log("[GL] Context changed: %p -> %p — invalidating stale GL resources", (void*)g_hCurrentGlrc, (void*)hglrc);
        // Do NOT call glDelete* — the old context may already be detached/gone.
        g_GLProgram    = 0;  g_GLVao     = 0;
        g_GLBlurTex    = 0;  g_GLBlurTexW = 0; g_GLBlurTexH = 0;
        g_GLBezelTex   = 0;  g_GLBezelPathCached[0] = L'\0';
        g_OsdGLProgram = 0;  g_OsdGLTexture = 0;
        g_OsdGLTexW    = 0;  g_OsdGLTexH    = 0;
        g_GLInited     = false;
        g_hCurrentGlrc = hglrc;
    }
    return r;
}

static bool InstallOpenGLHook() {
    bool anyHooked = false;

    HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    if (!gl) gl = LoadLibraryW(L"opengl32.dll");

    if (gl) {
        // Log full path of opengl32 we are hooking (detect game-local override)
        wchar_t glPath[MAX_PATH] = {};
        GetModuleFileNameW(gl, glPath, MAX_PATH);
        char glPathA[MAX_PATH] = {};
        WideCharToMultiByte(CP_ACP, 0, glPath, -1, glPathA, MAX_PATH, nullptr, nullptr);
        Log("[GL] Hooking opengl32 at: %s", glPathA);

        // ── Hook 1: wglMakeCurrent (diagnostic — detects context creation) ──
        auto pMC = (BYTE*)GetProcAddress(gl, "wglMakeCurrent");
        if (pMC) {
            g_MakeCurrentAddr = pMC;
            g_OrigMakeCurrent = (PFN_wglMakeCurrent)pMC;
            if (InstallHookAt(pMC, g_MakeCurrentOrig, g_MakeCurrentJmp, (void*)HookedMakeCurrent))
                Log("[GL] opengl32::wglMakeCurrent hooked at 0x%p (diagnostic)", (void*)pMC);
        }

        // ── Hook 2: wglSwapBuffers (standard WGL apps) ──
        auto p = (BYTE*)GetProcAddress(gl, "wglSwapBuffers");
        if (p) {
            g_SwapBuffersAddr = p;
            if (InstallHookAt(p, g_SwapBufOrigBytes, g_SwapBufHookJmp, (void*)HookedSwapBuffers)) {
                Log("[GL] opengl32::wglSwapBuffers hooked at 0x%p", (void*)p);
                anyHooked = true;
            }
        }
    }

    // ── Hook 3: gdi32::SwapBuffers (SDL2/FNA games — Celeste, etc.) ──
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) gdi = LoadLibraryW(L"gdi32.dll");
    if (gdi) {
        auto p = (BYTE*)GetProcAddress(gdi, "SwapBuffers");
        if (p) {
            g_GdiSwapBuffersAddr = p;
            if (InstallHookAt(p, g_GdiSwapBufOrigBytes, g_GdiSwapBufHookJmp, (void*)HookedGdiSwapBuffers)) {
                Log("[GL] gdi32::SwapBuffers hooked at 0x%p (SDL2/FNA support)", (void*)p);
                anyHooked = true;
            }
        }
    }

    if (!anyHooked) { Log("[GL] FAIL: neither wglSwapBuffers nor gdi32::SwapBuffers could be hooked"); return false; }
    Log("[GL] InstallOpenGLHook: restore-call-repatch hook installed OK (no trampoline)");
    return true;
}

// ── D3D11 vtable hook installation ───────────────────────────────────────
static bool InstallD3D11Hook() {
    // Only hook if the game already loaded D3D11/DXGI (don't force-load)
    if (!GetModuleHandleW(L"d3d11.dll") || !GetModuleHandleW(L"dxgi.dll")) {
        Log("[D3D11] InstallD3D11Hook: d3d11.dll or dxgi.dll not loaded, skipping");
        return false;
    }
    Log("[D3D11] InstallD3D11Hook: creating dummy device to get vtable");

    // Create dummy window + D3D11 device to discover Present vtable address
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW), 0, DefWindowProcW, 0, 0,
        GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr,
        L"S4W_DummyWC", nullptr };
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED,
        0, 0, 4, 4, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return false;

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 4;
    sd.BufferDesc.Height = 4;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;

    IDXGISwapChain* dummySC = nullptr;
    ID3D11Device* dummyDev = nullptr;
    ID3D11DeviceContext* dummyCtx = nullptr;
    D3D_FEATURE_LEVEL fl;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &sd, &dummySC, &dummyDev, &fl, &dummyCtx);

    if (FAILED(hr) || !dummySC) {
        Log("[D3D11] FAIL: D3D11CreateDeviceAndSwapChain hr=0x%08X", hr);
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    // Get vtable — Present is at index 8
    void** vtable = *(void***)dummySC;
    g_OrigPresent = (PFN_Present)vtable[8];

    // Patch Present (index 8)
    DWORD oldProtect;
    if (VirtualProtect(&vtable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        vtable[8] = (void*)HookedPresent;
        VirtualProtect(&vtable[8], sizeof(void*), oldProtect, &oldProtect);
        Log("[D3D11] vtable patched: Present@index8");
    } else {
        Log("[D3D11] WARN: VirtualProtect failed for vtable[8] — Present hook NOT installed");
    }

    // Patch Present1 (index 22, IDXGISwapChain1) — used by FNA D3D11 mode
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(dummySC->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1))) {
        void** vtable1 = *(void***)sc1;
        g_OrigPresent1 = (PFN_Present1)vtable1[22];
        DWORD op1;
        if (VirtualProtect(&vtable1[22], sizeof(void*), PAGE_EXECUTE_READWRITE, &op1)) {
            vtable1[22] = (void*)HookedPresent1;
            VirtualProtect(&vtable1[22], sizeof(void*), op1, &op1);
            Log("[D3D11] vtable patched: Present1@index22 (IDXGISwapChain1)");
        } else {
            Log("[D3D11] WARN: VirtualProtect failed for vtable1[22] — Present1 hook NOT installed");
        }
        sc1->Release();
    } else {
        Log("[D3D11] IDXGISwapChain1 not available — Present1 not hooked");
    }

    // Also install inline hooks on the function bodies — fallback for NVIDIA
    // per-instance vtables where the vtable patch above only affects the dummy
    // device's vtable and not the game's own swap chain vtable copy.
    // Both paths call HookedPresent; the restore-call-repatch in HookedPresent
    // removes the inline hook before calling the original to prevent re-entrancy.
    g_D3D11PresentAddr = (BYTE*)g_OrigPresent;
    if (g_D3D11PresentAddr) {
        if (InstallHookAt(g_D3D11PresentAddr, g_D3D11PresentOrig, g_D3D11PresentJmp, (void*)HookedPresent)) {
            Log("[D3D11] Inline hook installed: Present at 0x%p", (void*)g_D3D11PresentAddr);
        } else {
            Log("[D3D11] WARN: Could not install Present inline hook — vtable-only fallback");
            g_D3D11PresentAddr = nullptr;
        }
    }
    if (g_OrigPresent1) {
        g_D3D11Present1Addr = (BYTE*)g_OrigPresent1;
        if (InstallHookAt(g_D3D11Present1Addr, g_D3D11Present1Orig, g_D3D11Present1Jmp, (void*)HookedPresent1)) {
            Log("[D3D11] Inline hook installed: Present1 at 0x%p", (void*)g_D3D11Present1Addr);
        } else {
            Log("[D3D11] WARN: Could not install Present1 inline hook — vtable-only fallback");
            g_D3D11Present1Addr = nullptr;
        }
    }

    dummySC->Release();
    dummyDev->Release();
    dummyCtx->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    Log("[D3D11] InstallD3D11Hook: vtable + inline hooks installed (Present@8 + Present1@22)");
    return true;
}

// ══════════════════════════════════════════════════════════════════════════
//  D3D12 COMMAND-QUEUE CAPTURE (for D3D11On12)
// ──────────────────────────────────────────────────────────────────────────
//  D3D11On12CreateDevice needs the game's D3D12 command queue, which the swap
//  chain does not expose. The canonical way to obtain it is to hook
//  ID3D12CommandQueue::ExecuteCommandLists (vtable index 10) and grab the first
//  DIRECT queue that submits work. D3D12 runtime objects share a per-process
//  vtable, so patching a dummy queue's vtable also intercepts the game's queue.
//  Once captured, we restore the vtable (auto-unhook) so there is zero
//  steady-state overhead on this hot path.
// ──────────────────────────────────────────────────────────────────────────
static void STDMETHODCALLTYPE HookedExecuteCommandLists(
    ID3D12CommandQueue* queue, UINT numLists, ID3D12CommandList* const* lists)
{
    if (!g_GameCmdQueue && queue) {
        __try {
            D3D12_COMMAND_QUEUE_DESC d = queue->GetDesc();
            if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
                queue->AddRef();
                g_GameCmdQueue = queue;
                Log("[D3D12] Captured DIRECT command queue 0x%p via ExecuteCommandLists", (void*)queue);
                // Auto-unhook: restore the original vtable entry — no more overhead.
                if (g_ECLVtableSlot && g_OrigECL) {
                    DWORD op;
                    if (VirtualProtect(g_ECLVtableSlot, sizeof(void*), PAGE_EXECUTE_READWRITE, &op)) {
                        *g_ECLVtableSlot = (void*)g_OrigECL;
                        VirtualProtect(g_ECLVtableSlot, sizeof(void*), op, &op);
                    }
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_OrigECL(queue, numLists, lists);
}

static bool InstallD3D12QueueHook() {
    if (g_D3D12QueueHookInstalled) return true;
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (!d3d12) {
        // Not a D3D12 game — nothing to do (and we must not force-load d3d12.dll).
        return false;
    }
    auto pCreate = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(d3d12, "D3D12CreateDevice");
    if (!pCreate) { Log("[D3D12] D3D12CreateDevice not found"); return false; }

    // Dummy device + DIRECT queue purely to read the shared vtable.
    ID3D12Device* dummyDev = nullptr;
    HRESULT hr = pCreate(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&dummyDev);
    if (FAILED(hr) || !dummyDev) { Log("[D3D12] dummy D3D12CreateDevice hr=0x%08X", hr); return false; }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* dummyQ = nullptr;
    hr = dummyDev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&dummyQ);
    if (FAILED(hr) || !dummyQ) { Log("[D3D12] dummy CreateCommandQueue hr=0x%08X", hr); dummyDev->Release(); return false; }

    void** vt = *(void***)dummyQ;
    g_OrigECL       = (PFN_ExecuteCommandLists)vt[10];   // ExecuteCommandLists @ index 10
    g_ECLVtableSlot = &vt[10];
    DWORD op;
    bool ok = false;
    if (VirtualProtect(&vt[10], sizeof(void*), PAGE_EXECUTE_READWRITE, &op)) {
        vt[10] = (void*)HookedExecuteCommandLists;
        VirtualProtect(&vt[10], sizeof(void*), op, &op);
        ok = true;
        Log("[D3D12] ExecuteCommandLists@10 vtable hooked (orig=0x%p) — awaiting queue", (void*)g_OrigECL);
    } else {
        Log("[D3D12] WARN: VirtualProtect failed for ExecuteCommandLists vtable");
        g_OrigECL = nullptr; g_ECLVtableSlot = nullptr;
    }

    dummyQ->Release();
    dummyDev->Release();   // vtable lives in D3D12Core.dll — patch persists after release
    g_D3D12QueueHookInstalled = ok;
    return ok;
}

// ══════════════════════════════════════════════════════════════════════════
//  GOPHER64 DEDICATED D3D11 HOOK (PARALLEL PATH — ISOLATED FROM v1.2 CORE)
// ──────────────────────────────────────────────────────────────────────────
//  Gopher64 uses wgpu → D3D11 flip-model swap chains with backbuffer format
//  R10G10B10A2_UNORM (only 2 bits of alpha) on Windows 11. Writing ANY alpha
//  to the backbuffer causes DirectComposition to composite the window with
//  that alpha → transparent window → user sees the desktop (blackscreen).
//
//  This dedicated path uses a blend state with WriteMask = R|G|B (NO alpha),
//  format-aware backbuffer copy, and its own independent Present inline hook.
//  It NEVER touches any of the main hook globals — guarantees zero regression
//  for all other games/emulators.
//
//  Architecture:
//   - Fires only when g_IsGopher == true (set from DllMain by exe name)
//   - Parent process launches a child renderer (same exe name, --renderer flag)
//     → we hook CreateProcessW/A to inject this DLL into the child
//   - Child process installs the actual D3D11 Present hook and renders effects
// ══════════════════════════════════════════════════════════════════════════

// ── Gopher globals (all g_G_* prefix — no overlap with v1.2 state) ──────
static ID3D11Device*              g_G_Device     = nullptr;
static ID3D11DeviceContext*       g_G_Ctx        = nullptr;
static ID3D11VertexShader*        g_G_VS         = nullptr;
static ID3D11PixelShader*         g_G_PS         = nullptr;
static ID3D11Buffer*              g_G_CB         = nullptr;
static ID3D11BlendState*          g_G_Blend      = nullptr;   // multiplicative (alpha-safe mask)
static ID3D11BlendState*          g_G_BlendOver  = nullptr;   // overwrite (alpha-safe mask)
static ID3D11RasterizerState*     g_G_Raster     = nullptr;
static ID3D11SamplerState*        g_G_Sampler    = nullptr;
static ID3D11Texture2D*           g_G_BBCopy     = nullptr;
static ID3D11ShaderResourceView*  g_G_BBSRV      = nullptr;
static UINT                       g_G_LastBBW    = 0;
static UINT                       g_G_LastBBH    = 0;
static DXGI_FORMAT                g_G_LastBBFmt  = DXGI_FORMAT_UNKNOWN;
static bool                       g_G_Inited     = false;
static bool                       g_G_FirstFrame = true;

static PFN_Present                g_G_OrigPresent     = nullptr;
static BYTE*                      g_G_PresentAddr     = nullptr;
static BYTE                       g_G_PresentOrig[14] = {};
static BYTE                       g_G_PresentJmp[14]  = {};

// Present1 (IDXGISwapChain1::Present1, vtable[22]) — wgpu switches from Present
// to Present1 after the first window resize/fullscreen transition. Without this
// hook, effects vanish after every state change because our Present hook is never
// called again. Both hooks share the same g_G_HookBusy guard and effect logic.
static PFN_Present1               g_G_OrigPresent1     = nullptr;
static BYTE*                      g_G_Present1Addr     = nullptr;
static BYTE                       g_G_Present1Orig[14] = {};
static BYTE                       g_G_Present1Jmp[14]  = {};

// Pre-compiled shader blobs — filled by Gopher_PrecompileShaders() in
// GopherHookThread BEFORE the Present hook is installed.  Gopher_InitResources
// then just calls CreateVertexShader/CreatePixelShader (instantaneous) instead
// of D3DCompile (500ms-2s).  This eliminates the frame-stall / apparent freeze
// that occurred when g_G_Inited was reset by an alt-tab exception and the NEXT
// Present call re-triggered a slow shader compile inside the render thread.
static void*                      g_G_VSBytecode   = nullptr;
static SIZE_T                     g_G_VSBytecodeLen = 0;
static void*                      g_G_PSBytecode   = nullptr;
static SIZE_T                     g_G_PSBytecodeLen = 0;

// Global reentrancy guard — safer than thread_local on wgpu's multi-threaded
// renderer where different threads may call Present concurrently.
// 0 = idle, 1 = a thread is already inside the hook applying effects.
// We use InterlockedCompareExchange so the second thread skips effects
// (avoiding GPU draw on two threads simultaneously) rather than blocking.
static volatile LONG              g_G_HookBusy = 0;

// ── Pre-compile shaders in background (called from GopherHookThread) ─────
// D3DCompile is slow (500ms-2s). Do it ONCE in the background thread before
// the Present hook is installed. Result bytecodes stored in g_G_VSBytecode /
// g_G_PSBytecode, freed by Gopher_FreeBytecodes() on process exit.
static bool Gopher_PrecompileShaders() {
    Log("[GOPHER] Pre-compiling shaders (VS + PS) in background...");
    ID3DBlob* vsBlob = nullptr; ID3DBlob* errBlob = nullptr;
    // D3DCOMPILE_SKIP_OPTIMIZATION drastically reduces compile time (~1000ms → ~150ms)
    // with no visual difference — we're drawing a 3-vertex fullscreen triangle, not
    // a AAA game shader. The compiled bytecode runs fine; we trade shader speed
    // (irrelevant at our scale) for a much shorter gap between child spawn and effects.
    const UINT compileFlags = D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_OPTIMIZATION_LEVEL0;

    HRESULT hr = D3DCompile(VS_SRC, strlen(VS_SRC), "gvs", nullptr, nullptr,
        "main", "vs_5_0", compileFlags, 0, &vsBlob, &errBlob);
    if (FAILED(hr) || !vsBlob) {
        LogShaderError("GOPHER VS precompile", errBlob);
        if (errBlob) errBlob->Release();
        return false;
    }
    if (errBlob) { errBlob->Release(); errBlob = nullptr; }

    ID3DBlob* psBlob = nullptr;
    const char* psSrc = GetPSSrc();
    hr = D3DCompile(psSrc, strlen(psSrc), "gps", nullptr, nullptr,
        "main", "ps_5_0", compileFlags, 0, &psBlob, &errBlob);
    if (FAILED(hr) || !psBlob) {
        LogShaderError("GOPHER PS precompile", errBlob);
        vsBlob->Release();
        if (errBlob) errBlob->Release();
        return false;
    }
    if (errBlob) errBlob->Release();

    // Copy to plain heap buffers so CreateVertexShader can be called from any
    // thread without holding the COM blob alive.
    g_G_VSBytecodeLen = vsBlob->GetBufferSize();
    g_G_VSBytecode    = malloc(g_G_VSBytecodeLen);
    if (g_G_VSBytecode) memcpy(g_G_VSBytecode, vsBlob->GetBufferPointer(), g_G_VSBytecodeLen);
    vsBlob->Release();

    g_G_PSBytecodeLen = psBlob->GetBufferSize();
    g_G_PSBytecode    = malloc(g_G_PSBytecodeLen);
    if (g_G_PSBytecode) memcpy(g_G_PSBytecode, psBlob->GetBufferPointer(), g_G_PSBytecodeLen);
    psBlob->Release();

    if (!g_G_VSBytecode || !g_G_PSBytecode) {
        Log("[GOPHER] Pre-compile: malloc failed");
        return false;
    }
    Log("[GOPHER] Shaders pre-compiled OK (VS=%zu bytes, PS=%zu bytes)",
        g_G_VSBytecodeLen, g_G_PSBytecodeLen);
    return true;
}

// ── Release all Gopher D3D11 resources (safe to call multiple times) ─────
static void Gopher_ReleaseResources() {
    if (g_G_BBSRV)   { g_G_BBSRV->Release();   g_G_BBSRV   = nullptr; }
    if (g_G_BBCopy)  { g_G_BBCopy->Release();   g_G_BBCopy  = nullptr; }
    if (g_G_Sampler) { g_G_Sampler->Release();  g_G_Sampler = nullptr; }
    if (g_G_Raster)  { g_G_Raster->Release();   g_G_Raster  = nullptr; }
    if (g_G_BlendOver){ g_G_BlendOver->Release();g_G_BlendOver=nullptr; }
    if (g_G_Blend)   { g_G_Blend->Release();    g_G_Blend   = nullptr; }
    if (g_G_CB)      { g_G_CB->Release();       g_G_CB      = nullptr; }
    if (g_G_PS)      { g_G_PS->Release();       g_G_PS      = nullptr; }
    if (g_G_VS)      { g_G_VS->Release();       g_G_VS      = nullptr; }
    if (g_G_Ctx)     { g_G_Ctx->Release();      g_G_Ctx     = nullptr; }
    if (g_G_Device)  { g_G_Device->Release();   g_G_Device  = nullptr; }
    g_G_LastBBW = 0; g_G_LastBBH = 0; g_G_LastBBFmt = DXGI_FORMAT_UNKNOWN;
    g_G_FirstFrame = true;
}

// ── Initialize Gopher D3D11 resources (own pipeline, alpha-safe blends) ──
// Uses pre-compiled shader bytecodes — fast, safe to call inside Present.
static bool Gopher_InitResources(IDXGISwapChain* sc) {
    // Require pre-compiled shaders — if not ready, the caller skips effects.
    if (!g_G_VSBytecode || !g_G_PSBytecode) {
        Log("[GOPHER] InitResources: shader bytecodes not ready, skipping");
        return false;
    }

    // Release any leftovers from a previous (failed) init or reinit.
    Gopher_ReleaseResources();

    Log("[GOPHER] InitResources — acquiring device from SwapChain");
    HRESULT hr = sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_G_Device);
    if (FAILED(hr) || !g_G_Device) { Log("[GOPHER] FAIL: GetDevice hr=0x%08X", hr); return false; }
    g_G_Device->GetImmediateContext(&g_G_Ctx);
    if (!g_G_Ctx) { Log("[GOPHER] FAIL: GetImmediateContext returned null"); return false; }

    DXGI_SWAP_CHAIN_DESC scDesc = {};
    sc->GetDesc(&scDesc);
    Log("[GOPHER] SwapChain: %ux%u fmt=%u bufs=%u windowed=%d swapEffect=%u",
        scDesc.BufferDesc.Width, scDesc.BufferDesc.Height,
        scDesc.BufferDesc.Format, scDesc.BufferCount,
        scDesc.Windowed, (unsigned)scDesc.SwapEffect);

    // Create shaders from pre-compiled bytecodes (no D3DCompile — instantaneous)
    hr = g_G_Device->CreateVertexShader(g_G_VSBytecode, g_G_VSBytecodeLen, nullptr, &g_G_VS);
    if (FAILED(hr)) { Log("[GOPHER] FAIL: CreateVertexShader hr=0x%08X", hr); return false; }

    hr = g_G_Device->CreatePixelShader(g_G_PSBytecode, g_G_PSBytecodeLen, nullptr, &g_G_PS);
    if (FAILED(hr)) { Log("[GOPHER] FAIL: CreatePixelShader hr=0x%08X", hr); return false; }

    // Constant buffer (same layout as main path — 208 bytes / 13 float4 rows)
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = 208;
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = g_G_Device->CreateBuffer(&cbd, nullptr, &g_G_CB);
    if (FAILED(hr)) { Log("[GOPHER] FAIL: CreateBuffer hr=0x%08X", hr); return false; }

    // ── Multiplicative blend (scanline mask only) — ALPHA-SAFE ──
    // Output.rgb = Dest.rgb * Source.rgb ;  Output.a = Dest.a (UNCHANGED).
    // This preserves the opaque alpha the wgpu renderer wrote into the
    // R10G10B10A2_UNORM backbuffer. DirectComposition needs that alpha=1
    // (saturated from the 2-bit channel) to keep the window opaque.
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable    = TRUE;
    bd.RenderTarget[0].SrcBlend       = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlend      = D3D11_BLEND_SRC_COLOR;
    bd.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;   // keep dest alpha
    bd.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    // CRITICAL: write RGB only — NEVER touch the alpha channel. On
    // R10G10B10A2_UNORM flip-model swap chains, alpha drives DirectComposition.
    bd.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    hr = g_G_Device->CreateBlendState(&bd, &g_G_Blend);
    if (FAILED(hr)) { Log("[GOPHER] FAIL: CreateBlendState(mul) hr=0x%08X", hr); return false; }

    // ── Overwrite blend (used when the shader samples the backbuffer copy ──
    // and writes the final color: blur, bloom, CRT curvature, VHS, etc.).
    // Blend disabled, but WriteMask STILL excludes alpha so the existing
    // backbuffer alpha is left untouched.
    D3D11_BLEND_DESC bdOver = {};
    bdOver.RenderTarget[0].BlendEnable = FALSE;
    bdOver.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    hr = g_G_Device->CreateBlendState(&bdOver, &g_G_BlendOver);
    if (FAILED(hr)) { Log("[GOPHER] FAIL: CreateBlendState(over) hr=0x%08X", hr); return false; }

    // Linear sampler for blur/curvature sampling
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = g_G_Device->CreateSamplerState(&sd, &g_G_Sampler);
    if (FAILED(hr)) { Log("[GOPHER] FAIL: CreateSamplerState hr=0x%08X", hr); return false; }

    // Rasterizer
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_NONE;
    rd.ScissorEnable   = FALSE;
    rd.DepthClipEnable = TRUE;
    hr = g_G_Device->CreateRasterizerState(&rd, &g_G_Raster);
    if (FAILED(hr)) { Log("[GOPHER] FAIL: CreateRasterizerState hr=0x%08X", hr); return false; }

    // Share the shared memory across both code paths (just calls OpenFileMapping,
    // idempotent — no interference with main path behavior in non-Gopher procs).
    OpenSharedMem();

    g_G_Inited = true;
    Log("[GOPHER] Resources initialized OK (alpha-safe blend, WriteMask=R|G|B)");
    return true;
}

// ── Apply scanlines/effects for Gopher (independent pipeline) ────────────
static void Gopher_ApplyScanlines(IDXGISwapChain* sc) {
    // Skip effects if the swap chain's output window is minimized or not visible.
    // During alt-tab on flip-model swap chains, DWM holds the backbuffer and a
    // GPU draw call on it can deadlock against DWM's compositor. Bailing out
    // here prevents the freeze without dropping frames when the game is visible.
    {
        DXGI_SWAP_CHAIN_DESC _d; sc->GetDesc(&_d);
        if (_d.OutputWindow && IsIconic(_d.OutputWindow)) return;
        // Also bail if the window handle is gone (process exiting)
        if (_d.OutputWindow && !IsWindow(_d.OutputWindow)) {
            Log("[GOPHER] ApplyScanlines: output window gone, skipping");
            return;
        }
    }

    // ── Device-mismatch check (CRITICAL for fullscreen/maximize) ─────────
    // When wgpu changes window state (fullscreen ↔ windowed, maximize), it
    // often destroys the D3D11 device and creates a NEW one with a NEW swap
    // chain. Our cached g_G_Device becomes stale; any RTV / texture we create
    // with it will silently fail because the backbuffer now belongs to a
    // different device. Detect this and force a full re-init.
    {
        ID3D11Device* curDev = nullptr;
        HRESULT hrd = sc->GetDevice(__uuidof(ID3D11Device), (void**)&curDev);
        if (SUCCEEDED(hrd) && curDev) {
            if (g_G_Device && curDev != g_G_Device) {
                Log("[GOPHER] Device MISMATCH detected (cached=0x%p new=0x%p) — forcing re-init",
                    (void*)g_G_Device, (void*)curDev);
                curDev->Release();
                Gopher_ReleaseResources();
                g_G_Inited = false;
                if (!Gopher_InitResources(sc)) {
                    // Init failed this frame; try again next frame.
                    return;
                }
            } else {
                curDev->Release();
            }
        }
    }

    // ── Heartbeat log (every 300 frames ≈ 5s at 60fps) ──────────────────
    // Helps diagnose whether the hook is still firing after window-state
    // changes. If effects disappear and this log stops, the hook itself was
    // lost; if it keeps firing but effects are gone, something inside is
    // failing silently (e.g. RTV creation).
    {
        static LONG s_frame = 0;
        LONG f = InterlockedIncrement(&s_frame);
        if ((f % 300) == 1) {
            DXGI_SWAP_CHAIN_DESC _dd; sc->GetDesc(&_dd);
            Log("[GOPHER] Heartbeat frame=%ld sc=0x%p %ux%u fmt=%u swapEffect=%u",
                f, (void*)sc, _dd.BufferDesc.Width, _dd.BufferDesc.Height,
                (unsigned)_dd.BufferDesc.Format, (unsigned)_dd.SwapEffect);
        }
    }

    if (!g_Shared) { OpenSharedMem(); }
    if (!g_Shared) {
        if (g_G_FirstFrame) { Log("[GOPHER] ApplyScanlines: no shared memory"); g_G_FirstFrame = false; }
        return;
    }
    if (!g_Shared->active) {
        if (g_G_FirstFrame) { Log("[GOPHER] ApplyScanlines: active=0"); g_G_FirstFrame = false; }
        return;
    }
    if (!g_G_Ctx || !g_G_Device) return;

    // Fast-path: any effect?
    bool anyEffect =
        g_Shared->hEnabled ||
        g_Shared->vEnabled ||
        (g_Shared->blurEnabled      > 0.0f && g_Shared->blurIntensity      > 0.0f) ||
        (g_Shared->bloomEnabled     > 0.0f && g_Shared->bloomIntensity     > 0.0f) ||
        (g_Shared->curvatureEnabled > 0.0f && g_Shared->curvatureIntensity > 0.0f) ||
        (g_Shared->flickerEnabled   > 0.0f && g_Shared->flickerIntensity   > 0.0f) ||
        (g_Shared->phosphorEnabled  > 0.0f && g_Shared->phosphorIntensity  > 0.0f) ||
        (g_Shared->vhsEnabled       > 0.0f) ||
        (g_Shared->grainIntensity   > 0.0f) ||
        (g_Shared->tapeNoiseEnabled > 0.0f && g_Shared->tapeNoiseIntensity > 0.0f) ||
        (g_Shared->vignetteEnabled  > 0.0f) ||
        g_Shared->brightness != 0.0f || g_Shared->contrast    != 0.0f ||
        g_Shared->saturation != 0.0f || g_Shared->temperature != 0.0f ||
        g_Shared->blackLevel > 0.0f  ||
        (g_Shared->gamma != 1.0f && g_Shared->gamma != 0.0f) ||
        g_Shared->osdActive;
    if (!anyEffect) return;

    ID3D11Texture2D* backbuffer = nullptr;
    HRESULT hr = sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer);
    if (FAILED(hr) || !backbuffer) return;

    bool blurOn   = g_Shared->blurEnabled       > 0.0f && g_Shared->blurIntensity       > 0.0f;
    bool bloomOn  = g_Shared->bloomEnabled      > 0.0f && g_Shared->bloomIntensity      > 0.0f;
    bool curvOn   = g_Shared->curvatureEnabled  > 0.0f && g_Shared->curvatureIntensity  > 0.0f;
    bool bcOn     = g_Shared->brightness != 0.0f || g_Shared->contrast != 0.0f ||
                    g_Shared->saturation != 0.0f || g_Shared->temperature != 0.0f ||
                    g_Shared->blackLevel > 0.0f ||
                    (g_Shared->gamma != 1.0f && g_Shared->gamma != 0.0f);
    bool flickOn  = g_Shared->flickerEnabled    > 0.0f && g_Shared->flickerIntensity    > 0.0f;
    bool phosphOn = g_Shared->phosphorEnabled   > 0.0f && g_Shared->phosphorIntensity   > 0.0f;
    bool vhsOn    = g_Shared->vhsEnabled        > 0.0f;
    bool grainOn  = g_Shared->grainIntensity    > 0.0f;
    bool tapeOn   = g_Shared->tapeNoiseEnabled  > 0.0f && g_Shared->tapeNoiseIntensity > 0.0f;
    bool megaBzOn = g_Shared->megaBezelEnabled  > 0.0f;

    // ── Startup blackout (megabezel only) — Gopher path ──
    // Mirror of the regular HookedPresent blackout: ~1.5s of solid black at
    // game launch to hide the splash/clear-color flash before the megabezel
    // reflection fades in. See the regular path for full rationale.
    if (megaBzOn) {
        static float blackoutStart = -1.0f;
        const float BLACKOUT_SECONDS = 1.5f;
        if (blackoutStart < 0.0f) blackoutStart = GetTimeSeconds();
        if ((GetTimeSeconds() - blackoutStart) < BLACKOUT_SECONDS) {
            ID3D11RenderTargetView* rtvBlackout = nullptr;
            if (SUCCEEDED(g_G_Device->CreateRenderTargetView(backbuffer, nullptr, &rtvBlackout))) {
                float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                g_G_Ctx->ClearRenderTargetView(rtvBlackout, black);
                rtvBlackout->Release();
            }
            backbuffer->Release();
            return;
        }
    }

    bool needCopy = blurOn || bloomOn || curvOn || bcOn || flickOn || phosphOn || vhsOn || grainOn || tapeOn || megaBzOn;

    D3D11_TEXTURE2D_DESC bbDesc;
    backbuffer->GetDesc(&bbDesc);

    if (g_G_FirstFrame) {
        Log("[GOPHER] First frame — bb=%ux%u fmt=%u needCopy=%d hEn=%d vEn=%d",
            bbDesc.Width, bbDesc.Height, (unsigned)bbDesc.Format, needCopy?1:0,
            g_Shared->hEnabled, g_Shared->vEnabled);
        g_G_FirstFrame = false;
    }

    // Format-aware backbuffer copy (R10G10B10A2_UNORM natively supported)
    if (needCopy) {
        UINT bbW2 = bbDesc.Width, bbH2 = bbDesc.Height;
        if (!g_G_BBCopy || g_G_LastBBW != bbW2 || g_G_LastBBH != bbH2 || g_G_LastBBFmt != bbDesc.Format) {
            if (g_G_BBSRV)  { g_G_BBSRV->Release();  g_G_BBSRV  = nullptr; }
            if (g_G_BBCopy) { g_G_BBCopy->Release(); g_G_BBCopy = nullptr; }
            D3D11_TEXTURE2D_DESC td = {};
            td.Width      = bbW2;
            td.Height     = bbH2;
            td.MipLevels  = 1;
            td.ArraySize  = 1;
            td.Format     = bbDesc.Format;              // format-aware
            td.SampleDesc = { 1, 0 };
            td.Usage      = D3D11_USAGE_DEFAULT;
            td.BindFlags  = D3D11_BIND_SHADER_RESOURCE;
            if (SUCCEEDED(g_G_Device->CreateTexture2D(&td, nullptr, &g_G_BBCopy))) {
                g_G_Device->CreateShaderResourceView(g_G_BBCopy, nullptr, &g_G_BBSRV);
                g_G_LastBBW = bbW2; g_G_LastBBH = bbH2; g_G_LastBBFmt = bbDesc.Format;
                Log("[GOPHER] Backbuffer copy created %ux%u fmt=%u", bbW2, bbH2, bbDesc.Format);
            } else {
                Log("[GOPHER] WARN: CreateTexture2D for backbuffer copy FAILED (fmt=%u)", bbDesc.Format);
            }
        }
        if (g_G_BBCopy) {
            if (bbDesc.SampleDesc.Count > 1)
                g_G_Ctx->ResolveSubresource(g_G_BBCopy, 0, backbuffer, 0, bbDesc.Format);
            else
                g_G_Ctx->CopyResource(g_G_BBCopy, backbuffer);
        }
    }

    // RTV for backbuffer (native format)
    ID3D11RenderTargetView* rtv = nullptr;
    hr = g_G_Device->CreateRenderTargetView(backbuffer, nullptr, &rtv);
    backbuffer->Release();
    if (FAILED(hr) || !rtv) {
        // Log ONCE per failure streak so we can diagnose. This typically means
        // the backbuffer belongs to a different device than our cached one —
        // the device-mismatch check at the top SHOULD have caught this, but
        // if it still fails, force re-init next frame.
        static HRESULT s_lastRtvFail = S_OK;
        if (hr != s_lastRtvFail) {
            Log("[GOPHER] CreateRenderTargetView FAILED hr=0x%08X — forcing re-init next frame", hr);
            s_lastRtvFail = hr;
        }
        Gopher_ReleaseResources();
        g_G_Inited = false;
        return;
    }

    DXGI_SWAP_CHAIN_DESC scDesc;
    sc->GetDesc(&scDesc);
    D3D11_VIEWPORT vp = { 0, 0,
        (float)scDesc.BufferDesc.Width, (float)scDesc.BufferDesc.Height,
        0.0f, 1.0f };

    float bbW = (float)scDesc.BufferDesc.Width;
    float bbH = (float)scDesc.BufferDesc.Height;

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = g_G_Ctx->Map(g_G_CB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        ScanlineCBData cb;
        cb.screenW = bbW; cb.screenH = bbH;
        cb.hThickness = g_Shared->hThickness; cb.hGap = g_Shared->hGap;
        cb.hOpacity   = g_Shared->hOpacity;   cb.hStartX = 0.0f;
        cb.hWidth     = bbW;                  cb.hEnabled = g_Shared->hEnabled;
        cb.vThickness = g_Shared->vThickness; cb.vGap = g_Shared->vGap;
        cb.vOpacity   = g_Shared->vOpacity;   cb.vStartY = 0.0f;
        cb.vHeight    = bbH;                  cb.vEnabled = g_Shared->vEnabled;
        cb.blurEnabled       = g_Shared->blurEnabled;
        cb.blurIntensity     = g_Shared->blurIntensity;
        cb.bloomEnabled      = g_Shared->bloomEnabled;
        cb.bloomIntensity    = g_Shared->bloomIntensity;
        cb.curvatureEnabled  = g_Shared->curvatureEnabled;
        cb.curvatureIntensity = g_Shared->curvatureIntensity;
        cb.brightness        = g_Shared->brightness;
        cb.contrast          = g_Shared->contrast;
        cb.saturation        = g_Shared->saturation;
        cb.temperature       = g_Shared->temperature;
        cb.flickerEnabled    = g_Shared->flickerEnabled;
        cb.flickerIntensity  = g_Shared->flickerIntensity;
        cb.flickerRate       = g_Shared->flickerRate;
        cb.time              = GetTimeSeconds();
        cb.blackLevel        = g_Shared->blackLevel;
        cb.gamma             = g_Shared->gamma;
        cb.phosphorEnabled   = g_Shared->phosphorEnabled;
        cb.phosphorIntensity = g_Shared->phosphorIntensity;
        cb.vhsEnabled        = g_Shared->vhsEnabled;
        cb.vhsIntensity      = g_Shared->vhsIntensity;
        cb.grainIntensity    = g_Shared->grainIntensity;
        cb.tapeNoiseEnabled  = tapeOn ? 1.0f : 0.0f;
        cb.tapeNoiseIntensity = g_Shared->tapeNoiseIntensity;
        cb.vignetteEnabled    = g_Shared->vignetteEnabled;
        cb.megaBezelEnabled   = g_Shared->megaBezelEnabled;
        cb.megaBezelThickness = g_Shared->megaBezelThickness;
        cb.megaBezelOpacity   = g_Shared->megaBezelOpacity;
        cb.megaBezelBlur      = g_Shared->megaBezelBlur;
        cb.bezelHookActive    = (g_Shared->bezelHookActive > 0.0f && g_BezelSRV) ? g_Shared->bezelHookActive : 0.0f;
        cb.bezelHookOpacity   = g_Shared->bezelHookOpacity;
        cb.megaBezelRadius    = g_Shared->megaBezelRadius;
        cb.megaBezelReflectionWidth = g_Shared->megaBezelReflectionWidth;
        cb.megaBezelStartFade = GetMegaBezelStartFade(g_Shared->megaBezelEnabled > 0.0f);
        cb._cbpad3 = 0.0f;
        memcpy(mapped.pData, &cb, sizeof(ScanlineCBData));
        g_G_Ctx->Unmap(g_G_CB, 0);
    }

    // Save/restore game state — reuse v1.2's SavedState/SaveState/RestoreState
    SavedState saved; memset(&saved, 0, sizeof(saved));
    SaveState(g_G_Ctx, saved);

    float blendFactor[4] = { 1, 1, 1, 1 };
    g_G_Ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_G_Ctx->OMSetBlendState(needCopy ? g_G_BlendOver : g_G_Blend, blendFactor, 0xFFFFFFFF);
    g_G_Ctx->RSSetViewports(1, &vp);
    g_G_Ctx->RSSetState(g_G_Raster);
    g_G_Ctx->VSSetShader(g_G_VS, nullptr, 0);
    g_G_Ctx->PSSetShader(g_G_PS, nullptr, 0);
    g_G_Ctx->PSSetConstantBuffers(0, 1, &g_G_CB);
    if (needCopy && g_G_BBSRV && g_G_Sampler) {
        g_G_Ctx->PSSetShaderResources(0, 1, &g_G_BBSRV);
        g_G_Ctx->PSSetSamplers(0, 1, &g_G_Sampler);
    }
    g_G_Ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_G_Ctx->IASetInputLayout(nullptr);

    g_G_Ctx->Draw(3, 0);

    RestoreState(g_G_Ctx, saved);

    rtv->Release();
}

// ── Hooked Present for Gopher (independent from main path) ───────────────
static HRESULT STDMETHODCALLTYPE HookedPresent_Gopher(
    IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
    static bool s_firstCall = true;
    if (s_firstCall) { s_firstCall = false; Log("[GOPHER] HookedPresent_Gopher FIRED — pSC=0x%p", (void*)pSwapChain); }

    // Global reentrancy guard — wgpu may call Present from multiple threads.
    // If another thread is already inside our effect path, skip effects this
    // frame rather than double-applying or racing on GPU resources.
    // InterlockedCompareExchange: atomically set busy=1 only if it was 0.
    if (InterlockedCompareExchange(&g_G_HookBusy, 1, 0) == 0) {
        __try {
            if (!g_G_Inited) {
                // InitResources is now fast (uses pre-compiled bytecodes).
                // Safe to call here — no shader compile, just CreateVertexShader etc.
                Gopher_InitResources(pSwapChain);
            }
            if (g_G_Inited) {
                Gopher_ApplyScanlines(pSwapChain);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            // On exception (e.g. device lost, DWM transition during alt-tab):
            // release all resources cleanly and allow re-init on next frame.
            // Re-init is now fast (no D3DCompile), so this doesn't freeze.
            Log("[GOPHER] EXCEPTION 0x%08X in HookedPresent_Gopher — reinit next frame",
                GetExceptionCode());
            Gopher_ReleaseResources();
            g_G_Inited = false;
        }
        InterlockedExchange(&g_G_HookBusy, 0);
    }

    // Restore-call-repatch: temporarily restore the first 14 bytes of Present
    // so the real function body executes without re-entering our hook.
    if (g_G_PresentAddr) {
        DWORD op;
        VirtualProtect(g_G_PresentAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
        memcpy(g_G_PresentAddr, g_G_PresentOrig, HOOK_JMP_SIZE);
        FlushInstructionCache(GetCurrentProcess(), g_G_PresentAddr, HOOK_JMP_SIZE);
        VirtualProtect(g_G_PresentAddr, HOOK_JMP_SIZE, op, &op);
        HRESULT hr = g_G_OrigPresent(pSwapChain, SyncInterval, Flags);
        VirtualProtect(g_G_PresentAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
        memcpy(g_G_PresentAddr, g_G_PresentJmp, HOOK_JMP_SIZE);
        FlushInstructionCache(GetCurrentProcess(), g_G_PresentAddr, HOOK_JMP_SIZE);
        VirtualProtect(g_G_PresentAddr, HOOK_JMP_SIZE, op, &op);
        return hr;
    }
    return g_G_OrigPresent(pSwapChain, SyncInterval, Flags);
}

// ── Hooked Present1 for Gopher (mirrors HookedPresent_Gopher for Present1) ─
// wgpu calls IDXGISwapChain::Present for the very first frame after a resize,
// then permanently switches to IDXGISwapChain1::Present1 (vtable[22]).
// Without this hook, effects disappear after every window state change because
// our HookedPresent_Gopher is never reached again.
static HRESULT STDMETHODCALLTYPE HookedPresent1_Gopher(
    IDXGISwapChain1* pSwapChain, UINT SyncInterval, UINT Flags,
    const DXGI_PRESENT_PARAMETERS* pPresentParams)
{
    static bool s_firstCall = true;
    if (s_firstCall) { s_firstCall = false; Log("[GOPHER] HookedPresent1_Gopher FIRED — pSC=0x%p", (void*)pSwapChain); }

    if (InterlockedCompareExchange(&g_G_HookBusy, 1, 0) == 0) {
        __try {
            if (!g_G_Inited) {
                Gopher_InitResources((IDXGISwapChain*)pSwapChain);
            }
            if (g_G_Inited) {
                Gopher_ApplyScanlines((IDXGISwapChain*)pSwapChain);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            Log("[GOPHER] EXCEPTION 0x%08X in HookedPresent1_Gopher — reinit next frame",
                GetExceptionCode());
            Gopher_ReleaseResources();
            g_G_Inited = false;
        }
        InterlockedExchange(&g_G_HookBusy, 0);
    }

    // Restore-call-repatch for Present1.
    if (g_G_Present1Addr) {
        DWORD op;
        VirtualProtect(g_G_Present1Addr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
        memcpy(g_G_Present1Addr, g_G_Present1Orig, HOOK_JMP_SIZE);
        FlushInstructionCache(GetCurrentProcess(), g_G_Present1Addr, HOOK_JMP_SIZE);
        VirtualProtect(g_G_Present1Addr, HOOK_JMP_SIZE, op, &op);
        HRESULT hr = g_G_OrigPresent1(pSwapChain, SyncInterval, Flags, pPresentParams);
        VirtualProtect(g_G_Present1Addr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
        memcpy(g_G_Present1Addr, g_G_Present1Jmp, HOOK_JMP_SIZE);
        FlushInstructionCache(GetCurrentProcess(), g_G_Present1Addr, HOOK_JMP_SIZE);
        VirtualProtect(g_G_Present1Addr, HOOK_JMP_SIZE, op, &op);
        return hr;
    }
    return g_G_OrigPresent1(pSwapChain, SyncInterval, Flags, pPresentParams);
}

// ── Install Gopher Present inline hook (no vtable patch) ─────────────────
static bool InstallGopher64Hook() {
    if (!GetModuleHandleW(L"d3d11.dll") || !GetModuleHandleW(L"dxgi.dll")) {
        Log("[GOPHER] InstallGopher64Hook: d3d11.dll or dxgi.dll not loaded, skipping");
        return false;
    }
    Log("[GOPHER] InstallGopher64Hook: creating dummy device to extract Present address");

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW), 0, DefWindowProcW, 0, 0,
        GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr,
        L"S4W_GopherDummyWC", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED,
        0, 0, 4, 4, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return false;

    // Use BitBlt-model — Windows 11 shares Present vtable between BitBlt and
    // Flip swap chains, so the address we extract here is the same one the
    // Gopher wgpu flip-model swap chain will call.
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount        = 1;
    sd.BufferDesc.Width   = 4;
    sd.BufferDesc.Height  = 4;
    sd.BufferDesc.Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow       = hwnd;
    sd.SampleDesc.Count   = 1;
    sd.Windowed           = TRUE;

    IDXGISwapChain* dummySC = nullptr;
    ID3D11Device* dummyDev = nullptr;
    ID3D11DeviceContext* dummyCtx = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &sd, &dummySC, &dummyDev, &fl, &dummyCtx);
    if (FAILED(hr) || !dummySC) {
        Log("[GOPHER] FAIL: D3D11CreateDeviceAndSwapChain hr=0x%08X", hr);
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    void** vtable = *(void***)dummySC;
    g_G_OrigPresent  = (PFN_Present)vtable[8];
    g_G_PresentAddr  = (BYTE*)g_G_OrigPresent;

    // QI for IDXGISwapChain1 to extract Present1 (vtable[22]) — must be done
    // BEFORE releasing dummySC, otherwise the vtable pointer may become invalid.
    IDXGISwapChain1* dummySC1 = nullptr;
    if (SUCCEEDED(dummySC->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&dummySC1))) {
        void** vtable1 = *(void***)dummySC1;
        g_G_OrigPresent1 = (PFN_Present1)vtable1[22];
        g_G_Present1Addr = (BYTE*)g_G_OrigPresent1;
        dummySC1->Release();
        Log("[GOPHER] Present1 address extracted: 0x%p", (void*)g_G_Present1Addr);
    } else {
        Log("[GOPHER] WARN: IDXGISwapChain1 QI failed — Present1 hook NOT installed");
    }

    dummySC->Release();
    dummyDev->Release();
    dummyCtx->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    if (!g_G_PresentAddr) {
        Log("[GOPHER] FAIL: could not extract Present address");
        return false;
    }

    // Inline hook ONLY — no vtable patch. Gopher64 uses per-instance vtables
    // through wgpu's DXGI factory, so the vtable patch wouldn't take. The
    // inline hook on dxgi.dll's Present/Present1 byte-patches the function body,
    // which affects every swap chain in this process.
    bool ok = false;
    if (InstallHookAt(g_G_PresentAddr, g_G_PresentOrig, g_G_PresentJmp, (void*)HookedPresent_Gopher)) {
        Log("[GOPHER] Inline hook installed: Present at 0x%p", (void*)g_G_PresentAddr);
        ok = true;
    } else {
        Log("[GOPHER] FAIL: InstallHookAt(Present) returned false");
        g_G_PresentAddr = nullptr;
    }

    // Install Present1 inline hook — same function body address as the main D3D11
    // path. Guard against the rare case where Present and Present1 resolve to the
    // same address (some DXGI versions share the implementation).
    if (g_G_Present1Addr && g_G_Present1Addr != g_G_PresentAddr) {
        if (InstallHookAt(g_G_Present1Addr, g_G_Present1Orig, g_G_Present1Jmp, (void*)HookedPresent1_Gopher)) {
            Log("[GOPHER] Inline hook installed: Present1 at 0x%p", (void*)g_G_Present1Addr);
            ok = true;
        } else {
            Log("[GOPHER] WARN: InstallHookAt(Present1) returned false");
            g_G_Present1Addr = nullptr;
        }
    } else if (g_G_Present1Addr && g_G_Present1Addr == g_G_PresentAddr) {
        // Present and Present1 share the same function body — the Present hook
        // already covers both. No separate hook needed; clear Present1 addr so
        // HookedPresent1_Gopher's restore-call-repatch doesn't double-restore.
        Log("[GOPHER] Present1 == Present address — single hook covers both");
        g_G_Present1Addr = nullptr;
        g_G_OrigPresent1 = (PFN_Present1)(void*)g_G_OrigPresent;
    }

    return ok;
}

static bool SafeInstallGopher64Hook() {
    __try { return InstallGopher64Hook(); }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("[GOPHER] InstallGopher64Hook: EXCEPTION 0x%08X caught", GetExceptionCode());
        return false;
    }
}

// ── Gopher-gated CreateProcessW/A hook (child renderer injection) ────────
// Gopher64 parent spawns a child with the same exe name for rendering. Our
// DLL must be injected into the child before its D3D11 renderer boots. We
// install CREATE_SUSPENDED, LoadLibraryW-inject, then ResumeThread.
typedef BOOL (WINAPI *PFN_CreateProcessW)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
    LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
typedef BOOL (WINAPI *PFN_CreateProcessA)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES,
    LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);

static PFN_CreateProcessW g_G_OrigCreateProcessW = nullptr;
static BYTE*              g_G_CPWAddr            = nullptr;
static BYTE               g_G_CPWOrig[14]        = {};
static BYTE               g_G_CPWJmp[14]         = {};

static PFN_CreateProcessA g_G_OrigCreateProcessA = nullptr;
static BYTE*              g_G_CPAAddr            = nullptr;
static BYTE               g_G_CPAOrig[14]        = {};
static BYTE               g_G_CPAJmp[14]         = {};

// Inject this DLL into a process by path, using CreateRemoteThread(LoadLibraryW).
static bool InjectSelfInto(HANDLE hProc, HANDLE hThread) {
    wchar_t dllPath[MAX_PATH] = {};
    if (!GetModuleFileNameW(g_Module, dllPath, MAX_PATH)) {
        Log("[GOPHER] Inject: GetModuleFileNameW failed");
        return false;
    }
    SIZE_T pathBytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    LPVOID remoteMem = VirtualAllocEx(hProc, nullptr, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) { Log("[GOPHER] Inject: VirtualAllocEx failed"); return false; }
    SIZE_T written = 0;
    if (!WriteProcessMemory(hProc, remoteMem, dllPath, pathBytes, &written) || written != pathBytes) {
        Log("[GOPHER] Inject: WriteProcessMemory failed");
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        return false;
    }
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    LPVOID pLoadLib = hK32 ? (LPVOID)GetProcAddress(hK32, "LoadLibraryW") : nullptr;
    if (!pLoadLib) { Log("[GOPHER] Inject: GetProcAddress(LoadLibraryW) failed"); VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE); return false; }
    HANDLE hRT = CreateRemoteThread(hProc, nullptr, 0,
        (LPTHREAD_START_ROUTINE)pLoadLib, remoteMem, 0, nullptr);
    if (!hRT) { Log("[GOPHER] Inject: CreateRemoteThread failed gle=%u", GetLastError()); VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE); return false; }
    WaitForSingleObject(hRT, 5000);
    CloseHandle(hRT);
    // Don't free remoteMem — LoadLibraryW holds a reference until the child
    // copy of the string is read. Small leak, but guaranteed safe.
    Log("[GOPHER] Inject: DLL injected OK (%ls)", dllPath);
    return true;
}

static BOOL WINAPI HookedCreateProcessW_Gopher(
    LPCWSTR appName, LPWSTR cmdLine, LPSECURITY_ATTRIBUTES procAttr,
    LPSECURITY_ATTRIBUTES thrAttr, BOOL inheritHandles, DWORD flags,
    LPVOID env, LPCWSTR curDir, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi)
{
    DWORD forcedFlags = flags | CREATE_SUSPENDED;
    // Restore-call-repatch around the real CreateProcessW
    BOOL ok = FALSE;
    if (g_G_CPWAddr) {
        DWORD op;
        VirtualProtect(g_G_CPWAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
        memcpy(g_G_CPWAddr, g_G_CPWOrig, HOOK_JMP_SIZE);
        FlushInstructionCache(GetCurrentProcess(), g_G_CPWAddr, HOOK_JMP_SIZE);
        VirtualProtect(g_G_CPWAddr, HOOK_JMP_SIZE, op, &op);
        ok = g_G_OrigCreateProcessW(appName, cmdLine, procAttr, thrAttr, inheritHandles,
                                    forcedFlags, env, curDir, si, pi);
        VirtualProtect(g_G_CPWAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
        memcpy(g_G_CPWAddr, g_G_CPWJmp, HOOK_JMP_SIZE);
        FlushInstructionCache(GetCurrentProcess(), g_G_CPWAddr, HOOK_JMP_SIZE);
        VirtualProtect(g_G_CPWAddr, HOOK_JMP_SIZE, op, &op);
    } else {
        ok = g_G_OrigCreateProcessW(appName, cmdLine, procAttr, thrAttr, inheritHandles,
                                    forcedFlags, env, curDir, si, pi);
    }
    if (ok && pi && pi->hProcess && pi->hThread) {
        // Log what exe is being spawned (appName may be null if exe is in cmdLine)
        char exeLog[260] = "<embedded in cmdLine>";
        if (appName) WideCharToMultiByte(CP_ACP, 0, appName, -1, exeLog, 260, nullptr, nullptr);
        Log("[GOPHER] CreateProcessW intercepted pid=%u exe=%s — injecting DLL",
            pi->dwProcessId, exeLog);
        InjectSelfInto(pi->hProcess, pi->hThread);
        // Only resume if the caller didn't originally ask for suspended.
        if ((flags & CREATE_SUSPENDED) == 0) ResumeThread(pi->hThread);
    }
    return ok;
}

static BOOL WINAPI HookedCreateProcessA_Gopher(
    LPCSTR appName, LPSTR cmdLine, LPSECURITY_ATTRIBUTES procAttr,
    LPSECURITY_ATTRIBUTES thrAttr, BOOL inheritHandles, DWORD flags,
    LPVOID env, LPCSTR curDir, LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi)
{
    DWORD forcedFlags = flags | CREATE_SUSPENDED;
    BOOL ok = FALSE;
    if (g_G_CPAAddr) {
        DWORD op;
        VirtualProtect(g_G_CPAAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
        memcpy(g_G_CPAAddr, g_G_CPAOrig, HOOK_JMP_SIZE);
        FlushInstructionCache(GetCurrentProcess(), g_G_CPAAddr, HOOK_JMP_SIZE);
        VirtualProtect(g_G_CPAAddr, HOOK_JMP_SIZE, op, &op);
        ok = g_G_OrigCreateProcessA(appName, cmdLine, procAttr, thrAttr, inheritHandles,
                                    forcedFlags, env, curDir, si, pi);
        VirtualProtect(g_G_CPAAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
        memcpy(g_G_CPAAddr, g_G_CPAJmp, HOOK_JMP_SIZE);
        FlushInstructionCache(GetCurrentProcess(), g_G_CPAAddr, HOOK_JMP_SIZE);
        VirtualProtect(g_G_CPAAddr, HOOK_JMP_SIZE, op, &op);
    } else {
        ok = g_G_OrigCreateProcessA(appName, cmdLine, procAttr, thrAttr, inheritHandles,
                                    forcedFlags, env, curDir, si, pi);
    }
    if (ok && pi && pi->hProcess && pi->hThread) {
        Log("[GOPHER] CreateProcessA intercepted pid=%u — injecting DLL", pi->dwProcessId);
        InjectSelfInto(pi->hProcess, pi->hThread);
        if ((flags & CREATE_SUSPENDED) == 0) ResumeThread(pi->hThread);
    }
    return ok;
}

static bool InstallGopherChildProcessHook() {
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    if (!hK32) { Log("[GOPHER] kernel32.dll not loaded!? cannot hook CreateProcess"); return false; }
    g_G_OrigCreateProcessW = (PFN_CreateProcessW)GetProcAddress(hK32, "CreateProcessW");
    g_G_OrigCreateProcessA = (PFN_CreateProcessA)GetProcAddress(hK32, "CreateProcessA");
    if (g_G_OrigCreateProcessW) {
        g_G_CPWAddr = (BYTE*)g_G_OrigCreateProcessW;
        if (InstallHookAt(g_G_CPWAddr, g_G_CPWOrig, g_G_CPWJmp, (void*)HookedCreateProcessW_Gopher))
            Log("[GOPHER] CreateProcessW hooked at 0x%p", (void*)g_G_CPWAddr);
        else { Log("[GOPHER] WARN: CreateProcessW hook failed"); g_G_CPWAddr = nullptr; }
    }
    if (g_G_OrigCreateProcessA) {
        g_G_CPAAddr = (BYTE*)g_G_OrigCreateProcessA;
        if (InstallHookAt(g_G_CPAAddr, g_G_CPAOrig, g_G_CPAJmp, (void*)HookedCreateProcessA_Gopher))
            Log("[GOPHER] CreateProcessA hooked at 0x%p", (void*)g_G_CPAAddr);
        else { Log("[GOPHER] WARN: CreateProcessA hook failed"); g_G_CPAAddr = nullptr; }
    }
    return g_G_CPWAddr || g_G_CPAAddr;
}

// ── Check if command line contains a ROM file (direct "Open with" launch) ─
// When the user right-clicks a ROM and chooses "Open with > Gopher64", explorer
// launches gopher64.exe directly with the ROM path as an argument.  In this case
// there is no Gopher64 launcher parent — Gopher64 runs fully in-process as a
// renderer.  Detecting a ROM extension in the command line is the reliable signal.
static bool Gopher_HasRomInCommandLine() {
    const wchar_t* cmdLine = GetCommandLineW();
    if (!cmdLine) return false;

    // N64 ROM extensions Gopher64 can open.
    static const wchar_t* kExts[] = {
        L".n64", L".v64", L".z64", L".rom", L".ndd",
        L".N64", L".V64", L".Z64", L".ROM", L".NDD",
        nullptr
    };
    for (int i = 0; kExts[i]; ++i) {
        if (wcsstr(cmdLine, kExts[i])) {
            Log("[GOPHER] ROM extension '%ls' found in command line — direct launch detected", kExts[i]);
            return true;
        }
    }
    return false;
}

// ── Check if we are the child renderer (parent has same exe name) ────────
static bool Gopher_IsChildRenderer() {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return false;
    DWORD myPid = GetCurrentProcessId();
    DWORD parentPid = 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID == myPid) { parentPid = pe.th32ParentProcessID; break; }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    if (!parentPid) return false;

    // Query parent exe path
    wchar_t parentPath[MAX_PATH] = {}; DWORD pLen = MAX_PATH;
    HANDLE hParent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parentPid);
    if (!hParent) return false;
    BOOL okP = QueryFullProcessImageNameW(hParent, 0, parentPath, &pLen);
    CloseHandle(hParent);
    if (!okP) return false;

    wchar_t myPath[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, myPath, MAX_PATH)) return false;

    const wchar_t* pName = wcsrchr(parentPath, L'\\'); pName = pName ? pName+1 : parentPath;
    const wchar_t* mName = wcsrchr(myPath,     L'\\'); mName = mName ? mName+1 : myPath;
    bool sameName = (_wcsicmp(pName, mName) == 0);
    Log("[GOPHER] Parent pid=%u exe=%ls | me pid=%u exe=%ls | same=%d",
        parentPid, pName, myPid, mName, sameName?1:0);
    return sameName;
}

// ── Gopher parallel worker thread ────────────────────────────────────────
static DWORD GopherHookThread() {
    Log("[GOPHER] GopherHookThread started");

    // Detect role: renderer vs launcher.
    //
    // Two cases where we ARE the renderer and must install the Present hook:
    //   1. Normal flow:     parent exe == our exe (same=1) — Gopher64 launcher
    //                       spawned us as the child renderer process.
    //   2. "Open with" flow: explorer launched us directly with a ROM path as
    //                       argument (parent=explorer, same=0) — no separate
    //                       launcher exists; we ARE the renderer from the start.
    //
    // In the standard launcher flow with NO ROM arg, same=0 correctly means
    // "we are the GUI launcher; skip Present hook to avoid GPU deadlock."
    bool isChild   = Gopher_IsChildRenderer();
    bool hasRomArg = !isChild && Gopher_HasRomInCommandLine();

    if (isChild) {
        Log("[GOPHER] Parent has same exe name — we are the CHILD renderer → install Present hook");
    } else if (hasRomArg) {
        Log("[GOPHER] Direct launch with ROM argument (Open with / CLI) — treating as renderer → install Present hook");
        isChild = true; // renderer path from here on
    } else {
        Log("[GOPHER] Parent has different exe name — we appear to be the LAUNCHER (GUI)");
    }

    // Always install CreateProcess hook (both parent and child — cheap, idempotent).
    InstallGopherChildProcessHook();

    // Pre-compile shaders unconditionally (takes 0.5-2s, background thread).
    // Done here so InitResources (inside Present hook) is instantaneous.
    // Also allows the fallback d3d11 check below to see if we're really a renderer.
    if (!Gopher_PrecompileShaders()) {
        Log("[GOPHER] FATAL: shader pre-compile failed — effects will be disabled");
        return 0;
    }

    // If parent-name detection said "launcher", skip Present hook.
    // The LAUNCHER (parent=explorer.exe) uses wgpu/D3D11 for its own ROM-browser
    // UI — it loads d3d11+dxgi for itself. Installing the Present hook there causes
    // a GPU deadlock when DXGI transitions between fullscreen and windowed modes
    // (Present blocks in DWM while our GPU draw is in-flight on the same device).
    // All observed child renderers correctly detect same=1, so no fallback needed.
    if (!isChild) {
        Log("[GOPHER] Launcher mode — Present hook SKIPPED (parent is not a Gopher64 process)");
        return 0;
    }

    // No explicit sleep needed — Gopher_PrecompileShaders() already takes ~150ms
    // (with skip-optimisation flag), giving wgpu enough time to load d3d11/dxgi.
    // The retry loop below handles the rare case where wgpu initialises slower.
    bool ok = SafeInstallGopher64Hook();
    Log("[GOPHER] Initial Present hook install: %s", ok ? "OK" : "failed");
    if (!ok) {
        for (int i = 0; i < 30; i++) {
            Sleep(1000);
            ok = SafeInstallGopher64Hook();
            if (ok) { Log("[GOPHER] Present hook installed at T+~%ds", i + 2); break; }
        }
    }
    Log("[GOPHER] GopherHookThread done (hookInstalled=%s)", ok?"yes":"no");
    return 0;
}

// ══════════════════════════════════════════════════════════════════════════
//  D3D9 HOOK (for games/emulators using Direct3D 9)
// ══════════════════════════════════════════════════════════════════════════

typedef HRESULT(STDMETHODCALLTYPE* PFN_D3D9Present)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_D3D9EndScene)(IDirect3DDevice9*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_D3D9Reset)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

static PFN_D3D9EndScene    g_OrigD3D9EndScene  = nullptr;
static PFN_D3D9Present     g_OrigD3D9Present   = nullptr;
static PFN_D3D9Reset       g_OrigD3D9Reset     = nullptr;
static IDirect3DDevice9*   g_D3D9Device        = nullptr;
static IDirect3DPixelShader9*  g_D3D9PS        = nullptr;
static IDirect3DVertexShader9* g_D3D9VS        = nullptr;
static IDirect3DTexture9*  g_D3D9BBCopy        = nullptr;
// MegaBezel bezel PNG texture (D3D9). Loaded via WIC the first time the
// shared-memory path changes; bound to sampler s1 alongside the backbuffer
// copy on s0. Mirrors g_BezelTex / g_BezelSRV from the D3D11 path.
static IDirect3DTexture9*  g_D3D9BezelTex      = nullptr;
// Auto-detected game content bounds in UV space [0..1]. Used by MegaBezel
// reflection shader to ignore in-game letterbox/pillarbox black bars when
// sampling the reflection — so 4:3 / unusual ratios reflect the actual game
// pixels instead of the black bars around them.
static IDirect3DSurface9*  g_D3D9DetectRT      = nullptr;  // 64x36 DEFAULT RT
static IDirect3DSurface9*  g_D3D9DetectSys     = nullptr;  // 64x36 SYSTEMMEM
static const UINT          DETECT_W            = 64;
static const UINT          DETECT_H            = 36;
static float               g_GameBoundsL       = 0.0f;
static float               g_GameBoundsT       = 0.0f;
static float               g_GameBoundsR       = 1.0f;
static float               g_GameBoundsB       = 1.0f;
static int                 g_DetectFrame       = 0;
static wchar_t             g_D3D9BezelPathCached[260] = {};
static UINT                g_D3D9LastW = 0, g_D3D9LastH = 0;
static bool                g_D3D9Inited        = false;
static bool                g_D3D9HasCapture    = false; // true if g_D3D9BBCopy has valid scene data captured at EndScene
// Per-process kill-switch: true if this process is on the D3D9 incompatible list
// (e.g. Freedom Planet / GMS1 games where the default backbuffer never contains the
// presented image). When true, both D3D9CaptureScene and ApplyD3D9Scanlines become
// no-ops and the game renders fully untouched.
static bool                g_D3D9ProcBlocked   = false;
static bool                g_D3D9ProcChecked   = false;
// NOTE: effects are applied in HookedD3D9Present (once per frame), NOT in EndScene.
// MMF2 and some engines call EndScene multiple times per frame (one per layer), which
// would stack CRT passes and corrupt the image if applied there.

// Restore-call-repatch buffers for D3D9 inline hooks
static BYTE* g_D3D9EndSceneAddr = nullptr;
static BYTE  g_D3D9EndSceneOrig[14] = {};
static BYTE  g_D3D9EndSceneJmp[14]  = {};
static BYTE* g_D3D9PresentAddr  = nullptr;
static BYTE  g_D3D9PresentOrig[14]  = {};
static BYTE  g_D3D9PresentJmp[14]   = {};
static BYTE* g_D3D9ResetAddr    = nullptr;
static BYTE  g_D3D9ResetOrig[14]    = {};
static BYTE  g_D3D9ResetJmp[14]     = {};
// IDirect3DSwapChain9::Present (separate function from IDirect3DDevice9::Present).
// Some engines present via the swap chain object directly instead of the device,
// which bypasses our device-level Present hook.
static BYTE* g_D3D9SCPresentAddr = nullptr;
static BYTE  g_D3D9SCPresentOrig[14] = {};
static BYTE  g_D3D9SCPresentJmp[14]  = {};
// IDirect3DDevice9Ex::PresentEx — new method added in D3D9Ex interface at vtable slot 121.
static BYTE* g_D3D9PresentExAddr = nullptr;
static BYTE  g_D3D9PresentExOrig[14] = {};
static BYTE  g_D3D9PresentExJmp[14]  = {};
// IDirect3D9::CreateDevice / IDirect3D9Ex::CreateDeviceEx deferred hooks —
// fallback when our own dummy device creation fails (e.g. D3D9Ex has exclusive
// control, or the NVIDIA driver isn't ready yet).
// We hook the factory Create methods so we intercept the game's own device
// creation and install the device-level hooks at that point instead.
static BYTE* g_D3D9CreateDeviceAddr   = nullptr;
static BYTE  g_D3D9CreateDeviceOrig[14] = {};
static BYTE  g_D3D9CreateDeviceJmp[14]  = {};
static BYTE* g_D3D9CreateDeviceExAddr = nullptr;
static BYTE  g_D3D9CreateDeviceExOrig[14] = {};
static BYTE  g_D3D9CreateDeviceExJmp[14]  = {};
static bool  g_D3D9DeferredHookActive   = false;

// D3D9 HLSL pixel shader — CRT scanline mask + optional Gaussian blur
// c0  = (screenW, screenH, hThickness, hGap)
// c1  = (hOpacity, hStartX, hWidth, hEnabled)
// c2  = (vThickness, vGap, vOpacity, vStartY)
// c3  = (vHeight, vEnabled, blurEnabled, blurIntensity)
// c4  = (bloomEnabled, bloomIntensity, curvatureEnabled, curvatureIntensity)
// c5  = (brightness, contrast, saturation, temperature)
// c6  = (flickerEnabled, flickerIntensity, time, flickerRate)
// c7  = (blackLevel, gamma, phosphorEnabled, phosphorIntensity)
// c8  = (vhsEnabled, vhsIntensity, grainIntensity, 0)
// c9  = (tapeNoiseEnabled, tapeNoiseIntensity, vignetteEnabled, 0)
// c10 = (megaBezelEnabled, megaBezelThickness, megaBezelOpacity, megaBezelBlur)
// c11 = (megaBezelRadius, megaBezelReflectionWidth, megaBezelStartFade, _pad)
// c12 = (bezelHookActive, bezelHookOpacity, _pad, _pad)
static const char* D3D9_PS_SRC = R"(
sampler2D backBuf  : register(s0);
sampler2D bezelTex : register(s1);

float4 cb0  : register(c0);
float4 cb1  : register(c1);
float4 cb2  : register(c2);
float4 cb3  : register(c3);
float4 cb4  : register(c4);
float4 cb5  : register(c5);
float4 cb6  : register(c6);
float4 cb7  : register(c7);
float4 cb8  : register(c8);
float4 cb9  : register(c9);
float4 cb10 : register(c10);
float4 cb11 : register(c11);
float4 cb12 : register(c12);
float4 cb13 : register(c13);
float4 cb14 : register(c14);

float tnHash9(float n) { return frac(sin(n) * 43758.5453123); }
float tnN3d9(float3 x) {
    float3 p = floor(x);
    float3 f = frac(x);
    f = f*f*(3.0-2.0*f);
    float n = p.x + p.y*57.0 + 113.0*p.z;
    return lerp(lerp(lerp(tnHash9(n),        tnHash9(n+1.0),   f.x),
                     lerp(tnHash9(n+57.0),   tnHash9(n+58.0),  f.x), f.y),
               lerp(lerp(tnHash9(n+113.0),   tnHash9(n+114.0), f.x),
                     lerp(tnHash9(n+170.0),  tnHash9(n+171.0), f.x), f.y), f.z);
}
float tnNn9(float2 p, float fc) {
    float y = p.y;
    float s = fmod(fc * 0.15, 4837.0);
    float v = tnN3d9(float3(y*0.01  + s,        1.0, 1.0))
            * tnN3d9(float3(y*0.011 + 1000.0+s, 1.0, 1.0))
            * tnN3d9(float3(y*0.51  + 421.0+s,  1.0, 1.0));
    v *= tnHash9(p.x + fc * 0.01) + 0.3;
    v = pow(v + 0.3, 1.0);
    if (v < 0.99) v = 0.0;
    return v;
}

float scanlineIntensity(float pos, float thick, float gap, float opacity, float fw) {
    float period = thick + gap;
    if (period <= 0.0) return 1.0;
    float snapped = max(round(period), 1.0);
    float scale   = snapped / period;
    thick        *= scale;
    period        = snapped;
    float t = fmod(pos, period);
    float edge  = max(fw, 0.5);
    float inLine = 1.0 - smoothstep(thick - edge, thick + edge, t);
    float vis = 1.0 - smoothstep(period * 0.3, period * 0.5, fw);
    return 1.0 - inLine * opacity * vis;
}

float2 CurveUV(float2 uv, float strength) {
    float2 cc = uv * 2.0 - 1.0;
    cc *= 1.0 + strength * dot(cc, cc);
    return cc * 0.5 + 0.5;
}

float4 main(float2 vpos : VPOS, float2 uv : TEXCOORD0) : COLOR {
    // cb4.z = curvatureEnabled, cb4.w = curvatureIntensity
    // cb10.x = megaBezelEnabled (mirror reflection in border zone)
    // Scanlines stay in screen-space (vpos) — never warp with barrel distortion
    float2 sampleUV = uv;
    float2 scanPos  = vpos;
    bool curved = cb4.z >= 0.5 && cb4.w > 0.0;
    bool megaBz = cb10.x >= 0.5;

    // MegaBezel ON: ALWAYS shrink first so the resize slider always controls
    // the visible game viewport size, even when curvature is active. Curvature
    // (if on) is then applied INSIDE the shrunken viewport.
    // MegaBezel OFF + curvature ON: classic full-screen curvature (no shrink).
    if (megaBz) {
        float margin = cb10.y * 0.10;
        if (margin < 0.001) margin = 0.001;
        sampleUV.x = (uv.x - margin) / (1.0 - 2.0 * margin);
        sampleUV.y = (uv.y - margin) / (1.0 - 2.0 * margin);
        if (curved) {
            sampleUV = CurveUV(sampleUV, cb4.w * 0.25);
        }
    } else if (curved) {
        sampleUV = CurveUV(uv, cb4.w * 0.25);
    }

    // Rounded-rect SDF for the inner game viewport — works identically with or
    // without curvature since both produce sampleUV. The radius rounds the
    // visible corners of the game in both modes.
    // REFL. RADIUS (cb11.x = megaBezelRadius) — rounds the reflection's INNER corner
    // so it hugs a rounded CRT bezel PNG. 0 = square (original); higher rounds and
    // lets the reflection overflow inward over the game corner. Independent of GAME
    // CORNERS (cb9.z), which only rounds the in-game black mask below.
    float  mbR       = cb11.x * 0.025;
    float2 mbCV      = sampleUV - 0.5;
    float2 mbHalfExt = float2(0.5, 0.5);
    float2 mbQ       = abs(mbCV) - mbHalfExt + mbR;
    float  mbSDF     = length(max(mbQ, 0.0)) + min(max(mbQ.x, mbQ.y), 0.0) - mbR;

    // Reflection boundary = SQUARE (clean miter, no fan). Game corners rounded
    // separately below (REFL. RADIUS hard mask).
    bool outsideGame = megaBz ? (mbSDF > 0.0)
                    : (sampleUV.x < 0.0 || sampleUV.x > 1.0
                    || sampleUV.y < 0.0 || sampleUV.y > 1.0);

    if (outsideGame) {
        if (megaBz) {
            // ── Picture-frame mirror reflection ──
            // Sides: 45 deg miter (clean diagonal at corners — like a wood frame).
            // Rounded corners: radial mirror across the arc, smooth and seamless.
            float2 mUV;
            float bezelDepth;
            // Side (miter) reflection
            float depthX = sampleUV.x < 0.0 ? -sampleUV.x
                         : sampleUV.x > 1.0 ? sampleUV.x - 1.0 : 0.0;
            float depthY = sampleUV.y < 0.0 ? -sampleUV.y
                         : sampleUV.y > 1.0 ? sampleUV.y - 1.0 : 0.0;
            bool sInSide = depthX > 0.0;
            bool sInTopBot = depthY > 0.0;
            float2 mUV_side;
            if (sInSide && sInTopBot) {
                if (depthX >= depthY) {
                    mUV_side.x = sampleUV.x < 0.0 ? -sampleUV.x : 2.0 - sampleUV.x;
                    mUV_side.y = saturate(sampleUV.y);
                } else {
                    mUV_side.x = saturate(sampleUV.x);
                    mUV_side.y = sampleUV.y < 0.5 ? -sampleUV.y : 2.0 - sampleUV.y;
                }
            } else if (sInSide) {
                mUV_side.x = sampleUV.x < 0.0 ? -sampleUV.x : 2.0 - sampleUV.x;
                mUV_side.y = saturate(sampleUV.y);
            } else {
                mUV_side.x = saturate(sampleUV.x);
                mUV_side.y = sampleUV.y < 0.5 ? -sampleUV.y : 2.0 - sampleUV.y;
            }
            float bezelDepth_side = max(depthX, depthY);

            // Corner (radial through arc) reflection
            float2 arcCenter = sign(mbCV) * (mbHalfExt - mbR);
            float2 u         = mbCV - arcCenter;
            float  distU     = max(length(u), 1e-5);
            float2 mCV       = arcCenter + u * (2.0 * mbR - distU) / distU;
            float2 mUV_corner = mCV + 0.5;
            float bezelDepth_corner = max(distU - mbR, 0.0);

            // Smooth blend between miter and radial — eliminates the seam
            // visible at the side↔corner boundary (the 45° "cut" the user saw).
            // Pure 45° miter (radial corner mirror disabled — it caused the
            // "peacock fan" artifact on bright game-corner content).
            float blendCorner = 0.0;
            bezelDepth = lerp(bezelDepth_side, bezelDepth_corner, blendCorner);
            // ── Per-axis mirror across the ROUNDED game edge (radius mbR = REFL. RADIUS) ──
            // Mirror each axis across the rounded-edge position (arc in the corner,
            // straight elsewhere). The 45-deg split fills the rounded-off corner with the
            // two side reflections meeting at the diagonal — axis-aligned, NO radial fan.
            float2 cR    = sampleUV - 0.5;
            float2 sgnR  = sign(cR);
            float2 aR    = abs(cR);
            float  arcCo = 0.5 - mbR;
            float  xEdge = (aR.y > arcCo) ? (arcCo + sqrt(max(mbR*mbR - (aR.y-arcCo)*(aR.y-arcCo), 0.0))) : 0.5;
            float  yEdge = (aR.x > arcCo) ? (arcCo + sqrt(max(mbR*mbR - (aR.x-arcCo)*(aR.x-arcCo), 0.0))) : 0.5;
            float  depthXr = aR.x - xEdge;
            float  depthYr = aR.y - yEdge;
            float2 aM_X   = float2(2.0*xEdge - aR.x, aR.y);
            float2 aM_Y   = float2(aR.x, 2.0*yEdge - aR.y);
            float  blendW = max(mbR * 0.6, 1e-4);          // soft diagonal blend (tunable: bigger = smoother)
            float2 aM     = lerp(aM_Y, aM_X, smoothstep(-blendW, blendW, depthXr - depthYr));
            mUV = saturate(0.5 + sgnR * aM);
            // Remap mUV from full backbuffer [0,1] to the auto-detected game
            // content area cb14 = (boundsL, boundsT, boundsR, boundsB). This
            // skips in-game letterbox/pillarbox black bars (4:3, etc.) so the
            // reflection samples the actual game pixels.
            mUV.x = lerp(cb14.x, cb14.z, mUV.x);
            mUV.y = lerp(cb14.y, cb14.w, mUV.y);
            // 7x7 Gaussian blur (or single sample if blur off)
            float mbSigma = cb10.w * 5.0 + 0.0001;
            float2 mbTexel = 1.0 / float2(cb0.x, cb0.y);
            float3 reflColor;
            if (mbSigma > 0.05) {
                float3 sum = float3(0, 0, 0);
                float wSum = 0.0;
                for (int dy = -3; dy <= 3; dy++) {
                    for (int dx = -3; dx <= 3; dx++) {
                        float d2 = float(dx*dx + dy*dy);
                        float w = exp(-d2 / (2.0 * mbSigma * mbSigma));
                        sum += tex2D(backBuf, saturate(mUV + float2(dx, dy) * mbTexel * mbSigma)).rgb * w;
                        wSum += w;
                    }
                }
                reflColor = sum / wSum;
            } else {
                reflColor = tex2D(backBuf, mUV).rgb;
            }
            // Reflection width + fade (curved-depth per-axis, miter-aligned)
            float marginRef     = max(cb10.y * 0.10, 0.001);
            float gameWpx       = max(cb0.x * (1.0 - 2.0 * marginRef), 1.0);
            float gameHpx       = max(cb0.y * (1.0 - 2.0 * marginRef), 1.0);
            float reflW         = max(cb11.y, 0.001);
            float invShrink     = 1.0 / max(1.0 - 2.0 * marginRef, 1e-5);
            float curvStrength  = curved ? cb4.w * 0.25 : 0.0;
            float2 sUV_left  = float2((0.0 - marginRef) * invShrink,
                                       (uv.y - marginRef) * invShrink);
            float2 sUV_right = float2((1.0 - marginRef) * invShrink,
                                       (uv.y - marginRef) * invShrink);
            float2 sUV_top   = float2((uv.x - marginRef) * invShrink,
                                       (0.0 - marginRef) * invShrink);
            float2 sUV_bot   = float2((uv.x - marginRef) * invShrink,
                                       (1.0 - marginRef) * invShrink);
            float2 csu_left  = curved ? CurveUV(sUV_left,  curvStrength) : sUV_left;
            float2 csu_right = curved ? CurveUV(sUV_right, curvStrength) : sUV_right;
            float2 csu_top   = curved ? CurveUV(sUV_top,   curvStrength) : sUV_top;
            float2 csu_bot   = curved ? CurveUV(sUV_bot,   curvStrength) : sUV_bot;
            float depthXc = sampleUV.x < 0.0 ? -sampleUV.x
                          : sampleUV.x > 1.0 ? sampleUV.x - 1.0 : 0.0;
            float depthYc = sampleUV.y < 0.0 ? -sampleUV.y
                          : sampleUV.y > 1.0 ? sampleUV.y - 1.0 : 0.0;
            float maxDepthX = sampleUV.x < 0.0 ? -csu_left.x
                            : sampleUV.x > 1.0 ? csu_right.x - 1.0 : 1.0;
            float maxDepthY = sampleUV.y < 0.0 ? -csu_top.y
                            : sampleUV.y > 1.0 ? csu_bot.y - 1.0 : 1.0;
            float aspect = cb0.x / max(cb0.y, 1.0);
            float xFadeScale = max(1.0 / aspect, 1.0);
            float yFadeScale = max(aspect, 1.0);
            // Ray-rect intersection from arcCenter (inset corner) — distance
            // arcCenter→screenEdge along X = mbR + maxDepthX (idem Y).
            float2 dirCorn = u / max(distU, 1e-5);
            float corMaxX = (maxDepthX + mbR) / max(abs(dirCorn.x), 0.01);
            float corMaxY = (maxDepthY + mbR) / max(abs(dirCorn.y), 0.01);
            float corMaxRef = max(min(corMaxX, corMaxY) - mbR, 1e-5);
            float normDepth_corner = saturate(bezelDepth_corner / max(corMaxRef * reflW, 1e-5));
            float dxN = depthXc / max(maxDepthX * xFadeScale, 1e-5);
            float dyN = depthYc / max(maxDepthY * yFadeScale, 1e-5);
            float normDepth_side = saturate(max(dxN, dyN) / reflW);
            float normDepth = lerp(normDepth_side, normDepth_corner, blendCorner);
            float fade = 1.0 - normDepth;
            fade = fade * fade;
            float startFade = saturate(cb11.z);
            float3 reflected = reflColor * cb10.z * fade * startFade;
            if (cb12.x >= 0.5) {
                float4 bz = tex2D(bezelTex, uv);
                bz.rgb *= cb12.y;
                float ra = cb10.z * fade * startFade;
                return float4(reflected + bz.rgb * bz.a * (1.0 - ra), 1.0);
            }
            return float4(reflected, 1.0);
        }
        // Curvature with no MegaBezel: black outside (matches existing behavior).
        if (curved) return float4(0, 0, 0, 1);
    }
)";

// D3D9_PS_SRC2: remainder of pixel shader main() — split to stay under the
// MSVC raw-string-literal size limit (~16 KB per token).
static const char* D3D9_PS_SRC2 = R"(
    float mask = 1.0;
    float fwY = fwidth(scanPos.y);
    float fwX = fwidth(scanPos.x);

    if (cb1.w > 0.5 && cb0.z > 0 && (cb0.z + cb0.w) > 0) {
        float inBandX = step(cb1.y, scanPos.x) * step(scanPos.x, cb1.y + cb1.z);
        if (inBandX > 0.5) mask *= scanlineIntensity(scanPos.y, cb0.z, cb0.w, cb1.x, fwY);
    }

    if (cb3.y > 0.5 && cb2.x > 0 && (cb2.x + cb2.y) > 0) {
        float inBandY = step(cb2.w, scanPos.y) * step(scanPos.y, cb2.w + cb3.x);
        if (inBandY > 0.5) mask *= scanlineIntensity(scanPos.x, cb2.x, cb2.y, cb2.z, fwX);
    }

    // cb3.z = blurEnabled, cb3.w = blurIntensity
    // cb4.x = bloomEnabled, cb4.y = bloomIntensity
    // cb4.z = curvatureEnabled, cb4.w = curvatureIntensity
    // cb5.x = brightness, cb5.y = contrast, cb5.z = saturation, cb5.w = temperature
    // cb6.x = flickerEnabled, cb6.y = flickerIntensity, cb6.z = time, cb6.w = flickerRate
    // cb7.x = blackLevel, cb7.y = gamma, cb7.z = phosphorEnabled, cb7.w = phosphorIntensity
    // cb8.x = vhsEnabled, cb8.y = vhsIntensity, cb8.z = grainIntensity
    // cb9.x = tapeNoiseEnabled, cb9.y = tapeNoiseIntensity
    bool needTex = cb3.z >= 0.5 || cb4.x >= 0.5 || curved
                || cb6.x >= 0.5 || cb7.z >= 0.5
                || abs(cb5.x) > 0.001 || abs(cb5.y) > 0.001
                || abs(cb5.z) > 0.001 || abs(cb5.w) > 0.001
                || cb7.x > 0.001 || abs(cb7.y - 1.0) > 0.001
                || cb8.x >= 0.5 || cb8.z > 0.001 || cb9.x >= 0.5
                || cb9.z > 0.0 || megaBz || cb12.x >= 0.5 || cb13.x >= 0.5;
    if (!needTex) {
        if (cb9.z > 0.0) {
            float  r    = max(cb9.z * 0.10, 0.022);  // radius floored at fade width (gradient unchanged)
            float2 qv   = abs(uv - 0.5) - 0.5 + r;
            float  rSDF = length(max(qv, 0.0)) + min(max(qv.x, qv.y), 0.0) - r;
            float2 outN;
            if (qv.x > 0.0 && qv.y > 0.0) outN = normalize(qv);
            else if (qv.x > 0.0)          outN = float2(1.0, 0.0);
            else                           outN = float2(0.0, 1.0);
            float fadeW = length(float2(outN.x * 0.008, outN.y * 0.020));
            mask *= smoothstep(0.0, fadeW, -rSDF);
        }
        return float4(mask, mask, mask, 1.0);
    }

    float2 texel = 1.0 / float2(cb0.x, cb0.y);

    // NOTE: previous versions cropped 2% off the game's top/bottom here to hide
    // the game's intrinsic letterbox. That hardcoded crop also stretched games
    // WITHOUT letterbox, eating HUD pixels. Removed: the game is now sampled
    // unstretched at its original aspect. Letterbox detection is handled by the
    // reflection sampler (cb14 bounds) so reflection still uses real game pixels.
    float4 color = tex2D(backBuf, sampleUV);

    // Gaussian blur (must run before B/C/S/T — blur re-samples original texture)
    if (cb3.z >= 0.5) {
        float sigma = cb3.w * 2.0 + 0.0001;
        float4 blurred = float4(0, 0, 0, 0);
        float totalW = 0;
        for (int y = -2; y <= 2; y++) {
            for (int x = -2; x <= 2; x++) {
                float d2 = float(x*x + y*y);
                float w = exp(-d2 / (2.0 * sigma * sigma));
                blurred += tex2D(backBuf, sampleUV + float2(x, y) * texel * sigma) * w;
                totalW += w;
            }
        }
        color = blurred / totalW;
    }

    // Bloom (re-samples original texture)
    if (cb4.x >= 0.5) {
        float sigma = cb4.y * 3.0 + 0.5;
        float4 glow = float4(0, 0, 0, 0);
        float wTotal = 0;
        for (int by = -3; by <= 3; by++) {
            for (int bx = -3; bx <= 3; bx++) {
                float4 s = tex2D(backBuf, sampleUV + float2(bx, by) * texel * sigma);
                float lum = dot(s.rgb, float3(0.299, 0.587, 0.114));
                float bright = saturate((lum - 0.4) * 2.5);
                glow += s * bright;
                wTotal += bright;
            }
        }
        if (wTotal > 0.001) glow /= wTotal;
        color.rgb += glow.rgb * cb4.y * 0.6;
        color = saturate(color);
    }

    // Brightness/Contrast/Saturation/Temperature — applied after blur/bloom
    color.rgb *= 1.0 + cb5.x;
    color.rgb = (color.rgb - 0.5) * (1.0 + cb5.y) + 0.5;
    float luma9 = dot(color.rgb, float3(0.299, 0.587, 0.114));
    color.rgb = lerp(float3(luma9, luma9, luma9), color.rgb, 1.0 + cb5.z);
    color.r += cb5.w * 0.1;
    color.b -= cb5.w * 0.1;
    color = saturate(color);

    // Phosphor Glow — tight micro-halo around bright pixels (CRT phosphor persistence)
    if (cb7.z >= 0.5) {
        float4 glow = float4(0, 0, 0, 0);
        float wTotal = 0;
        for (int gy = -2; gy <= 2; gy++) {
            for (int gx = -2; gx <= 2; gx++) {
                float d2 = float(gx*gx + gy*gy);
                float w = exp(-d2 / 1.28);
                float4 s = tex2D(backBuf, sampleUV + float2(gx, gy) * texel);
                float lp = dot(s.rgb, float3(0.2126, 0.7152, 0.0722));
                float bright = saturate((lp - 0.3) * 3.0);
                glow += s * (w * bright);
                wTotal += w * bright;
            }
        }
        if (wTotal > 0.001)
            color.rgb += (glow.rgb / wTotal) * cb7.w * 0.3;
        color = saturate(color);
    }

    // Vignette
    if (curved) {
        float2 vigUV = sampleUV * 2.0 - 1.0;
        float vig = 1.0 - dot(vigUV, vigUV) * 0.3 * cb4.w;
        color.rgb *= saturate(vig);
    }

    // Sub-pixel dither — breaks 8-bit banding on vignette/gradients.
    // vpos is VPOS (screen pixel coords) — always aligned, never warped.
    float dither = frac(52.9829189 * frac(dot(vpos, float2(0.06711056, 0.00583715))));
    color.rgb += (dither - 0.5) * (1.0 / 255.0);

    // Apply scanline mask
    float4 output = color * mask;

    // Final grade: Black Level + Gamma — applied last as fine-tuning
    // Black Level: luminance-masked crush — only affects shadows below ~30% luma
    if (cb7.x > 0.001) {
        float lum9b = dot(output.rgb, float3(0.2126, 0.7152, 0.0722));
        float darkMask9b = 1.0 - smoothstep(0.0, 0.3, lum9b);
        output.rgb = max(output.rgb - cb7.x * darkMask9b, 0.0);
    }
    if (abs(cb7.y - 1.0) > 0.001)
        output.rgb = pow(max(output.rgb, 0.0001), 1.0 / cb7.y);

    // CRT Flicker — smooth sine LFO with vertical phase offset
    if (cb6.x >= 0.5) {
        float freq = 1.0 + cb6.w * 19.0;
        float phase = uv.y * 0.15;
        float flicker = sin(cb6.z * 6.28318 * freq + phase) * 0.05 * cb6.y;
        output.rgb *= 1.0 + flicker;
    }

    // VHS tape effect — realistic analog tape degradation + NTSC composite artifacts
    // cb8.x = vhsEnabled, cb8.y = vhsIntensity, cb8.z = grainIntensity
    if (cb8.x >= 0.5 && uv.y <= 0.96) {
        float inten = cb8.y;
        float t9    = cb6.z;
        // outsideGame: follows curvature + MegaBezel resize boundary exactly.
        // cb14: further clips to detected game content (handles emulator letterbox).
        float contentMask9 = (!outsideGame &&
                              uv.x >= cb14.x && uv.x <= cb14.z &&
                              uv.y >= cb14.y && uv.y <= cb14.w) ? 1.0 : 0.0;

        // — 1. LINE JITTER: sparse spike lines only (no global sinusoidal shift) —
        float lineIdx9 = floor(uv.y * 720.0);
        float sH9    = frac(sin(lineIdx9 * 127.1 + floor(t9 * 10.0) * 311.7) * 43758.5);
        float spike9 = (sH9 > 0.97) ? (sH9 - 0.97) / 0.03 * 0.016 - 0.008 : 0.0;
        float2 jUV9  = sampleUV + float2(spike9 * inten, 0.0);

        float4 s09   = tex2D(backBuf, jUV9);

        // — 3. NTSC DOT CRAWL: animated color shimmer on chroma edges —
        float luma09  = dot(s09.rgb, float3(0.299, 0.587, 0.114));
        float chrMag9 = saturate(length(s09.rgb - luma09) * 2.5);
        float dotPhi9 = (uv.x * 240.0 + floor(t9 * 29.97) * 0.5) * 3.14159265;
        output.r += sin(dotPhi9)         * inten * 0.008 * chrMag9 * contentMask9;
        output.b += sin(dotPhi9 + 1.047) * inten * 0.006 * chrMag9 * contentMask9;

        // — 4. LUMA NOISE: horizontal tape hiss streaks —
        float ny9     = floor(uv.y * 200.0);
        float nx9     = floor(uv.x * 15.0);
        float nt9     = floor(t9 * 25.0);
        float streak9 = frac(sin(dot(float2(nx9 + ny9 * 200.0, nt9), float2(127.1, 311.7))) * 43758.5) - 0.5;
        output.rgb   += streak9 * inten * 0.022 * contentMask9;

        // — 5. HEAD-SWITCHING BAND: bottom ~3% of screen, mechanical artifact —
        float headZone9 = smoothstep(0.97, 1.00, uv.y) * contentMask9;
        float headH9    = frac(sin(floor(uv.y * 300.0) * 127.1 + floor(t9 * 30.0) * 311.7) * 43758.5);
        float headOff9  = (headH9 - 0.5) * inten * 0.030;
        float4 headS9   = tex2D(backBuf, jUV9 + float2(headOff9, 0.0));
        output.rgb      = lerp(output.rgb, headS9.rgb * mask + headH9 * inten * 0.35, headZone9);

        // — 6. COLOR GRADING: desaturation + luma lift + warm shadows —
        float vLuma9 = dot(output.rgb, float3(0.299, 0.587, 0.114));
        output.rgb   = lerp(output.rgb, float3(vLuma9, vLuma9, vLuma9), inten * 0.25 * contentMask9);
        output.rgb  *= lerp(1.0, 1.06, inten * contentMask9);          // analog luma lift
        output.r    += inten * 0.020 * (1.0 - vLuma9) * contentMask9;  // warm shadows
        output.b    -= inten * 0.014 * (1.0 - vLuma9) * contentMask9;
        output       = saturate(output);
    }

    // Film Grain — true per-pixel noise, hash without sine (no periodic banding)
    if (cb8.z > 0.001) {
        float grainMask9 = (!outsideGame &&
                            uv.x >= cb14.x && uv.x <= cb14.z &&
                            uv.y >= cb14.y && uv.y <= cb14.w) ? 1.0 : 0.0;
        float lum9g  = dot(output.rgb, float3(0.299, 0.587, 0.114));
        float3 p39   = frac(float3(vpos, floor(cb6.z * 24.0) + 1.0)
                          * float3(0.1031, 0.1030, 0.0973));
        p39         += dot(p39, p39.yzx + 33.33);
        float grain9 = frac((p39.x + p39.y) * p39.z) * 2.0 - 1.0;
        float amp9   = 1.0 - (2.0 * lum9g - 1.0) * (2.0 * lum9g - 1.0);
        output.rgb  += grain9 * cb8.z * 0.18 * amp9 * grainMask9;
        output       = saturate(output);
    }

    // Tape Noise — libretro-style analog tape interference spikes
    // cb9.x = tapeNoiseEnabled, cb9.y = tapeNoiseIntensity
    if (cb9.x >= 0.5) {
        float tnMask9 = (!outsideGame &&
                         uv.x >= cb14.x && uv.x <= cb14.z &&
                         uv.y >= cb14.y && uv.y <= cb14.w) ? 1.0 : 0.0;
        float fc9    = cb6.z * 24.0;
        float2 tnUV9 = uv * cb0.y * 4.0;
        float col9   = tnNn9(tnUV9, fc9);
        output.rgb  += clamp(float3(col9, col9, col9), 0.0, 0.5) * cb9.y * tnMask9;
        output       = saturate(output);
    }

    if (cb9.z > 0.0) {
        float  r    = max(cb9.z * 0.10, 0.022);  // radius floored at fade width (gradient unchanged)
        float2 qv   = abs(sampleUV - 0.5) - 0.5 + r;
        float  rSDF = length(max(qv, 0.0)) + min(max(qv.x, qv.y), 0.0) - r;
        float2 outN;
        if (qv.x > 0.0 && qv.y > 0.0) outN = normalize(qv);
        else if (qv.x > 0.0)          outN = float2(1.0, 0.0);
        else                           outN = float2(0.0, 1.0);
        float fadeW = length(float2(outN.x * 0.008, outN.y * 0.020));
        output.rgb *= smoothstep(0.0, fadeW, -rSDF);
    }

    // REFL. RADIUS: hard-mask the rounded-off game corner to black (bezel covers).
    // mbSDF = rounded-rect SDF with radius mbR (= cb11.x = REFL. RADIUS); >0 only
    // in the corner triangle, 0 on straight edges. No-op when REFL. RADIUS=0.
    if (megaBz && mbSDF > 0.0) output.rgb = float3(0.0, 0.0, 0.0);

    // Bezel PNG overlay (inside-game pixels). Standard alpha "over" composite
    // so the bezel art layers ON TOP of the rendered game frame. Reflection
    // pixels in the border zone already returned earlier with bezel composited.
    if (cb12.x >= 0.5) {
        float4 bz = tex2D(bezelTex, uv);
        float a = bz.a * cb12.y;
        output.rgb = output.rgb * (1.0 - a) + bz.rgb * a;
    }

    return output;
}
)";

// ── Combined D3D9 PS source (D3D9_PS_SRC + D3D9_PS_SRC2) — built once ────
// Mirrors GetPSSrc() for D3D11. Each part is a separate static const char*
// declaration so neither exceeds the MSVC raw-string-literal size limit.
static char* g_D3D9_PS_SRC_FULL = nullptr;
static const char* GetD3D9PSSrc() {
    if (!g_D3D9_PS_SRC_FULL) {
        size_t l1 = strlen(D3D9_PS_SRC), l2 = strlen(D3D9_PS_SRC2);
        g_D3D9_PS_SRC_FULL = (char*)malloc(l1 + l2 + 1);
        if (g_D3D9_PS_SRC_FULL) {
            memcpy(g_D3D9_PS_SRC_FULL,      D3D9_PS_SRC,  l1);
            memcpy(g_D3D9_PS_SRC_FULL + l1, D3D9_PS_SRC2, l2 + 1);
        } else {
            return D3D9_PS_SRC;
        }
    }
    return g_D3D9_PS_SRC_FULL;
}

// ── D3D9 bezel PNG loader ────────────────────────────────────────────────
// Mirror of LoadBezelTexture (D3D11). Decodes the PNG via WIC into BGRA bytes
// (matches D3DFMT_A8R8G8B8 byte order on little-endian) and uploads to a
// D3DPOOL_MANAGED IDirect3DTexture9. Pool=MANAGED so the texture survives a
// device Reset transparently — matches the lifetime of the captured bezel
// path (released only when the path changes or the hook is unloaded).
static void ReleaseBezelTextureD3D9() {
    if (g_D3D9BezelTex) { g_D3D9BezelTex->Release(); g_D3D9BezelTex = nullptr; }
    g_D3D9BezelPathCached[0] = 0;
}

// Detect the in-game content bounds (excluding black letterbox/pillarbox bars).
// Downscales the current backbuffer to 64x36 in SYSTEMMEM, scans rows/columns
// from each edge, returns the first row/column with enough non-black pixels.
// Bounds are smoothed temporally to avoid jitter on dark scenes.
static void DetectGameBoundsD3D9(IDirect3DDevice9* dev) {
    if (!dev) return;
    if (!g_D3D9DetectRT) {
        if (FAILED(dev->CreateRenderTarget(DETECT_W, DETECT_H, D3DFMT_A8R8G8B8,
            D3DMULTISAMPLE_NONE, 0, FALSE, &g_D3D9DetectRT, nullptr))) return;
    }
    if (!g_D3D9DetectSys) {
        if (FAILED(dev->CreateOffscreenPlainSurface(DETECT_W, DETECT_H, D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM, &g_D3D9DetectSys, nullptr))) return;
    }

    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    HRESULT hr = dev->StretchRect(bb, nullptr, g_D3D9DetectRT, nullptr, D3DTEXF_LINEAR);
    bb->Release();
    if (FAILED(hr)) return;
    if (FAILED(dev->GetRenderTargetData(g_D3D9DetectRT, g_D3D9DetectSys))) return;

    D3DLOCKED_RECT lr;
    if (FAILED(g_D3D9DetectSys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;

    BYTE* px    = (BYTE*)lr.pBits;
    int   pitch = lr.Pitch;
    const int LUMA_THRESHOLD = 24;          // sum of R+G+B > 24 (~3% intensity)
    const int MIN_CONTENT    = (int)(DETECT_W * 0.05f); // 5% of pixels in row must be non-black

    auto rowHasContent = [&](int y) -> bool {
        BYTE* row = px + y * pitch;
        int n = 0;
        for (UINT x = 0; x < DETECT_W; x++) {
            int s = row[x*4 + 0] + row[x*4 + 1] + row[x*4 + 2];
            if (s > LUMA_THRESHOLD) n++;
        }
        return n >= MIN_CONTENT;
    };
    auto colHasContent = [&](int x) -> bool {
        int n = 0;
        for (UINT y = 0; y < DETECT_H; y++) {
            BYTE* p = px + y * pitch + x * 4;
            int s = p[0] + p[1] + p[2];
            if (s > LUMA_THRESHOLD) n++;
        }
        return n >= (int)(DETECT_H * 0.05f);
    };

    int top = 0;                    while (top < (int)DETECT_H && !rowHasContent(top)) top++;
    int bot = (int)DETECT_H - 1;    while (bot > top && !rowHasContent(bot)) bot--;
    int lft = 0;                    while (lft < (int)DETECT_W && !colHasContent(lft)) lft++;
    int rgt = (int)DETECT_W - 1;    while (rgt > lft && !colHasContent(rgt)) rgt--;

    g_D3D9DetectSys->UnlockRect();

    // Whole screen is black/near-black — keep previous bounds (avoid resetting on fade-outs)
    if (bot <= top || rgt <= lft) return;

    float newL = (float)lft       / (float)DETECT_W;
    float newT = (float)top       / (float)DETECT_H;
    float newR = (float)(rgt + 1) / (float)DETECT_W;
    float newB = (float)(bot + 1) / (float)DETECT_H;

    // Temporal smoothing: 30% lerp toward new value per detection
    g_GameBoundsL = g_GameBoundsL * 0.7f + newL * 0.3f;
    g_GameBoundsT = g_GameBoundsT * 0.7f + newT * 0.3f;
    g_GameBoundsR = g_GameBoundsR * 0.7f + newR * 0.3f;
    g_GameBoundsB = g_GameBoundsB * 0.7f + newB * 0.3f;
}

static bool LoadBezelTextureD3D9(IDirect3DDevice9* dev, const wchar_t* path) {
    Log("[BEZEL-D3D9] ENTER dev=%p path=%p path[0]=%d", dev, path, (path ? (int)path[0] : -1));
    ReleaseBezelTextureD3D9();
    if (!dev || !path || !path[0]) {
        Log("[BEZEL-D3D9] LoadBezelTextureD3D9 skipped (empty)");
        return false;
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IWICImagingFactory*    wic       = nullptr;
    IWICBitmapDecoder*     decoder   = nullptr;
    IWICBitmapFrameDecode* frame     = nullptr;
    IWICFormatConverter*   converter = nullptr;
    BYTE*                  pixels    = nullptr;
    bool ok = false;
    UINT w = 0, h = 0;
    UINT stride = 0, imgSize = 0;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(hr)) { Log("[BEZEL-D3D9] FAIL step1 CoCreateInstance hr=0x%08X", hr); goto cleanup; }
    Log("[BEZEL-D3D9] step1 OK wic=%p", wic);

    hr = wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) { Log("[BEZEL-D3D9] FAIL step2 decode '%ls' hr=0x%08X", path, hr); goto cleanup; }
    Log("[BEZEL-D3D9] step2 OK decoder=%p", decoder);

    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) { Log("[BEZEL-D3D9] FAIL step3 GetFrame hr=0x%08X", hr); goto cleanup; }
    hr = wic->CreateFormatConverter(&converter);
    if (FAILED(hr)) { Log("[BEZEL-D3D9] FAIL step4 CreateFormatConverter hr=0x%08X", hr); goto cleanup; }
    // D3DFMT_A8R8G8B8 in memory is BGRA byte-order (little-endian DWORDs are 0xAARRGGBB).
    hr = converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) { Log("[BEZEL-D3D9] FAIL step5 Initialize hr=0x%08X", hr); goto cleanup; }

    {
        UINT srcW = 0, srcH = 0;
        converter->GetSize(&srcW, &srcH);
        if (srcW == 0 || srcH == 0) { Log("[BEZEL-D3D9] FAIL zero src size"); goto cleanup; }

        // Query device caps to honor MaxTextureWidth/Height + power-of-2 requirements.
        D3DCAPS9 caps = {};
        dev->GetDeviceCaps(&caps);
        UINT maxW = caps.MaxTextureWidth  ? caps.MaxTextureWidth  : 2048;
        UINT maxH = caps.MaxTextureHeight ? caps.MaxTextureHeight : 2048;
        bool needPow2 = (caps.TextureCaps & D3DPTEXTURECAPS_POW2)
                      && !(caps.TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL);

        UINT tgtW = srcW, tgtH = srcH;
        if (tgtW > maxW) tgtW = maxW;
        if (tgtH > maxH) tgtH = maxH;
        if (needPow2) {
            UINT p = 1; while (p < tgtW && p < maxW) p <<= 1; tgtW = (p > maxW) ? maxW : p;
            UINT q = 1; while (q < tgtH && q < maxH) q <<= 1; tgtH = (q > maxH) ? maxH : q;
        }

        if (tgtW != srcW || tgtH != srcH) {
            IWICBitmapScaler* scaler = nullptr;
            HRESULT hsc = wic->CreateBitmapScaler(&scaler);
            if (SUCCEEDED(hsc) && scaler) {
                hsc = scaler->Initialize(converter, tgtW, tgtH, WICBitmapInterpolationModeFant);
                if (SUCCEEDED(hsc)) {
                    Log("[BEZEL-D3D9] scaling %ux%u -> %ux%u (caps max=%ux%u pow2=%d)",
                        srcW, srcH, tgtW, tgtH, maxW, maxH, needPow2 ? 1 : 0);
                    w = tgtW; h = tgtH;
                    stride  = w * 4;
                    imgSize = stride * h;
                    pixels  = new BYTE[imgSize];
                    hr = scaler->CopyPixels(nullptr, stride, imgSize, pixels);
                    scaler->Release();
                    if (FAILED(hr)) { Log("[BEZEL-D3D9] FAIL scaler->CopyPixels hr=0x%08X", hr); goto cleanup; }
                } else {
                    scaler->Release();
                    Log("[BEZEL-D3D9] FAIL scaler->Initialize hr=0x%08X", hsc); goto cleanup;
                }
            } else {
                Log("[BEZEL-D3D9] FAIL CreateBitmapScaler hr=0x%08X", hsc); goto cleanup;
            }
        } else {
            w = srcW; h = srcH;
            stride  = w * 4;
            imgSize = stride * h;
            pixels  = new BYTE[imgSize];
            hr = converter->CopyPixels(nullptr, stride, imgSize, pixels);
            if (FAILED(hr)) { Log("[BEZEL-D3D9] FAIL step7 CopyPixels hr=0x%08X", hr); goto cleanup; }
        }
        Log("[BEZEL-D3D9] step7 OK pixels copied (final %ux%u)", w, h);
    }

    {
        // D3D9Ex devices (used by Cyber Shadow et al.) prohibit D3DPOOL_MANAGED.
        // Use a SYSTEMMEM staging texture (lockable) then UpdateTexture into a
        // DEFAULT-pool texture (sampleable). The DEFAULT texture is lost on
        // device Reset — HookedD3D9Reset releases it via D3D9ReleaseOurResources.
        IDirect3DTexture9* staging = nullptr;
        HRESULT hr2 = dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM, &staging, nullptr);
        if (FAILED(hr2) || !staging) {
            Log("[BEZEL-D3D9] FAIL step8a SYSTEMMEM staging hr=0x%08X w=%u h=%u", hr2, w, h);
            goto cleanup;
        }

        D3DLOCKED_RECT lr;
        HRESULT hr3 = staging->LockRect(0, &lr, nullptr, 0);
        if (FAILED(hr3)) {
            Log("[BEZEL-D3D9] FAIL step8b staging LockRect hr=0x%08X", hr3);
            staging->Release();
            goto cleanup;
        }
        {
            BYTE* src = pixels;
            BYTE* dst = (BYTE*)lr.pBits;
            for (UINT y = 0; y < h; y++) {
                memcpy(dst, src, stride);
                src += stride;
                dst += lr.Pitch;
            }
        }
        staging->UnlockRect(0);

        HRESULT hr4 = dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT, &g_D3D9BezelTex, nullptr);
        if (FAILED(hr4) || !g_D3D9BezelTex) {
            Log("[BEZEL-D3D9] FAIL step8c DEFAULT tex hr=0x%08X", hr4);
            staging->Release();
            goto cleanup;
        }

        HRESULT hr5 = dev->UpdateTexture(staging, g_D3D9BezelTex);
        staging->Release();
        if (FAILED(hr5)) {
            Log("[BEZEL-D3D9] FAIL step8d UpdateTexture hr=0x%08X", hr5);
            g_D3D9BezelTex->Release(); g_D3D9BezelTex = nullptr;
            goto cleanup;
        }
        Log("[BEZEL-D3D9] step8 OK (staging+default) tex=%p", g_D3D9BezelTex);
    }

    wcscpy_s(g_D3D9BezelPathCached, 260, path);
    Log("[BEZEL-D3D9] Loaded '%ls' (%ux%u) into D3D9 texture", path, w, h);
    ok = true;

cleanup:
    if (pixels)    delete[] pixels;
    if (converter) converter->Release();
    if (frame)     frame->Release();
    if (decoder)   decoder->Release();
    if (wic)       wic->Release();
    if (!ok)       ReleaseBezelTextureD3D9();
    return ok;
}

// D3D9 resources initialized lazily on first EndScene
static bool InitD3D9Resources(IDirect3DDevice9* dev) {
    if (g_D3D9Inited) return true;

    Log("[D3D9] InitD3D9Resources — first EndScene call, compiling ps_3_0 shader");

    // Log device caps
    D3DCAPS9 caps = {};
    if (SUCCEEDED(dev->GetDeviceCaps(&caps))) {
        Log("[D3D9] DeviceCaps: PS version %u.%u, VS version %u.%u",
            D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion),
            D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion),
            D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion),
            D3DSHADER_VERSION_MINOR(caps.VertexShaderVersion));
    }

    // Compile pixel shader (ps_3_0 for VPOS support)
    // MMF2 optimisation: reuse cached bytecode blob across Resets instead of
    // recompiling from source every time (~2.5s → ~1ms per Reset).
    ID3DBlob* psBlob = nullptr;
    if (g_IsMMF2 && g_D3D9PSCachedBlob) {
        // Reuse previously compiled bytecode — skip D3DCompile entirely
        psBlob = g_D3D9PSCachedBlob;
        psBlob->AddRef();   // balance the Release() below
        Log("[D3D9] MMF2 shader cache HIT — skipping recompilation");
    } else {
        ID3DBlob* errBlob = nullptr;
        const char* d3d9PSFull = GetD3D9PSSrc();
        HRESULT hr = D3DCompile(d3d9PSFull, strlen(d3d9PSFull), "d3d9ps", nullptr, nullptr, "main", "ps_3_0", 0, 0, &psBlob, &errBlob);
        if (FAILED(hr) || !psBlob) {
            LogShaderError("D3D9 PS ps_3_0", errBlob);
            if (errBlob) errBlob->Release();
            return false;
        }
        if (errBlob) errBlob->Release();

        // MMF2: store bytecode for reuse on subsequent Resets
        if (g_IsMMF2) {
            g_D3D9PSCachedBlob = psBlob;
            g_D3D9PSCachedBlob->AddRef();   // keep alive beyond the Release() below
            Log("[D3D9] MMF2 shader cache STORED — will reuse on next Reset");
        }
    }

    HRESULT hr = dev->CreatePixelShader((const DWORD*)psBlob->GetBufferPointer(), &g_D3D9PS);
    psBlob->Release();
    if (FAILED(hr)) { Log("[D3D9] FAIL: CreatePixelShader hr=0x%08X", hr); return false; }

    OpenSharedMem();
    g_D3D9Device = dev;
    g_D3D9Inited = true;
    Log("[D3D9] Resources initialized OK — scanlines active");
    return true;
}

static bool g_D3D9FirstFrame = true;

// ── D3D9 frame counter and diagnostic state ──
static int  g_D3D9FrameCount  = 0;
static int  g_D3D9PresentCalls = 0;   // total Present hook calls (for periodic log)
static int  g_D3D9EndSceneCalls = 0;  // total EndScene hook calls (for periodic log)

// Samples the center pixel of a surface by copying it to a SYSTEMMEM surface and locking it.
// Returns true on success and fills out_r/g/b/a (0-255).
static bool D3D9SamplePixel(IDirect3DDevice9* dev, IDirect3DSurface9* src,
    UINT* outW, UINT* outH, UINT* outFmt,
    BYTE* outR, BYTE* outG, BYTE* outB, BYTE* outA) {
    if (!src) return false;
    D3DSURFACE_DESC desc;
    if (FAILED(src->GetDesc(&desc))) return false;
    if (outW) *outW = desc.Width;
    if (outH) *outH = desc.Height;
    if (outFmt) *outFmt = (UINT)desc.Format;

    IDirect3DSurface9* sys = nullptr;
    HRESULT hrC = dev->CreateOffscreenPlainSurface(desc.Width, desc.Height,
        desc.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    if (FAILED(hrC) || !sys) return false;

    HRESULT hrG = dev->GetRenderTargetData(src, sys);
    if (FAILED(hrG)) { sys->Release(); return false; }

    D3DLOCKED_RECT lr;
    if (FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) { sys->Release(); return false; }

    UINT cx = desc.Width / 2;
    UINT cy = desc.Height / 2;
    BYTE* row = (BYTE*)lr.pBits + cy * lr.Pitch;
    // Assume X8R8G8B8 / A8R8G8B8 layout (most common for GMS1/backbuffer)
    DWORD px = ((DWORD*)row)[cx];
    if (outA) *outA = (BYTE)((px >> 24) & 0xFF);
    if (outR) *outR = (BYTE)((px >> 16) & 0xFF);
    if (outG) *outG = (BYTE)((px >> 8)  & 0xFF);
    if (outB) *outB = (BYTE)(px & 0xFF);

    sys->UnlockRect();
    sys->Release();
    return true;
}

// Runs at specific Present-frame milestones. Enumerates swap chains, samples
// pixels from RT0 / BB / the capture texture, logs everything so we can see
// where the actual game pixels live and whether our StretchRect captured content.
static void D3D9Diagnose(IDirect3DDevice9* dev, int frame) {
    Log("[D3D9] ===== DIAG @ Present frame %d =====", frame);

    // Swap chain count
    UINT scCount = dev->GetNumberOfSwapChains();
    Log("[D3D9] DIAG swap chain count = %u", scCount);

    // Primary swap chain info
    IDirect3DSwapChain9* sc0 = nullptr;
    if (SUCCEEDED(dev->GetSwapChain(0, &sc0)) && sc0) {
        D3DPRESENT_PARAMETERS pp = {};
        if (SUCCEEDED(sc0->GetPresentParameters(&pp))) {
            Log("[D3D9] DIAG sc0 BB=%ux%u fmt=%u count=%u swapEffect=%u multiSample=%u windowed=%d",
                pp.BackBufferWidth, pp.BackBufferHeight, (unsigned)pp.BackBufferFormat,
                pp.BackBufferCount, (unsigned)pp.SwapEffect, (unsigned)pp.MultiSampleType,
                (int)pp.Windowed);
        }

        // Sample every backbuffer of sc0
        UINT bbCount = pp.BackBufferCount ? pp.BackBufferCount : 1;
        for (UINT i = 0; i < bbCount; i++) {
            IDirect3DSurface9* bb = nullptr;
            if (SUCCEEDED(sc0->GetBackBuffer(i, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
                UINT w = 0, h = 0, fmt = 0;
                BYTE r = 0, g = 0, b = 0, a = 0;
                bool ok = D3D9SamplePixel(dev, bb, &w, &h, &fmt, &r, &g, &b, &a);
                Log("[D3D9] DIAG sc0 BB[%u]: %s size=%ux%u fmt=%u center=(R=%u G=%u B=%u A=%u)",
                    i, ok ? "OK" : "readback-FAIL", w, h, fmt, r, g, b, a);
                bb->Release();
            } else {
                Log("[D3D9] DIAG sc0 BB[%u]: GetBackBuffer failed", i);
            }
        }
        sc0->Release();
    } else {
        Log("[D3D9] DIAG GetSwapChain(0) failed");
    }

    // Also sample GetRenderTarget(0) (whatever is currently bound)
    IDirect3DSurface9* rt0 = nullptr;
    if (SUCCEEDED(dev->GetRenderTarget(0, &rt0)) && rt0) {
        UINT w = 0, h = 0, fmt = 0;
        BYTE r = 0, g = 0, b = 0, a = 0;
        bool ok = D3D9SamplePixel(dev, rt0, &w, &h, &fmt, &r, &g, &b, &a);
        Log("[D3D9] DIAG RT0 (current): %s size=%ux%u fmt=%u center=(R=%u G=%u B=%u A=%u)",
            ok ? "OK" : "readback-FAIL", w, h, fmt, r, g, b, a);
        rt0->Release();
    }

    // Enumerate additional swap chains
    for (UINT i = 1; i < scCount; i++) {
        IDirect3DSwapChain9* sc = nullptr;
        if (SUCCEEDED(dev->GetSwapChain(i, &sc)) && sc) {
            IDirect3DSurface9* bb = nullptr;
            if (SUCCEEDED(sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
                UINT w = 0, h = 0, fmt = 0;
                BYTE r = 0, g = 0, b = 0, a = 0;
                bool ok = D3D9SamplePixel(dev, bb, &w, &h, &fmt, &r, &g, &b, &a);
                Log("[D3D9] DIAG sc%u BB[0]: %s size=%ux%u fmt=%u center=(R=%u G=%u B=%u A=%u)",
                    i, ok ? "OK" : "readback-FAIL", w, h, fmt, r, g, b, a);
                bb->Release();
            }
            sc->Release();
        }
    }

    // Sample the capture texture (g_D3D9BBCopy) to verify StretchRect populated it.
    // If this sample shows non-zero content but BB/RT0 are black, our StretchRect
    // is capturing from a surface that's populated at a different time than we sampled.
    if (g_D3D9BBCopy) {
        IDirect3DSurface9* capSurf = nullptr;
        if (SUCCEEDED(g_D3D9BBCopy->GetSurfaceLevel(0, &capSurf)) && capSurf) {
            UINT w = 0, h = 0, fmt = 0;
            BYTE r = 0, g = 0, b = 0, a = 0;
            bool ok = D3D9SamplePixel(dev, capSurf, &w, &h, &fmt, &r, &g, &b, &a);
            Log("[D3D9] DIAG capture texture: %s size=%ux%u fmt=%u center=(R=%u G=%u B=%u A=%u) hasCapture=%d",
                ok ? "OK" : "readback-FAIL", w, h, fmt, r, g, b, a, (int)g_D3D9HasCapture);
            capSurf->Release();
        }
    } else {
        Log("[D3D9] DIAG capture texture: NULL");
    }
    Log("[D3D9] ===== DIAG end =====");
}

// ── D3D9 per-process kill-switch ────────────────────────────────────────
// Some D3D9 games (notably Freedom Planet / GameMaker Studio 1) never expose
// their presented frame on the default swap chain backbuffer at EndScene/Present
// time — the engine composites its "application surface" through a code path
// that bypasses what our hook can observe. For such games we cannot safely
// redirect drawing to the backbuffer without producing a black/blob image.
// Instead we detect them by process name and leave the D3D9 pipeline completely
// untouched (no capture, no effect pass), so the game looks exactly as before
// the hook was injected. All other D3D9/D3D11 games are unaffected.
static void D3D9CheckProcessBlocked() {
    if (g_D3D9ProcChecked) return;
    g_D3D9ProcChecked = true;

    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return;

    // Extract base name and lowercase it
    wchar_t* base = wcsrchr(path, L'\\');
    base = base ? (base + 1) : path;
    wchar_t name[MAX_PATH] = {};
    size_t i = 0;
    for (; base[i] && i < MAX_PATH - 1; i++) {
        wchar_t c = base[i];
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c + 32);
        name[i] = c;
    }
    name[i] = 0;

    // Known-incompatible D3D9 process names (lowercase, substring match).
    // GMS1 "application surface" engines compose through a path our hook cannot
    // intercept — we let them render unmodified.
    static const wchar_t* kBlocked[] = {
        L"freedomplanet",
        L"freedom planet",
    };
    for (size_t k = 0; k < sizeof(kBlocked)/sizeof(kBlocked[0]); k++) {
        if (wcsstr(name, kBlocked[k])) {
            g_D3D9ProcBlocked = true;
            Log("[D3D9] Process '%ls' is on the D3D9 incompatible list — hook will leave the game untouched", name);
            return;
        }
    }
    Log("[D3D9] Process '%ls' — D3D9 effects enabled", name);
}

// ── D3D9CaptureScene — called from HookedD3D9EndScene AFTER the real EndScene ──
// At this point the game has committed all its drawing to its render target and
// (for engines like GMS1) its internal composite to the backbuffer is done.
// We capture the default backbuffer — which is what will actually be presented —
// into g_D3D9BBCopy for use by the Present-time effect pass.
//
// Adobe AIR exception: AIR creates a tiny (e.g. 16x16) dummy swap chain and renders
// the actual game content to a separate render target (RT0).  When the BB is smaller
// than 64×64 we fall back to capturing RT0, which holds the real framebuffer.
static void D3D9CaptureScene(IDirect3DDevice9* dev) {
    if (!dev) return;
    // Reset per-frame: must be set to true each frame by a successful StretchRect.
    g_D3D9HasCapture = false;

    IDirect3DSurface9* src = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &src)) || !src) return;

    D3DSURFACE_DESC desc;
    if (FAILED(src->GetDesc(&desc))) { src->Release(); return; }

    // If the swap chain BB is a tiny dummy (Adobe AIR pattern), try RT0 instead.
    if (desc.Width < 64 || desc.Height < 64) {
        IDirect3DSurface9* rt0 = nullptr;
        if (SUCCEEDED(dev->GetRenderTarget(0, &rt0)) && rt0) {
            D3DSURFACE_DESC rtDesc;
            if (SUCCEEDED(rt0->GetDesc(&rtDesc)) && rtDesc.Width >= 64 && rtDesc.Height >= 64) {
                static bool s_loggedRTFallback = false;
                if (!s_loggedRTFallback) {
                    s_loggedRTFallback = true;
                    Log("[D3D9] Tiny BB (%ux%u) — falling back to RT0 (%ux%u) for capture",
                        desc.Width, desc.Height, rtDesc.Width, rtDesc.Height);
                }
                src->Release();
                src  = rt0;
                desc = rtDesc;
            } else {
                rt0->Release();
            }
        }
    }

    if (!g_D3D9BBCopy || g_D3D9LastW != desc.Width || g_D3D9LastH != desc.Height) {
        if (g_D3D9BBCopy) { g_D3D9BBCopy->Release(); g_D3D9BBCopy = nullptr; }
        HRESULT hrC = dev->CreateTexture(desc.Width, desc.Height, 1,
            D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &g_D3D9BBCopy, nullptr);
        if (SUCCEEDED(hrC)) {
            g_D3D9LastW = desc.Width;
            g_D3D9LastH = desc.Height;
            Log("[D3D9] Present capture texture created: %ux%u fmt=%u",
                desc.Width, desc.Height, (unsigned)desc.Format);
        } else {
            Log("[D3D9] Present capture texture creation FAILED: 0x%08X (fmt=%u %ux%u)",
                (unsigned)hrC, (unsigned)desc.Format, desc.Width, desc.Height);
            src->Release();
            return;
        }
    }

    if (g_D3D9BBCopy) {
        IDirect3DSurface9* dstSurf = nullptr;
        if (SUCCEEDED(g_D3D9BBCopy->GetSurfaceLevel(0, &dstSurf)) && dstSurf) {
            HRESULT hrSR = dev->StretchRect(src, nullptr, dstSurf, nullptr, D3DTEXF_NONE);
            if (SUCCEEDED(hrSR)) {
                g_D3D9HasCapture = true;
            } else {
                static bool s_loggedFail = false;
                if (!s_loggedFail) {
                    s_loggedFail = true;
                    Log("[D3D9] Present StretchRect FAILED: 0x%08X (bb=%ux%u fmt=%u ms=%u)",
                        (unsigned)hrSR, desc.Width, desc.Height,
                        (unsigned)desc.Format, (unsigned)desc.MultiSampleType);
                }
            }
            dstSurf->Release();
        }
    }

    src->Release();
}

static void ApplyD3D9Scanlines(IDirect3DDevice9* dev) {
    if (!g_Shared) { OpenSharedMem(); }
    if (!g_Shared) {
        if (g_D3D9FirstFrame) { Log("[D3D9] ApplyD3D9Scanlines: no shared memory"); g_D3D9FirstFrame = false; }
        return;
    }
    if (!g_Shared->active) {
        if (g_D3D9FirstFrame) { Log("[D3D9] ApplyD3D9Scanlines: active=0 (disabled from S4W UI)"); g_D3D9FirstFrame = false; }
        return;
    }
    if (!g_D3D9PS) return;

    if (g_D3D9FirstFrame) {
        D3DVIEWPORT9 dbgVp;
        dev->GetViewport(&dbgVp);
        Log("[D3D9] First scanline draw: viewport=%ux%u hEnabled=%d hThick=%.1f hGap=%.1f hOpacity=%.2f ",
            dbgVp.Width, dbgVp.Height,
            g_Shared->hEnabled, g_Shared->hThickness, g_Shared->hGap, g_Shared->hOpacity);
        g_D3D9FirstFrame = false;
    }

    // Save ALL D3D9 device state via a state block — this is the ONLY reliable way
    // to avoid polluting the game's state (shader constants, stream sources, vertex
    // declaration, texture stages, samplers, viewport, etc). Manual save/restore
    // invariably misses something and causes the game to render black on the next frame.
    IDirect3DStateBlock9* prevState = nullptr;
    HRESULT hrSB = dev->CreateStateBlock(D3DSBT_ALL, &prevState);
    if (FAILED(hrSB) || !prevState) {
        static bool s_loggedSBFail = false;
        if (!s_loggedSBFail) {
            s_loggedSBFail = true;
            Log("[D3D9] ApplyD3D9Scanlines: CreateStateBlock failed 0x%08X — skipping draw to avoid state leak", (unsigned)hrSB);
        }
        return;
    }
    IDirect3DSurface9* prevRT0 = nullptr;
    dev->GetRenderTarget(0, &prevRT0);

    // Set pixel shader constants — INJECTION MODE: full viewport
    D3DVIEWPORT9 vpCheck;
    dev->GetViewport(&vpCheck);
    float d3d9VpW = (float)vpCheck.Width;
    float d3d9VpH = (float)vpCheck.Height;
    float constants[16];
    constants[0]  = d3d9VpW;                      // screenW = viewport width
    constants[1]  = d3d9VpH;                      // screenH = viewport height
    constants[2]  = g_Shared->hThickness;
    constants[3]  = g_Shared->hGap;
    constants[4]  = g_Shared->hOpacity;
    constants[5]  = 0.0f;                          // hStartX = 0 (full viewport)
    constants[6]  = d3d9VpW;                       // hWidth = full viewport width
    constants[7]  = (float)g_Shared->hEnabled;
    constants[8]  = g_Shared->vThickness;
    constants[9]  = g_Shared->vGap;
    constants[10] = g_Shared->vOpacity;
    constants[11] = 0.0f;                          // vStartY = 0 (full viewport)
    constants[12] = d3d9VpH;                       // vHeight = full viewport height
    constants[13] = (float)g_Shared->vEnabled;
    constants[14] = g_Shared->blurEnabled;
    constants[15] = g_Shared->blurIntensity;
    float constants2[4];
    constants2[0] = g_Shared->bloomEnabled;
    constants2[1] = g_Shared->bloomIntensity;
    constants2[2] = g_Shared->curvatureEnabled;
    constants2[3] = g_Shared->curvatureIntensity;
    float constants3[4];
    constants3[0] = g_Shared->brightness;
    constants3[1] = g_Shared->contrast;
    constants3[2] = g_Shared->saturation;
    constants3[3] = g_Shared->temperature;
    float constants4[4];
    constants4[0] = g_Shared->flickerEnabled;
    constants4[1] = g_Shared->flickerIntensity;
    constants4[2] = GetTimeSeconds();
    constants4[3] = g_Shared->flickerRate;
    float constants5[4];
    constants5[0] = g_Shared->blackLevel;
    constants5[1] = g_Shared->gamma;
    constants5[2] = g_Shared->phosphorEnabled;
    constants5[3] = g_Shared->phosphorIntensity;
    float constants6[4];
    constants6[0] = g_Shared->vhsEnabled;
    constants6[1] = g_Shared->vhsIntensity;
    constants6[2] = g_Shared->grainIntensity;
    constants6[3] = 0.0f;
    float constants7[4];
    constants7[0] = g_Shared->tapeNoiseEnabled;
    constants7[1] = g_Shared->tapeNoiseIntensity;
    constants7[2] = g_Shared->vignetteEnabled;
    constants7[3] = 0.0f;
    // c10 = (megaBezelEnabled, megaBezelThickness, megaBezelOpacity, megaBezelBlur)
    float constants10[4];
    constants10[0] = g_Shared->megaBezelEnabled;
    constants10[1] = g_Shared->megaBezelThickness;
    constants10[2] = g_Shared->megaBezelOpacity;
    constants10[3] = g_Shared->megaBezelBlur;
    // c11 = (megaBezelRadius, megaBezelReflectionWidth, megaBezelStartFade, _pad)
    float constants11[4];
    constants11[0] = g_Shared->megaBezelRadius;
    constants11[1] = g_Shared->megaBezelReflectionWidth;
    constants11[2] = GetMegaBezelStartFade(g_Shared->megaBezelEnabled > 0.0f);
    constants11[3] = 0.0f;
    // c12 = (bezelHookActive, bezelHookOpacity, _pad, _pad)
    float constants12[4];
    constants12[0] = g_Shared->bezelHookActive;
    constants12[1] = g_Shared->bezelHookOpacity;
    constants12[2] = 0.0f;
    constants12[3] = 0.0f;
    dev->SetPixelShaderConstantF(0, constants,  4); // c0-c3
    dev->SetPixelShaderConstantF(4, constants2, 1); // c4
    dev->SetPixelShaderConstantF(5, constants3, 1); // c5 (brightness, contrast, saturation, temperature)
    dev->SetPixelShaderConstantF(6, constants4, 1); // c6 (flickerEnabled, flickerIntensity, time, flickerRate)
    dev->SetPixelShaderConstantF(7, constants5, 1); // c7 (blackLevel, gamma, phosphorEnabled, phosphorIntensity)
    dev->SetPixelShaderConstantF(8, constants6, 1); // c8 (vhsEnabled, vhsIntensity, grainIntensity, 0)
    dev->SetPixelShaderConstantF(9, constants7, 1); // c9 (tapeNoiseEnabled, tapeNoiseIntensity, vignetteEnabled, 0)
    // c14 = (gameBoundsL, gameBoundsT, gameBoundsR, gameBoundsB) — auto-detected
    // game content area in UV space, used by reflection to skip in-game letterbox.
    float constants14[4] = { g_GameBoundsL, g_GameBoundsT, g_GameBoundsR, g_GameBoundsB };
    dev->SetPixelShaderConstantF(10, constants10, 1); // c10 megaBezel core
    dev->SetPixelShaderConstantF(11, constants11, 1); // c11 megaBezel extra
    dev->SetPixelShaderConstantF(12, constants12, 1); // c12 bezel-PNG composite
    dev->SetPixelShaderConstantF(14, constants14, 1); // c14 game content bounds

    // Copy render target when blur, bloom, curvature or brightness/contrast is active
    bool d3d9BlurOn   = g_Shared->blurEnabled      > 0.0f && g_Shared->blurIntensity      > 0.0f;
    bool d3d9BloomOn  = g_Shared->bloomEnabled     > 0.0f && g_Shared->bloomIntensity     > 0.0f;
    bool d3d9CurvOn   = g_Shared->curvatureEnabled > 0.0f && g_Shared->curvatureIntensity > 0.0f;
    bool d3d9BcOn     = g_Shared->brightness != 0.0f || g_Shared->contrast != 0.0f || g_Shared->saturation != 0.0f || g_Shared->temperature != 0.0f
                     || g_Shared->blackLevel > 0.0f || (g_Shared->gamma != 1.0f && g_Shared->gamma != 0.0f);
    bool d3d9FlickOn    = g_Shared->flickerEnabled  > 0.0f && g_Shared->flickerIntensity  > 0.0f;
    bool d3d9PhosphorOn = g_Shared->phosphorEnabled > 0.0f && g_Shared->phosphorIntensity > 0.0f;
    bool d3d9VhsOn        = g_Shared->vhsEnabled        > 0.0f;
    bool d3d9GrainOn      = g_Shared->grainIntensity    > 0.0f;
    bool d3d9TapeNoiseOn  = g_Shared->tapeNoiseEnabled  > 0.0f && g_Shared->tapeNoiseIntensity > 0.0f;
    bool d3d9MegaBzOn     = g_Shared->megaBezelEnabled  > 0.0f;
    bool d3d9BezelHookOn  = g_Shared->bezelHookActive   > 0.0f;
    // Auto-detect game content bounds when reflection OR VHS/grain/tapeNoise is
    // active — used both by the MegaBezel reflection sampler (cb14) and by the
    // VHS content mask to exclude letterbox bars. ~50µs per call (64x36 readback).
    bool needBounds = d3d9MegaBzOn || d3d9VhsOn || d3d9GrainOn || d3d9TapeNoiseOn;
    if (needBounds) {
        if ((g_DetectFrame++ % 30) == 0) DetectGameBoundsD3D9(dev);
    } else {
        // No effect needs bounds → reset to full-screen so reflection is neutral.
        g_GameBoundsL = 0.0f; g_GameBoundsT = 0.0f;
        g_GameBoundsR = 1.0f; g_GameBoundsB = 1.0f;
        g_DetectFrame = 0;
    }
    {
        static bool s_bezelWasOn = false;
        static int  s_bezelPollCount = 0;
        if (d3d9BezelHookOn != s_bezelWasOn) {
            Log("[D3D9] bezelHookActive CHANGED: raw=%.2f on=%d path[0]=%d megaBz=%.2f",
                g_Shared->bezelHookActive, d3d9BezelHookOn ? 1 : 0,
                (int)g_Shared->bezelHookPath[0], g_Shared->megaBezelEnabled);
            s_bezelWasOn = d3d9BezelHookOn;
        }
        if (++s_bezelPollCount == 600) {
            Log("[D3D9] bezel poll: active=%.2f megaBz=%.2f path[0]=%d",
                g_Shared->bezelHookActive, g_Shared->megaBezelEnabled,
                (int)g_Shared->bezelHookPath[0]);
            s_bezelPollCount = 0;
        }
    }
    bool d3d9NeedCopy = d3d9BlurOn || d3d9BloomOn || d3d9CurvOn || d3d9BcOn || d3d9FlickOn || d3d9PhosphorOn || d3d9VhsOn || d3d9GrainOn || d3d9TapeNoiseOn || d3d9MegaBzOn || d3d9BezelHookOn;

    // Redirect drawing to the actual backbuffer — this is where Present will swap from.
    // The game may have had a custom RT bound, or an intermediate surface; we need to
    // land our final composited pixels on the real backbuffer regardless.
    // Exception: Adobe AIR (and similar DirectComposition games) use a tiny dummy swap
    // chain (e.g. 16×16) while rendering to a separate RT that dcomp reads directly.
    // In that case skip the redirect and draw to the current RT (the real framebuffer).
    {
        IDirect3DSurface9* bbSurf = nullptr;
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bbSurf)) && bbSurf) {
            D3DSURFACE_DESC bbDesc;
            bool bbIsDummy = FAILED(bbSurf->GetDesc(&bbDesc)) || bbDesc.Width < 64 || bbDesc.Height < 64;
            if (!bbIsDummy) {
                dev->SetRenderTarget(0, bbSurf);
                // Re-read viewport (SetRenderTarget resets it to the new RT's bounds)
                dev->GetViewport(&vpCheck);
                d3d9VpW = (float)vpCheck.Width;
                d3d9VpH = (float)vpCheck.Height;
                // Update size constants to match the actual draw target (backbuffer)
                constants[0] = d3d9VpW;
                constants[1] = d3d9VpH;
                constants[6] = d3d9VpW;
                constants[12] = d3d9VpH;
                dev->SetPixelShaderConstantF(0, constants, 4);
            }
            // else: dummy BB — keep current RT (game's real framebuffer, e.g. AIR RT0)
            bbSurf->Release();
        }
    }

    // NOTE: The game's scene has already been captured into g_D3D9BBCopy from
    // HookedD3D9EndScene (via D3D9CaptureScene). We do NOT StretchRect from the
    // backbuffer here — GMS1, MMF2, and similar engines may have changed RT bindings
    // or the backbuffer may not yet hold the final game image at Present time.
    // Skip rendering the effect pass entirely if the capture texture was never populated.
    if (d3d9NeedCopy && !g_D3D9HasCapture) {
        // No valid capture — fall back to pure multiplicative scanline mask pass
        // (which blends against whatever is already on the backbuffer).
        d3d9NeedCopy = false;
    }

    // Set render state
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, d3d9NeedCopy ? FALSE : TRUE);
    if (!d3d9NeedCopy) {
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
    }
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetPixelShader(g_D3D9PS);

    // Bind blur copy texture
    if (d3d9NeedCopy && g_D3D9BBCopy) {
        dev->SetTexture(0, g_D3D9BBCopy);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    } else {
        dev->SetTexture(0, nullptr);
    }

    // Bezel PNG texture (sampler s1). Reload from disk when the shared-mem path
    // changes — drop cached texture when the bezel is turned off.
    // Also retry if texture is null but path is non-empty (race: C# writes
    // bezelHookActive before bezelHookPath, so first frame may see empty path).
    if (d3d9BezelHookOn) {
        bool pathChanged = wcscmp(g_Shared->bezelHookPath, g_D3D9BezelPathCached) != 0;
        if (pathChanged || (!g_D3D9BezelTex && g_Shared->bezelHookPath[0])) {
            Log("[D3D9] BEZEL LOAD TRIGGER: pathChanged=%d tex=%p path[0]=%d cached[0]=%d",
                pathChanged?1:0, g_D3D9BezelTex, (int)g_Shared->bezelHookPath[0], (int)g_D3D9BezelPathCached[0]);
            LoadBezelTextureD3D9(dev, g_Shared->bezelHookPath);
        }
        if (g_D3D9BezelTex) {
            dev->SetTexture(1, g_D3D9BezelTex);
            dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(1, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
            dev->SetSamplerState(1, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
        } else {
            dev->SetTexture(1, nullptr);
        }
    } else {
        if (g_D3D9BezelTex) ReleaseBezelTextureD3D9();
        dev->SetTexture(1, nullptr);
    }
    if (d3d9BezelHookOn && !g_D3D9BezelTex) {
        float fixup[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        dev->SetPixelShaderConstantF(12, fixup, 1);
    }

    // Draw fullscreen quad with UVs for texture sampling
    struct VERTEX { float x, y, z, rhw; float u, v; };
    D3DVIEWPORT9 vp;
    dev->GetViewport(&vp);
    float w = (float)vp.Width;
    float h = (float)vp.Height;
    VERTEX quad[4] = {
        { 0, 0, 0, 1, 0, 0 },
        { w, 0, 0, 1, 1, 0 },
        { 0, h, 0, 1, 0, 1 },
        { w, h, 0, 1, 1, 1 },
    };
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetVertexShader(nullptr);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(VERTEX));

    // Restore render target FIRST (state block does not capture RT0 on all drivers
    // reliably — we save/restore it explicitly), then Apply() restores everything
    // else: shaders, constants, render states, samplers, textures, FVF, streams,
    // indices, vertex declaration, viewport, scissor, etc.
    if (prevRT0) { dev->SetRenderTarget(0, prevRT0); prevRT0->Release(); }
    prevState->Apply();
    prevState->Release();
}

// ── Hooked EndScene (D3D9) — restore-call-repatch inline hook ────────────
// Effects are NOT applied here — moved to HookedD3D9Present so they run exactly
// once per frame even when the engine (e.g. MMF2) calls EndScene multiple times.
static HRESULT STDMETHODCALLTYPE HookedD3D9EndScene(IDirect3DDevice9* dev) {
    static bool s_firstCall = true;
    if (s_firstCall) { s_firstCall = false; Log("[D3D9] HookedD3D9EndScene FIRED — dev=0x%p", (void*)dev); }
    g_D3D9EndSceneCalls++;
    // Periodic heartbeat so we can verify EndScene is called continuously
    if (g_D3D9EndSceneCalls == 1 || g_D3D9EndSceneCalls % 600 == 0) {
        Log("[D3D9] EndScene call #%d", g_D3D9EndSceneCalls);
    }
    __try {
        // Only initialise resources here; effects are drawn in Present.
        if (!g_D3D9Inited) InitD3D9Resources(dev);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("[D3D9] EXCEPTION 0x%08X in HookedD3D9EndScene (init)", GetExceptionCode());
    }

    // Restore original bytes → call real EndScene → repatch
    DWORD oldProt;
    VirtualProtect(g_D3D9EndSceneAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_D3D9EndSceneAddr, g_D3D9EndSceneOrig, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_D3D9EndSceneAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_D3D9EndSceneAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    HRESULT hr = ((PFN_D3D9EndScene)g_D3D9EndSceneAddr)(dev);

    VirtualProtect(g_D3D9EndSceneAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_D3D9EndSceneAddr, g_D3D9EndSceneJmp, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_D3D9EndSceneAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_D3D9EndSceneAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    // EndScene is called N times per frame on many engines (one per layer).
    // We no longer capture here — the backbuffer may be incomplete at this point
    // (e.g. Cyber Shadow calls EndScene ~120x before the first Present, and the
    // backbuffer is only fully composed just before Present fires).
    // Capture is done inside HookedD3D9Present instead.
    return hr;
}

// ── Hooked Present (D3D9) — restore-call-repatch inline hook ────────────
// All D3D9 effects are applied HERE — once per frame, after all game rendering is done.
// Using Present (not EndScene) avoids double-application on engines like MMF2 that
// call EndScene N times per frame (one per render layer).
// Shared "before real Present" work: counters, diagnostics, capture, draw effects.
// Used by all three entry points (device Present, device PresentEx, swapchain Present).
static void D3D9PresentCore(IDirect3DDevice9* dev, const char* source) {
    g_D3D9PresentCalls++;
    if (g_D3D9PresentCalls == 1 || g_D3D9PresentCalls % 300 == 0) {
        Log("[D3D9] Present call #%d via %s (endScene total=%d)",
            g_D3D9PresentCalls, source, g_D3D9EndSceneCalls);
    }
    __try {
        D3D9CheckProcessBlocked();
        if (!g_D3D9Inited) InitD3D9Resources(dev);
        if (g_D3D9Inited && !g_D3D9ProcBlocked) {
            // ── Startup blackout (megabezel only) — D3D9 path ──
            // Mirror the D3D11 1.5s blackout: clear the backbuffer to black
            // and skip the effect pass for the first ~1.5s after megabezel
            // first turns on. Hides the game's launch splash / clear color
            // (vivid blue, skybox, etc.) which would otherwise be reflected
            // into the bezel zone AND shine through inside the game viewport.
            // After blackout ends, the megaBezelStartFade uniform smoothly
            // ramps the reflection up over its own 1.5s window.
            bool d3d9PresentMegaBzOn = g_Shared && g_Shared->active &&
                                       g_Shared->megaBezelEnabled > 0.0f;
            if (d3d9PresentMegaBzOn) {
                static float blackoutStartD3D9 = -1.0f;
                const float D3D9_BLACKOUT_SECONDS = 1.5f;
                if (blackoutStartD3D9 < 0.0f) blackoutStartD3D9 = GetTimeSeconds();
                if ((GetTimeSeconds() - blackoutStartD3D9) < D3D9_BLACKOUT_SECONDS) {
                    IDirect3DSurface9* bbSurf = nullptr;
                    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bbSurf)) && bbSurf) {
                        D3DSURFACE_DESC bbDesc;
                        if (SUCCEEDED(bbSurf->GetDesc(&bbDesc)) &&
                            bbDesc.Width >= 64 && bbDesc.Height >= 64) {
                            IDirect3DSurface9* prevRT = nullptr;
                            dev->GetRenderTarget(0, &prevRT);
                            dev->SetRenderTarget(0, bbSurf);
                            dev->Clear(0, nullptr, D3DCLEAR_TARGET,
                                D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
                            if (prevRT) {
                                dev->SetRenderTarget(0, prevRT);
                                prevRT->Release();
                            }
                        }
                        bbSurf->Release();
                    }
                    return;  // skip capture + ApplyScanlines this frame
                }
            }

            g_D3D9FrameCount++;
            if (g_D3D9FrameCount == 10 || g_D3D9FrameCount == 60 ||
                g_D3D9FrameCount == 300 || g_D3D9FrameCount == 600) {
                D3D9Diagnose(dev, g_D3D9FrameCount);
            }
            D3D9CaptureScene(dev);
            HRESULT hrBS = dev->BeginScene();
            if (SUCCEEDED(hrBS)) {
                ApplyD3D9Scanlines(dev);
                dev->EndScene();
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("[D3D9] EXCEPTION 0x%08X in PresentCore (%s) — scanlines disabled",
            GetExceptionCode(), source);
        g_D3D9Inited = false;
    }
}

static HRESULT STDMETHODCALLTYPE HookedD3D9Present(
    IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND hWndOverride, const RGNDATA* dirty)
{
    static bool s_firstCall = true;
    if (s_firstCall) { s_firstCall = false; Log("[D3D9] HookedD3D9Present FIRED — dev=0x%p", (void*)dev); }
    D3D9PresentCore(dev, "Device::Present");

    DWORD oldProt;
    VirtualProtect(g_D3D9PresentAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_D3D9PresentAddr, g_D3D9PresentOrig, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_D3D9PresentAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_D3D9PresentAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    HRESULT hr = ((PFN_D3D9Present)g_D3D9PresentAddr)(dev, src, dst, hWndOverride, dirty);

    VirtualProtect(g_D3D9PresentAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_D3D9PresentAddr, g_D3D9PresentJmp, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_D3D9PresentAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_D3D9PresentAddr, HOOK_JMP_SIZE, oldProt, &oldProt);
    return hr;
}

// ── Hooked IDirect3DSwapChain9::Present ─────────────────────────────────
// Some engines call swapChain->Present() directly instead of device->Present().
// This is a separate function in d3d9.dll and requires its own inline hook.
typedef HRESULT(STDMETHODCALLTYPE* PFN_D3D9SCPresent)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
static HRESULT STDMETHODCALLTYPE HookedD3D9SCPresent(
    IDirect3DSwapChain9* sc, const RECT* src, const RECT* dst,
    HWND hWndOverride, const RGNDATA* dirty, DWORD flags)
{
    static bool s_firstCall = true;
    if (s_firstCall) { s_firstCall = false; Log("[D3D9] HookedD3D9SCPresent FIRED — sc=0x%p", (void*)sc); }

    __try {
        IDirect3DDevice9* dev = nullptr;
        if (sc && SUCCEEDED(sc->GetDevice(&dev)) && dev) {
            D3D9PresentCore(dev, "SwapChain::Present");
            dev->Release();
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("[D3D9] EXCEPTION 0x%08X in HookedD3D9SCPresent", GetExceptionCode());
    }

    DWORD oldProt;
    VirtualProtect(g_D3D9SCPresentAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_D3D9SCPresentAddr, g_D3D9SCPresentOrig, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_D3D9SCPresentAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_D3D9SCPresentAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    HRESULT hr = ((PFN_D3D9SCPresent)g_D3D9SCPresentAddr)(sc, src, dst, hWndOverride, dirty, flags);

    VirtualProtect(g_D3D9SCPresentAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_D3D9SCPresentAddr, g_D3D9SCPresentJmp, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_D3D9SCPresentAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_D3D9SCPresentAddr, HOOK_JMP_SIZE, oldProt, &oldProt);
    return hr;
}

// ── Hooked IDirect3DDevice9Ex::PresentEx ────────────────────────────────
// IDirect3DDevice9Ex adds PresentEx at vtable slot 121. Games using D3D9Ex
// may call PresentEx instead of the base Present, bypassing our Present hook.
typedef HRESULT(STDMETHODCALLTYPE* PFN_D3D9PresentEx)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
static HRESULT STDMETHODCALLTYPE HookedD3D9PresentEx(
    IDirect3DDevice9Ex* dev, const RECT* src, const RECT* dst,
    HWND hWndOverride, const RGNDATA* dirty, DWORD flags)
{
    static bool s_firstCall = true;
    if (s_firstCall) { s_firstCall = false; Log("[D3D9] HookedD3D9PresentEx FIRED — dev=0x%p", (void*)dev); }

    D3D9PresentCore(dev, "Device::PresentEx");

    DWORD oldProt;
    VirtualProtect(g_D3D9PresentExAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_D3D9PresentExAddr, g_D3D9PresentExOrig, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_D3D9PresentExAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_D3D9PresentExAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    HRESULT hr = ((PFN_D3D9PresentEx)g_D3D9PresentExAddr)(dev, src, dst, hWndOverride, dirty, flags);

    VirtualProtect(g_D3D9PresentExAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_D3D9PresentExAddr, g_D3D9PresentExJmp, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_D3D9PresentExAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_D3D9PresentExAddr, HOOK_JMP_SIZE, oldProt, &oldProt);
    return hr;
}

// ── Hooked Reset (D3D9) — restore-call-repatch inline hook ──────────────
static HRESULT STDMETHODCALLTYPE HookedD3D9Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    Log("[D3D9] HookedD3D9Reset — releasing resources before Reset");
    if (g_D3D9BBCopy) { g_D3D9BBCopy->Release(); g_D3D9BBCopy = nullptr; }
    g_D3D9LastW = 0; g_D3D9LastH = 0;
    g_D3D9HasCapture = false;
    // Reset frame counter so the DIAG re-runs after device Reset (alt+enter, resolution change)
    g_D3D9FrameCount = 0;
    if (g_D3D9PS) { g_D3D9PS->Release(); g_D3D9PS = nullptr; }
    if (g_D3D9VS) { g_D3D9VS->Release(); g_D3D9VS = nullptr; }
    // Bezel + auto-detect surfaces are in DEFAULT pool — MUST release before Reset
    // or Reset returns D3DERR_INVALIDCALL (the device "is still in use"). Sets
    // g_GameBoundsLTRB back to defaults so reflection works correctly post-Reset.
    ReleaseBezelTextureD3D9();
    if (g_D3D9DetectRT)  { g_D3D9DetectRT->Release();  g_D3D9DetectRT  = nullptr; }
    if (g_D3D9DetectSys) { g_D3D9DetectSys->Release(); g_D3D9DetectSys = nullptr; }
    g_GameBoundsL = 0.0f; g_GameBoundsT = 0.0f;
    g_GameBoundsR = 1.0f; g_GameBoundsB = 1.0f;
    g_DetectFrame = 0;
    g_D3D9Inited = false;

    DWORD oldProt;
    VirtualProtect(g_D3D9ResetAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_D3D9ResetAddr, g_D3D9ResetOrig, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_D3D9ResetAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_D3D9ResetAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    HRESULT hr = ((PFN_D3D9Reset)g_D3D9ResetAddr)(dev, pp);

    VirtualProtect(g_D3D9ResetAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_D3D9ResetAddr, g_D3D9ResetJmp, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_D3D9ResetAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_D3D9ResetAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    if (SUCCEEDED(hr)) {
        Log("[D3D9] Reset succeeded — reinitializing resources");
        InitD3D9Resources(dev);
    } else {
        Log("[D3D9] Reset failed hr=0x%08X", hr);
    }
    return hr;
}

// ── D3D9 resource release helper (MAME device-lifecycle) ─────────────────
// MAME destroys its D3D9 device when launching a ROM from the menu, then calls
// IDirect3D9::CreateDevice again for the new fullscreen exclusive device.
// If our PS / VS / BBCopy still hold COM refs on the old device the adapter is
// "in use" and the new CreateDevice returns D3DERR_INVALIDCALL (0x8876086C).
// This helper releases exactly the resources that hold those refs — mirroring
// HookedD3D9Reset — and clears g_D3D9Inited so InitD3D9Resources re-runs on
// the next EndScene/Present call with the brand-new device.
// NOTE: g_D3D9Device is cleared but NOT Release()d here because by the time
// MAME calls CreateDevice again the old device COM object may already be dead;
// attempting Release() on a zombie device can crash.
static void D3D9ReleaseOurResources() {
    if (g_D3D9BBCopy) { g_D3D9BBCopy->Release(); g_D3D9BBCopy = nullptr; }
    g_D3D9LastW = 0; g_D3D9LastH = 0;
    g_D3D9HasCapture = false;
    g_D3D9FrameCount = 0;
    if (g_D3D9PS) { g_D3D9PS->Release(); g_D3D9PS = nullptr; }
    if (g_D3D9VS) { g_D3D9VS->Release(); g_D3D9VS = nullptr; }
    // Bezel PNG texture is in D3DPOOL_DEFAULT (D3D9Ex doesn't allow MANAGED) so
    // it MUST be released before Reset, or Reset fails. Same for device-recreation.
    ReleaseBezelTextureD3D9();
    // Auto-detect surfaces (DEFAULT + SYSTEMMEM) — also lost on Reset.
    if (g_D3D9DetectRT)  { g_D3D9DetectRT->Release();  g_D3D9DetectRT  = nullptr; }
    if (g_D3D9DetectSys) { g_D3D9DetectSys->Release(); g_D3D9DetectSys = nullptr; }
    g_GameBoundsL = 0.0f; g_GameBoundsT = 0.0f;
    g_GameBoundsR = 1.0f; g_GameBoundsB = 1.0f;
    g_DetectFrame = 0;
    g_D3D9Device = nullptr; // cleared without Release — old device may be a zombie
    g_D3D9Inited = false;
    Log("[MAME] D3D9ReleaseOurResources: PS/VS/BBCopy/Bezel released, device cleared");
}

// ── Deferred D3D9 device-hook installer ──────────────────────────────────
// Fires when the GAME calls IDirect3D9::CreateDevice (vtable slot 16).
// Non-MAME: one-shot hook — original bytes restored before call, NOT re-patched.
// MAME: permanent restore-call-repatch — releases our D3D9 resources first so
//       the new exclusive-fullscreen device can be created without INVALIDCALL.
typedef HRESULT(STDMETHODCALLTYPE* PFN_D3D9CDev)(
    IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
    D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);

static HRESULT STDMETHODCALLTYPE HookedD3D9CreateDevice(
    IDirect3D9* pThis, UINT Adapter, D3DDEVTYPE DeviceType,
    HWND hFocusWindow, DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS* pPP, IDirect3DDevice9** ppDevice)
{
    if (g_IsMame) {
        // ── MAME path: permanent restore-call-repatch ─────────────────────
        // MAME calls CreateDevice again each time a ROM is launched. Release
        // our COM resources first so the adapter isn't "in use", then let the
        // real call proceed, then re-patch so we intercept the NEXT launch too.
        Log("[MAME] HookedD3D9CreateDevice — releasing cached resources for device recreation");
        D3D9ReleaseOurResources();

        DWORD op;
        VirtualProtect(g_D3D9CreateDeviceAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
        memcpy(g_D3D9CreateDeviceAddr, g_D3D9CreateDeviceOrig, HOOK_JMP_SIZE);
        FlushInstructionCache(GetCurrentProcess(), g_D3D9CreateDeviceAddr, HOOK_JMP_SIZE);
        VirtualProtect(g_D3D9CreateDeviceAddr, HOOK_JMP_SIZE, op, &op);

        HRESULT hr = ((PFN_D3D9CDev)g_D3D9CreateDeviceAddr)(
            pThis, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPP, ppDevice);

        // Re-patch — keep hook active for subsequent ROM launches.
        VirtualProtect(g_D3D9CreateDeviceAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
        memcpy(g_D3D9CreateDeviceAddr, g_D3D9CreateDeviceJmp, HOOK_JMP_SIZE);
        FlushInstructionCache(GetCurrentProcess(), g_D3D9CreateDeviceAddr, HOOK_JMP_SIZE);
        VirtualProtect(g_D3D9CreateDeviceAddr, HOOK_JMP_SIZE, op, &op);

        Log("[MAME] CreateDevice hr=0x%08X — resources will reinit at next EndScene/Present", (unsigned)hr);
        return hr;
    }

    // ── Non-MAME path: one-shot deferred fallback (original v1.2 behavior) ─
    DWORD op;
    VirtualProtect(g_D3D9CreateDeviceAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
    memcpy(g_D3D9CreateDeviceAddr, g_D3D9CreateDeviceOrig, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_D3D9CreateDeviceAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_D3D9CreateDeviceAddr, HOOK_JMP_SIZE, op, &op);
    g_D3D9DeferredHookActive = false;

    HRESULT hr = ((PFN_D3D9CDev)g_D3D9CreateDeviceAddr)(
        pThis, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPP, ppDevice);
    Log("[D3D9] Deferred CreateDevice hr=0x%08X", (unsigned)hr);

    if (SUCCEEDED(hr) && ppDevice && *ppDevice && !g_D3D9EndSceneAddr) {
        void** vtbl = *(void***)(*ppDevice);
        g_D3D9EndSceneAddr  = (BYTE*)vtbl[42];
        g_D3D9PresentAddr   = (BYTE*)vtbl[17];
        g_D3D9ResetAddr     = (BYTE*)vtbl[16];

        IDirect3DSwapChain9* sc = nullptr;
        if (SUCCEEDED((*ppDevice)->GetSwapChain(0, &sc)) && sc) {
            g_D3D9SCPresentAddr = (BYTE*)(*(void***)sc)[3];
            sc->Release();
        }
        Log("[D3D9] Deferred: EndScene=0x%p Present=0x%p Reset=0x%p SC::Present=0x%p",
            (void*)g_D3D9EndSceneAddr, (void*)g_D3D9PresentAddr,
            (void*)g_D3D9ResetAddr,   (void*)g_D3D9SCPresentAddr);

        if (g_D3D9EndSceneAddr)
            InstallHookAt(g_D3D9EndSceneAddr, g_D3D9EndSceneOrig, g_D3D9EndSceneJmp, (void*)HookedD3D9EndScene);
        if (g_D3D9PresentAddr)
            InstallHookAt(g_D3D9PresentAddr, g_D3D9PresentOrig, g_D3D9PresentJmp, (void*)HookedD3D9Present);
        if (g_D3D9ResetAddr)
            InstallHookAt(g_D3D9ResetAddr, g_D3D9ResetOrig, g_D3D9ResetJmp, (void*)HookedD3D9Reset);
        if (g_D3D9SCPresentAddr)
            InstallHookAt(g_D3D9SCPresentAddr, g_D3D9SCPresentOrig, g_D3D9SCPresentJmp, (void*)HookedD3D9SCPresent);

        Log("[D3D9] Deferred inline hooks installed OK");
    }
    return hr;
}

// Same as above but for IDirect3D9Ex::CreateDeviceEx (vtable slot 20).
// Games that call Direct3DCreate9Ex + CreateDeviceEx bypass the CreateDevice hook above.
typedef HRESULT(STDMETHODCALLTYPE* PFN_D3D9CDevEx)(
    IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD,
    D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);

static HRESULT STDMETHODCALLTYPE HookedD3D9CreateDeviceEx(
    IDirect3D9Ex* pThis, UINT Adapter, D3DDEVTYPE DeviceType,
    HWND hFocusWindow, DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS* pPP, D3DDISPLAYMODEEX* pMode,
    IDirect3DDevice9Ex** ppDevice)
{
    if (g_IsMame) {
        // ── MAME path: permanent restore-call-repatch ─────────────────────
        Log("[MAME] HookedD3D9CreateDeviceEx — releasing cached resources for device recreation");
        D3D9ReleaseOurResources();

        DWORD op;
        VirtualProtect(g_D3D9CreateDeviceExAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
        memcpy(g_D3D9CreateDeviceExAddr, g_D3D9CreateDeviceExOrig, HOOK_JMP_SIZE);
        FlushInstructionCache(GetCurrentProcess(), g_D3D9CreateDeviceExAddr, HOOK_JMP_SIZE);
        VirtualProtect(g_D3D9CreateDeviceExAddr, HOOK_JMP_SIZE, op, &op);

        HRESULT hr = ((PFN_D3D9CDevEx)g_D3D9CreateDeviceExAddr)(
            pThis, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPP, pMode, ppDevice);

        VirtualProtect(g_D3D9CreateDeviceExAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
        memcpy(g_D3D9CreateDeviceExAddr, g_D3D9CreateDeviceExJmp, HOOK_JMP_SIZE);
        FlushInstructionCache(GetCurrentProcess(), g_D3D9CreateDeviceExAddr, HOOK_JMP_SIZE);
        VirtualProtect(g_D3D9CreateDeviceExAddr, HOOK_JMP_SIZE, op, &op);

        Log("[MAME] CreateDeviceEx hr=0x%08X — resources will reinit at next EndScene/Present", (unsigned)hr);
        return hr;
    }

    // ── Non-MAME path: one-shot deferred fallback ────────────────────────
    DWORD op;
    VirtualProtect(g_D3D9CreateDeviceExAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
    memcpy(g_D3D9CreateDeviceExAddr, g_D3D9CreateDeviceExOrig, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_D3D9CreateDeviceExAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_D3D9CreateDeviceExAddr, HOOK_JMP_SIZE, op, &op);
    g_D3D9DeferredHookActive = false;

    HRESULT hr = ((PFN_D3D9CDevEx)g_D3D9CreateDeviceExAddr)(
        pThis, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPP, pMode, ppDevice);
    Log("[D3D9] Deferred CreateDeviceEx hr=0x%08X", (unsigned)hr);

    if (SUCCEEDED(hr) && ppDevice && *ppDevice && !g_D3D9EndSceneAddr) {
        void** vtbl = *(void***)(*ppDevice);
        g_D3D9EndSceneAddr  = (BYTE*)vtbl[42];
        g_D3D9PresentAddr   = (BYTE*)vtbl[17];
        g_D3D9ResetAddr     = (BYTE*)vtbl[16];
        g_D3D9PresentExAddr = (BYTE*)vtbl[121];

        IDirect3DSwapChain9* sc = nullptr;
        if (SUCCEEDED((*ppDevice)->GetSwapChain(0, &sc)) && sc) {
            g_D3D9SCPresentAddr = (BYTE*)(*(void***)sc)[3];
            sc->Release();
        }
        Log("[D3D9] Deferred(Ex): EndScene=0x%p Present=0x%p Reset=0x%p SC::Present=0x%p PresentEx=0x%p",
            (void*)g_D3D9EndSceneAddr, (void*)g_D3D9PresentAddr,
            (void*)g_D3D9ResetAddr,   (void*)g_D3D9SCPresentAddr,
            (void*)g_D3D9PresentExAddr);

        if (g_D3D9EndSceneAddr)
            InstallHookAt(g_D3D9EndSceneAddr, g_D3D9EndSceneOrig, g_D3D9EndSceneJmp, (void*)HookedD3D9EndScene);
        if (g_D3D9PresentAddr)
            InstallHookAt(g_D3D9PresentAddr, g_D3D9PresentOrig, g_D3D9PresentJmp, (void*)HookedD3D9Present);
        if (g_D3D9ResetAddr)
            InstallHookAt(g_D3D9ResetAddr, g_D3D9ResetOrig, g_D3D9ResetJmp, (void*)HookedD3D9Reset);
        if (g_D3D9SCPresentAddr)
            InstallHookAt(g_D3D9SCPresentAddr, g_D3D9SCPresentOrig, g_D3D9SCPresentJmp, (void*)HookedD3D9SCPresent);
        if (g_D3D9PresentExAddr)
            InstallHookAt(g_D3D9PresentExAddr, g_D3D9PresentExOrig, g_D3D9PresentExJmp, (void*)HookedD3D9PresentEx);

        Log("[D3D9] Deferred(Ex) inline hooks installed OK");
    }
    return hr;
}

// ── D3D9 inline hook installation (restore-call-repatch) ─────────────────
// Unlike vtable patching (which fails when NVIDIA uses per-instance vtables),
// this patches the FUNCTION BODY itself — works for ALL D3D9 devices.
static bool InstallD3D9Hook() {
    // ANGLE (libGLESv2.dll) proxies OpenGL ES → D3D11 and exposes a stub d3d9.dll.
    // Calling CreateDevice on that stub fails with E_INVALIDARG and wastes 70-250ms per
    // attempt. Since ANGLE games are already covered by the D3D11 Present hook, skip D3D9
    // entirely and report success so the retry loop stops trying.
    if (GetModuleHandleA("libGLESv2.dll")) {
        Log("[D3D9] InstallD3D9Hook: ANGLE detected (libGLESv2.dll loaded) — skipping D3D9, game is D3D11 via ANGLE");
        return true;
    }

    HMODULE d3d9mod = GetModuleHandleW(L"d3d9.dll");
    if (!d3d9mod) { Log("[D3D9] InstallD3D9Hook: d3d9.dll not loaded, skipping"); return false; }
    Log("[D3D9] InstallD3D9Hook: d3d9.dll found at 0x%p", (void*)d3d9mod);

    // Create a dummy device just to discover function addresses from the vtable.
    // We DON'T patch the vtable — we patch the functions themselves.
    typedef IDirect3D9* (WINAPI* PFN_Direct3DCreate9)(UINT);
    auto pDirect3DCreate9 = (PFN_Direct3DCreate9)GetProcAddress(d3d9mod, "Direct3DCreate9");
    if (!pDirect3DCreate9) { Log("[D3D9] FAIL: Direct3DCreate9 not found"); return false; }

    IDirect3D9* d3d9 = pDirect3DCreate9(D3D_SDK_VERSION);
    if (!d3d9) { Log("[D3D9] FAIL: Direct3DCreate9 returned null"); return false; }

    WNDCLASSEXW wc9 = { sizeof(WNDCLASSEXW), 0, DefWindowProcW, 0, 0,
        GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr,
        L"S4W_D3D9Dummy", nullptr };
    RegisterClassExW(&wc9);
    HWND hwnd9 = CreateWindowExW(0, wc9.lpszClassName, L"", WS_OVERLAPPED,
        0, 0, 4, 4, nullptr, nullptr, wc9.hInstance, nullptr);
    if (!hwnd9) { d3d9->Release(); return false; }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd9;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;

    IDirect3DDevice9* dummyDev = nullptr;
    HRESULT hr = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd9,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dummyDev);
    if (FAILED(hr)) {
        // HAL failed — try NULLREF as a fallback. On Windows 10/11, d3d9.dll uses
        // a single COM class for all device types, so the vtable entries (EndScene,
        // Present, etc.) point to the same functions regardless of device type.
        Log("[D3D9] HAL CreateDevice failed (0x%08X) — trying NULLREF for vtable discovery", (unsigned)hr);
        hr = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, hwnd9,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dummyDev);
        if (SUCCEEDED(hr))
            Log("[D3D9] NULLREF device OK — using its vtable for function address discovery");
    }
    if (FAILED(hr) || !dummyDev) {
        Log("[D3D9] FAIL: CreateDevice hr=0x%08X", (unsigned)hr);
        d3d9->Release(); DestroyWindow(hwnd9);
        UnregisterClassW(wc9.lpszClassName, wc9.hInstance);

        // Both HAL and NULLREF failed (or device already existed via D3D9Ex).
        // Install deferred hooks on IDirect3D9::CreateDevice AND
        // IDirect3D9Ex::CreateDeviceEx so we catch whichever path the game uses.
        if (g_D3D9DeferredHookActive) return true;

        IDirect3D9* probe = pDirect3DCreate9(D3D_SDK_VERSION);
        if (probe) {
            void** vtbl9 = *(void***)probe;
            g_D3D9CreateDeviceAddr = (BYTE*)vtbl9[16];
            probe->Release();
            if (InstallHookAt(g_D3D9CreateDeviceAddr, g_D3D9CreateDeviceOrig,
                              g_D3D9CreateDeviceJmp, (void*)HookedD3D9CreateDevice))
                Log("[D3D9] Deferred hook: IDirect3D9::CreateDevice at 0x%p",
                    (void*)g_D3D9CreateDeviceAddr);
        }

        // Also hook IDirect3D9Ex::CreateDeviceEx (slot 20) — many modern D3D9
        // games use the Ex interface and never call plain CreateDevice.
        typedef HRESULT(WINAPI* PFN_Direct3DCreate9Ex)(UINT, IDirect3D9Ex**);
        auto pCreate9Ex = (PFN_Direct3DCreate9Ex)GetProcAddress(d3d9mod, "Direct3DCreate9Ex");
        if (pCreate9Ex) {
            IDirect3D9Ex* probeEx = nullptr;
            if (SUCCEEDED(pCreate9Ex(D3D_SDK_VERSION, &probeEx)) && probeEx) {
                void** vtbl9ex = *(void***)probeEx;
                g_D3D9CreateDeviceExAddr = (BYTE*)vtbl9ex[20];
                probeEx->Release();
                if (InstallHookAt(g_D3D9CreateDeviceExAddr, g_D3D9CreateDeviceExOrig,
                                  g_D3D9CreateDeviceExJmp, (void*)HookedD3D9CreateDeviceEx))
                    Log("[D3D9] Deferred hook: IDirect3D9Ex::CreateDeviceEx at 0x%p",
                        (void*)g_D3D9CreateDeviceExAddr);
            }
        }

        if (g_D3D9CreateDeviceAddr || g_D3D9CreateDeviceExAddr) {
            g_D3D9DeferredHookActive = true;
            return true; // stop retry loop; hooks fire on game's CreateDevice call
        }
        return false;
    }

    // Get function addresses from dummy device's vtable
    void** vtable9 = *(void***)dummyDev;
    g_D3D9EndSceneAddr = (BYTE*)vtable9[42];
    g_D3D9PresentAddr  = (BYTE*)vtable9[17];
    g_D3D9ResetAddr    = (BYTE*)vtable9[16];

    // Get SwapChain::Present from the device's implicit swap chain (vtable slot 3)
    {
        IDirect3DSwapChain9* sc = nullptr;
        if (SUCCEEDED(dummyDev->GetSwapChain(0, &sc)) && sc) {
            void** vtsc = *(void***)sc;
            g_D3D9SCPresentAddr = (BYTE*)vtsc[3];
            sc->Release();
        }
    }

    // Also check D3D9Ex — if the EndScene function address differs, use the Ex one
    // (the game likely uses D3D9Ex, so the Ex function is the one that gets called)
    typedef HRESULT(WINAPI* PFN_Direct3DCreate9Ex)(UINT, IDirect3D9Ex**);
    auto pCreate9Ex = (PFN_Direct3DCreate9Ex)GetProcAddress(d3d9mod, "Direct3DCreate9Ex");
    if (pCreate9Ex) {
        IDirect3D9Ex* d3d9ex = nullptr;
        if (SUCCEEDED(pCreate9Ex(D3D_SDK_VERSION, &d3d9ex))) {
            D3DPRESENT_PARAMETERS ppEx = {};
            ppEx.Windowed = TRUE;
            ppEx.SwapEffect = D3DSWAPEFFECT_DISCARD;
            ppEx.hDeviceWindow = hwnd9;
            ppEx.BackBufferFormat = D3DFMT_UNKNOWN;

            IDirect3DDevice9Ex* dummyDevEx = nullptr;
            if (SUCCEEDED(d3d9ex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd9,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &ppEx, nullptr, &dummyDevEx))) {

                void** vt9ex = *(void***)dummyDevEx;
                BYTE* exEndScene = (BYTE*)vt9ex[42];
                BYTE* exPresent  = (BYTE*)vt9ex[17];
                BYTE* exReset    = (BYTE*)vt9ex[16];

                // PresentEx is slot 121 of IDirect3DDevice9Ex vtable
                g_D3D9PresentExAddr = (BYTE*)vt9ex[121];

                // SwapChain::Present from D3D9Ex device's implicit swap chain
                {
                    IDirect3DSwapChain9* scEx = nullptr;
                    if (SUCCEEDED(dummyDevEx->GetSwapChain(0, &scEx)) && scEx) {
                        void** vtscEx = *(void***)scEx;
                        BYTE* exSCPresent = (BYTE*)vtscEx[3];
                        if (exSCPresent != g_D3D9SCPresentAddr) {
                            Log("[D3D9] D3D9Ex SC::Present at 0x%p differs from D3D9 at 0x%p — using D3D9Ex",
                                (void*)exSCPresent, (void*)g_D3D9SCPresentAddr);
                            g_D3D9SCPresentAddr = exSCPresent;
                        }
                        scEx->Release();
                    }
                }

                // If function addresses differ, the D3D9Ex functions are separate implementations.
                // We prefer to hook the D3D9Ex functions since that's what the game likely uses.
                if (exEndScene != g_D3D9EndSceneAddr) {
                    Log("[D3D9] D3D9Ex EndScene at 0x%p differs from D3D9 at 0x%p — using D3D9Ex",
                        (void*)exEndScene, (void*)g_D3D9EndSceneAddr);
                    g_D3D9EndSceneAddr = exEndScene;
                }
                if (exPresent != g_D3D9PresentAddr) {
                    Log("[D3D9] D3D9Ex Present at 0x%p differs from D3D9 at 0x%p — using D3D9Ex",
                        (void*)exPresent, (void*)g_D3D9PresentAddr);
                    g_D3D9PresentAddr = exPresent;
                }
                if (exReset != g_D3D9ResetAddr) {
                    Log("[D3D9] D3D9Ex Reset at 0x%p differs from D3D9 at 0x%p — using D3D9Ex",
                        (void*)exReset, (void*)g_D3D9ResetAddr);
                    g_D3D9ResetAddr = exReset;
                }
                dummyDevEx->Release();
            }
            d3d9ex->Release();
        }
    }

    // ── MAME: install permanent CreateDevice/CreateDeviceEx hooks ───────────
    // When the dummy device succeeded (NULLREF path — typical for MAME), the
    // deferred fallback path never fires, so g_D3D9CreateDeviceAddr is still
    // null. We install the permanent hooks HERE, before releasing d3d9, while
    // the factory vtable pointer is still valid.
    // For non-MAME processes this block is a complete no-op.
    if (g_IsMame) {
        void** vtbl9mame = *(void***)d3d9;
        g_D3D9CreateDeviceAddr = (BYTE*)vtbl9mame[16];
        if (InstallHookAt(g_D3D9CreateDeviceAddr, g_D3D9CreateDeviceOrig,
                          g_D3D9CreateDeviceJmp, (void*)HookedD3D9CreateDevice))
            Log("[MAME] Permanent hook: IDirect3D9::CreateDevice at 0x%p",
                (void*)g_D3D9CreateDeviceAddr);
        else
            Log("[MAME] WARN: failed to install permanent CreateDevice hook");

        // Also hook IDirect3D9Ex::CreateDeviceEx (slot 20) — MAME may use the Ex path.
        if (pCreate9Ex) {
            IDirect3D9Ex* probeEx = nullptr;
            if (SUCCEEDED(pCreate9Ex(D3D_SDK_VERSION, &probeEx)) && probeEx) {
                void** vtbl9ex = *(void***)probeEx;
                g_D3D9CreateDeviceExAddr = (BYTE*)vtbl9ex[20];
                probeEx->Release();
                if (InstallHookAt(g_D3D9CreateDeviceExAddr, g_D3D9CreateDeviceExOrig,
                                  g_D3D9CreateDeviceExJmp, (void*)HookedD3D9CreateDeviceEx))
                    Log("[MAME] Permanent hook: IDirect3D9Ex::CreateDeviceEx at 0x%p",
                        (void*)g_D3D9CreateDeviceExAddr);
                else
                    Log("[MAME] WARN: failed to install permanent CreateDeviceEx hook");
            }
        }
    }

    dummyDev->Release();
    d3d9->Release();

    Log("[D3D9] Function addresses: EndScene=0x%p Present=0x%p Reset=0x%p",
        (void*)g_D3D9EndSceneAddr, (void*)g_D3D9PresentAddr, (void*)g_D3D9ResetAddr);
    Log("[D3D9] Extra addresses: SC::Present=0x%p PresentEx=0x%p",
        (void*)g_D3D9SCPresentAddr, (void*)g_D3D9PresentExAddr);

    // Install restore-call-repatch inline hooks on the function bodies
    bool ok = true;
    if (InstallHookAt(g_D3D9EndSceneAddr, g_D3D9EndSceneOrig, g_D3D9EndSceneJmp, (void*)HookedD3D9EndScene)) {
        Log("[D3D9] Inline hook installed: EndScene at 0x%p", (void*)g_D3D9EndSceneAddr);
    } else {
        Log("[D3D9] WARN: Failed to install EndScene inline hook"); ok = false;
    }
    if (InstallHookAt(g_D3D9PresentAddr, g_D3D9PresentOrig, g_D3D9PresentJmp, (void*)HookedD3D9Present)) {
        Log("[D3D9] Inline hook installed: Present at 0x%p", (void*)g_D3D9PresentAddr);
    } else {
        Log("[D3D9] WARN: Failed to install Present inline hook"); ok = false;
    }
    if (InstallHookAt(g_D3D9ResetAddr, g_D3D9ResetOrig, g_D3D9ResetJmp, (void*)HookedD3D9Reset)) {
        Log("[D3D9] Inline hook installed: Reset at 0x%p", (void*)g_D3D9ResetAddr);
    } else {
        Log("[D3D9] WARN: Failed to install Reset inline hook"); ok = false;
    }
    if (g_D3D9SCPresentAddr) {
        if (InstallHookAt(g_D3D9SCPresentAddr, g_D3D9SCPresentOrig, g_D3D9SCPresentJmp, (void*)HookedD3D9SCPresent)) {
            Log("[D3D9] Inline hook installed: SwapChain::Present at 0x%p", (void*)g_D3D9SCPresentAddr);
        } else {
            Log("[D3D9] WARN: Failed to install SwapChain::Present inline hook");
        }
    }
    if (g_D3D9PresentExAddr) {
        if (InstallHookAt(g_D3D9PresentExAddr, g_D3D9PresentExOrig, g_D3D9PresentExJmp, (void*)HookedD3D9PresentEx)) {
            Log("[D3D9] Inline hook installed: Device9Ex::PresentEx at 0x%p", (void*)g_D3D9PresentExAddr);
        } else {
            Log("[D3D9] WARN: Failed to install PresentEx inline hook");
        }
    }

    DestroyWindow(hwnd9);
    UnregisterClassW(wc9.lpszClassName, wc9.hInstance);

    Log("[D3D9] InstallD3D9Hook: %s (restore-call-repatch inline hooks)", ok ? "OK" : "PARTIAL");
    return ok;
}

// ══════════════════════════════════════════════════════════════════════════
//  DIRECTDRAW HOOK (for legacy emulators using DirectDraw / Direct3D 7)
//  Fusion, Gens, Kega Fusion, ZSNES, etc.
// ══════════════════════════════════════════════════════════════════════════

// DDraw / D3D7 GUIDs (defined inline to avoid ddraw.h / d3d.h dependency)
static const GUID S4W_IID_IDirectDraw7 =
    { 0x15e65ec0, 0x3b9c, 0x11d2, { 0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b } };
// IDirect3D7 — for QueryInterface from IDirectDraw7
static const GUID S4W_IID_IDirect3D7 =
    { 0xf5049e77, 0x4861, 0x11d2, { 0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8 } };
// IDirect3DHALDevice — hardware-accelerated D3D7 device CLSID
static const GUID S4W_IID_IDirect3DHALDevice =
    { 0x84E63dE0, 0x46AA, 0x11CF, { 0x81, 0x30, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
// IDirect3DRGBDevice — software reference device (always creatable, same D3DIM700 vtable)
static const GUID S4W_IID_IDirect3DRGBDevice =
    { 0xe6de8b64, 0x3b55, 0x11cf, { 0xa8, 0xe4, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

// IDirect3D7 vtable indices
#define D3D7VT_CREATEDEVICE   4   // IDirect3D7::CreateDevice
// IDirect3DDevice7 vtable indices
#define D3D7DEVVT_ENDSCENE       6   // IDirect3DDevice7::EndScene
#define D3D7DEVVT_GETRENDERTARGET 9  // IDirect3DDevice7::GetRenderTarget

typedef HRESULT(STDMETHODCALLTYPE* PFN_D3D7CreateDevice)(
    void* self, const GUID* rclsid, void* pDDS, void** ppDevice);
typedef HRESULT(STDMETHODCALLTYPE* PFN_D3D7EndScene)(void* self);
typedef HRESULT(STDMETHODCALLTYPE* PFN_D3D7GetRenderTarget)(void* self, void** ppSurface);

// Function pointer for DirectDrawCreateEx (loaded dynamically from ddraw.dll)
typedef HRESULT(WINAPI* PFN_DirectDrawCreateEx)(GUID*, void**, REFIID, IUnknown*);

// IDirectDrawSurface7 vtable indices
#define DDSVT_BLT        5
#define DDSVT_FLIP       11
#define DDSVT_GETDC      17
#define DDSVT_RELEASEDC  26

// IDirectDraw7 vtable indices
#define DDVT_RELEASE             2
#define DDVT_CREATESURFACE       6
#define DDVT_SETCOOPERATIVELEVEL 20

// DDSURFACEDESC2 layout constants (no header needed)
#ifdef _WIN64
#define S4W_DDSURFACEDESC2_SIZE    132
#define S4W_DDSURFACEDESC2_CAPS    112   // offset of ddsCaps.dwCaps
#else
#define S4W_DDSURFACEDESC2_SIZE    124
#define S4W_DDSURFACEDESC2_CAPS    104
#endif
#define S4W_DDSD_CAPS              0x00000001
#define S4W_DDSCAPS_PRIMARYSURFACE 0x00000200
#define S4W_DDSCL_NORMAL           0x00000008

// DDraw Blt signature (COM method with implicit this)
typedef HRESULT(STDMETHODCALLTYPE* PFN_DDrawBlt)(
    void* self, RECT* destRect, void* srcSurface, RECT* srcRect, DWORD flags, void* bltFx);
// DDraw Flip signature
typedef HRESULT(STDMETHODCALLTYPE* PFN_DDrawFlip)(
    void* self, void* targetOverride, DWORD flags);
// DDraw BltFast signature
typedef HRESULT(STDMETHODCALLTYPE* PFN_DDrawBltFast)(
    void* self, DWORD dwX, DWORD dwY, void* srcSurface, RECT* srcRect, DWORD flags);
// DDraw GetAttachedSurface
typedef HRESULT(STDMETHODCALLTYPE* PFN_DDrawGetAttachedSurface)(
    void* self, void* lpDDSCaps, void** lplpDDAttachedSurface);
// DDraw GetDC / ReleaseDC
typedef HRESULT(STDMETHODCALLTYPE* PFN_DDrawGetDC)(void* self, HDC* hdc);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DDrawReleaseDC)(void* self, HDC hdc);
typedef ULONG(STDMETHODCALLTYPE* PFN_DDrawSurfRelease)(void* self);
// DDraw IDirectDraw7::CreateSurface
typedef HRESULT(STDMETHODCALLTYPE* PFN_DDrawCreateSurface)(
    void* self, void* lpDesc, void** lplpSurface, void* pUnkOuter);

// IDirectDrawSurface7 vtable indices for BltFast / Flip / GetAttachedSurface
#define DDSVT_BLTFAST             7
#define DDSVT_FLIP                11
#define DDSVT_GETATTACHEDSURFACE  12
#define DDSVT_LOCK                25
#define DDSVT_UNLOCK              32
// DDSCAPS_BACKBUFFER for GetAttachedSurface
#define S4W_DDSCAPS_BACKBUFFER    0x00000004
// DDraw Lock flags
#define S4W_DDLOCK_WAIT           0x00000001
#define S4W_DDLOCK_WRITEONLY      0x00000020
// DDSURFACEDESC2 field offsets (x86) used when locking
#define S4W_DDSD_HEIGHT_OFF       8
#define S4W_DDSD_WIDTH_OFF        12
#define S4W_DDSD_PITCH_OFF        16
#define S4W_DDSD_LPSURFACE_OFF    36
#define S4W_DDSD_BITCOUNT_OFF     84   // ddpfPixelFormat.dwRGBBitCount

// ── DDraw globals ────────────────────────────────────────────────────────
static BYTE* g_DDrawBltAddr          = nullptr;
static BYTE  g_DDrawBltOrig[14]      = {};
static BYTE  g_DDrawBltJmp[14]       = {};
static BYTE* g_DDrawFlipAddr         = nullptr;
static BYTE  g_DDrawFlipOrig[14]     = {};
static BYTE  g_DDrawFlipJmp[14]      = {};
static int   g_DDrawFlipCalls        = 0;
static BYTE* g_DDrawBltFastAddr      = nullptr;
static BYTE  g_DDrawBltFastOrig[14]  = {};
static BYTE  g_DDrawBltFastJmp[14]   = {};
static int   g_DDrawBltFastCalls     = 0;
static BYTE* g_DDrawCSAddr           = nullptr;  // IDirectDraw7::CreateSurface
static BYTE  g_DDrawCSOrig[14]       = {};
static BYTE  g_DDrawCSJmp[14]        = {};
static int   g_DDrawCSCalls          = 0;
static bool  g_DDrawHALHooked        = false;    // set true once HAL vtable hooked
static bool  g_DDrawHooked           = false;

// ── IDirectDrawSurface v1 hooks (separate vtable from v7) ───────────────
// Kega Fusion and similar emulators use DirectDrawCreate (v1 API) which
// gives IDirectDrawSurface objects whose vtable pointers differ from the
// IDirectDrawSurface7 vtable we hook above.  We probe DirectDrawCreate at
// init time and install a second set of hooks if the addresses differ.
static BYTE* g_DDrawBltV1Addr        = nullptr;
static BYTE  g_DDrawBltV1Orig[14]    = {};
static BYTE  g_DDrawBltV1Jmp[14]     = {};
static BYTE* g_DDrawFlipV1Addr       = nullptr;
static BYTE  g_DDrawFlipV1Orig[14]   = {};
static BYTE  g_DDrawFlipV1Jmp[14]    = {};
static int   g_DDrawFlipV1Calls      = 0;
static BYTE* g_DDrawBltFastV1Addr    = nullptr;
static BYTE  g_DDrawBltFastV1Orig[14]= {};
static BYTE  g_DDrawBltFastV1Jmp[14] = {};
static int   g_DDrawBltFastV1Calls   = 0;
static int   g_DDrawBltV1Calls       = 0;
static bool  g_DDrawV1Hooked         = false;

// ── Direct3D 7 hooks (CreateDevice → EndScene chain) ────────────────────
static BYTE* g_D3D7CDAddr            = nullptr;  // IDirect3D7::CreateDevice
static BYTE  g_D3D7CDOrig[14]        = {};
static BYTE  g_D3D7CDJmp[14]         = {};
static int   g_D3D7CDCalls           = 0;
static BYTE* g_D3D7ESAddr            = nullptr;  // IDirect3DDevice7::EndScene
static BYTE  g_D3D7ESOrig[14]        = {};
static BYTE  g_D3D7ESJmp[14]         = {};
static int   g_D3D7ESCalls           = 0;
static bool  g_D3D7ESHooked          = false;

// Cached scanline bitmap (recreated only when dimensions or settings change)
static HDC     g_DDrawMemDC          = nullptr;
static HBITMAP g_DDrawBmp            = nullptr;
static HGDIOBJ g_DDrawOldBmp         = nullptr;
static int     g_DDrawCachedW        = 0;
static int     g_DDrawCachedH        = 0;
static float   g_DDrawCachedHGap     = -1;
static float   g_DDrawCachedHThick   = -1;
static float   g_DDrawCachedVGap     = -1;
static float   g_DDrawCachedVThick   = -1;
static float   g_DDrawCachedHOpacity = -1;
static float   g_DDrawCachedVOpacity = -1;
static int     g_DDrawCachedHEn      = -1;
static int     g_DDrawCachedVEn      = -1;
static int     g_DDrawBltCalls       = 0;

// AlphaBlend from msimg32.dll (loaded once)
typedef BOOL(WINAPI* PFN_AlphaBlend)(HDC, int, int, int, int, HDC, int, int, int, int, BLENDFUNCTION);
static PFN_AlphaBlend g_pAlphaBlend = nullptr;

// ── Rebuild the cached scanline bitmap ───────────────────────────────────
static void DDrawRebuildScanlineBitmap(int w, int h) {
    if (g_DDrawMemDC && g_DDrawOldBmp) SelectObject(g_DDrawMemDC, g_DDrawOldBmp);
    if (g_DDrawBmp) { DeleteObject(g_DDrawBmp); g_DDrawBmp = nullptr; }
    if (!g_DDrawMemDC) g_DDrawMemDC = CreateCompatibleDC(nullptr);
    if (!g_DDrawMemDC) return;

    float hGap   = g_Shared->hGap;
    float hThick = g_Shared->hThickness;
    float hOp    = g_Shared->hOpacity;
    int   hEn    = g_Shared->hEnabled;
    float vGap   = g_Shared->vGap;
    float vThick = g_Shared->vThickness;
    float vOp    = g_Shared->vOpacity;
    int   vEn    = g_Shared->vEnabled;

    // Create 32-bit ARGB DIB section (bottom-up, so row 0 = bottom of image)
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;  // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    DWORD* bits = nullptr;
    g_DDrawBmp = CreateDIBSection(g_DDrawMemDC, &bmi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
    if (!g_DDrawBmp || !bits) return;
    g_DDrawOldBmp = SelectObject(g_DDrawMemDC, g_DDrawBmp);

    // Fill bitmap: transparent everywhere, black+alpha where scanlines go
    memset(bits, 0, (size_t)w * h * 4);

    // Horizontal scanlines
    if (hEn && hThick > 0 && hGap > 0) {
        int lineSpacing = (int)(hGap + hThick);
        if (lineSpacing < 1) lineSpacing = 1;
        BYTE alpha = (BYTE)(hOp * 255.0f);
        DWORD pixel = ((DWORD)alpha << 24); // premultiplied black with alpha
        for (int y = 0; y < h; y++) {
            int posInPattern = y % lineSpacing;
            if (posInPattern < (int)hThick) {
                DWORD* row = bits + (size_t)y * w;
                for (int x = 0; x < w; x++)
                    row[x] = pixel;
            }
        }
    }

    // Vertical scanlines (composited on top)
    if (vEn && vThick > 0 && vGap > 0) {
        int lineSpacing = (int)(vGap + vThick);
        if (lineSpacing < 1) lineSpacing = 1;
        BYTE alpha = (BYTE)(vOp * 255.0f);
        DWORD pixel = ((DWORD)alpha << 24);
        for (int x = 0; x < w; x++) {
            int posInPattern = x % lineSpacing;
            if (posInPattern < (int)vThick) {
                for (int y = 0; y < h; y++) {
                    DWORD* p = bits + (size_t)y * w + x;
                    // Composite: if horizontal already wrote something, take the darker
                    if (*p == 0)
                        *p = pixel;
                    else {
                        // Both H+V overlap → use max alpha (darker)
                        BYTE existingA = (BYTE)(*p >> 24);
                        if (alpha > existingA)
                            *p = pixel;
                    }
                }
            }
        }
    }

    // Cache state so we know when to rebuild
    g_DDrawCachedW  = w;   g_DDrawCachedH  = h;
    g_DDrawCachedHGap   = hGap;   g_DDrawCachedHThick = hThick;
    g_DDrawCachedHOpacity = hOp;   g_DDrawCachedHEn = hEn;
    g_DDrawCachedVGap   = vGap;   g_DDrawCachedVThick = vThick;
    g_DDrawCachedVOpacity = vOp;   g_DDrawCachedVEn = vEn;
}

// ── Check whether the cached bitmap needs rebuilding ─────────────────────
static bool DDrawNeedsRebuild(int w, int h) {
    if (!g_DDrawBmp) return true;
    if (w != g_DDrawCachedW || h != g_DDrawCachedH) return true;
    return g_DDrawCachedHGap   != g_Shared->hGap
        || g_DDrawCachedHThick != g_Shared->hThickness
        || g_DDrawCachedHOpacity != g_Shared->hOpacity
        || g_DDrawCachedHEn    != g_Shared->hEnabled
        || g_DDrawCachedVGap   != g_Shared->vGap
        || g_DDrawCachedVThick != g_Shared->vThickness
        || g_DDrawCachedVOpacity != g_Shared->vOpacity
        || g_DDrawCachedVEn    != g_Shared->vEnabled;
}

// ── Apply scanlines by locking the DDraw surface and writing pixels directly ──
// Used as primary path for DDraw surfaces where GDI AlphaBlend is unsupported
// (NVIDIA HAL surfaces return a valid DC but AlphaBlend always fails on them).
static bool DDrawApplyScanlinesViaLock(void* surface) {
    void** vtable = *(void***)surface;

    typedef HRESULT(STDMETHODCALLTYPE* PFN_Lock)(void*, RECT*, void*, DWORD, HANDLE);
    typedef HRESULT(STDMETHODCALLTYPE* PFN_Unlock)(void*, RECT*);

    // Lock the surface — fills the desc with pitch/pointer/format
    BYTE desc[256] = {};
    *(DWORD*)desc = S4W_DDSURFACEDESC2_SIZE;
    HRESULT hr = ((PFN_Lock)vtable[DDSVT_LOCK])(
        surface, nullptr, desc, S4W_DDLOCK_WAIT | S4W_DDLOCK_WRITEONLY, nullptr);
    if (FAILED(hr)) {
        static int s_lockFail = 0;
        if (++s_lockFail <= 3)
            Log("[DDRAW] Lock failed hr=0x%08X surface=0x%p", hr, surface);
        return false;
    }

    DWORD bitCount = *(DWORD*)(desc + S4W_DDSD_BITCOUNT_OFF);
    LONG  pitch    = *(LONG*) (desc + S4W_DDSD_PITCH_OFF);
    void* pixels   = *(void**)(desc + S4W_DDSD_LPSURFACE_OFF);
    int   surfW    = (int)*(DWORD*)(desc + S4W_DDSD_WIDTH_OFF);
    int   surfH    = (int)*(DWORD*)(desc + S4W_DDSD_HEIGHT_OFF);

    static int s_lockLog = 0;
    if (++s_lockLog <= 2)
        Log("[DDRAW] Lock OK: %dx%d %dbpp pitch=%d pixels=0x%p",
            surfW, surfH, bitCount, pitch, pixels);

    if (bitCount != 32 || !pixels || pitch <= 0 || surfW <= 0 || surfH <= 0) {
        if (s_lockLog <= 2)
            Log("[DDRAW] Lock: unsupported format or null ptr — skipping");
        ((PFN_Unlock)vtable[DDSVT_UNLOCK])(surface, nullptr);
        return false;
    }

    float hOp    = g_Shared->hOpacity;
    int   hEn    = g_Shared->hEnabled;
    float hGap   = g_Shared->hGap;
    float hThick = g_Shared->hThickness;
    float vOp    = g_Shared->vOpacity;
    int   vEn    = g_Shared->vEnabled;
    float vGap   = g_Shared->vGap;
    float vThick = g_Shared->vThickness;

    // Build per-row darkening mask: 1 = apply scanline, 0 = skip
    // We process H and V scanlines together: darken a pixel if it falls
    // in either an H or V scanline, using the appropriate opacity.
    int hSpacing = (hEn && hThick > 0 && hGap > 0) ? max(1, (int)(hGap + hThick)) : 0;
    int hThickI  = hSpacing ? (int)hThick : 0;
    DWORD hFactor = hSpacing ? (DWORD)max(0, min(256, (int)((1.0f - hOp) * 256.0f))) : 256;

    int vSpacing = (vEn && vThick > 0 && vGap > 0) ? max(1, (int)(vGap + vThick)) : 0;
    int vThickI  = vSpacing ? (int)vThick : 0;
    DWORD vFactor = vSpacing ? (DWORD)max(0, min(256, (int)((1.0f - vOp) * 256.0f))) : 256;

    for (int y = 0; y < surfH; y++) {
        bool hLine = hSpacing && ((y % hSpacing) < hThickI);
        DWORD* row = (DWORD*)((BYTE*)pixels + (size_t)y * pitch);

        for (int x = 0; x < surfW; x++) {
            bool vLine = vSpacing && ((x % vSpacing) < vThickI);
            if (!hLine && !vLine) continue;

            // Use the darkest (lowest) factor when both H and V apply
            DWORD f = 256;
            if (hLine && vLine) f = min(hFactor, vFactor);
            else if (hLine)     f = hFactor;
            else                f = vFactor;

            DWORD px = row[x];
            BYTE r = (BYTE)(((px >> 16) & 0xFF) * f >> 8);
            BYTE g = (BYTE)(((px >>  8) & 0xFF) * f >> 8);
            BYTE b = (BYTE)(( px        & 0xFF) * f >> 8);
            row[x] = (px & 0xFF000000) | ((DWORD)r << 16) | ((DWORD)g << 8) | b;
        }
    }

    ((PFN_Unlock)vtable[DDSVT_UNLOCK])(surface, nullptr);
    return true;
}

// ── Apply scanlines onto a DDraw surface ─────────────────────────────────
// Tries GDI AlphaBlend first; falls back to direct Lock on failure.
// The Lock path is permanently preferred once AlphaBlend fails once,
// to avoid paying GetDC overhead every frame on unsupported surfaces.
static bool g_DDrawUseLock = false;

static void DDrawApplyScanlines(void* surface) {
    if (!g_Shared || !g_Shared->active) return;
    if (!g_Shared->hEnabled && !g_Shared->vEnabled) return;

    // ── Fast path: Lock (used after first AlphaBlend failure) ────────────
    if (g_DDrawUseLock) {
        DDrawApplyScanlinesViaLock(surface);
        return;
    }

    // ── GDI path: AlphaBlend ─────────────────────────────────────────────
    if (!g_pAlphaBlend) {
        HMODULE msimg = LoadLibraryW(L"msimg32.dll");
        if (msimg) g_pAlphaBlend = (PFN_AlphaBlend)GetProcAddress(msimg, "AlphaBlend");
        if (!g_pAlphaBlend) {
            Log("[DDRAW] AlphaBlend not found — switching to Lock path");
            g_DDrawUseLock = true;
            DDrawApplyScanlinesViaLock(surface);
            return;
        }
    }

    void** vtable = *(void***)surface;
    HDC hdc = nullptr;
    HRESULT hr = ((PFN_DDrawGetDC)vtable[DDSVT_GETDC])(surface, &hdc);
    if (FAILED(hr) || !hdc) {
        static int s_getDCFail = 0;
        if (++s_getDCFail <= 3)
            Log("[DDRAW] GetDC failed hr=0x%08X — switching to Lock path", hr);
        g_DDrawUseLock = true;
        DDrawApplyScanlinesViaLock(surface);
        return;
    }

    RECT rc;
    GetClipBox(hdc, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    bool blendOk = false;
    if (w > 0 && h > 0) {
        if (DDrawNeedsRebuild(w, h)) DDrawRebuildScanlineBitmap(w, h);
        if (g_DDrawBmp && g_DDrawMemDC) {
            BLENDFUNCTION bf = {};
            bf.BlendOp             = AC_SRC_OVER;
            bf.SourceConstantAlpha = 255;
            bf.AlphaFormat         = AC_SRC_ALPHA;
            blendOk = (g_pAlphaBlend(hdc, 0, 0, w, h, g_DDrawMemDC, 0, 0, w, h, bf) != FALSE);
        }
    }

    ((PFN_DDrawReleaseDC)vtable[DDSVT_RELEASEDC])(surface, hdc);

    if (!blendOk) {
        Log("[DDRAW] AlphaBlend failed — switching permanently to Lock path");
        g_DDrawUseLock = true;
        DDrawApplyScanlinesViaLock(surface);
    }
}

// ── Hooked DDraw Blt (restore-call-repatch) ──────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedDDrawBlt(
    void* self, RECT* destRect, void* srcSurface, RECT* srcRect, DWORD flags, void* bltFx)
{
    g_DDrawBltCalls++;
    if (g_DDrawBltCalls == 1 || g_DDrawBltCalls == 60 || (g_DDrawBltCalls % 600) == 0)
        Log("[DDRAW] HookedBlt call #%d — self=0x%p src=0x%p flags=0x%08X",
            g_DDrawBltCalls, self, srcSurface, flags);

    // Draw scanlines on the SOURCE surface BEFORE the Blt.
    // Fusion uses DDBLT_ASYNC: the GPU copies src→dst asynchronously after this
    // call returns.  Any GDI drawn on the destination after-the-fact is overwritten
    // by that GPU copy.  Drawing on src first bakes the scanlines into what the GPU
    // copies, so they survive the async transfer to the primary.
    __try {
        if (srcSurface) {
            if (!g_Shared) OpenSharedMem();
            if (g_Shared && g_Shared->active)
                DDrawApplyScanlines(srcSurface);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[DDRAW] EXCEPTION 0x%08X drawing scanlines on Blt src", GetExceptionCode());
    }

    // Restore original bytes
    DWORD oldProt;
    VirtualProtect(g_DDrawBltAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_DDrawBltAddr, g_DDrawBltOrig, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_DDrawBltAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_DDrawBltAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    // Call original Blt — GPU copies src (now with scanlines) → dst
    HRESULT hr = ((PFN_DDrawBlt)g_DDrawBltAddr)(self, destRect, srcSurface, srcRect, flags, bltFx);

    // Re-patch
    VirtualProtect(g_DDrawBltAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_DDrawBltAddr, g_DDrawBltJmp, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_DDrawBltAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_DDrawBltAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    return hr;
}

// ── Hooked DDraw Flip (restore-call-repatch) ─────────────────────────────
// Fusion (and most D3D7-via-DDraw games) present via Flip on the primary
// surface. We draw scanlines on the back buffer BEFORE flipping so they
// appear on the next visible frame without tearing.
static HRESULT STDMETHODCALLTYPE HookedDDrawFlip(
    void* self, void* targetOverride, DWORD flags)
{
    g_DDrawFlipCalls++;
    if (g_DDrawFlipCalls == 1)
        Log("[DDRAW] HookedFlip FIRED — self=0x%p flags=0x%08X", self, flags);

    // Apply scanlines to the back buffer (the surface that will become front
    // after Flip). Falls back to drawing on `self` if no back buffer found.
    __try {
        if (!g_Shared) OpenSharedMem();
        if (g_Shared && g_Shared->active && self) {
            void** vt = *(void***)self;
            // DDSCAPS2 = { dwCaps, dwCaps2, dwCaps3, dwCaps4 }
            DWORD caps[4] = { S4W_DDSCAPS_BACKBUFFER, 0, 0, 0 };
            void* backBuf = nullptr;
            HRESULT hrAS = ((PFN_DDrawGetAttachedSurface)vt[DDSVT_GETATTACHEDSURFACE])(
                self, caps, &backBuf);
            if (SUCCEEDED(hrAS) && backBuf) {
                DDrawApplyScanlines(backBuf);
                // Release the AddRef'd back buffer reference
                void** bvt = *(void***)backBuf;
                ((PFN_DDrawSurfRelease)bvt[2])(backBuf);
            } else {
                // No back buffer attached — try drawing directly on self
                DDrawApplyScanlines(self);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[DDRAW] EXCEPTION 0x%08X in HookedFlip pre-Flip", GetExceptionCode());
    }

    // Restore original bytes
    DWORD oldProt;
    VirtualProtect(g_DDrawFlipAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_DDrawFlipAddr, g_DDrawFlipOrig, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_DDrawFlipAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_DDrawFlipAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    // Call original Flip
    HRESULT hr = ((PFN_DDrawFlip)g_DDrawFlipAddr)(self, targetOverride, flags);

    // Re-patch
    VirtualProtect(g_DDrawFlipAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_DDrawFlipAddr, g_DDrawFlipJmp, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_DDrawFlipAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_DDrawFlipAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    return hr;
}

// ── Hooked DDraw BltFast (restore-call-repatch) ──────────────────────────
// Used by most windowed-mode DDraw emulators (Fusion, Kega, etc.) to copy
// the offscreen Genesis framebuffer onto the primary surface every frame.
static HRESULT STDMETHODCALLTYPE HookedDDrawBltFast(
    void* self, DWORD dwX, DWORD dwY, void* srcSurface, RECT* srcRect, DWORD flags)
{
    g_DDrawBltFastCalls++;
    if (g_DDrawBltFastCalls == 1 ||
        g_DDrawBltFastCalls == 60 ||
        (g_DDrawBltFastCalls % 600) == 0)
        Log("[DDRAW] HookedBltFast call #%d — self=0x%p src=0x%p (%lu,%lu)",
            g_DDrawBltFastCalls, self, srcSurface, dwX, dwY);

    // Draw scanlines on the SOURCE surface BEFORE BltFast (same async-blit reason as Blt).
    __try {
        if (srcSurface) {
            if (!g_Shared) OpenSharedMem();
            if (g_Shared && g_Shared->active)
                DDrawApplyScanlines(srcSurface);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[DDRAW] EXCEPTION 0x%08X drawing scanlines on BltFast src", GetExceptionCode());
    }

    // Restore original bytes
    DWORD oldProt;
    VirtualProtect(g_DDrawBltFastAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_DDrawBltFastAddr, g_DDrawBltFastOrig, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_DDrawBltFastAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_DDrawBltFastAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    // Call original BltFast — copies src (now with scanlines) → primary
    HRESULT hr = ((PFN_DDrawBltFast)g_DDrawBltFastAddr)(self, dwX, dwY, srcSurface, srcRect, flags);

    // Re-patch
    VirtualProtect(g_DDrawBltFastAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_DDrawBltFastAddr, g_DDrawBltFastJmp, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_DDrawBltFastAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_DDrawBltFastAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    return hr;
}

// ── IDirectDrawSurface v1 hooks (same pattern, separate globals) ─────────

static HRESULT STDMETHODCALLTYPE HookedDDrawBltV1(
    void* self, RECT* destRect, void* srcSurface, RECT* srcRect, DWORD flags, void* bltFx)
{
    g_DDrawBltV1Calls++;
    if (g_DDrawBltV1Calls == 1 || g_DDrawBltV1Calls == 60 || (g_DDrawBltV1Calls % 600) == 0)
        Log("[DDRAWv1] HookedBlt call #%d — self=0x%p src=0x%p flags=0x%08X",
            g_DDrawBltV1Calls, self, srcSurface, flags);

    DWORD oldProt;
    __try {
        if (srcSurface) {
            if (!g_Shared) OpenSharedMem();
            if (g_Shared && g_Shared->active)
                DDrawApplyScanlines(srcSurface);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[DDRAWv1] EXCEPTION 0x%08X drawing scanlines on BltV1 src", GetExceptionCode());
    }

    VirtualProtect(g_DDrawBltV1Addr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_DDrawBltV1Addr, g_DDrawBltV1Orig, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_DDrawBltV1Addr, HOOK_JMP_SIZE);
    VirtualProtect(g_DDrawBltV1Addr, HOOK_JMP_SIZE, oldProt, &oldProt);

    HRESULT hr = ((PFN_DDrawBlt)g_DDrawBltV1Addr)(self, destRect, srcSurface, srcRect, flags, bltFx);

    VirtualProtect(g_DDrawBltV1Addr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_DDrawBltV1Addr, g_DDrawBltV1Jmp, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_DDrawBltV1Addr, HOOK_JMP_SIZE);
    VirtualProtect(g_DDrawBltV1Addr, HOOK_JMP_SIZE, oldProt, &oldProt);

    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedDDrawFlipV1(void* self, void* targetOverride, DWORD flags)
{
    g_DDrawFlipV1Calls++;
    if (g_DDrawFlipV1Calls == 1)
        Log("[DDRAWv1] HookedFlip FIRED — self=0x%p flags=0x%08X", self, flags);

    __try {
        if (!g_Shared) OpenSharedMem();
        if (g_Shared && g_Shared->active && self) {
            void** vt = *(void***)self;
            DWORD caps[4] = { S4W_DDSCAPS_BACKBUFFER, 0, 0, 0 };
            void* backBuf = nullptr;
            HRESULT hrAS = ((PFN_DDrawGetAttachedSurface)vt[DDSVT_GETATTACHEDSURFACE])(
                self, caps, &backBuf);
            if (SUCCEEDED(hrAS) && backBuf) {
                DDrawApplyScanlines(backBuf);
                void** bvt = *(void***)backBuf;
                ((PFN_DDrawSurfRelease)bvt[2])(backBuf);
            } else {
                DDrawApplyScanlines(self);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[DDRAWv1] EXCEPTION 0x%08X in FlipV1 pre-Flip", GetExceptionCode());
    }

    DWORD oldProt;
    VirtualProtect(g_DDrawFlipV1Addr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_DDrawFlipV1Addr, g_DDrawFlipV1Orig, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_DDrawFlipV1Addr, HOOK_JMP_SIZE);
    VirtualProtect(g_DDrawFlipV1Addr, HOOK_JMP_SIZE, oldProt, &oldProt);

    HRESULT hr = ((PFN_DDrawFlip)g_DDrawFlipV1Addr)(self, targetOverride, flags);

    VirtualProtect(g_DDrawFlipV1Addr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_DDrawFlipV1Addr, g_DDrawFlipV1Jmp, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_DDrawFlipV1Addr, HOOK_JMP_SIZE);
    VirtualProtect(g_DDrawFlipV1Addr, HOOK_JMP_SIZE, oldProt, &oldProt);

    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedDDrawBltFastV1(
    void* self, DWORD dwX, DWORD dwY, void* srcSurface, RECT* srcRect, DWORD flags)
{
    g_DDrawBltFastV1Calls++;
    if (g_DDrawBltFastV1Calls == 1 || g_DDrawBltFastV1Calls == 60 ||
        (g_DDrawBltFastV1Calls % 600) == 0)
        Log("[DDRAWv1] HookedBltFast call #%d — self=0x%p src=0x%p (%lu,%lu)",
            g_DDrawBltFastV1Calls, self, srcSurface, dwX, dwY);

    __try {
        if (srcSurface) {
            if (!g_Shared) OpenSharedMem();
            if (g_Shared && g_Shared->active)
                DDrawApplyScanlines(srcSurface);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[DDRAWv1] EXCEPTION 0x%08X drawing scanlines on BltFastV1 src", GetExceptionCode());
    }

    DWORD oldProt;
    VirtualProtect(g_DDrawBltFastV1Addr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_DDrawBltFastV1Addr, g_DDrawBltFastV1Orig, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_DDrawBltFastV1Addr, HOOK_JMP_SIZE);
    VirtualProtect(g_DDrawBltFastV1Addr, HOOK_JMP_SIZE, oldProt, &oldProt);

    HRESULT hr = ((PFN_DDrawBltFast)g_DDrawBltFastV1Addr)(self, dwX, dwY, srcSurface, srcRect, flags);

    VirtualProtect(g_DDrawBltFastV1Addr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_DDrawBltFastV1Addr, g_DDrawBltFastV1Jmp, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_DDrawBltFastV1Addr, HOOK_JMP_SIZE);
    VirtualProtect(g_DDrawBltFastV1Addr, HOOK_JMP_SIZE, oldProt, &oldProt);

    return hr;
}

// ── Detect HAL surface vtable and re-hook if different from HEL ──────────
// Called from HookedDDrawCreateSurface when the game creates its real surfaces.
// If the game uses HAL (hardware-accelerated) DDraw, the surface vtable entries
// for Blt/BltFast/Flip will differ from the dummy HEL primary we used at init.
// We uninstall the old hooks and reinstall on the correct addresses.
static void DDrawUpdateSurfaceHooks(void* pSurface) {
    if (!pSurface || g_DDrawHALHooked) return;
    void** vt = *(void***)pSurface;
    BYTE* newBlt     = (BYTE*)vt[DDSVT_BLT];
    BYTE* newBltFast = (BYTE*)vt[DDSVT_BLTFAST];
    BYTE* newFlip    = (BYTE*)vt[DDSVT_FLIP];

    // If all three match what we already have, nothing to do
    if (newBlt == g_DDrawBltAddr &&
        newBltFast == g_DDrawBltFastAddr &&
        newFlip == g_DDrawFlipAddr)
        return;

    Log("[DDRAW] HAL surface vtable differs — updating hooks");
    Log("[DDRAW] HAL Blt=0x%p BltFast=0x%p Flip=0x%p", newBlt, newBltFast, newFlip);

    // Restore the old (HEL) hooks so the original bytes are correct again
    DWORD op;
    if (g_DDrawBltAddr && g_DDrawHooked) {
        VirtualProtect(g_DDrawBltAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
        memcpy(g_DDrawBltAddr, g_DDrawBltOrig, HOOK_JMP_SIZE);
        FlushInstructionCache(GetCurrentProcess(), g_DDrawBltAddr, HOOK_JMP_SIZE);
        VirtualProtect(g_DDrawBltAddr, HOOK_JMP_SIZE, op, &op);
    }
    if (g_DDrawBltFastAddr && g_DDrawHooked) {
        VirtualProtect(g_DDrawBltFastAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
        memcpy(g_DDrawBltFastAddr, g_DDrawBltFastOrig, HOOK_JMP_SIZE);
        FlushInstructionCache(GetCurrentProcess(), g_DDrawBltFastAddr, HOOK_JMP_SIZE);
        VirtualProtect(g_DDrawBltFastAddr, HOOK_JMP_SIZE, op, &op);
    }
    if (g_DDrawFlipAddr && g_DDrawHooked) {
        VirtualProtect(g_DDrawFlipAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
        memcpy(g_DDrawFlipAddr, g_DDrawFlipOrig, HOOK_JMP_SIZE);
        FlushInstructionCache(GetCurrentProcess(), g_DDrawFlipAddr, HOOK_JMP_SIZE);
        VirtualProtect(g_DDrawFlipAddr, HOOK_JMP_SIZE, op, &op);
    }

    // Point globals to the new HAL addresses and reinstall
    g_DDrawBltAddr     = newBlt;
    g_DDrawBltFastAddr = newBltFast;
    g_DDrawFlipAddr    = newFlip;

    bool ok = true;
    if (!InstallHookAt(g_DDrawBltAddr, g_DDrawBltOrig, g_DDrawBltJmp, (void*)HookedDDrawBlt))
        { Log("[DDRAW] HAL Blt hook failed"); ok = false; }
    if (!InstallHookAt(g_DDrawBltFastAddr, g_DDrawBltFastOrig, g_DDrawBltFastJmp, (void*)HookedDDrawBltFast))
        { Log("[DDRAW] HAL BltFast hook failed"); ok = false; }
    if (!InstallHookAt(g_DDrawFlipAddr, g_DDrawFlipOrig, g_DDrawFlipJmp, (void*)HookedDDrawFlip))
        { Log("[DDRAW] HAL Flip hook failed"); ok = false; }

    if (ok) {
        g_DDrawHALHooked = true;
        Log("[DDRAW] HAL surface hooks installed OK (Blt/BltFast/Flip)");
    }
}

// ── Hooked IDirectDraw7::CreateSurface ───────────────────────────────────
// Intercepts every surface creation so we can detect the HAL vtable and
// update our Blt/BltFast/Flip hooks to the correct addresses.
static HRESULT STDMETHODCALLTYPE HookedDDrawCreateSurface(
    void* self, void* lpDesc, void** lplpSurface, void* pUnkOuter)
{
    // Restore original bytes
    DWORD oldProt;
    VirtualProtect(g_DDrawCSAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_DDrawCSAddr, g_DDrawCSOrig, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_DDrawCSAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_DDrawCSAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    // Call original CreateSurface
    HRESULT hr = ((PFN_DDrawCreateSurface)g_DDrawCSAddr)(self, lpDesc, lplpSurface, pUnkOuter);

    // Re-patch
    VirtualProtect(g_DDrawCSAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_DDrawCSAddr, g_DDrawCSJmp, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_DDrawCSAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_DDrawCSAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    g_DDrawCSCalls++;
    if (g_DDrawCSCalls <= 3)
        Log("[DDRAW] CreateSurface call #%d hr=0x%08X surface=0x%p",
            g_DDrawCSCalls, hr, lplpSurface ? *lplpSurface : nullptr);

    if (SUCCEEDED(hr) && lplpSurface && *lplpSurface) {
        __try { DDrawUpdateSurfaceHooks(*lplpSurface); }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            Log("[DDRAW] EXCEPTION 0x%08X in DDrawUpdateSurfaceHooks", GetExceptionCode());
        }
    }
    return hr;
}

// ═════════════════════════════════════════════════════════════════════════
// ── Direct3D 7 EndScene hook ──
// Fusion.exe (and similar D3D7 HAL emulators) render entirely through the
// D3D7 pipeline — no GDI, no DDraw Blt/Flip COM dispatch.
// We intercept IDirect3D7::CreateDevice (obtained from IDirectDraw7 via QI)
// to get the real HAL device, then hook IDirect3DDevice7::EndScene on it.
// EndScene fires once per frame, after all draw calls and before the flip.
// We call GetRenderTarget to get the back buffer surface and draw scanlines
// via the existing DDrawApplyScanlines path (GetDC → AlphaBlend → ReleaseDC).
// ═════════════════════════════════════════════════════════════════════════

// ── HookedD3D7EndScene ────────────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedD3D7EndScene(void* self) {
    g_D3D7ESCalls++;
    if (g_D3D7ESCalls <= 3 || g_D3D7ESCalls % 600 == 0)
        Log("[D3D7] EndScene call #%d device=0x%p", g_D3D7ESCalls, self);

    DWORD oldProt;
    VirtualProtect(g_D3D7ESAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_D3D7ESAddr, g_D3D7ESOrig, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_D3D7ESAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_D3D7ESAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    // Call real EndScene
    HRESULT hr = ((PFN_D3D7EndScene)g_D3D7ESAddr)(self);

    // Apply scanlines on the render target (back buffer)
    __try {
        if (SUCCEEDED(hr)) {
            if (!g_Shared) OpenSharedMem();
            // GetRenderTarget — vtable[9] of IDirect3DDevice7
            void** devVtable = *(void***)self;
            void* pRT = nullptr;
            HRESULT hrRT = ((PFN_D3D7GetRenderTarget)devVtable[D3D7DEVVT_GETRENDERTARGET])(self, &pRT);
            if (SUCCEEDED(hrRT) && pRT) {
                DDrawApplyScanlines(pRT);
                // Release the AddRef from GetRenderTarget
                typedef ULONG(STDMETHODCALLTYPE* PFN_Rel)(void*);
                ((PFN_Rel)(*(void***)pRT)[2])(pRT);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[D3D7] EXCEPTION 0x%08X in EndScene scanline apply", GetExceptionCode());
    }

    VirtualProtect(g_D3D7ESAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_D3D7ESAddr, g_D3D7ESJmp, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_D3D7ESAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_D3D7ESAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    return hr;
}

// ── HookedD3D7CreateDevice ────────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedD3D7CreateDevice(
    void* self, const GUID* rclsid, void* pDDS, void** ppDevice)
{
    g_D3D7CDCalls++;
    Log("[D3D7] CreateDevice call #%d — hooking EndScene on real HAL device", g_D3D7CDCalls);

    DWORD oldProt;
    VirtualProtect(g_D3D7CDAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_D3D7CDAddr, g_D3D7CDOrig, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_D3D7CDAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_D3D7CDAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    HRESULT hr = ((PFN_D3D7CreateDevice)g_D3D7CDAddr)(self, rclsid, pDDS, ppDevice);

    __try {
        if (SUCCEEDED(hr) && ppDevice && *ppDevice && !g_D3D7ESHooked) {
            void* pDevice = *ppDevice;
            void** devVtable = *(void***)pDevice;
            g_D3D7ESAddr = (BYTE*)devVtable[D3D7DEVVT_ENDSCENE];
            Log("[D3D7] Real HAL device=0x%p EndScene at 0x%p", pDevice, (void*)g_D3D7ESAddr);
            if (InstallHookAt(g_D3D7ESAddr, g_D3D7ESOrig, g_D3D7ESJmp, (void*)HookedD3D7EndScene)) {
                g_D3D7ESHooked = true;
                Log("[D3D7] EndScene hooked OK at 0x%p", (void*)g_D3D7ESAddr);
            } else {
                Log("[D3D7] EndScene hook FAILED");
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[D3D7] EXCEPTION 0x%08X in CreateDevice hook", GetExceptionCode());
    }

    VirtualProtect(g_D3D7CDAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_D3D7CDAddr, g_D3D7CDJmp, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_D3D7CDAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_D3D7CDAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    return hr;
}

// ═════════════════════════════════════════════════════════════════════════
// ── GDI hook (BitBlt / StretchBlt / StretchDIBits) ──
// Some old DDraw emulators (e.g. Fusion.exe) present their framebuffer via
// GDI rather than via IDirectDrawSurface::Blt. Hooking these GDI functions
// lets us apply scanlines onto whatever window-DC rendering they do.
// This hook is installed ONLY when ddraw.dll is loaded, so it doesn't touch
// any D3D11/D3D9/OpenGL pipeline that already works.
// ═════════════════════════════════════════════════════════════════════════
typedef BOOL(WINAPI* PFN_BitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);
typedef BOOL(WINAPI* PFN_StretchBlt)(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
typedef int (WINAPI* PFN_StretchDIBits)(HDC, int, int, int, int, int, int, int, int,
                                        const void*, const BITMAPINFO*, UINT, DWORD);

static BYTE* g_GdiBitBltAddr          = nullptr;
static BYTE  g_GdiBitBltOrig[14]      = {};
static BYTE  g_GdiBitBltJmp[14]       = {};
static int   g_GdiBitBltCalls         = 0;

static BYTE* g_GdiStretchBltAddr      = nullptr;
static BYTE  g_GdiStretchBltOrig[14]  = {};
static BYTE  g_GdiStretchBltJmp[14]   = {};
static int   g_GdiStretchBltCalls     = 0;

static BYTE* g_GdiStretchDIBitsAddr   = nullptr;
static BYTE  g_GdiStretchDIBitsOrig[14] = {};
static BYTE  g_GdiStretchDIBitsJmp[14]  = {};
static int   g_GdiStretchDIBitsCalls  = 0;

static bool  g_GDIHooked              = false;
// Re-entrancy guard: AlphaBlend internally calls BitBlt/StretchBlt, which
// would recurse into our hook. This TLS-like flag prevents infinite loops.
static thread_local bool tls_InGDIHook = false;

// ── Apply scanlines directly to a HDC (no DDraw surface needed) ──────────
static void GDIApplyScanlinesToHDC(HDC hdc, int w, int h) {
    if (!g_Shared || !g_Shared->active) return;
    if (!g_Shared->hEnabled && !g_Shared->vEnabled) return;
    if (w < 320 || h < 240) return;  // skip tiny blits (UI / cursor / tooltips)

    if (!g_pAlphaBlend) {
        HMODULE msimg = LoadLibraryW(L"msimg32.dll");
        if (msimg) g_pAlphaBlend = (PFN_AlphaBlend)GetProcAddress(msimg, "AlphaBlend");
        if (!g_pAlphaBlend) return;
    }

    if (DDrawNeedsRebuild(w, h))
        DDrawRebuildScanlineBitmap(w, h);

    if (g_DDrawBmp && g_DDrawMemDC) {
        BLENDFUNCTION bf = {};
        bf.BlendOp             = AC_SRC_OVER;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat         = AC_SRC_ALPHA;
        g_pAlphaBlend(hdc, 0, 0, w, h, g_DDrawMemDC, 0, 0, w, h, bf);
    }
}

// ── Decide whether a destination HDC should get scanlines ──
// We only apply to top-level window DCs owned by a visible window of our
// own process. This avoids scanlining memory DCs, tooltips, menus, etc.
static bool GDIShouldApply(HDC hdcDest, int w, int h) {
    if (tls_InGDIHook) return false;
    // Skip if a GPU API (D3D11/D3D9/OpenGL) has already claimed rendering for
    // this process.  Those APIs apply scanlines in their own Present/SwapBuffers
    // hook; letting the GDI hook also fire would double-apply the effect on any
    // incidental GDI blits the game makes (UI elements, debug overlays, etc.).
    if (g_Inited || g_GLInited || g_D3D9Inited) return false;
    HWND hwnd = WindowFromDC(hdcDest);
    if (!hwnd) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return false;
    if (!IsWindowVisible(hwnd)) return false;
    // Reject child/tool windows: only accept top-level application windows
    if (GetParent(hwnd) != nullptr) return false;
    RECT rc; if (!GetClientRect(hwnd, &rc)) return false;
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw < 320 || ch < 240) return false;
    // Blit must cover a reasonable portion of the client area
    if (w < cw / 2 || h < ch / 2) return false;
    return true;
}

// ── Hooked BitBlt ─────────────────────────────────────────────────────────
static BOOL WINAPI HookedGdiBitBlt(
    HDC hdcDest, int x, int y, int w, int h,
    HDC hdcSrc, int xSrc, int ySrc, DWORD rop)
{
    g_GdiBitBltCalls++;
    if (g_GdiBitBltCalls <= 3 || g_GdiBitBltCalls % 600 == 0)
        Log("[GDI] BitBlt call #%d dest=0x%p %dx%d rop=0x%08X",
            g_GdiBitBltCalls, hdcDest, w, h, rop);

    DWORD oldProt;
    VirtualProtect(g_GdiBitBltAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_GdiBitBltAddr, g_GdiBitBltOrig, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_GdiBitBltAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_GdiBitBltAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    BOOL ok = ((PFN_BitBlt)g_GdiBitBltAddr)(hdcDest, x, y, w, h, hdcSrc, xSrc, ySrc, rop);

    __try {
        if (ok && GDIShouldApply(hdcDest, w, h)) {
            tls_InGDIHook = true;
            if (!g_Shared) OpenSharedMem();
            GDIApplyScanlinesToHDC(hdcDest, w, h);
            tls_InGDIHook = false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        tls_InGDIHook = false;
        Log("[GDI] EXCEPTION 0x%08X in BitBlt scanline apply", GetExceptionCode());
    }

    VirtualProtect(g_GdiBitBltAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_GdiBitBltAddr, g_GdiBitBltJmp, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_GdiBitBltAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_GdiBitBltAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    return ok;
}

// ── Hooked StretchBlt ─────────────────────────────────────────────────────
static BOOL WINAPI HookedGdiStretchBlt(
    HDC hdcDest, int xD, int yD, int wD, int hD,
    HDC hdcSrc,  int xS, int yS, int wS, int hS, DWORD rop)
{
    g_GdiStretchBltCalls++;
    if (g_GdiStretchBltCalls <= 3 || g_GdiStretchBltCalls % 600 == 0)
        Log("[GDI] StretchBlt call #%d dest=0x%p %dx%d rop=0x%08X",
            g_GdiStretchBltCalls, hdcDest, wD, hD, rop);

    DWORD oldProt;
    VirtualProtect(g_GdiStretchBltAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_GdiStretchBltAddr, g_GdiStretchBltOrig, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_GdiStretchBltAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_GdiStretchBltAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    BOOL ok = ((PFN_StretchBlt)g_GdiStretchBltAddr)(hdcDest, xD, yD, wD, hD, hdcSrc, xS, yS, wS, hS, rop);

    __try {
        if (ok && GDIShouldApply(hdcDest, wD, hD)) {
            tls_InGDIHook = true;
            if (!g_Shared) OpenSharedMem();
            GDIApplyScanlinesToHDC(hdcDest, wD, hD);
            tls_InGDIHook = false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        tls_InGDIHook = false;
        Log("[GDI] EXCEPTION 0x%08X in StretchBlt scanline apply", GetExceptionCode());
    }

    VirtualProtect(g_GdiStretchBltAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_GdiStretchBltAddr, g_GdiStretchBltJmp, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_GdiStretchBltAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_GdiStretchBltAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    return ok;
}

// ── Hooked StretchDIBits ──────────────────────────────────────────────────
static int WINAPI HookedGdiStretchDIBits(
    HDC hdc, int xD, int yD, int wD, int hD,
    int xS, int yS, int wS, int hS,
    const void* bits, const BITMAPINFO* bmi, UINT usage, DWORD rop)
{
    g_GdiStretchDIBitsCalls++;
    if (g_GdiStretchDIBitsCalls <= 3 || g_GdiStretchDIBitsCalls % 600 == 0)
        Log("[GDI] StretchDIBits call #%d dest=0x%p %dx%d rop=0x%08X",
            g_GdiStretchDIBitsCalls, hdc, wD, hD, rop);

    DWORD oldProt;
    VirtualProtect(g_GdiStretchDIBitsAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_GdiStretchDIBitsAddr, g_GdiStretchDIBitsOrig, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_GdiStretchDIBitsAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_GdiStretchDIBitsAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    int scanlines = ((PFN_StretchDIBits)g_GdiStretchDIBitsAddr)(
        hdc, xD, yD, wD, hD, xS, yS, wS, hS, bits, bmi, usage, rop);

    __try {
        if (scanlines > 0 && GDIShouldApply(hdc, wD, hD)) {
            tls_InGDIHook = true;
            if (!g_Shared) OpenSharedMem();
            GDIApplyScanlinesToHDC(hdc, wD, hD);
            tls_InGDIHook = false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        tls_InGDIHook = false;
        Log("[GDI] EXCEPTION 0x%08X in StretchDIBits scanline apply", GetExceptionCode());
    }

    VirtualProtect(g_GdiStretchDIBitsAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(g_GdiStretchDIBitsAddr, g_GdiStretchDIBitsJmp, HOOK_JMP_SIZE);
    FlushInstructionCache(GetCurrentProcess(), g_GdiStretchDIBitsAddr, HOOK_JMP_SIZE);
    VirtualProtect(g_GdiStretchDIBitsAddr, HOOK_JMP_SIZE, oldProt, &oldProt);

    return scanlines;
}

// ── GDI hook installation ─────────────────────────────────────────────────
// Installed for ALL processes (not just DDraw ones).  GDIShouldApply() gates
// the actual scanline application: it returns false when D3D11/D3D9/OpenGL
// has claimed rendering, preventing interference.  For pure-GDI or D2D-HWND
// games (e.g. Clickteam Fusion software mode, Direct2D HWND render targets)
// this is the only presentation hook that can intercept frames.
static bool InstallGDIHook() {
    if (g_GDIHooked) return true;

    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) gdi = LoadLibraryW(L"gdi32.dll");
    if (!gdi) { Log("[GDI] gdi32.dll not loadable"); return false; }

    g_GdiBitBltAddr        = (BYTE*)GetProcAddress(gdi, "BitBlt");
    g_GdiStretchBltAddr    = (BYTE*)GetProcAddress(gdi, "StretchBlt");
    g_GdiStretchDIBitsAddr = (BYTE*)GetProcAddress(gdi, "StretchDIBits");

    bool any = false;
    if (g_GdiBitBltAddr && InstallHookAt(g_GdiBitBltAddr, g_GdiBitBltOrig, g_GdiBitBltJmp, (void*)HookedGdiBitBlt)) {
        Log("[GDI] BitBlt hooked at 0x%p", (void*)g_GdiBitBltAddr); any = true;
    } else Log("[GDI] BitBlt hook failed");

    if (g_GdiStretchBltAddr && InstallHookAt(g_GdiStretchBltAddr, g_GdiStretchBltOrig, g_GdiStretchBltJmp, (void*)HookedGdiStretchBlt)) {
        Log("[GDI] StretchBlt hooked at 0x%p", (void*)g_GdiStretchBltAddr); any = true;
    } else Log("[GDI] StretchBlt hook failed");

    if (g_GdiStretchDIBitsAddr && InstallHookAt(g_GdiStretchDIBitsAddr, g_GdiStretchDIBitsOrig, g_GdiStretchDIBitsJmp, (void*)HookedGdiStretchDIBits)) {
        Log("[GDI] StretchDIBits hooked at 0x%p", (void*)g_GdiStretchDIBitsAddr); any = true;
    } else Log("[GDI] StretchDIBits hook failed");

    if (any) { g_GDIHooked = true; Log("[GDI] InstallGDIHook: at least one function hooked"); }
    return any;
}

static bool SafeInstallGDIHook() {
    __try { return InstallGDIHook(); }
    __except(EXCEPTION_EXECUTE_HANDLER) { Log("[GDI] InstallGDIHook: EXCEPTION 0x%08X caught", GetExceptionCode()); return false; }
}

// ── DDraw hook installation ──────────────────────────────────────────────
static bool InstallDDrawHook() {
    HMODULE ddmod = GetModuleHandleW(L"ddraw.dll");
    if (!ddmod) { Log("[DDRAW] InstallDDrawHook: ddraw.dll not loaded, skipping"); return false; }

    auto pDDCreateEx = (PFN_DirectDrawCreateEx)GetProcAddress(ddmod, "DirectDrawCreateEx");
    if (!pDDCreateEx) { Log("[DDRAW] DirectDrawCreateEx not found"); return false; }
    Log("[DDRAW] InstallDDrawHook: ddraw.dll found, creating dummy objects");

    // Create dummy IDirectDraw7
    void* pDD7 = nullptr;
    HRESULT hr = pDDCreateEx(nullptr, &pDD7, S4W_IID_IDirectDraw7, nullptr);
    if (FAILED(hr) || !pDD7) { Log("[DDRAW] DirectDrawCreateEx failed hr=0x%08X", hr); return false; }

    void** ddVtable = *(void***)pDD7;

    // SetCooperativeLevel(desktop, DDSCL_NORMAL)
    typedef HRESULT(STDMETHODCALLTYPE* PFN_SetCoop)(void*, HWND, DWORD);
    hr = ((PFN_SetCoop)ddVtable[DDVT_SETCOOPERATIVELEVEL])(pDD7, GetDesktopWindow(), S4W_DDSCL_NORMAL);
    if (FAILED(hr)) {
        Log("[DDRAW] SetCooperativeLevel failed hr=0x%08X", hr);
        typedef HRESULT(STDMETHODCALLTYPE* PFN_Rel)(void*);
        ((PFN_Rel)ddVtable[DDVT_RELEASE])(pDD7);
        return false;
    }

    // Create dummy primary surface to discover Blt vtable address
    BYTE desc[256] = {};
    *(DWORD*)(desc + 0) = S4W_DDSURFACEDESC2_SIZE;       // dwSize
    *(DWORD*)(desc + 4) = S4W_DDSD_CAPS;                 // dwFlags
    *(DWORD*)(desc + S4W_DDSURFACEDESC2_CAPS) = S4W_DDSCAPS_PRIMARYSURFACE;  // ddsCaps.dwCaps

    void* pSurface = nullptr;
    typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSurf)(void*, void*, void**, IUnknown*);
    hr = ((PFN_CreateSurf)ddVtable[DDVT_CREATESURFACE])(pDD7, desc, &pSurface, nullptr);
    if (FAILED(hr) || !pSurface) {
        Log("[DDRAW] CreateSurface failed hr=0x%08X", hr);
        typedef HRESULT(STDMETHODCALLTYPE* PFN_Rel)(void*);
        ((PFN_Rel)ddVtable[DDVT_RELEASE])(pDD7);
        return false;
    }

    // Get Blt + BltFast + Flip function addresses from surface vtable
    void** surfVtable = *(void***)pSurface;
    g_DDrawBltAddr     = (BYTE*)surfVtable[DDSVT_BLT];
    g_DDrawBltFastAddr = (BYTE*)surfVtable[DDSVT_BLTFAST];
    g_DDrawFlipAddr    = (BYTE*)surfVtable[DDSVT_FLIP];
    Log("[DDRAW] Surface Blt at 0x%p, BltFast at 0x%p, Flip at 0x%p",
        (void*)g_DDrawBltAddr, (void*)g_DDrawBltFastAddr, (void*)g_DDrawFlipAddr);

    // Grab IDirectDraw7::CreateSurface address for HAL surface interception
    g_DDrawCSAddr = (BYTE*)ddVtable[DDVT_CREATESURFACE];
    Log("[DDRAW] IDirectDraw7::CreateSurface at 0x%p", (void*)g_DDrawCSAddr);

    // ── Probe IDirectDraw v1 (DirectDrawCreate) for a potentially different vtable ──
    // Kega Fusion and similar emulators call DirectDrawCreate (not DirectDrawCreateEx)
    // and use IDirectDrawSurface (v1).  On many Windows versions the underlying
    // function pointers are the same as v7, but on some systems they differ.
    // We create a v1 dummy primary surface and hook its Blt/BltFast/Flip only if
    // those addresses are different from the v7 ones we already have.
    {
        typedef HRESULT(WINAPI* PFN_DDCreateV1)(GUID*, void**, void*);
        auto pDDCreate = (PFN_DDCreateV1)GetProcAddress(ddmod, "DirectDrawCreate");
        if (pDDCreate) {
            void* pDD1 = nullptr;
            HRESULT hrV1 = pDDCreate(nullptr, &pDD1, nullptr);
            if (SUCCEEDED(hrV1) && pDD1) {
                void** dd1Vtable = *(void***)pDD1;
                // SetCooperativeLevel — same index (20) for v1 and v7
                typedef HRESULT(STDMETHODCALLTYPE* PFN_SCL)(void*, HWND, DWORD);
                ((PFN_SCL)dd1Vtable[DDVT_SETCOOPERATIVELEVEL])(pDD1, GetDesktopWindow(), S4W_DDSCL_NORMAL);

                // Build a DDSURFACEDESC (v1, 108 bytes on x86) primary surface descriptor.
                // The caps field is at the same offset as DDSURFACEDESC2 on x86 (104).
                BYTE desc1[256] = {};
                *(DWORD*)(desc1 + 0) = 108;                     // dwSize = sizeof(DDSURFACEDESC)
                *(DWORD*)(desc1 + 4) = S4W_DDSD_CAPS;           // dwFlags
                *(DWORD*)(desc1 + S4W_DDSURFACEDESC2_CAPS) = S4W_DDSCAPS_PRIMARYSURFACE;

                void* pSurf1 = nullptr;
                typedef HRESULT(STDMETHODCALLTYPE* PFN_CS1)(void*, void*, void**, void*);
                HRESULT hrS = ((PFN_CS1)dd1Vtable[DDVT_CREATESURFACE])(pDD1, desc1, &pSurf1, nullptr);
                Log("[DDRAWv1] DirectDrawCreate v1 surface hr=0x%08X pSurf=0x%p", hrS, pSurf1);

                if (SUCCEEDED(hrS) && pSurf1) {
                    void** sv1 = *(void***)pSurf1;
                    BYTE* v1Blt     = (BYTE*)sv1[DDSVT_BLT];
                    BYTE* v1BltFast = (BYTE*)sv1[DDSVT_BLTFAST];
                    BYTE* v1Flip    = (BYTE*)sv1[DDSVT_FLIP];
                    Log("[DDRAWv1] v1 Blt=0x%p BltFast=0x%p Flip=0x%p", v1Blt, v1BltFast, v1Flip);
                    Log("[DDRAWv1] v7 Blt=0x%p BltFast=0x%p Flip=0x%p",
                        (void*)g_DDrawBltAddr, (void*)g_DDrawBltFastAddr, (void*)g_DDrawFlipAddr);

                    bool bltDiff     = (v1Blt     != g_DDrawBltAddr);
                    bool bltFastDiff = (v1BltFast != g_DDrawBltFastAddr);
                    bool flipDiff    = (v1Flip    != g_DDrawFlipAddr);

                    if (bltDiff || bltFastDiff || flipDiff) {
                        Log("[DDRAWv1] v1 vtable differs from v7 — installing v1 hooks");
                        g_DDrawBltV1Addr     = v1Blt;
                        g_DDrawBltFastV1Addr = v1BltFast;
                        g_DDrawFlipV1Addr    = v1Flip;
                        // Hooks installed later alongside v7 hooks
                    } else {
                        Log("[DDRAWv1] v1 vtable identical to v7 — no extra hooks needed");
                    }

                    // Release v1 surface
                    typedef ULONG(STDMETHODCALLTYPE* PFN_RelS)(void*);
                    ((PFN_RelS)sv1[2])(pSurf1);
                }

                // Release v1 IDirectDraw
                typedef ULONG(STDMETHODCALLTYPE* PFN_Rel1)(void*);
                ((PFN_Rel1)dd1Vtable[2])(pDD1);
            } else {
                Log("[DDRAWv1] DirectDrawCreate failed hr=0x%08X — v1 hook skipped", hrV1);
            }
        } else {
            Log("[DDRAWv1] DirectDrawCreate not found in ddraw.dll — v1 hook skipped");
        }
    }

    // Query IDirect3D7 from IDirectDraw7 to get EndScene vtable address.
    // Strategy: create a throw-away HAL device from a small offscreen surface,
    // read its vtable[6] (EndScene), and hook it immediately.
    // This covers games like Fusion that create their D3D7 device BEFORE injection —
    // CreateDevice hook would be too late for them.
    {
        typedef HRESULT(STDMETHODCALLTYPE* PFN_QI)(void*, const GUID*, void**);
        typedef ULONG(STDMETHODCALLTYPE* PFN_Rel7)(void*);
        void* pD3D7 = nullptr;
        HRESULT hrQI = ((PFN_QI)ddVtable[0])(pDD7, &S4W_IID_IDirect3D7, &pD3D7);
        if (SUCCEEDED(hrQI) && pD3D7) {
            void** d3d7Vtable = *(void***)pD3D7;
            g_D3D7CDAddr = (BYTE*)d3d7Vtable[D3D7VT_CREATEDEVICE];
            Log("[D3D7] IDirect3D7 obtained, CreateDevice at 0x%p", (void*)g_D3D7CDAddr);

            // Create a small offscreen 3D surface for the dummy device.
            // DDSD_CAPS(0x1)|DDSD_HEIGHT(0x2)|DDSD_WIDTH(0x4)|DDSD_PIXELFORMAT(0x1000) = 0x1007
            // DDPIXELFORMAT offset: 72 (x86) — 32-bit RGB needed to avoid E_INVALIDARG
            // ddsCaps: DDSCAPS_OFFSCREENPLAIN(0x40)|DDSCAPS_3DDEVICE(0x2000) = 0x2040
            BYTE rtDesc[256] = {};
            *(DWORD*)(rtDesc + 0)   = S4W_DDSURFACEDESC2_SIZE;
            *(DWORD*)(rtDesc + 4)   = 0x00001007;  // DDSD_CAPS|DDSD_HEIGHT|DDSD_WIDTH|DDSD_PIXELFORMAT
            *(DWORD*)(rtDesc + 8)   = 64;           // dwHeight
            *(DWORD*)(rtDesc + 12)  = 64;           // dwWidth
            *(DWORD*)(rtDesc + 72)  = 32;           // ddpfPixelFormat.dwSize
            *(DWORD*)(rtDesc + 76)  = 0x00000040;   // DDPF_RGB
            *(DWORD*)(rtDesc + 80)  = 0;            // dwFourCC
            *(DWORD*)(rtDesc + 84)  = 32;           // dwRGBBitCount
            *(DWORD*)(rtDesc + 88)  = 0x00FF0000;   // dwRBitMask
            *(DWORD*)(rtDesc + 92)  = 0x0000FF00;   // dwGBitMask
            *(DWORD*)(rtDesc + 96)  = 0x000000FF;   // dwBBitMask
            *(DWORD*)(rtDesc + S4W_DDSURFACEDESC2_CAPS) = 0x00002040; // OFFSCREENPLAIN|3DDEVICE

            void* pRTSurf = nullptr;
            typedef HRESULT(STDMETHODCALLTYPE* PFN_CS2)(void*, void*, void**, IUnknown*);
            HRESULT hrRT = ((PFN_CS2)ddVtable[DDVT_CREATESURFACE])(pDD7, rtDesc, &pRTSurf, nullptr);
            Log("[D3D7] Offscreen 3D surface hr=0x%08X pRTSurf=0x%p", hrRT, pRTSurf);

            if (SUCCEEDED(hrRT) && pRTSurf) {
                // Try HAL first; fall back to RGB software device if HAL is unavailable
                // (e.g. NVIDIA refuses a second HAL device while Fusion already has one).
                // D3DIM700.DLL uses the SAME IDirect3DDevice7 vtable for all device types,
                // so the RGB device's vtable[6] == HAL device's vtable[6] == EndScene.
                void* pDummyDev = nullptr;
                HRESULT hrDev = ((PFN_D3D7CreateDevice)d3d7Vtable[D3D7VT_CREATEDEVICE])(
                    pD3D7, &S4W_IID_IDirect3DHALDevice, pRTSurf, &pDummyDev);
                Log("[D3D7] Dummy HAL device hr=0x%08X", hrDev);
                if (FAILED(hrDev) || !pDummyDev) {
                    hrDev = ((PFN_D3D7CreateDevice)d3d7Vtable[D3D7VT_CREATEDEVICE])(
                        pD3D7, &S4W_IID_IDirect3DRGBDevice, pRTSurf, &pDummyDev);
                    Log("[D3D7] Dummy RGB device hr=0x%08X pDev=0x%p", hrDev, pDummyDev);
                }

                if (SUCCEEDED(hrDev) && pDummyDev) {
                    void** devVt = *(void***)pDummyDev;
                    g_D3D7ESAddr = (BYTE*)devVt[D3D7DEVVT_ENDSCENE];
                    Log("[D3D7] Dummy device vtable[6]=EndScene at 0x%p", (void*)g_D3D7ESAddr);
                    if (InstallHookAt(g_D3D7ESAddr, g_D3D7ESOrig, g_D3D7ESJmp, (void*)HookedD3D7EndScene)) {
                        g_D3D7ESHooked = true;
                        Log("[D3D7] EndScene hooked at 0x%p — covers pre-existing device", (void*)g_D3D7ESAddr);
                    } else {
                        Log("[D3D7] EndScene hook FAILED at 0x%p", (void*)g_D3D7ESAddr);
                    }
                    ((PFN_Rel7)(*(void***)pDummyDev)[2])(pDummyDev);
                }
                ((PFN_Rel7)(*(void***)pRTSurf)[2])(pRTSurf);
            }

            ((PFN_Rel7)(*(void***)pD3D7)[2])(pD3D7);  // Release IDirect3D7
        } else {
            Log("[D3D7] QueryInterface for IDirect3D7 failed hr=0x%08X — D3D7 hook skipped", hrQI);
        }
    }

    // Release dummy objects
    typedef HRESULT(STDMETHODCALLTYPE* PFN_Rel)(void*);
    ((PFN_Rel)surfVtable[2])(pSurface);
    ((PFN_Rel)ddVtable[DDVT_RELEASE])(pDD7);

    // Install inline hook on Blt
    if (!InstallHookAt(g_DDrawBltAddr, g_DDrawBltOrig, g_DDrawBltJmp, (void*)HookedDDrawBlt)) {
        Log("[DDRAW] Failed to install Blt inline hook"); return false;
    }
    // Install inline hook on Flip
    if (!InstallHookAt(g_DDrawFlipAddr, g_DDrawFlipOrig, g_DDrawFlipJmp, (void*)HookedDDrawFlip)) {
        Log("[DDRAW] Failed to install Flip inline hook (Blt still active)");
    } else {
        Log("[DDRAW] Flip hooked OK at 0x%p", (void*)g_DDrawFlipAddr);
    }
    // Install inline hook on BltFast
    if (!InstallHookAt(g_DDrawBltFastAddr, g_DDrawBltFastOrig, g_DDrawBltFastJmp, (void*)HookedDDrawBltFast)) {
        Log("[DDRAW] Failed to install BltFast inline hook (Blt still active)");
    } else {
        Log("[DDRAW] BltFast hooked OK at 0x%p", (void*)g_DDrawBltFastAddr);
    }

    // Hook CreateSurface to detect HAL surface vtable at runtime
    if (!InstallHookAt(g_DDrawCSAddr, g_DDrawCSOrig, g_DDrawCSJmp, (void*)HookedDDrawCreateSurface))
        Log("[DDRAW] CreateSurface hook failed (HAL detection disabled)");
    else
        Log("[DDRAW] CreateSurface hooked OK at 0x%p — will detect HAL vtable", (void*)g_DDrawCSAddr);

    // Hook IDirect3D7::CreateDevice to intercept the real HAL D3D7 device
    if (g_D3D7CDAddr) {
        if (!InstallHookAt(g_D3D7CDAddr, g_D3D7CDOrig, g_D3D7CDJmp, (void*)HookedD3D7CreateDevice))
            Log("[D3D7] CreateDevice hook failed");
        else
            Log("[D3D7] CreateDevice hooked OK at 0x%p — will hook EndScene on first device", (void*)g_D3D7CDAddr);
    }

    // ── Install v1 hooks if the vtable addresses differ from v7 ─────────────
    if (g_DDrawBltV1Addr && g_DDrawBltV1Addr != g_DDrawBltAddr) {
        if (InstallHookAt(g_DDrawBltV1Addr, g_DDrawBltV1Orig, g_DDrawBltV1Jmp, (void*)HookedDDrawBltV1))
            Log("[DDRAWv1] Blt v1 hooked OK at 0x%p", (void*)g_DDrawBltV1Addr);
        else
            Log("[DDRAWv1] Blt v1 hook FAILED at 0x%p", (void*)g_DDrawBltV1Addr);
    }
    if (g_DDrawFlipV1Addr && g_DDrawFlipV1Addr != g_DDrawFlipAddr) {
        if (InstallHookAt(g_DDrawFlipV1Addr, g_DDrawFlipV1Orig, g_DDrawFlipV1Jmp, (void*)HookedDDrawFlipV1))
            Log("[DDRAWv1] Flip v1 hooked OK at 0x%p", (void*)g_DDrawFlipV1Addr);
        else
            Log("[DDRAWv1] Flip v1 hook FAILED at 0x%p", (void*)g_DDrawFlipV1Addr);
    }
    if (g_DDrawBltFastV1Addr && g_DDrawBltFastV1Addr != g_DDrawBltFastAddr) {
        if (InstallHookAt(g_DDrawBltFastV1Addr, g_DDrawBltFastV1Orig, g_DDrawBltFastV1Jmp, (void*)HookedDDrawBltFastV1))
            Log("[DDRAWv1] BltFast v1 hooked OK at 0x%p", (void*)g_DDrawBltFastV1Addr);
        else
            Log("[DDRAWv1] BltFast v1 hook FAILED at 0x%p", (void*)g_DDrawBltFastV1Addr);
        g_DDrawV1Hooked = true;
    }

    g_DDrawHooked = true;
    Log("[DDRAW] InstallDDrawHook: Blt hooked OK at 0x%p", (void*)g_DDrawBltAddr);
    return true;
}

// ── Safe hook installer wrappers (SEH-protected) ─────────────────────────
static bool SafeInstallDDrawHook() {
    __try { return InstallDDrawHook(); }
    __except(EXCEPTION_EXECUTE_HANDLER) { Log("[DDRAW] InstallDDrawHook: EXCEPTION 0x%08X caught", GetExceptionCode()); return false; }
}
static bool SafeInstallD3D11Hook() {
    __try { return InstallD3D11Hook(); }
    __except(EXCEPTION_EXECUTE_HANDLER) { Log("[D3D11] InstallD3D11Hook: EXCEPTION 0x%08X caught", GetExceptionCode()); return false; }
}
static bool SafeInstallD3D12QueueHook() {
    __try { return InstallD3D12QueueHook(); }
    __except(EXCEPTION_EXECUTE_HANDLER) { Log("[D3D12] InstallD3D12QueueHook: EXCEPTION 0x%08X caught", GetExceptionCode()); return false; }
}
static bool SafeInstallD3D9Hook() {
    __try { return InstallD3D9Hook(); }
    __except(EXCEPTION_EXECUTE_HANDLER) { Log("[D3D9] InstallD3D9Hook: EXCEPTION 0x%08X caught", GetExceptionCode()); return false; }
}
static bool SafeInstallOpenGLHook() {
    __try { return InstallOpenGLHook(); }
    __except(EXCEPTION_EXECUTE_HANDLER) { Log("[GL] InstallOpenGLHook: EXCEPTION 0x%08X caught", GetExceptionCode()); return false; }
}

// ── Forward declaration for parallel Gopher hook path ───────────────────
static DWORD GopherHookThread();

// ── Worker thread ────────────────────────────────────────────────────────
static DWORD WINAPI HookThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // ── Gopher64 parallel path ──
    // When g_IsGopher is TRUE, branch to a completely isolated code path that
    // never touches the main D3D11/D3D9/OpenGL/DDraw/GDI hooks below. When
    // FALSE, the rest of this function is byte-for-byte identical to v1.2.
    if (g_IsGopher) {
        Log("[GOPHER] HookThread: diverging to GopherHookThread (parallel path)");
        return GopherHookThread();
    }

    // EARLY DDraw install (before the 2s wait) — needed for games like Fusion
    // that create their HAL surfaces very early at process startup. If ddraw.dll
    // is already loaded at T+0, try to hook Blt/BltFast/Flip/CreateSurface
    // immediately so we can intercept surface creation before the game does it.
    bool earlyDDrawOk = false;
    if (GetModuleHandleW(L"ddraw.dll")) {
        Log("[THREAD] ddraw.dll already loaded at T+0 — installing DDraw hook early");
        earlyDDrawOk = SafeInstallDDrawHook();
        Log("[THREAD] Early DDraw hook: %s", earlyDDrawOk ? "OK" : "failed");
    }

    // Wait a moment for the game to initialize its graphics API
    Log("[THREAD] HookThread started — waiting 2s for game to load graphics API");
    Sleep(2000);

    // Log which graphics DLLs are currently loaded
    Log("[THREAD] Loaded DLLs check: d3d11=%s d3d9=%s opengl32=%s dxgi=%s d3d12=%s vulkan=%s ddraw=%s",
        GetModuleHandleW(L"d3d11.dll")     ? "YES" : "no",
        GetModuleHandleW(L"d3d9.dll")      ? "YES" : "no",
        GetModuleHandleW(L"opengl32.dll")  ? "YES" : "no",
        GetModuleHandleW(L"dxgi.dll")      ? "YES" : "no",
        GetModuleHandleW(L"d3d12.dll")     ? "YES" : "no",
        GetModuleHandleW(L"vulkan-1.dll")  ? "YES" : "no",
        GetModuleHandleW(L"ddraw.dll")     ? "YES" : "no");

    // Log ALL loaded DLLs with full paths (to detect game-local opengl32/d3d11 overrides)
    {
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
        if (hSnap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W me = { sizeof(me) };
            Log("[THREAD] All loaded modules (T+2s):");
            if (Module32FirstW(hSnap, &me)) {
                do {
                    char path[MAX_PATH];
                    WideCharToMultiByte(CP_ACP, 0, me.szExePath, -1, path, MAX_PATH, nullptr, nullptr);
                    Log("[MODULE]  %s  base=0x%p", path, (void*)me.modBaseAddr);
                } while (Module32NextW(hSnap, &me));
            }
            CloseHandle(hSnap);
        }
    }

    // Deferred MMF2 detection — mmfs2.dll may not be loaded yet at DLL_PROCESS_ATTACH
    // (MMF2 loads its runtime DLLs a few seconds after startup).
    if (!g_IsMMF2 && (GetModuleHandleW(L"mmfs2.dll") || GetModuleHandleW(L"mmf2d3d9.dll"))) {
        g_IsMMF2 = true;
        Log("[MMF2] Clickteam MMF2 runtime detected (deferred) — enabling shader bytecode cache");
    }

    // Try all hooks — only for APIs already loaded by the game
    bool d3d11ok = SafeInstallD3D11Hook();
    bool d3d9ok  = SafeInstallD3D9Hook();
    bool glOk    = SafeInstallOpenGLHook();
    // Reuse early DDraw install result if it succeeded; otherwise try now
    bool ddrawOk = earlyDDrawOk ? true : SafeInstallDDrawHook();
    // GDI hook — installed for all processes; GDIShouldApply() prevents
    // interference with D3D11/D3D9/OpenGL games.  Catches pure-GDI games
    // (Clickteam Fusion software mode, Direct2D HWND render targets, etc.).
    bool gdiOk  = SafeInstallGDIHook();
    // D3D12 command-queue capture — only when the game actually uses D3D12.
    // Independent of the D3D11 hook: the shared DXGI Present hook (installed by
    // SafeInstallD3D11Hook) catches the frame; this just grabs the queue so we
    // can graft a D3D11On12 device. Returns false (harmless) for non-D3D12 games.
    bool d3d12ok = SafeInstallD3D12QueueHook();

    Log("[THREAD] Hook results: D3D11=%s D3D9=%s OpenGL=%s DDraw=%s GDI=%s D3D12=%s",
        d3d11ok ? "OK" : "fail", d3d9ok ? "OK" : "fail",
        glOk ? "OK" : "fail", ddrawOk ? "OK" : "fail", gdiOk ? "OK" : "fail",
        d3d12ok ? "OK" : "n/a");

    // Second module snapshot at T+8s — catches DLLs loaded after game fully inits
    Sleep(6000);
    {
        HANDLE hSnap2 = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
        if (hSnap2 != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W me2 = { sizeof(me2) };
            Log("[THREAD] All loaded modules (T+8s — late-loading DLLs):");
            if (Module32FirstW(hSnap2, &me2)) {
                do {
                    char path2[MAX_PATH];
                    WideCharToMultiByte(CP_ACP, 0, me2.szExePath, -1, path2, MAX_PATH, nullptr, nullptr);
                    Log("[MODULE2]  %s  base=0x%p", path2, (void*)me2.modBaseAddr);
                } while (Module32NextW(hSnap2, &me2));
            }
            CloseHandle(hSnap2);
        }
    }

    // If ANY hook is missing, keep retrying — some games delay-load their graphics
    // API (e.g. MMF2 loads d3d9.dll via mmf2d3d9.dll several seconds after init).
    if (!d3d11ok || !d3d9ok || !glOk || !ddrawOk) {
        Log("[THREAD] Some hooks not yet installed — retrying for up to 30s "
            "(D3D11=%s D3D9=%s GL=%s DDraw=%s)",
            d3d11ok?"OK":"missing", d3d9ok?"OK":"missing",
            glOk?"OK":"missing", ddrawOk?"OK":"missing");
        for (int i = 0; i < 30; i++) {
            Sleep(1000);
            bool prevD3D9 = d3d9ok, prevD3D11 = d3d11ok, prevGL = glOk, prevDD = ddrawOk;
            if (!d3d11ok) d3d11ok = SafeInstallD3D11Hook();
            if (!d3d9ok)  d3d9ok  = SafeInstallD3D9Hook();
            if (!glOk)    glOk    = SafeInstallOpenGLHook();
            if (!ddrawOk) ddrawOk = SafeInstallDDrawHook();
            // Retry GDI hook if it failed on first attempt
            if (!gdiOk) gdiOk = SafeInstallGDIHook();
            // Retry D3D12 queue capture — d3d12.dll may load a few seconds late
            if (!d3d12ok) d3d12ok = SafeInstallD3D12QueueHook();
            // Log whenever a new hook gets installed
            if ((!prevD3D11 && d3d11ok) || (!prevD3D9 && d3d9ok) ||
                (!prevGL && glOk) || (!prevDD && ddrawOk))
                Log("[THREAD] New hook(s) installed at T+%ds: D3D11=%s D3D9=%s GL=%s DDraw=%s",
                    i + 3, d3d11ok?"OK":"missing", d3d9ok?"OK":"missing",
                    glOk?"OK":"missing", ddrawOk?"OK":"missing");
            if (d3d11ok && d3d9ok && glOk && ddrawOk) {
                Log("[THREAD] All hooks installed — stopping retry loop");
                break;
            }
        }
        Log("[THREAD] Final status after retries: D3D11=%s D3D9=%s GL=%s DDraw=%s",
            d3d11ok?"OK":"not hooked", d3d9ok?"OK":"not hooked",
            glOk?"OK":"not hooked", ddrawOk?"OK":"not hooked");
    }

    return 0;
}

// ── Cleanup ──────────────────────────────────────────────────────────────
static void Cleanup() {
    // Restore hooked functions to original bytes before unloading
    if (g_SwapBuffersAddr) {
        DWORD op; VirtualProtect(g_SwapBuffersAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
        memcpy(g_SwapBuffersAddr, g_SwapBufOrigBytes, HOOK_JMP_SIZE);
        VirtualProtect(g_SwapBuffersAddr, HOOK_JMP_SIZE, op, &op);
        g_SwapBuffersAddr = nullptr;
    }
    if (g_GdiSwapBuffersAddr) {
        DWORD op; VirtualProtect(g_GdiSwapBuffersAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
        memcpy(g_GdiSwapBuffersAddr, g_GdiSwapBufOrigBytes, HOOK_JMP_SIZE);
        VirtualProtect(g_GdiSwapBuffersAddr, HOOK_JMP_SIZE, op, &op);
        g_GdiSwapBuffersAddr = nullptr;
    }
    if (g_MakeCurrentAddr) {
        DWORD op; VirtualProtect(g_MakeCurrentAddr, HOOK_JMP_SIZE, PAGE_EXECUTE_READWRITE, &op);
        memcpy(g_MakeCurrentAddr, g_MakeCurrentOrig, HOOK_JMP_SIZE);
        VirtualProtect(g_MakeCurrentAddr, HOOK_JMP_SIZE, op, &op);
        g_MakeCurrentAddr = nullptr;
    }
    if (g_D3D9PSCachedBlob) { g_D3D9PSCachedBlob->Release(); g_D3D9PSCachedBlob = nullptr; }
    if (g_Shared) { UnmapViewOfFile(g_Shared); g_Shared = nullptr; }
    if (g_MapFile) { CloseHandle(g_MapFile); g_MapFile = nullptr; }
    if (g_Raster) { g_Raster->Release(); g_Raster = nullptr; }
    if (g_Blend) { g_Blend->Release(); g_Blend = nullptr; }
    if (g_CB) { g_CB->Release(); g_CB = nullptr; }
    if (g_PS) { g_PS->Release(); g_PS = nullptr; }
    if (g_VS) { g_VS->Release(); g_VS = nullptr; }
    if (g_Ctx) { g_Ctx->Release(); g_Ctx = nullptr; }
    if (g_Device) { g_Device->Release(); g_Device = nullptr; }
    // ── D3D12 / D3D11On12 parallel-path resources ──
    ReleaseBezelTexture12();
    if (g_D2DCtx12)    { g_D2DCtx12->Release();    g_D2DCtx12 = nullptr; }
    if (g_D2DDevice12) { g_D2DDevice12->Release(); g_D2DDevice12 = nullptr; }
    if (g_D2DFactory12){ g_D2DFactory12->Release();g_D2DFactory12 = nullptr; }
    if (g_BBSRV12)     { g_BBSRV12->Release();     g_BBSRV12 = nullptr; }
    if (g_BBCopy12)    { g_BBCopy12->Release();    g_BBCopy12 = nullptr; }
    if (g_Raster12)    { g_Raster12->Release();    g_Raster12 = nullptr; }
    if (g_BezelSamp12) { g_BezelSamp12->Release(); g_BezelSamp12 = nullptr; }
    if (g_Sampler12)   { g_Sampler12->Release();   g_Sampler12 = nullptr; }
    if (g_BlendOver12) { g_BlendOver12->Release(); g_BlendOver12 = nullptr; }
    if (g_Blend12)     { g_Blend12->Release();     g_Blend12 = nullptr; }
    if (g_CB12)        { g_CB12->Release();        g_CB12 = nullptr; }
    if (g_PS12)        { g_PS12->Release();        g_PS12 = nullptr; }
    if (g_VS12)        { g_VS12->Release();        g_VS12 = nullptr; }
    if (g_SC3)         { g_SC3->Release();         g_SC3 = nullptr; }
    if (g_Ctx11on12)   { g_Ctx11on12->Release();   g_Ctx11on12 = nullptr; }
    if (g_On12)        { g_On12->Release();        g_On12 = nullptr; }
    if (g_Dev11on12)   { g_Dev11on12->Release();   g_Dev11on12 = nullptr; }
    if (g_GameCmdQueue){ g_GameCmdQueue->Release();g_GameCmdQueue = nullptr; }
    if (g_D3D12Device) { g_D3D12Device->Release(); g_D3D12Device = nullptr; }
}

// ── DLL Entry Point ──────────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_Module = hModule;
        DisableThreadLibraryCalls(hModule);
        LogInit();
        // ── Gopher64 detection (sets g_IsGopher BEFORE HookThread starts) ──
        // When the host exe name matches a Gopher64 binary, HookThread takes
        // a completely different parallel code path — none of the main
        // D3D11/D3D9/OpenGL/DDraw/GDI hooks are installed. This guarantees
        // byte-for-byte v1.2 behavior for every other game/emulator.
        {
            wchar_t exePath[MAX_PATH] = {};
            DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            if (n > 0) {
                const wchar_t* exeName = wcsrchr(exePath, L'\\');
                exeName = exeName ? (exeName + 1) : exePath;
                if (_wcsicmp(exeName, L"gopher64-windows-x86_64.exe") == 0 ||
                    _wcsicmp(exeName, L"gopher64.exe") == 0) {
                    g_IsGopher = true;
                    Log("[GOPHER] Host exe detected as Gopher64 (%ls) — enabling parallel hook path", exeName);
                }
                // MAME detection: matches mame.exe, mame64.exe, mame0270.exe, etc.
                // _wcsnicmp checks only the first 4 chars so any MAME versioned binary matches.
                if (_wcsnicmp(exeName, L"mame", 4) == 0) {
                    g_IsMame = true;
                    Log("[MAME] Host exe detected as MAME (%ls) — enabling device-lifecycle hook", exeName);
                }
            }
        }
        // MMF2 (Clickteam) detection — check for runtime DLL in process modules.
        // MMF2 games call Reset multiple times per level transition; when detected
        // we cache the compiled shader bytecode so Reset takes ~1ms instead of ~2.5s.
        if (GetModuleHandleW(L"mmfs2.dll") || GetModuleHandleW(L"mmf2d3d9.dll")) {
            g_IsMMF2 = true;
            Log("[MMF2] Clickteam MMF2 runtime detected — enabling shader bytecode cache");
        }
        CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        Log("[CLEANUP] DLL_PROCESS_DETACH — shutting down");
        Cleanup();
        LogCleanup();
        break;
    }
    return TRUE;
}
