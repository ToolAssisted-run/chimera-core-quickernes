# miniHawk Level B witness driver.
# Plays each test's project through Chimera's window (run headless) against the WATERBOX package
# - core.wbx driven by miniHawk's one generic adapter - and dumps the final 2KB
# RAM. In -Record mode the dumps become goldens; otherwise dumps are
# byte-compared against the stored goldens.
#
# The goldens predate the waterbox port: they were recorded through the retired
# managed package and cross-validated against the native tester's own dumps
# (goldens/native/). Reproducing them byte-for-byte is exactly the proof that
# sandboxing changed no emulation.
#
# The window needs no display; instances still start on a separate hidden Windows
# desktop, and up to -Parallel of them run concurrently, each with private
# settings, cache, recovery and job files.
#
# Usage:
#   .\run-level-b.ps1                 # verify the free-to-distribute set (CI default)
#   .\run-level-b.ps1 -Set full       # the whole witness set (commercial roms)
#   .\run-level-b.ps1 -Record        # record goldens from current build
#   .\run-level-b.ps1 -Filter super  # only tests whose name matches

param(
    [switch]$Record,
    [switch]$SkipExisting,
    # "free" is the default because it is the only set that can run anywhere: its
    # roms are redistributable and vendored in suite/roms, so CI needs nothing
    # provisioned. "full" adds the commercial-rom tests, which live in a local rom
    # library - run it for significant changes, before a push, and whenever the
    # core or the ABI moves.
    [ValidateSet("free", "full")] [string]$Set = "free",
    [string]$Filter = "",
    [int]$Checkpoint = 0,
    [int]$Parallel = 8,
    [int]$TimeoutSec = 7200, # superOffroad.anyPercent is 182,180 frames; a smaller cap reports it as TIMEOUT
    [string]$ChimeraRoot = ""
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

if ($ChimeraRoot -eq "") {
    foreach ($candidate in @("..\..\..\chimera", (Join-Path $env:USERPROFILE "chimera"))) {
        $p = Join-Path $PSScriptRoot $candidate
        if (Test-Path -LiteralPath $p) { $ChimeraRoot = $p; break }
    }
}
if ($ChimeraRoot -eq "" -or -not (Test-Path -LiteralPath $ChimeraRoot)) { throw "chimera checkout not found; pass -ChimeraRoot <path>" }
$repoRoot   = (Resolve-Path $ChimeraRoot).Path
# vendored snapshot of the quickerNES regression suite (upstream: TASEmulators/quickerNES, tests/)
$testsDir   = Join-Path $PSScriptRoot "suite"
$romsDirs   = @((Join-Path $testsDir "roms"), "C:\Users\sergiom\Documents\TAS\roms\nes")
$chimeraExe = Join-Path $repoRoot "build\meson-windows\source\ui\chimera.exe"
# cores load explicitly (no discovery): every instance is told which package to use
$corePackage = Join-Path $repoRoot "build\Cores\quickernes.chimeraCore"
if (-not (Test-Path -LiteralPath $chimeraExe)) { throw "Chimera not built: $chimeraExe" }
$chimeraRun = Join-Path $repoRoot "build\dll\chimera-run.exe"
if (-not (Test-Path -LiteralPath $corePackage)) { throw "core package not found at $corePackage (run quickerNES/waterbox/build-package.sh first)" }
$harnessDir = $PSScriptRoot
$workDir    = Join-Path $harnessDir "work"
$runDir     = Join-Path $workDir "run"
$goldenDir  = Join-Path $harnessDir "goldens\levelB"
$playLua    = Join-Path $harnessDir "playmovie.lua"
$moviesDir  = Join-Path $harnessDir "movies"

# Tests excluded from the witness set (see miniHawk's docs/design-principles.md for rationale)
# Tests whose roms are free to distribute and are vendored in suite/roms. This is
# the CI set: it needs nothing provisioned.
$freeSet = @(
    "sprilo.anyPercent",
    "novaTheSquirrel.anyPercent"
)

$excluded = @(
    "castlevania3.playaround",        # mapper 5 deliberately disabled in pinned core
    "arkanoid2.arkFamicomController", # local ROM dump SHA1 mismatch
    "microMachines.race20",           # starts from quickerNES-native .state (Level A only)
    "saiyuukiWorld.lastHalf"          # starts from quickerNES-native .state (Level A only)
)

New-Item -ItemType Directory -Force $workDir, $runDir | Out-Null
if ($Record) { New-Item -ItemType Directory -Force $goldenDir | Out-Null }


# Tests whose roms are free to distribute and are vendored in suite/roms. This is
# the CI set: it needs nothing provisioned.
$freeSet = @(
    "sprilo.anyPercent",
    "novaTheSquirrel.anyPercent"
)

$excluded = @(
    "castlevania3.playaround",        # mapper 5 deliberately disabled in pinned core
    "arkanoid2.arkFamicomController", # local ROM dump SHA1 mismatch
    "microMachines.race20",           # starts from quickerNES-native .state (Level A only)
    "saiyuukiWorld.lastHalf"          # starts from quickerNES-native .state (Level A only)
)

New-Item -ItemType Directory -Force $workDir, $runDir | Out-Null
if ($Record) { New-Item -ItemType Directory -Force $goldenDir | Out-Null }


# Every test runs from the same base settings: none. Peripherals (Four Score,
# Arkanoid paddles) are carried in the movie's own sync settings.
$env:ALSOFT_DRIVERS = "null"
$env:CHIMERA_CORES_DIR = Join-Path $repoRoot "build\Cores"
$env:CHIMERA_QT_CHIMERA_SETTINGS = Join-Path $workDir "no-chimera-config.ini"

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
    if ($Set -eq "free" -and ($freeSet -notcontains $name)) { continue }

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
    if ($Record -and $SkipExisting -and (Test-Path -LiteralPath (Join-Path $goldenDir "$name."simple".ram.bin"))) {
        [void]$results.Add([pscustomobject]@{ Test = $name; Result = "RECORDED"; Detail = "already present (skipped)" })
        continue
    }
    $moviePath = Join-Path $moviesDir "$name.chimeraProject"
    if (-not (Test-Path -LiteralPath $moviePath)) {
        [void]$results.Add([pscustomobject]@{ Test = $name; Result = "SKIP"; Detail = "no movie: run tools/make-movies.sh" })
        continue
    }
    $jobs.Enqueue([pscustomobject]@{
        Name = $name; Test = $t; RomPath = $romPath; MoviePath = $moviePath
    })
}

# ---------- run with up to $Parallel concurrent hidden windows ----------
$running = New-Object System.Collections.ArrayList

function Start-TestJob($job) {
    $name     = $job.Name
    $outFile  = Join-Path $workDir "$name."simple".ram.bin"
    $metaFile = Join-Path $workDir "$name."simple".meta.txt"
    Remove-Item -Force -ErrorAction SilentlyContinue $outFile, $metaFile

    # private settings + job file per instance (the window rewrites its settings on exit)
    $cfgRun = Join-Path $runDir "settings.$name.json"
    Remove-Item -Force -ErrorAction SilentlyContinue $cfgRun
    $jobFile = Join-Path $runDir "job.$name.txt"
    @(
        "out=$outFile",
        "meta=$metaFile"
    ) | Out-File -LiteralPath $jobFile -Encoding ascii

    # A game runs only in a project (Chimera, 2026-10-26): the test's movie (its
    # settings and input; it names no game file) and the rom, made into one
    $projDir = Join-Path $runDir "$name.project"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $projDir
    New-Item -ItemType Directory -Force $projDir | Out-Null
    Copy-Item -LiteralPath $job.RomPath $projDir
    $movie = Get-Content -Raw -LiteralPath $job.MoviePath | ConvertFrom-Json
    $movieTxt = Join-Path $projDir "movie.txt"
    [IO.File]::WriteAllText($movieTxt, $movie.input)
    $settings = if ($movie.settings) { $movie.settings | ConvertTo-Json -Compress } else { "{}" }
    $project = Join-Path $projDir "$name.chimeraProject"
    $romCopy = Join-Path $projDir (Split-Path -Leaf $job.RomPath)
    # quotes inside an argument are written \" for the program's command line
    $makeArgs = "--new-project `"$project`" `"$corePackage`" `"$romCopy`" `"$movieTxt`" --settings `"$($settings -replace '"', '\"')`""
    $make = Start-Process -FilePath $chimeraRun -ArgumentList $makeArgs -Wait -PassThru -NoNewWindow `
        -RedirectStandardOutput (Join-Path $projDir "make.out") -RedirectStandardError (Join-Path $projDir "make.log")
    if ($make.ExitCode -ne 0) { Write-Warning "${name}: no project made (see $projDir\make.log)" }

    $env:MINIHAWK_JOB = $jobFile   # snapshotted into the child env at CreateProcess
    $env:CHIMERA_QT_CACHE = Join-Path $runDir "qt-cache.$name"
    $env:CHIMERA_QT_RECOVERY = Join-Path $runDir "qt-recovery.$name"
    $env:CHIMERA_QT_HASHES = Join-Path $runDir "qt-file-hashes.$name.txt"
    $cmdLine = "`"$chimeraExe`" --headless `"--config=$cfgRun`" `"--core=$corePackage`" `"--lua=$playLua`" `"--project=$project`""
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
        Copy-Item -LiteralPath $slot.OutFile (Join-Path $goldenDir "$name."simple".ram.bin") -Force
        [void]$results.Add([pscustomobject]@{ Test = $name; Result = "RECORDED"; Detail = "$($metaContent['frames']) frames in ${secs}s" })
        return
    }
    $goldenFile = Join-Path $goldenDir "$name."simple".ram.bin"
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
Write-Host "$(@($results | Where-Object { $_.Result -in @('PASS','RECORDED') }).Count) ok, $($failed.Count) failed, $(@($results | Where-Object Result -eq 'SKIP').Count) skipped  (set: $Set)"
if ($failed.Count -gt 0) { exit 1 }
exit 0
