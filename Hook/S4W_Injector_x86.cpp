// ═══════════════════════════════════════════════════════════════════════════
//  S4W_Injector_x86.exe — 32-bit helper injector
//
//  Compiled as a 32-bit executable. Used by S4W (64-bit) to inject
//  S4W_Hook_x86.dll into 32-bit (WOW64) game processes.
//
//  A 64-bit process cannot reliably inject into a 32-bit process using
//  CreateRemoteThread + LoadLibraryW because the kernel32.dll address
//  differs between 64-bit and 32-bit address spaces.
//
//  Usage: S4W_Injector_x86.exe <PID> <DLL_PATH>
//  Exit codes: 0 = success, 1 = failure (error written to stderr)
// ═══════════════════════════════════════════════════════════════════════════

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char* argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: S4W_Injector_x86.exe <PID> <DLL_PATH>\n");
        return 1;
    }

    int pid = atoi(argv[1]);
    const char* dllPathA = argv[2];

    if (pid <= 0) {
        fprintf(stderr, "Invalid PID: %s\n", argv[1]);
        return 1;
    }

    // Convert DLL path to wide string
    wchar_t dllPath[MAX_PATH] = {};
    MultiByteToWideChar(CP_UTF8, 0, dllPathA, -1, dllPath, MAX_PATH);

    // Verify DLL exists
    DWORD attr = GetFileAttributesW(dllPath);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "DLL not found: %ls\n", dllPath);
        return 1;
    }

    // Open target process
    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE, (DWORD)pid);

    if (!hProcess) {
        fprintf(stderr, "OpenProcess failed for PID=%d, error=%lu\n", pid, GetLastError());
        return 1;
    }

    // Allocate memory in target for DLL path
    size_t pathBytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    LPVOID remoteMem = VirtualAllocEx(hProcess, NULL, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        fprintf(stderr, "VirtualAllocEx failed, error=%lu\n", GetLastError());
        CloseHandle(hProcess);
        return 1;
    }

    // Write DLL path
    SIZE_T written = 0;
    if (!WriteProcessMemory(hProcess, remoteMem, dllPath, pathBytes, &written)) {
        fprintf(stderr, "WriteProcessMemory failed, error=%lu\n", GetLastError());
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return 1;
    }

    // Get LoadLibraryW address — this is the 32-bit kernel32.dll address
    // because THIS exe is 32-bit, so GetModuleHandle returns the 32-bit kernel32
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLibAddr = GetProcAddress(hKernel32, "LoadLibraryW");
    if (!loadLibAddr) {
        fprintf(stderr, "GetProcAddress(LoadLibraryW) failed\n");
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return 1;
    }

    // Create remote thread
    DWORD threadId = 0;
    HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0,
        (LPTHREAD_START_ROUTINE)loadLibAddr, remoteMem, 0, &threadId);

    if (!hThread) {
        fprintf(stderr, "CreateRemoteThread failed, error=%lu\n", GetLastError());
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return 1;
    }

    // Wait for completion (max 10 seconds)
    DWORD waitResult = WaitForSingleObject(hThread, 10000);

    // Check thread exit code (LoadLibrary return value)
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);

    CloseHandle(hThread);
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    if (waitResult == WAIT_TIMEOUT) {
        fprintf(stderr, "LoadLibraryW timed out (10s)\n");
        return 1;
    }

    if (exitCode == 0) {
        fprintf(stderr, "LoadLibraryW returned NULL (DLL failed to load)\n");
        return 1;
    }

    printf("OK\n");
    return 0;
}
