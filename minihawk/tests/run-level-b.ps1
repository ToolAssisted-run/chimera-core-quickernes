# miniHawk Level B witness driver.
# Replays each quickerNES .sol test through EmuHawk (QuickNes core) and dumps
# the final 2KB RAM. In -Record mode the dumps become goldens; otherwise dumps
# are byte-compared against the stored goldens.
#
# EmuHawk instances run on a separate hidden Windows desktop (no window appears)
# and up to -Parallel of them run concurrently, each with private config/job files.
#
# Usage:
#   .\run-level-b.ps1                 # verify against goldens (simple mode)
#   .\run-level-b.ps1 -Record        # record goldens from current build
#   .\run-level-b.ps1 -Mode rerecord # per-frame savestate round-trip variant
#   .\run-level-b.ps1 -Filter super  # only tests whose name matches

param(
    [switch]$Record,
    [switch]$SkipExisting,
    [ValidateSet("simple", "rerecord")] [string]$Mode = "simple",
    [string]$Filter = "",
    [int]$Checkpoint = 0,
    [int]$Parallel = 8,
    [int]$TimeoutSec = 1800,
    [string]$MiniHawkRoot = ""
)

$ErrorActionPreference = "Stop"

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class HiddenLauncher
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct STARTUPINFO
    {
        public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2;
        public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateDesktop(string lpszDesktop, IntPtr lpszDevice, IntPtr pDevmode, int dwFlags, uint dwDesiredAccess, IntPtr lpsa);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr OpenDesktop(string lpszDesktop, int dwFlags, bool fInherit, uint dwDesiredAccess);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateProcess(string lpApplicationName, string lpCommandLine, IntPtr lpProcessAttributes, IntPtr lpThreadAttributes, bool bInheritHandles, int dwCreationFlags, IntPtr lpEnvironment, string lpCurrentDirectory, ref STARTUPINFO lpStartupInfo, out PROCESS_INFORMATION lpProcessInformation);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetExitCodeProcess(IntPtr hProcess, out int lpExitCode);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool TerminateProcess(IntPtr hProcess, int code);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    const uint GENERIC_ALL = 0x10000000;
    static IntPtr _desk = IntPtr.Zero;

    static void EnsureDesktop(string desktopName)
    {
        if (_desk != IntPtr.Zero) return;
        _desk = OpenDesktop(desktopName, 0, true, GENERIC_ALL);
        if (_desk == IntPtr.Zero) _desk = CreateDesktop(desktopName, IntPtr.Zero, IntPtr.Zero, 0, GENERIC_ALL, IntPtr.Zero);
        if (_desk == IntPtr.Zero) throw new Exception("CreateDesktop failed: " + Marshal.GetLastWin32Error());
    }

    // Launch without waiting; returns the process handle. Environment is
    // snapshotted at creation, so per-process env vars are safe to rotate.
    public static IntPtr Launch(string desktopName, string commandLine, string workingDir)
    {
        EnsureDesktop(desktopName);
        var si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
        si.lpDesktop = desktopName;
        PROCESS_INFORMATION pi;
        if (!CreateProcess(null, commandLine, IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, workingDir, ref si, out pi))
            throw new Exception("CreateProcess failed: " + Marshal.GetLastWin32Error());
        CloseHandle(pi.hThread);
        return pi.hProcess;
    }

    public static bool HasExited(IntPtr hProcess)
    {
        return WaitForSingleObject(hProcess, 0) == 0;
    }

    public static void Kill(IntPtr hProcess) { TerminateProcess(hProcess, 1); }

    public static void Close(IntPtr hProcess) { CloseHandle(hProcess); }
}
"@

if ($MiniHawkRoot -eq "") {
    foreach ($candidate in @("..\..\..\miniHawk", "..\..\..\BizHawk")) {
        $p = Join-Path $PSScriptRoot $candidate
        if (Test-Path -LiteralPath $p) { $MiniHawkRoot = $p; break }
    }
}
if ($MiniHawkRoot -eq "" -or -not (Test-Path -LiteralPath $MiniHawkRoot)) { throw "miniHawk checkout not found; pass -MiniHawkRoot <path>" }
$repoRoot   = (Resolve-Path $MiniHawkRoot).Path
# vendored snapshot of the quickerNES regression suite (upstream: TASEmulators/quickerNES, tests/)
$testsDir   = Join-Path $PSScriptRoot "suite"
$romsDirs   = @((Join-Path $testsDir "roms"), "C:\Users\sergiom\Documents\TAS\roms\nes")
$emuHawk    = Join-Path $repoRoot "build\EmuHawk.exe"
# cores load explicitly (no discovery): every instance is told which package to use
$corePackage = Join-Path $repoRoot "build\Cores\quickernes.zip"
if (-not (Test-Path -LiteralPath $corePackage)) { throw "core package not found at $corePackage (run quickerNES/minihawk/build-package.ps1 first)" }
$harnessDir = $PSScriptRoot
$workDir    = Join-Path $harnessDir "work"
$runDir     = Join-Path $workDir "run"
$goldenDir  = Join-Path $harnessDir "goldens\levelB"
$replayLua  = Join-Path $harnessDir "replay.lua"

# Tests excluded from the witness set (see miniHawk's docs/design-principles.md for rationale)
$excluded = @(
    "castlevania3.playaround",        # mapper 5 deliberately disabled in pinned core
    "novaTheSquirrel.anyPercent",     # pinned-core segfault (mapper 30 serializeState)
    "arkanoid2.arkFamicomController", # local ROM dump SHA1 mismatch
    "microMachines.race20",           # starts from quickerNES-native .state (Level A only)
    "saiyuukiWorld.lastHalf"          # starts from quickerNES-native .state (Level A only)
)

New-Item -ItemType Directory -Force $workDir, $runDir | Out-Null
if ($Record) { New-Item -ItemType Directory -Force $goldenDir | Out-Null }

# Base config: created by EmuHawk on first ever run; harness settings enforced here.
$configFilePath = Join-Path $workDir "config.ini"
if (Test-Path -LiteralPath $configFilePath) {
    $raw = Get-Content -LiteralPath $configFilePath -Raw
    $new = $raw -replace '"OpposingDirPolicy": \d', '"OpposingDirPolicy": 2'
    # GDI+ display: software-GL rendering on a hidden display wastes cores; display method cannot affect emulation
    $new = $new -replace '"DispMethod": \d', '"DispMethod": 1'
    $new = $new -replace '"SoundEnabled": true', '"SoundEnabled": false'
    if ($new -ne $raw) { Set-Content -LiteralPath $configFilePath -Value $new -Encoding utf8 }
}

# Some tests need non-default QuickNes sync settings (port peripherals).
# Port enum values: Port1 Gamepad=1 FourScore=2 ArkanoidNES=4 ArkanoidFamicom=5;
# Port2 Unplugged=0 Gamepad=1 FourScore2=3.
function Get-ConfigVariant([string]$tag, [int]$port1, [int]$port2) {
    $variant = Join-Path $workDir "config.$tag.ini"
    if (-not (Test-Path -LiteralPath $variant)) {
        if (-not (Test-Path -LiteralPath $configFilePath)) { return $null }
        $cfg = Get-Content -LiteralPath $configFilePath -Raw | ConvertFrom-Json
        $qn = [pscustomobject]@{ Port1 = $port1; Port2 = $port2 }
        if ($null -eq $cfg.CoreSyncSettings) {
            $cfg | Add-Member -NotePropertyName CoreSyncSettings -NotePropertyValue ([pscustomobject]@{}) -Force
        }
        $cfg.CoreSyncSettings | Add-Member -NotePropertyName "BizHawk.Emulation.Cores.Consoles.Nintendo.QuickNES.QuickNES" -NotePropertyValue $qn -Force
        $cfg | ConvertTo-Json -Depth 100 | Out-File -LiteralPath $variant -Encoding utf8
    }
    return $variant
}

function Get-ConfigForTest($testJson) {
    switch ($testJson."Controller 1 Type") {
        "ArkanoidNES"     { return Get-ConfigVariant "arkanoidNES" 4 0 }
        "ArkanoidFamicom" { return Get-ConfigVariant "arkanoidFamicom" 5 0 }
        "FourScore1"      { return Get-ConfigVariant "fourscore" 2 3 }
        default           { return $configFilePath }
    }
}

function Get-RomPath([string]$romFileEntry) {
    $name = $romFileEntry -replace "^roms/", ""
    foreach ($dir in $romsDirs) {
        $p = Join-Path $dir $name
        if (Test-Path -LiteralPath $p) { return $p }
    }
    return $null
}

# ---------- build the job list ----------
$results = New-Object System.Collections.ArrayList
$jobs = New-Object System.Collections.Queue
$tests = Get-ChildItem $testsDir -Filter *.test | Sort-Object Name
foreach ($testFile in $tests) {
    $name = $testFile.BaseName
    if ($excluded -contains $name) { continue }
    if ($Filter -and ($name -notmatch $Filter)) { continue }

    $t = Get-Content -LiteralPath $testFile.FullName -Raw | ConvertFrom-Json
    $romPath = Get-RomPath $t."Rom File"
    if ($null -eq $romPath) {
        [void]$results.Add([pscustomobject]@{ Test = $name; Result = "SKIP"; Detail = "ROM not found: $($t.'Rom File')" })
        continue
    }
    $sha1 = (Get-FileHash -LiteralPath $romPath -Algorithm SHA1).Hash
    if ($sha1 -ne $t."Expected ROM SHA1") {
        [void]$results.Add([pscustomobject]@{ Test = $name; Result = "SKIP"; Detail = "ROM SHA1 mismatch" })
        continue
    }
    if ($Record -and $SkipExisting -and (Test-Path -LiteralPath (Join-Path $goldenDir "$name.$Mode.ram.bin"))) {
        [void]$results.Add([pscustomobject]@{ Test = $name; Result = "RECORDED"; Detail = "already present (skipped)" })
        continue
    }
    $cfgTemplate = Get-ConfigForTest $t
    if ($null -eq $cfgTemplate) {
        [void]$results.Add([pscustomobject]@{ Test = $name; Result = "SKIP"; Detail = "base config.ini not generated yet; rerun" })
        continue
    }
    $jobs.Enqueue([pscustomobject]@{
        Name = $name; Test = $t; RomPath = $romPath; CfgTemplate = $cfgTemplate
    })
}

# ---------- run with up to $Parallel concurrent hidden EmuHawks ----------
$running = New-Object System.Collections.ArrayList

function Start-TestJob($job) {
    $name     = $job.Name
    $outFile  = Join-Path $workDir "$name.$Mode.ram.bin"
    $metaFile = Join-Path $workDir "$name.$Mode.meta.txt"
    Remove-Item -Force -ErrorAction SilentlyContinue $outFile, $metaFile

    # private config + job file per instance (EmuHawk rewrites config on exit)
    $cfgRun = Join-Path $runDir "config.$name.ini"
    Copy-Item -LiteralPath $job.CfgTemplate $cfgRun -Force
    $jobFile = Join-Path $runDir "job.$name.txt"
    @(
        "sol=$(Join-Path $testsDir $job.Test.'Sequence File')",
        "out=$outFile",
        "meta=$metaFile",
        "controller1=$($job.Test.'Controller 1 Type')",
        "controller2=$($job.Test.'Controller 2 Type')",
        "mode=$Mode",
        "checkpoint=$Checkpoint"
    ) | Out-File -LiteralPath $jobFile -Encoding ascii

    $env:MINIHAWK_JOB = $jobFile   # snapshotted into the child env at CreateProcess
    $cmdLine = "`"$emuHawk`" --headless `"--config=$cfgRun`" `"--core=$corePackage`" `"--lua=$replayLua`" `"$($job.RomPath)`""
    $h = [HiddenLauncher]::Launch("minihawk_hidden", $cmdLine, $repoRoot)
    [void]$running.Add([pscustomobject]@{
        Name = $name; Handle = $h; Sw = [System.Diagnostics.Stopwatch]::StartNew()
        OutFile = $outFile; MetaFile = $metaFile
    })
}

function Complete-TestJob($slot) {
    $name = $slot.Name
    if (-not (Test-Path -LiteralPath $slot.MetaFile)) {
        [void]$results.Add([pscustomobject]@{ Test = $name; Result = "FAIL"; Detail = "no meta produced" })
        return
    }
    $metaContent = @{}
    Get-Content -LiteralPath $slot.MetaFile | ForEach-Object {
        $k, $v = $_ -split "=", 2
        $metaContent[$k] = $v
    }
    if ($metaContent["status"] -ne "OK") {
        [void]$results.Add([pscustomobject]@{ Test = $name; Result = "FAIL"; Detail = "$($metaContent['status']): $($metaContent['detail'])" })
        return
    }
    $secs = [int]$slot.Sw.Elapsed.TotalSeconds
    if ($Record) {
        Copy-Item -LiteralPath $slot.OutFile (Join-Path $goldenDir "$name.$Mode.ram.bin") -Force
        [void]$results.Add([pscustomobject]@{ Test = $name; Result = "RECORDED"; Detail = "$($metaContent['frames']) frames in ${secs}s" })
        return
    }
    $goldenFile = Join-Path $goldenDir "$name.$Mode.ram.bin"
    if (-not (Test-Path -LiteralPath $goldenFile)) {
        [void]$results.Add([pscustomobject]@{ Test = $name; Result = "NOGOLDEN"; Detail = "" })
        return
    }
    $a = [System.IO.File]::ReadAllBytes($slot.OutFile)
    $b = [System.IO.File]::ReadAllBytes($goldenFile)
    $same = ($a.Length -eq $b.Length)
    if ($same) {
        for ($i = 0; $i -lt $a.Length; $i++) {
            if ($a[$i] -ne $b[$i]) { $same = $false; break }
        }
    }
    $verdict = "PASS"
    if (-not $same) { $verdict = "FAIL" }
    [void]$results.Add([pscustomobject]@{ Test = $name; Result = $verdict; Detail = "$($metaContent['frames']) frames in ${secs}s" })
}

while ($jobs.Count -gt 0 -or $running.Count -gt 0) {
    while ($jobs.Count -gt 0 -and $running.Count -lt $Parallel) {
        Start-TestJob $jobs.Dequeue()
    }
    Start-Sleep -Milliseconds 250
    for ($s = $running.Count - 1; $s -ge 0; $s--) {
        $slot = $running[$s]
        if ([HiddenLauncher]::HasExited($slot.Handle)) {
            [HiddenLauncher]::Close($slot.Handle)
            $running.RemoveAt($s)
            Complete-TestJob $slot
        }
        elseif ($slot.Sw.Elapsed.TotalSeconds -gt $TimeoutSec) {
            [HiddenLauncher]::Kill($slot.Handle)
            [HiddenLauncher]::Close($slot.Handle)
            $running.RemoveAt($s)
            [void]$results.Add([pscustomobject]@{ Test = $slot.Name; Result = "TIMEOUT"; Detail = "$TimeoutSec s" })
        }
    }
}

$results | Sort-Object Test | Format-Table -AutoSize
$failed = @($results | Where-Object { $_.Result -in @("FAIL", "TIMEOUT", "NOGOLDEN") })
Write-Host ""
Write-Host "$(@($results | Where-Object { $_.Result -in @('PASS','RECORDED') }).Count) ok, $($failed.Count) failed, $(@($results | Where-Object Result -eq 'SKIP').Count) skipped"
if ($failed.Count -gt 0) { exit 1 }
exit 0
