# Builds the SyncPlay Windows installer.
#
#   1. (optionally) builds the Release binary
#   2. stages the exe + all runtime DLLs + the VC++ runtime into packaging\stage
#   3. compiles packaging\SyncPlay.iss with Inno Setup (ISCC) -> packaging\dist
#
# Usage:
#   pwsh packaging\build_installer.ps1 [-Version 1.0.0] [-SkipBuild]

param(
    [string]$Version = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot

# Default the version to the one declared in CMakeLists.txt (single source of truth).
if (-not $Version) {
    $projLine = Select-String -Path (Join-Path $repo "CMakeLists.txt") -Pattern 'project\(SyncPlay VERSION ([0-9.]+)'
    if ($projLine) { $Version = $projLine.Matches[0].Groups[1].Value }
    if (-not $Version) { throw "Could not parse version from CMakeLists.txt; pass -Version explicitly." }
    Write-Host "==> Version from CMakeLists.txt: $Version" -ForegroundColor DarkGray
}
$buildDir = Join-Path $repo "cmake-build-release"
$stage = Join-Path $PSScriptRoot "stage"
$dist = Join-Path $PSScriptRoot "dist"

# --- toolchain (refreshes PATH, imports vcvars64, sets VCToolsRedistDir) ---
# Local dev box helper; CI enters with the MSVC environment already configured.
if (Test-Path C:\packages\build-setup.ps1) {
    . C:\packages\build-setup.ps1 | Out-Null
}

# --- 1. build ---
if (-not $SkipBuild) {
    Write-Host "==> Building Release..." -ForegroundColor Cyan
    cmake --build $buildDir --target SyncPlay
    if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)" }
}

$exe = Join-Path $buildDir "SyncPlay.exe"
if (-not (Test-Path $exe)) { throw "SyncPlay.exe not found at $exe - build first." }

# --- 2. stage ---
Write-Host "==> Staging runtime into $stage" -ForegroundColor Cyan
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null

Copy-Item $exe $stage
# All runtime DLLs the build placed next to the exe (libmpv, datachannel, juice,
# libssl/libcrypto, ...). Copying the whole set is robust against missed deps.
Copy-Item (Join-Path $buildDir "*.dll") $stage

# The app loads assets\SyncPlay.png at runtime (window icon + idle screen), so the
# assets folder must ship alongside the exe.
$assets = Join-Path $buildDir "assets"
if (Test-Path $assets) { Copy-Item $assets $stage -Recurse }

# VC++ runtime (the exe imports MSVCP140 / VCRUNTIME140 / VCRUNTIME140_1; ship the
# msvcp satellites too so the STL's transitive deps are covered). Not guaranteed on
# a clean PC, so deploy app-local.
$crtDir = Get-ChildItem (Join-Path $env:VCToolsRedistDir "x64") -Directory -Filter "*.CRT" |
          Select-Object -First 1 -ExpandProperty FullName
if (-not $crtDir) { throw "VC++ redist CRT folder not found under $env:VCToolsRedistDir" }
$crtDlls = @(
    "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_atomic_wait.dll",
    "vcruntime140.dll", "vcruntime140_1.dll"
)
foreach ($d in $crtDlls) {
    $src = Join-Path $crtDir $d
    if (Test-Path $src) { Copy-Item $src $stage }
}

Write-Host "Staged files:" -ForegroundColor DarkGray
Get-ChildItem $stage | ForEach-Object { "  {0,-28} {1,8:N0} KB" -f $_.Name, ($_.Length / 1KB) }

# --- 3. compile installer ---
$iscc = @(
    "C:\Users\$env:USERNAME\AppData\Local\Programs\Inno Setup 6\ISCC.exe",
    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
    "C:\Program Files\Inno Setup 6\ISCC.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) {
    $cmd = Get-Command iscc -ErrorAction SilentlyContinue
    if ($cmd) { $iscc = $cmd.Source }
}
if (-not $iscc) { throw "ISCC.exe (Inno Setup) not found. Install with: winget install JRSoftware.InnoSetup" }

if (-not (Test-Path $dist)) { New-Item -ItemType Directory -Path $dist -Force | Out-Null }
Write-Host "==> Compiling installer with $iscc" -ForegroundColor Cyan
& $iscc "/DMyAppVersion=$Version" (Join-Path $PSScriptRoot "SyncPlay.iss")
if ($LASTEXITCODE -ne 0) { throw "ISCC failed (exit $LASTEXITCODE)" }

$out = Join-Path $dist "SyncPlay-Setup-$Version.exe"
Write-Host ""
Write-Host "==> Installer ready: $out" -ForegroundColor Green
if (Test-Path $out) { "    {0:N1} MB" -f ((Get-Item $out).Length / 1MB) }
