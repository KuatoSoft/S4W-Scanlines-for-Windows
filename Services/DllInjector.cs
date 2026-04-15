using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace S4W.Services;

/// <summary>
/// Injects a native DLL into a target process using CreateRemoteThread + LoadLibraryW.
/// For 32-bit targets, delegates to S4W_Injector_x86.exe helper (a 64-bit process
/// cannot use CreateRemoteThread into a 32-bit process because the kernel32.dll
/// LoadLibraryW address differs between 64-bit and 32-bit address spaces).
/// </summary>
public static class DllInjector
{
    /// <summary>
    /// Injects a DLL into the target process.
    /// Returns true if injection succeeded.
    /// </summary>
    public static bool Inject(int processId, string dllPath, out string? error, bool isTarget32bit = false)
    {
        error = null;

        if (!File.Exists(dllPath))
        {
            error = $"Hook DLL not found: {dllPath}";
            return false;
        }

        dllPath = Path.GetFullPath(dllPath);

        // 32-bit targets require the 32-bit helper injector
        if (isTarget32bit)
            return Inject32bit(processId, dllPath, out error);

        return Inject64bit(processId, dllPath, out error);
    }

    /// <summary>
    /// Injects into a 32-bit process using S4W_Injector_x86.exe helper.
    /// </summary>
    private static bool Inject32bit(int processId, string dllPath, out string? error)
    {
        error = null;

        // Find the helper injector
        string exeDir = AppDomain.CurrentDomain.BaseDirectory;
        string injectorPath = Path.Combine(exeDir, "S4W_Injector_x86.exe");
        if (!File.Exists(injectorPath))
        {
            injectorPath = Path.Combine(exeDir, "Hook", "S4W_Injector_x86.exe");
        }
        if (!File.Exists(injectorPath))
        {
            error = $"32-bit injector helper not found: S4W_Injector_x86.exe";
            return false;
        }

        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = injectorPath,
                Arguments = $"{processId} \"{dllPath}\"",
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
            };

            using var proc = Process.Start(psi);
            if (proc == null)
            {
                error = "Failed to start S4W_Injector_x86.exe";
                return false;
            }

            string stdout = proc.StandardOutput.ReadToEnd();
            string stderr = proc.StandardError.ReadToEnd();
            proc.WaitForExit(15000);

            if (proc.ExitCode == 0)
                return true;

            error = $"S4W_Injector_x86.exe failed (exit={proc.ExitCode}): {stderr.Trim()}";
            return false;
        }
        catch (Exception ex)
        {
            error = $"32-bit injection exception: {ex.Message}";
            return false;
        }
    }

    /// <summary>
    /// Standard 64-bit injection via CreateRemoteThread + LoadLibraryW.
    /// </summary>
    private static bool Inject64bit(int processId, string dllPath, out string? error)
    {
        error = null;
        IntPtr hProcess = IntPtr.Zero;
        IntPtr remoteMemory = IntPtr.Zero;

        try
        {
            hProcess = OpenProcess(
                PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                false, processId);

            if (hProcess == IntPtr.Zero)
            {
                error = $"OpenProcess failed (PID={processId}), error={Marshal.GetLastWin32Error()}";
                return false;
            }

            byte[] dllPathBytes = Encoding.Unicode.GetBytes(dllPath + "\0");
            remoteMemory = VirtualAllocEx(hProcess, IntPtr.Zero,
                (uint)dllPathBytes.Length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

            if (remoteMemory == IntPtr.Zero)
            {
                error = $"VirtualAllocEx failed, error={Marshal.GetLastWin32Error()}";
                return false;
            }

            if (!WriteProcessMemory(hProcess, remoteMemory, dllPathBytes,
                (uint)dllPathBytes.Length, out _))
            {
                error = $"WriteProcessMemory failed, error={Marshal.GetLastWin32Error()}";
                return false;
            }

            IntPtr kernel32 = GetModuleHandle("kernel32.dll");
            IntPtr loadLibAddr = GetProcAddress(kernel32, "LoadLibraryW");
            if (loadLibAddr == IntPtr.Zero)
            {
                error = "GetProcAddress(LoadLibraryW) failed";
                return false;
            }

            IntPtr hThread = CreateRemoteThread(hProcess, IntPtr.Zero, 0,
                loadLibAddr, remoteMemory, 0, out _);

            if (hThread == IntPtr.Zero)
            {
                error = $"CreateRemoteThread failed, error={Marshal.GetLastWin32Error()}";
                return false;
            }

            WaitForSingleObject(hThread, 10000);
            GetExitCodeThread(hThread, out uint exitCode);
            CloseHandle(hThread);

            if (exitCode == 0)
            {
                error = "LoadLibraryW returned NULL in target process — DLL failed to load (missing dependency or bitness mismatch?)";
                return false;
            }

            return true;
        }
        catch (Exception ex)
        {
            error = $"Injection exception: {ex.Message}";
            return false;
        }
        finally
        {
            if (remoteMemory != IntPtr.Zero && hProcess != IntPtr.Zero)
                VirtualFreeEx(hProcess, remoteMemory, 0, MEM_RELEASE);
            if (hProcess != IntPtr.Zero)
                CloseHandle(hProcess);
        }
    }

    // ── Win32 interop ────────────────────────────────────────────────

    private const uint PROCESS_CREATE_THREAD     = 0x0002;
    private const uint PROCESS_VM_OPERATION      = 0x0008;
    private const uint PROCESS_VM_WRITE          = 0x0020;
    private const uint PROCESS_VM_READ           = 0x0010;
    private const uint PROCESS_QUERY_INFORMATION = 0x0400;
    private const uint MEM_COMMIT   = 0x1000;
    private const uint MEM_RESERVE  = 0x2000;
    private const uint MEM_RELEASE  = 0x8000;
    private const uint PAGE_READWRITE = 0x04;

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint access, bool inherit, int pid);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr VirtualAllocEx(IntPtr hProcess, IntPtr lpAddress,
        uint dwSize, uint flAllocType, uint flProtect);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool VirtualFreeEx(IntPtr hProcess, IntPtr lpAddress,
        uint dwSize, uint dwFreeType);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress,
        byte[] lpBuffer, uint nSize, out uint lpNumberOfBytesWritten);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr CreateRemoteThread(IntPtr hProcess, IntPtr lpThreadAttributes,
        uint dwStackSize, IntPtr lpStartAddress, IntPtr lpParameter, uint dwCreationFlags, out uint lpThreadId);

    [DllImport("kernel32.dll")]
    private static extern IntPtr GetModuleHandle(string lpModuleName);

    [DllImport("kernel32.dll")]
    private static extern IntPtr GetProcAddress(IntPtr hModule, string lpProcName);

    [DllImport("kernel32.dll")]
    private static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr hObject);

    [DllImport("kernel32.dll")]
    private static extern bool GetExitCodeThread(IntPtr hThread, out uint lpExitCode);
}
