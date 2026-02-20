<#
.SYNOPSIS
    Compile and run the RIVR C-layer acceptance tests on the host.

.DESCRIPTION
    Searches for a C11-capable compiler (gcc preferred, cl.exe fallback),
    builds the acceptance test binary from firmware_core + tests/ sources,
    runs it, and exits with 0 (all pass) or 1 (at least one failure).

    Run from the project root:
        cd e:\Projects\Rivr
        .\tests\run.ps1

    To keep the binary after the run:
        .\tests\run.ps1 -Keep
#>
param([switch]$Keep)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path $PSScriptRoot -Parent
Push-Location $ProjectRoot

try {

# ── Locate a C11 compiler ───────────────────────────────────────────────────

$Gcc     = $null
$UseMsvc = $false

# 1. gcc already in PATH
if (Get-Command gcc -ErrorAction SilentlyContinue) {
    $Gcc = "gcc"
}

# 2. MSYS2 (typical Chocolatey/manual install)
if (-not $Gcc) {
    $candidates = @(
        "C:\msys64\mingw64\bin\gcc.exe",
        "C:\msys64\usr\bin\gcc.exe",
        "C:\msys32\mingw32\bin\gcc.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $Gcc = $c; break }
    }
}

# 3. Git for Windows
if (-not $Gcc) {
    $gitGcc = "C:\Program Files\Git\usr\bin\gcc.exe"
    if (Test-Path $gitGcc) { $Gcc = $gitGcc }
}

# 4. ESP-IDF MinGW toolchain (native gcc only, skip xtensa/riscv cross-compilers)
if (-not $Gcc) {
    $espTools = "$env:USERPROFILE\.espressif\tools"
    if (Test-Path $espTools) {
        $found = Get-ChildItem -Path $espTools -Recurse -Filter "gcc.exe" -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -notmatch "xtensa|riscv" } |
            Select-Object -First 1
        if ($found) { $Gcc = $found.FullName }
    }
}

# 5. MSVC cl.exe via vswhere (VS 2019 / 2022 / Build Tools)
$VsDevCmd = $null
if (-not $Gcc) {
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vsWhere)) {
        $vsWhere = "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
    }
    if (Test-Path $vsWhere) {
        $vsInstall = & $vsWhere -latest -property installationPath 2>$null |
                     Select-Object -Last 1
        if ($vsInstall) {
            $candidate = "$vsInstall\Common7\Tools\VsDevCmd.bat"
            if (Test-Path $candidate) { $VsDevCmd = $candidate }
        }
    }
    if ($VsDevCmd) { $UseMsvc = $true }
}

if (-not $Gcc -and -not $UseMsvc) {
    Write-Error @"
No C11 compiler found.  Install one of:
  - MSYS2:  https://www.msys2.org/  (then: pacman -S mingw-w64-x86_64-gcc)
  - Git for Windows (ships with gcc in Git Bash)
  - Manual MinGW:  https://winlibs.com/
After installing, re-open this terminal or add the bin/ folder to PATH.
"@
    exit 1
}

if ($Gcc) {
    Write-Host "Compiler: $Gcc (gcc)" -ForegroundColor Cyan
} else {
    Write-Host "Compiler: MSVC via $VsDevCmd" -ForegroundColor Cyan
}

# ── Source files ─────────────────────────────────────────────────────────────

$Sources = @(
    "firmware_core/protocol.c",
    "firmware_core/routing.c",
    "firmware_core/route_cache.c",
    "firmware_core/pending_queue.c",
    "tests/test_stubs.c",
    "tests/test_acceptance.c"
)

$OutExe = "tests\acceptance_test.exe"

# ── Compile ───────────────────────────────────────────────────────────────────

Write-Host "`nCompiling..." -ForegroundColor Cyan

if ($Gcc) {
    # ── GCC / Clang path ─────────────────────────────────────────────────────
    $CompileArgs = @(
        "-std=c11", "-Ifirmware_core",
        "-Wall", "-Wextra", "-Wno-unused-parameter", "-g"
    ) + $Sources + @("-o", $OutExe)

    Write-Host ("  $Gcc " + ($CompileArgs -join " "))
    & $Gcc @CompileArgs
    if ($LASTEXITCODE -ne 0) { Write-Error "GCC compilation failed"; exit 1 }

} else {
    # ── MSVC path — run cl.exe inside a VsDevCmd.bat environment ─────────────
    # <stdatomic.h> supported in MSVC with /std:c11 since VS 2022 17.5.
    # /DIRAM_ATTR= removes the ESP32-specific function attribute.
    $SrcQuoted = ($Sources | ForEach-Object { "`"$_`"" }) -join " "
    # /Itests must come BEFORE /Ifirmware_core so tests/stdatomic.h
    # (the single-threaded shim) shadows the broken MSVC system header.
    $MsvcFlags = "/nologo /TC /std:c11 /W3 /Itests /Ifirmware_core /DIRAM_ATTR="
    $OutFlag   = "/Fe:`"$OutExe`""
    $CmdLine   = "`"$VsDevCmd`" -no_logo && cl.exe $MsvcFlags $SrcQuoted $OutFlag"

    Write-Host "  cmd /c $CmdLine"
    cmd /c $CmdLine
    if ($LASTEXITCODE -ne 0) { Write-Error "MSVC compilation failed"; exit 1 }

    # cl.exe emits .obj files in the current directory (project root).
    Remove-Item "*.obj" -Force -ErrorAction SilentlyContinue
}

Write-Host "Build OK → $OutExe`n" -ForegroundColor Green

# ── Run ───────────────────────────────────────────────────────────────────────

Write-Host "Running acceptance tests..." -ForegroundColor Cyan
Write-Host ("─" * 50)

& ".\$OutExe"
$TestExit = $LASTEXITCODE

Write-Host ("─" * 50)

if ($TestExit -eq 0) {
    Write-Host "ALL TESTS PASSED" -ForegroundColor Green
} else {
    Write-Host "SOME TESTS FAILED (exit $TestExit)" -ForegroundColor Red
}

# ── Clean up ──────────────────────────────────────────────────────────────────

if (-not $Keep -and (Test-Path $OutExe)) { Remove-Item $OutExe }

exit $TestExit

} finally {
    Pop-Location
}
