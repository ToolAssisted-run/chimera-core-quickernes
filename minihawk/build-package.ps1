# Builds the QuickerNES core package for miniHawk and installs quickernes.zip into
# <MiniHawkRoot>/build/Cores/.
# Prereq: the miniHawk solution has been built (this package references the contract
# DLLs and the settings source generator from <MiniHawkRoot>/build/dll).
param(
    [string]$Configuration = "Release",
    [string]$MiniHawkRoot = ""
)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
if ($MiniHawkRoot -eq "") { $MiniHawkRoot = Join-Path $here "..\..\BizHawk" }
$MiniHawkRoot = (Resolve-Path $MiniHawkRoot).Path

dotnet build (Join-Path $here "MiniHawk.QuickerNES.csproj") -c $Configuration -p:MiniHawkRoot=$MiniHawkRoot -v q --nologo
if ($LASTEXITCODE -ne 0) { throw "package build failed" }

$staging = Join-Path $here "bin\package-staging"
if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Force $staging | Out-Null

Copy-Item (Join-Path $here "minihawk-core.json") $staging
Copy-Item (Join-Path $here "bin\$Configuration\MiniHawk.QuickerNES.dll") $staging
Copy-Item (Join-Path $here "defctrl.json") $staging
Copy-Item (Join-Path $here "lua") (Join-Path $staging "lua") -Recurse
Copy-Item (Join-Path $here "natives\libquicknes.dll") $staging
if (Test-Path (Join-Path $here "natives\libquicknes.so")) { Copy-Item (Join-Path $here "natives\libquicknes.so") $staging }

$coresDir = Join-Path $MiniHawkRoot "build\Cores"
New-Item -ItemType Directory -Force $coresDir | Out-Null
$zipPath = Join-Path $coresDir "quickernes.zip"
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Compress-Archive -Path (Join-Path $staging "*") -DestinationPath $zipPath
# stale extracted caches are keyed by zip timestamp and cleaned up by the loader
# itself; clear them here too, best-effort, to keep the tree tidy
Get-ChildItem (Join-Path $coresDir "_cache") -Directory -Filter "quickernes-*" -ErrorAction SilentlyContinue |
    ForEach-Object { try { Remove-Item -Recurse -Force $_.FullName } catch {} }
Write-Host "packaged -> $zipPath"
