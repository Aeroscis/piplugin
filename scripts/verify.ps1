# verify.ps1 - one command that runs every automated check in this repository.
#
# CI calls this (.github/workflows/ci.yml), so the checks live here rather than
# in workflow YAML: they can be run on a laptop before pushing, and changing them
# is reviewed like code. The workflow only installs dependencies and builds.
#
# Checks:
#   1. ctest                -- core unit suite, headless smoke, negative version case
#   2. conformance harness  -- real plugin lifecycle + resize round trip (needs a GUI session)
#   3. clang-format drift   -- reported, NOT enforced (see the note at check 3)
#
# Exit code 0 = every enforced check passed.
#
# Keep this file ASCII-only: it has no BOM, and Windows PowerShell 5.1 decodes a
# BOM-less script as ANSI, where non-ASCII bytes mis-decode and can eat the
# following line (a mangled byte turning into a backtick/backslash continuation).

param(
    [string]$BuildDir = "",
    [string]$Config   = "Debug",
    [string]$BinDir   = "",
    [int]$Cycles      = 2,
    [switch]$SkipGui,
    [switch]$SkipFormat
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $repoRoot "build" }
if (-not $BinDir)   { $BinDir   = Join-Path $repoRoot "bin\$Config" }

if (-not (Test-Path (Join-Path $BuildDir "CTestTestfile.cmake"))) {
    Write-Host ("FAIL - no configured build in {0}" -f $BuildDir) -ForegroundColor Red
    Write-Host "  hint: configure and build first (see README 'Getting started')"
    exit 1
}

$failures = @()

function Write-Section([string]$text) {
    Write-Host ""
    Write-Host ("== {0} ==" -f $text) -ForegroundColor Cyan
}

# ---------------------------------------------------------------------------
# 1. ctest: the non-GUI regression (unit suite + headless smoke + version gate)
# ---------------------------------------------------------------------------
Write-Section "1/3  ctest (-C $Config)"
& ctest --test-dir $BuildDir -C $Config --output-on-failure
if ($LASTEXITCODE -ne 0) {
    $failures += "ctest"
    Write-Host "ctest: FAIL" -ForegroundColor Red
} else {
    Write-Host "ctest: PASS" -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# 2. conformance harness: load -> attach -> idle -> resize -> unload, per plugin
#    Which plugins are tested is discovered from the bin directory, so this works
#    both with the full build (Qt + imgui plugins) and with CI, where the Qt
#    targets are off because Qt5 is a local install there.
#
#    The examples/ plugins are in the list on purpose: the harness is the
#    documented acceptance for "an adapter kit written from
#    docs/design/adapter-spec.md works with an official host" (ECO-01), and it
#    costs one more cycle per plugin.
# ---------------------------------------------------------------------------
Write-Section "2/3  conformance harness"
$available = @()
foreach ($name in 'pi_test_plugin_qt.dll', 'pi_test_plugin_imgui.dll',
                  'pi_example_plugin_imgui.dll', 'pi_example_plugin_qt.dll',
                  'pi_example_plugin_win32.dll') {
    if (Test-Path (Join-Path $BinDir $name)) { $available += $name }
}

if ($SkipGui) {
    Write-Host "SKIP - -SkipGui was given (the harness needs a GUI session)" -ForegroundColor Yellow
} elseif ($available.Count -eq 0) {
    Write-Host ("SKIP - no test plugin found in {0}" -f $BinDir) -ForegroundColor Yellow
} elseif (-not (Test-Path (Join-Path $BinDir "pi_test_host_imgui.exe"))) {
    Write-Host ("SKIP - no imgui test host in {0}" -f $BinDir) -ForegroundColor Yellow
} else {
    & (Join-Path $PSScriptRoot "run_selftest.ps1") -Plugin ($available -join ',') -Cycles $Cycles -BinDir $BinDir
    if ($LASTEXITCODE -ne 0) {
        $failures += "conformance harness"
        Write-Host "conformance harness: FAIL" -ForegroundColor Red
    } else {
        Write-Host "conformance harness: PASS" -ForegroundColor Green
    }
}

# ---------------------------------------------------------------------------
# 3. clang-format drift -- REPORTED, NOT ENFORCED.
#    Every C/C++ file currently differs from the committed .clang-format
#    (include ordering plus indentation and wrapping drift). Reformatting them in
#    one go would produce a diff that buries every other change, so this check
#    only reports and never fails the run. To enforce it: add the drifted files
#    to $failures, reformat first.
# ---------------------------------------------------------------------------
Write-Section "3/3  clang-format drift (informational)"
if ($SkipFormat) {
    Write-Host "SKIP - -SkipFormat was given" -ForegroundColor Yellow
} elseif (-not (Get-Command clang-format -ErrorAction SilentlyContinue)) {
    Write-Host "SKIP - clang-format not on PATH" -ForegroundColor Yellow
} else {
    $files = git -C $repoRoot ls-files '*.c' '*.cpp' '*.h' '*.hpp' |
             Where-Object { $_ -notmatch '^adapters/imgui/backends/' }
    $drifted = @()
    foreach ($f in $files) {
        & clang-format --dry-run --Werror (Join-Path $repoRoot $f) *> $null
        if ($LASTEXITCODE -ne 0) { $drifted += $f }
    }
    Write-Host ("checked {0} file(s); {1} would be reformatted" -f $files.Count, $drifted.Count)
    if ($drifted.Count -gt 0) {
        Write-Host "::warning::clang-format drift in $($drifted.Count)/$($files.Count) file(s); run 'clang-format -i' on them and then enforce this check"
    } else {
        Write-Host "clang-format: clean" -ForegroundColor Green
    }
}

# ---------------------------------------------------------------------------
# Verdict
# ---------------------------------------------------------------------------
Write-Section "verdict"
if ($failures.Count -gt 0) {
    Write-Host ("RESULT: FAIL - {0}" -f ($failures -join ', ')) -ForegroundColor Red
    exit 1
}
Write-Host "RESULT: PASS" -ForegroundColor Green
exit 0
