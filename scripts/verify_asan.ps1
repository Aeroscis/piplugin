# verify_asan.ps1 - the sanitizer track (roadmap W-03).
#
# Rebuilds the existing CMake build tree with AddressSanitizer compiled into this
# project's own targets, runs the non-GUI part of the ctest suite under it, and
# then puts the tree back the way it found it: the values the tree carried before
# it was ever instrumented are recorded on the first run, restored afterwards, and
# the tree is rebuilt, so bin/<Config> ends up holding the normal (uninstrumented)
# binaries again. Until that restore rebuild has run, `verify.ps1` against the same
# tree would legitimately run the instrumented binaries - the tree is never left in
# a state where its own outputs and bin/<Config> disagree.
#
# Why this shape:
#   * The GUI suite is deliberately out. DWM composition plus a swap chain adds
#     noise that is not ours to fix; unit / unit_cpp / headless cover the load,
#     instantiate and unload paths (the unwrap order in the teardown sequence)
#     that this track exists for.
#   * No CMakeLists.txt change. The sanitizer flags are injected as cache
#     variables on the configure command line, so the CI / script line stays
#     inside .github/workflows/ci.yml and scripts/, and the build/packaging line
#     keeps owning the root CMakeLists.txt and cmake/.
#   * The tree is reconfigured in place rather than built twice. Every test
#     binary is deployed into <repo>/bin/<Config> by pi_project.cmake, so a
#     second build tree would overwrite those files with instrumented copies
#     while the first tree still believed they were its own; a plain
#     `verify.ps1` afterwards would then fail to even start them (the ASan
#     runtime DLL would not be on PATH).
#
# Usage:
#   pwsh -NoProfile -File scripts/verify_asan.ps1              # full: ASan run, then restore
#   pwsh -NoProfile -File scripts/verify_asan.ps1 -SkipRestore # CI: leave the tree instrumented
#
# Because the build tree is reconfigured in place and every test binary is
# deployed into <repo>/bin/<Config>, do NOT point this at a tree somebody else is
# building in right now: for the duration of the run that tree's outputs are
# instrumented. CI runs it in its own checkout; on a shared working copy, run it
# in a disposable checkout instead.
#
# Exit code 0 = the instrumented suite passed.
#
# Keep this file ASCII-only: it has no BOM, and Windows PowerShell 5.1 decodes a
# BOM-less script as ANSI, where non-ASCII bytes mis-decode and can eat the
# following line (a mangled byte turning into a backtick/backslash continuation).

param(
    [string]$BuildDir  = "",
    [string]$Config    = "Debug",
    # The non-GUI suite: plain prefix match on the ctest test names. ctest test
    # names are the contract here (unit / unit_cpp / unit_threads / headless_host_*
    # / descriptor_properties_* / capability_gate_* / app_defined_host_service_* /
    # version_gate_*); the GUI cases (multi_plugin_qt_in_one_process,
    # events_two_way_loop) and the two examples are intentionally not matched.
    [string]$TestRegex = '^(unit|headless_host|descriptor_properties|capability_gate|app_defined_host_service|version_gate)',
    [switch]$SkipRestore,
    # Serial MSBuild. The multi-node build needs named pipes, which some sandboxes
    # deny; CI does not need this switch.
    [switch]$NoParallel
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $repoRoot "build" }

$cacheFile = Join-Path $BuildDir "CMakeCache.txt"
if (-not (Test-Path (Join-Path $BuildDir "CTestTestfile.cmake")) -or
    -not (Test-Path $cacheFile)) {
    Write-Host ("FAIL - no configured build in {0}" -f $BuildDir) -ForegroundColor Red
    Write-Host "  hint: conan install . --build=missing -s build_type=Debug"
    Write-Host "        cmake --preset conan-default"
    exit 1
}

function Write-Section([string]$text) {
    Write-Host ""
    Write-Host ("== {0} ==" -f $text) -ForegroundColor Cyan
}

# The cache is the only record of what the tree was configured with, so both the
# compiler identification and the values to restore are read from it.
function Get-CacheValue([string]$name) {
    $hit = Select-String -Path $cacheFile -Pattern ("^" + [regex]::Escape($name) + ":[^=]*=(.*)$") -List
    if ($null -eq $hit) { return $null }
    return $hit.Matches[0].Groups[1].Value
}

function Join-Flags([string[]]$parts) {
    return (($parts | Where-Object { $_ -and $_.Trim() }) -join ' ').Trim()
}

$generator    = Get-CacheValue "CMAKE_GENERATOR"
$cxxCompiler  = Get-CacheValue "CMAKE_CXX_COMPILER"
$linker       = Get-CacheValue "CMAKE_LINKER"
$vsInstance   = Get-CacheValue "CMAKE_GENERATOR_INSTANCE"

# The Visual Studio generator does not put CMAKE_CXX_COMPILER in the cache - it
# only records the generator (and CMAKE_LINKER, which points into the toolset's
# bin directory). cl.exe is what that generator drives, so the generator name is
# the compiler identification in that case.
if ($cxxCompiler) {
    $compilerName = (Split-Path -Leaf $cxxCompiler) -replace '\.exe$', ''
} elseif ($generator -like '*Visual Studio*') {
    $compilerName = 'cl'
} else {
    Write-Host "FAIL - cannot tell which compiler this tree was configured with" -ForegroundColor Red
    Write-Host ("  CMAKE_GENERATOR = '{0}', CMAKE_CXX_COMPILER is not cached" -f $generator)
    exit 1
}

$isMsvc = $compilerName -eq 'cl'
$isGnuLike = $compilerName -like 'g++*' -or $compilerName -like 'gcc*' -or
             $compilerName -like 'clang++*' -or $compilerName -like 'clang-*' -or
             $compilerName -eq 'clang++' -or $compilerName -eq 'clang'

if (-not $isMsvc -and -not $isGnuLike) {
    Write-Host ("FAIL - compiler '{0}' is not one this track knows how to instrument" -f $compilerName) -ForegroundColor Red
    Write-Host "  supported: MSVC (/fsanitize=address), gcc and clang (-fsanitize=address)"
    exit 1
}

# MSVC links AddressSanitizer dynamically: clang_rt.asan*_dynamic-<arch>.dll has
# to be found at run time, and it ships in the toolset's bin directory next to
# cl.exe/link.exe. That directory is only on PATH inside a developer prompt, so
# this script puts it there for the ctest run. Looked up up front so a missing
# runtime is a configuration failure with a reason, not a pile of test failures
# where every instrumented binary refuses to start (0xC0000135).
function Find-AsanRuntimeDir {
    if (-not $isMsvc) { return $null }
    $patterns = @()
    foreach ($toolPath in @($linker, $cxxCompiler)) {
        if ($toolPath) { $patterns += (Join-Path (Split-Path -Parent $toolPath) 'clang_rt.asan*_dynamic*.dll') }
    }
    if ($vsInstance) {
        $patterns += (Join-Path $vsInstance 'VC\Tools\MSVC\*\bin\Host*\*\clang_rt.asan*_dynamic*.dll')
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        foreach ($path in @(& $vswhere -products * -property installationPath)) {
            if ($path) { $patterns += (Join-Path $path 'VC\Tools\MSVC\*\bin\Host*\*\clang_rt.asan*_dynamic*.dll') }
        }
    }
    foreach ($pattern in $patterns) {
        $hit = Get-ChildItem $pattern -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { return $hit.DirectoryName }
    }
    return $null
}

$asanRuntimeDir = Find-AsanRuntimeDir
if ($isMsvc -and -not $asanRuntimeDir) {
    Write-Host "FAIL - no AddressSanitizer runtime (clang_rt.asan*_dynamic*.dll) in any MSVC toolset" -ForegroundColor Red
    Write-Host "  the instrumented binaries cannot start without it; install the Visual Studio"
    Write-Host "  component 'C++ AddressSanitizer' (Microsoft.VisualStudio.Component.VC.ASAN)"
    exit 1
}

# ---------------------------------------------------------------------------
# 0/3  recover the values the tree carried before it was instrumented
#
# Restoring means putting back the pre-ASan values, and the cache only still holds
# those on the first run: a run with -SkipRestore leaves the flags in place, and
# from then on the cache would hand the instrumentation back as if it were the
# original. So the first run records them next to the build tree, later runs
# restore from that record, and the record is dropped once the restore has
# succeeded. A record is ignored when the tree no longer carries the flag (it was
# restored, or reconfigured by other means), which is what keeps a stale file from
# resurrecting old flags.
# ---------------------------------------------------------------------------
$overrideNames = @('CMAKE_C_FLAGS', 'CMAKE_CXX_FLAGS',
                   'CMAKE_C_FLAGS_DEBUG', 'CMAKE_CXX_FLAGS_DEBUG',
                   'CMAKE_EXE_LINKER_FLAGS', 'CMAKE_SHARED_LINKER_FLAGS')

$compileFlag  = if ($isMsvc) { '/fsanitize=address' } else { '-fsanitize=address' }
$pristineFile = Join-Path $BuildDir ".verify-asan-original.json"

$cacheNow = @{}
foreach ($name in $overrideNames) { $cacheNow[$name] = Get-CacheValue $name }

$looksInstrumented = $false
foreach ($name in $overrideNames) {
    if ($cacheNow[$name] -and $cacheNow[$name].Contains($compileFlag)) { $looksInstrumented = $true; break }
}

$pristine = @{}
if ($looksInstrumented -and (Test-Path $pristineFile)) {
    $recorded = Get-Content -Path $pristineFile -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($name in $overrideNames) {
        $value = $recorded.$name
        $pristine[$name] = if ($null -eq $value) { "" } else { [string]$value }
    }
    Write-Host ("pre-ASan values: recorded earlier in {0}" -f (Split-Path -Leaf $pristineFile))
} else {
    foreach ($name in $overrideNames) { $pristine[$name] = $cacheNow[$name] }
    ($pristine | ConvertTo-Json) | Out-File -FilePath $pristineFile -Encoding utf8
    Write-Host ("pre-ASan values: recorded now in {0}" -f (Split-Path -Leaf $pristineFile))
}

# ---------------------------------------------------------------------------
# 1/3  configure with ASan in place
# ---------------------------------------------------------------------------
Write-Section ("1/3  configure with AddressSanitizer ({0}, {1})" -f $compilerName, $Config)

# Only the variables this compiler family actually needs are overridden, so the
# restore step puts back exactly what it changed.
$plan = [ordered]@{}

if ($isMsvc) {
    # /fsanitize=address is a compiler option only: cl records the ASan runtime in
    # the object files' .drectve sections and the linker picks it up from there.
    # Passing it to link.exe is LNK4044 "unrecognized option ... ignored", so the
    # linker flags are deliberately left alone.
    $plan['CMAKE_C_FLAGS']   = Join-Flags @($pristine['CMAKE_C_FLAGS'], '/fsanitize=address')
    $plan['CMAKE_CXX_FLAGS'] = Join-Flags @($pristine['CMAKE_CXX_FLAGS'], '/fsanitize=address')
    # /RTC1, CMake's Debug default on MSVC, is rejected together with
    # /fsanitize=address (D8016: incompatible command-line options).
    foreach ($name in @('CMAKE_C_FLAGS_DEBUG', 'CMAKE_CXX_FLAGS_DEBUG')) {
        $plan[$name] = (($pristine[$name]) -replace '/RTC1', '').Trim()
    }
} else {
    $compileFlags = '-fsanitize=address -fno-omit-frame-pointer'
    $plan['CMAKE_C_FLAGS']             = Join-Flags @($pristine['CMAKE_C_FLAGS'], $compileFlags)
    $plan['CMAKE_CXX_FLAGS']           = Join-Flags @($pristine['CMAKE_CXX_FLAGS'], $compileFlags)
    # gcc/clang need the runtime at link time as well.
    $plan['CMAKE_EXE_LINKER_FLAGS']    = Join-Flags @($pristine['CMAKE_EXE_LINKER_FLAGS'], '-fsanitize=address')
    $plan['CMAKE_SHARED_LINKER_FLAGS'] = Join-Flags @($pristine['CMAKE_SHARED_LINKER_FLAGS'], '-fsanitize=address')
}

$configureArgs = @('-S', $repoRoot, '-B', $BuildDir)
foreach ($name in $plan.Keys) { $configureArgs += ("-D{0}={1}" -f $name, $plan[$name]) }

& cmake @configureArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host "configure with ASan: FAIL" -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------------------
# 2/3  build and run the non-GUI suite under it
# ---------------------------------------------------------------------------
Write-Section ("2/3  build + ctest -R '{0}'" -f $TestRegex)

$buildArgs = @('--build', $BuildDir, '--config', $Config)
if (-not $NoParallel) { $buildArgs += '--parallel' }

& cmake @buildArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host "ASan build: FAIL" -ForegroundColor Red
    exit 1
}

if ($asanRuntimeDir) {
    $env:PATH = "{0};{1}" -f $asanRuntimeDir, $env:PATH
    Write-Host ("asan runtime dir: {0}" -f $asanRuntimeDir)
}

# The gcc/clang runtime is asked for leak detection explicitly so the verdict
# does not depend on the host's ASAN_OPTIONS. It must NOT be asked of the MSVC
# runtime: on Windows AddressSanitizer implements no leak detection at all, and
# `detect_leaks=1` makes it die on startup with "detect_leaks is not supported on
# this platform" - every test then fails before main() without a single line of
# output. The leak half of this track therefore stays where it already was: the
# `_CrtDumpMemoryLeaks()` assertion inside the unit_cpp case.
#
# Two things in the log are expected and do not mean failure: LNK4044
# ("unrecognized option /fsanitize=address" - a compile-time switch that reaches
# the linker too) and LNK4300 (/INCREMENTAL ignored because the inputs carry ASan
# metadata). And a startup failure is easy to misread: with detect_leaks=1 the run
# reports "Required regular expression not found" for every case and "0% tests
# passed", which looks like broken assertions rather than a runtime that refused
# to start before main().
if (-not $isMsvc) {
    $env:ASAN_OPTIONS = Join-Flags @('detect_leaks=1', 'halt_on_error=1', $env:ASAN_OPTIONS)
}

$log = Join-Path $BuildDir "verify-asan.log"
"" | Out-File -FilePath $log -Encoding utf8
& ctest --test-dir $BuildDir -C $Config -R $TestRegex --output-on-failure 2>&1 |
    ForEach-Object {
        $_ | Out-File -FilePath $log -Append -Encoding utf8
        Write-Host $_
    }
$asanPassed = ($LASTEXITCODE -eq 0)

if ($asanPassed) {
    Write-Host "sanitizer suite: PASS" -ForegroundColor Green
} else {
    Write-Host "sanitizer suite: FAIL" -ForegroundColor Red
}
Write-Host ("log: {0}" -f $log)

# ---------------------------------------------------------------------------
# 3/3  put the tree back
# ---------------------------------------------------------------------------
if ($SkipRestore) {
    Write-Section "3/3  restore (skipped)"
    Write-Host "-SkipRestore was given: the tree stays configured with ASan."
    Write-Host ("  bin\{0} now holds instrumented binaries." -f $Config)
    Write-Host ("  {0} keeps the pre-ASan values for a later restore." -f (Split-Path -Leaf $pristineFile))
} else {
    Write-Section "3/3  restore the tree and rebuild uninstrumented"
    $restoreArgs = @('-S', $repoRoot, '-B', $BuildDir)
    foreach ($name in $plan.Keys) {
        $value = $pristine[$name]
        if ($null -eq $value) { $value = "" }
        $restoreArgs += ("-D{0}={1}" -f $name, $value)
    }
    & cmake @restoreArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "restore configure: FAIL - the tree is left configured with ASan" -ForegroundColor Red
        exit 1
    }
    # Changing the flags back invalidates every object, so this rebuild also
    # re-runs the POST_BUILD deploy step and bin\<Config> gets the normal
    # binaries back.
    & cmake @buildArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "restore build: FAIL - the tree is left configured without ASan, but bin may hold instrumented binaries" -ForegroundColor Red
        exit 1
    }
    Remove-Item -Force $pristineFile -ErrorAction SilentlyContinue
    Write-Host "tree restored: run scripts/verify.ps1 for the normal verdict" -ForegroundColor Green
}

if ($asanPassed) { exit 0 }
exit 1
