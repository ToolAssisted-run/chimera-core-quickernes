# Runs a command line on the hidden minihawk desktop and waits for it to exit.
# Usage: .\hidden-run.ps1 -CommandLine '"C:\...\EmuHawk.exe" "--lua=..." "rom.nes"' [-TimeoutMs 120000]
param(
    [Parameter(Mandatory = $true)] [string]$CommandLine,
    [int]$TimeoutMs = 120000
)

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class MiniHawkHiddenRun
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct STARTUPINFO { public int cb; public string lpReserved; public string lpDesktop; public string lpTitle; public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags; public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError; }
    [StructLayout(LayoutKind.Sequential)]
    public struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr OpenDesktop(string d, int f, bool i, uint acc);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr CreateDesktop(string d, IntPtr a, IntPtr b, int f, uint acc, IntPtr sa);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool CreateProcess(string an, string cl, IntPtr pa, IntPtr ta, bool ih, int cf, IntPtr env, string cd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool TerminateProcess(IntPtr h, int c);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    public static int Run(string cmdLine, int timeoutMs)
    {
        IntPtr d = OpenDesktop("minihawk_hidden", 0, true, 0x10000000);
        if (d == IntPtr.Zero) d = CreateDesktop("minihawk_hidden", IntPtr.Zero, IntPtr.Zero, 0, 0x10000000, IntPtr.Zero);
        if (d == IntPtr.Zero) throw new Exception("CreateDesktop failed: " + Marshal.GetLastWin32Error());
        var si = new STARTUPINFO(); si.cb = Marshal.SizeOf(typeof(STARTUPINFO)); si.lpDesktop = "minihawk_hidden";
        PROCESS_INFORMATION pi;
        if (!CreateProcess(null, cmdLine, IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, null, ref si, out pi)) throw new Exception("CreateProcess failed: " + Marshal.GetLastWin32Error());
        int result = 0;
        if (WaitForSingleObject(pi.hProcess, (uint)timeoutMs) != 0) { TerminateProcess(pi.hProcess, 1); result = -999; }
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        return result;
    }
}
"@

exit ([MiniHawkHiddenRun]::Run($CommandLine, $TimeoutMs))
