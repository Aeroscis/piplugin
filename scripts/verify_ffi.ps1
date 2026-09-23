# verify_ffi.ps1 - run the FFI examples (examples/ffi) and report a verdict.
#
# roadmap ECO-06: "pure C ABI" is a promise about *other languages*, so the claim
# is only worth anything if it is executed. Each demo loads the framework DLL and
# an official test plugin, QueryInterfaces the factory, reads the descriptor, and
# creates / initializes / terminates an instance with a HOST OBJECT BUILT IN THAT
# LANGUAGE (a vtbl of callbacks) - then unloads and reloads.
#
# A language whose toolchain is not installed is SKIPPED with a visible line, not
# failed: a machine without cargo should still be able to run the rest.
#
# Usage: pwsh -NoProfile -File scripts\verify_ffi.ps1 [-Plugin <name.dll>] [-BinDir <dir>]
# Exit code 0 = every language that could run, passed.
#
# Keep this file ASCII-only: it has no BOM, and Windows PowerShell 5.1 decodes a
# BOM-less script as ANSI, where non-ASCII bytes mis-decode and can eat the
# following line.

param(
    [string]$Plugin = "pi_test_plugin_imgui.dll",
    [string]$BinDir = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BinDir) { $BinDir = Join-Path $repoRoot "bin\Debug" }

if (-not (Test-Path (Join-Path $BinDir $Plugin))) {
    Write-Host ("FAIL - plugin not found: {0}" -f (Join-Path $BinDir $Plugin)) -ForegroundColor Red
    Write-Host "  hint: build first (cmake --build --preset conan-debug)"
    exit 1
}

$ran = 0
$skipped = @()
$failed = @()

function Write-Section([string]$text) {
    Write-Host ""
    Write-Host ("== {0} ==" -f $text) -ForegroundColor Cyan
}

# ---------------------------------------------------------------------------
# Python (ctypes) - always present in this project's environment
# ---------------------------------------------------------------------------
Write-Section "Python"
$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    Write-Host "SKIP - python not on PATH" -ForegroundColor Yellow
    $skipped += "python"
} else {
    & python (Join-Path $repoRoot "examples\ffi\python\pi_ffi_demo.py") (Join-Path $BinDir $Plugin)
    if ($LASTEXITCODE -eq 0) { Write-Host "python: PASS" -ForegroundColor Green; $ran++ }
    else { Write-Host "python: FAIL" -ForegroundColor Red; $failed += "python" }
}

# ---------------------------------------------------------------------------
# Rust - no crates needed, so this builds offline
# ---------------------------------------------------------------------------
Write-Section "Rust"
if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) {
    Write-Host "SKIP - cargo not on PATH" -ForegroundColor Yellow
    $skipped += "rust"
} else {
    # keep cargo's build directory inside build/ instead of the source tree
    $env:CARGO_TARGET_DIR = Join-Path $repoRoot "build\cargo-target"
    Push-Location $repoRoot
    & cargo run --quiet --manifest-path "examples\ffi\rust\Cargo.toml" -- (Join-Path $BinDir $Plugin)
    $rustExit = $LASTEXITCODE
    Pop-Location
    if ($rustExit -eq 0) { Write-Host "rust: PASS" -ForegroundColor Green; $ran++ }
    else { Write-Host "rust: FAIL" -ForegroundColor Red; $failed += "rust" }
}

# ---------------------------------------------------------------------------
# C# (.NET) - plain console app, no NuGet packages
# ---------------------------------------------------------------------------
Write-Section "C#"
if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    Write-Host "SKIP - dotnet not on PATH" -ForegroundColor Yellow
    $skipped += "csharp"
} else {
    $env:DOTNET_CLI_TELEMETRY_OPTOUT = "1"
    Push-Location $repoRoot
    & dotnet run --project "examples\ffi\csharp" -- (Join-Path $BinDir $Plugin)
    $csExit = $LASTEXITCODE
    Pop-Location
    if ($csExit -eq 0) { Write-Host "csharp: PASS" -ForegroundColor Green; $ran++ }
    else { Write-Host "csharp: FAIL" -ForegroundColor Red; $failed += "csharp" }
}

# ---------------------------------------------------------------------------
# Verdict
# ---------------------------------------------------------------------------
Write-Section "verdict"
Write-Host ("ran={0} skipped={1} failed={2}" -f $ran, ($skipped -join ','), ($failed -join ','))
if ($failed.Count -gt 0) {
    Write-Host ("RESULT: FAIL - {0}" -f ($failed -join ', ')) -ForegroundColor Red
    exit 1
}
if ($ran -eq 0) {
    Write-Host "RESULT: SKIP - no FFI toolchain available on this machine" -ForegroundColor Yellow
    exit 0
}
Write-Host "RESULT: PASS" -ForegroundColor Green
exit 0
