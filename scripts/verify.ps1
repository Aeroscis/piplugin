# verify.ps1 - one command that runs every automated check in this repository.
#
# CI calls this (.github/workflows/ci.yml), so the checks live here rather than
# in workflow YAML: they can be run on a laptop before pushing, and changing them
# is reviewed like code. The workflow only installs dependencies and builds.
#
# Checks:
#   1. ctest                -- core unit suite, headless smoke, negative cases
#   2. conformance harness  -- real plugin lifecycle + resize round trip (needs a GUI session)
#   3. FFI examples         -- the C ABI consumed from Python / Rust / C# (skips missing toolchains)
#   4. clang-format drift   -- reported, NOT enforced (see the note at check 4)
#   5. documentation drift  -- the repository must not contradict itself (see check 5)
#
# The sanitizer track is not a check here: it needs its own instrumented build of
# the same tree, so it is a separate entry point, scripts/verify_asan.ps1.
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
    [switch]$SkipFfi,
    [switch]$SkipFormat,
    [switch]$SkipDocDrift
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
Write-Section "1/5  ctest (-C $Config)"
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
Write-Section "2/5  conformance harness"
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
# 3. FFI examples: the C ABI consumed from other languages (roadmap ECO-06).
#    "Pure C ABI" is a claim about Python / Rust / C#; verify_ffi.ps1 executes it
#    and skips a language whose toolchain is missing, so a machine without cargo
#    still checks the other two. A toolchain that IS present must pass.
# ---------------------------------------------------------------------------
Write-Section "3/5  FFI examples (python / rust / c#)"
if ($SkipFfi) {
    Write-Host "SKIP - -SkipFfi was given" -ForegroundColor Yellow
} else {
    & (Join-Path $PSScriptRoot "verify_ffi.ps1") -BinDir $BinDir
    if ($LASTEXITCODE -ne 0) {
        $failures += "FFI examples"
        Write-Host "FFI examples: FAIL" -ForegroundColor Red
    } else {
        Write-Host "FFI examples: PASS" -ForegroundColor Green
    }
}

# ---------------------------------------------------------------------------
# 4. clang-format drift -- REPORTED, NOT ENFORCED.
#    Every C/C++ file currently differs from the committed .clang-format
#    (include ordering plus indentation and wrapping drift). Reformatting them in
#    one go would produce a diff that buries every other change, so this check
#    only reports and never fails the run. To enforce it: add the drifted files
#    to $failures, reformat first.
# ---------------------------------------------------------------------------
Write-Section "4/5  clang-format drift (informational)"
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
        # The decision material for "report or enforce" (W-13) is the list itself,
        # so print it rather than only a count: enforcing this check starts with
        # reformatting exactly these files, and the size of that diff is what the
        # maintainer has to weigh. Kept as a report until that call is made.
        $shown = 0
        foreach ($f in $drifted) {
            if ($shown -ge 40) {
                Write-Host ("  ... and {0} more" -f ($drifted.Count - $shown))
                break
            }
            Write-Host ("  would reformat: {0}" -f $f)
            $shown++
        }
        Write-Host "::warning::clang-format drift in $($drifted.Count)/$($files.Count) file(s); run 'clang-format -i' on them and then enforce this check"
    } else {
        Write-Host "clang-format: clean" -ForegroundColor Green
    }
}

# ---------------------------------------------------------------------------
# 5. documentation drift -- the repository must not contradict itself.
#    The rules are data, not code: scripts/doc_drift_rules.json pairs a feature
#    of the checkout with statements that stop being true once that feature
#    exists ("tests/unit/pi_unit_tests.c exists" -> nothing may still say "there
#    are no unit tests"). Enforced: a wrong sentence in a document is a defect
#    like any other, and because a rule only fires while its feature really is in
#    the tree, the table cannot rot into a list of assertions about a repository
#    that moved on.
#
#    Documents and rules are read as UTF-8 explicitly - both are Chinese, and
#    Get-Content's default encoding would mangle the patterns on Windows
#    PowerShell 5.1.
# ---------------------------------------------------------------------------
Write-Section "5/5  documentation drift (feature -> forbidden claim)"
if ($SkipDocDrift) {
    Write-Host "SKIP - -SkipDocDrift was given" -ForegroundColor Yellow
} else {
    $rulesFile = Join-Path $PSScriptRoot "doc_drift_rules.json"
    if (-not (Test-Path $rulesFile)) {
        $failures += "documentation drift"
        Write-Host ("FAIL - {0} is missing" -f $rulesFile) -ForegroundColor Red
    } else {
        $rules       = Get-Content -Path $rulesFile -Raw -Encoding UTF8 | ConvertFrom-Json
        $excluded    = @($rules.exclude)
        $ignoreMark  = [string]$rules.ignore_marker
        $documents   = @(git -C $repoRoot ls-files '*.md')
        $findings    = @()
        $scanned     = 0

        foreach ($file in $documents) {
            if ($excluded -contains $file) { continue }
            $lines = @(Get-Content -Path (Join-Path $repoRoot $file) -Encoding UTF8)
            $scanned++
            # No section is exempt. A finished todo item states a conclusion about
            # today - what exists and where it is verified - so it is checked like
            # any other sentence; the wording of the plan it replaced belongs to
            # git and to CHANGELOG.md (excluded), not to a live document.
            for ($i = 0; $i -lt $lines.Count; $i++) {
                $line     = $lines[$i]
                $previous = if ($i -gt 0) { $lines[$i - 1] } else { "" }

                if ($ignoreMark -and ($line.Contains($ignoreMark) -or $previous.Contains($ignoreMark))) { continue }

                foreach ($rule in @($rules.rules)) {
                    if (-not (Test-Path (Join-Path $repoRoot $rule.feature))) { continue }
                    $context = @($rule.context | Where-Object { $_ })
                    if ($context.Count -gt 0) {
                        $inContext = $false
                        foreach ($word in $context) {
                            if ($line.Contains($word)) { $inContext = $true; break }
                        }
                        if (-not $inContext) { continue }
                    }
                    foreach ($claim in @($rule.claims)) {
                        if ($line.Contains($claim)) {
                            $findings += [pscustomobject]@{
                                File    = $file
                                Line    = $i + 1
                                Claim   = $claim
                                Feature = $rule.feature
                                Note    = $rule.note
                            }
                        }
                    }
                }
            }
        }

        if ($scanned -eq 0) {
            $failures += "documentation drift"
            Write-Host "FAIL - no markdown document found to check (is this a git checkout?)" -ForegroundColor Red
        } elseif ($findings.Count -gt 0) {
            $failures += "documentation drift"
            Write-Host ("{0} claim(s) the repository contradicts:" -f $findings.Count) -ForegroundColor Red
            foreach ($finding in $findings) {
                Write-Host ("  {0}:{1}: says '{2}', but {3} exists" -f `
                    $finding.File, $finding.Line, $finding.Claim, $finding.Feature) -ForegroundColor Red
                Write-Host ("      {0}" -f $finding.Note)
            }
            Write-Host "documentation drift: FAIL" -ForegroundColor Red
        } else {
            Write-Host ("{0} document(s) checked; no claim contradicted by the repository" -f $scanned)
            Write-Host "documentation drift: clean" -ForegroundColor Green
        }
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
