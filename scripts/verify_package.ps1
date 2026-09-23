# verify_package.ps1 - prove the PACKAGE works for an outside consumer (roadmap ECO-04).
#
# Unit tests and the conformance harness say the framework works; neither says
# anything about what somebody else receives. This script builds the consumer in
# examples/conan_consumer/ against two shapes of the distribution, and runs it:
#
#   A. install tree   -- cmake --install into a temporary prefix, then
#                        find_package(piplugin) + link pi::piplugin / _host /
#                        _events / _imgui and run the result.
#   B. Conan package  -- conan create (builds the recipe into the local cache),
#                        then a consumer that installs piplugin/<version> as a
#                        Conan requirement, configures with the generated
#                        toolchain and runs.
#
# Phase B is the slow one (it rebuilds the whole project inside Conan's cache).
# Use -SkipConan to run only phase A.
#
# Requirements for the consumer: imgui (a real Conan dependency) and, for the Qt
# components, a local Qt5 - the package deliberately does NOT require Qt5 (see
# src/cmake/piConfig.cmake.in), so the consumer here links the core, the two host
# kits and the imgui adapter.
#
# Usage: pwsh -NoProfile -File scripts\verify_package.ps1 [-SkipConan] [-KeepTemp]
# Exit code 0 = every phase that ran passed.
#
# Keep this file ASCII-only: it has no BOM, and Windows PowerShell 5.1 decodes a
# BOM-less script as ANSI, where non-ASCII bytes mis-decode and can eat the
# following line.

param(
    [string]$BuildDir = "",
    [string]$Config   = "Debug",
    [switch]$SkipConan,
    [switch]$KeepTemp
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $repoRoot "build" }

$consumerSrc = Join-Path $repoRoot "examples\conan_consumer"
if (-not (Test-Path (Join-Path $consumerSrc "CMakeLists.txt"))) {
    Write-Host "FAIL - consumer fixture not found in $consumerSrc" -ForegroundColor Red
    exit 1
}

$work = Join-Path $repoRoot ("build\package-verify-" + $Config)
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Force -Path $work | Out-Null

$failures = @()

function Write-Section([string]$text) {
    Write-Host ""
    Write-Host ("== {0} ==" -f $text) -ForegroundColor Cyan
}

# ---------------------------------------------------------------------------
# A. install tree
# ---------------------------------------------------------------------------
Write-Section "A. install tree"

$prefix = Join-Path $work "install"
& cmake --install $BuildDir --config $Config --prefix $prefix | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL - cmake --install" -ForegroundColor Red; exit 1 }
Write-Host ("installed into {0}" -f $prefix)

$hasQtKit = Test-Path (Join-Path $prefix "lib\$Config\piplugin_qtd.lib")
Write-Host ("Qt adapter kit installed: {0} (the consumer does not need it)" -f $hasQtKit)

$conanToolchain = Join-Path $BuildDir "generators\conan_toolchain.cmake"
$installConsumer = Join-Path $work "consumer-install"
& cmake -S $consumerSrc -B $installConsumer -G "Visual Studio 17 2022" -A x64 `
    "-DCMAKE_TOOLCHAIN_FILE=$conanToolchain" `
    "-DCMAKE_PREFIX_PATH=$prefix" -DPI_CONSUMER_LINK_IMGUI=ON 2>&1 | ForEach-Object { Write-Host "    $_" }
if ($LASTEXITCODE -ne 0) {
    $failures += "install-tree configure"
    Write-Host "FAIL - configure against the install tree" -ForegroundColor Red
} else {
    & cmake --build $installConsumer --config $Config --parallel 2>&1 | ForEach-Object { Write-Host "    $_" }
    if ($LASTEXITCODE -ne 0) {
        $failures += "install-tree build"
        Write-Host "FAIL - build against the install tree" -ForegroundColor Red
    } else {
        $exe = Join-Path $installConsumer "$Config\pi_consumer.exe"
        # the core is SHARED: put the installed runtime next to the exe
        Copy-Item (Join-Path $prefix "bin\$Config\*.dll") (Split-Path $exe) -Force
        & $exe
        if ($LASTEXITCODE -ne 0) {
            $failures += "install-tree run"
            Write-Host "FAIL - running against the install tree" -ForegroundColor Red
        } else {
            Write-Host "install tree: PASS" -ForegroundColor Green
        }
    }
}

# ---------------------------------------------------------------------------
# B. Conan package
# ---------------------------------------------------------------------------
if ($SkipConan) {
    Write-Section "B. Conan package (SKIPPED)"
    Write-Host "SKIP - -SkipConan was given" -ForegroundColor Yellow
} else {
    Write-Section "B. Conan package"

    Push-Location $repoRoot
    & conan create . --build=missing -s build_type=$Config -o PI_BUILD_TESTS=False 2>&1 |
        ForEach-Object { Write-Host "    $_" }
    $createExit = $LASTEXITCODE
    Pop-Location

    if ($createExit -ne 0) {
        $failures += "conan create"
        Write-Host "FAIL - conan create" -ForegroundColor Red
    } else {
        $version = (Select-String -Path (Join-Path $repoRoot "conanfile.py") -Pattern '^\s+version = "(.+)"' |
                    Select-Object -First 1).Matches[0].Groups[1].Value
        $conanWork = Join-Path $work "conan-consumer"
        New-Item -ItemType Directory -Force -Path $conanWork | Out-Null

        # A consumer declares the requirement the normal way: a conanfile.txt with
        # [requires]. (Conan 2 rejects --requires together with a path argument.)
        Set-Content -Path (Join-Path $conanWork "conanfile.txt") -Encoding Ascii -Value @"
[requires]
piplugin/$version

[generators]
CMakeDeps
CMakeToolchain
"@

        # The deployer copies the package's runtime DLLs out of the Conan cache.
        # It cannot relocate between drives, and the cache lives on the system
        # drive, so the deploy folder goes there too (not under the repo).
        $deployRoot = Join-Path $env:TEMP "piplugin-pkg-deps-$Config"
        if (Test-Path $deployRoot) { Remove-Item $deployRoot -Recurse -Force }

        Push-Location $conanWork
        & conan install . -s build_type=$Config --build=missing `
            --deployer=full_deploy "--deployer-folder=$deployRoot" 2>&1 |
            ForEach-Object { Write-Host "    $_" }
        $genExit = $LASTEXITCODE
        Pop-Location

        if ($genExit -ne 0) {
            $failures += "conan install (consumer)"
            Write-Host "FAIL - conan install for the consumer" -ForegroundColor Red
        } else {
            $conanConsumer = Join-Path $work "consumer-conan"
            & cmake -S $consumerSrc -B $conanConsumer -G "Visual Studio 17 2022" -A x64 `
                "-DCMAKE_TOOLCHAIN_FILE=$conanWork\conan_toolchain.cmake" -DCMAKE_BUILD_TYPE=$Config `
                -DPI_CONSUMER_LINK_IMGUI=OFF 2>&1 |
                ForEach-Object { Write-Host "    $_" }
            if ($LASTEXITCODE -ne 0) {
                $failures += "conan consumer configure"
                Write-Host "FAIL - configure against the Conan package" -ForegroundColor Red
            } else {
                & cmake --build $conanConsumer --config $Config --parallel 2>&1 |
                    ForEach-Object { Write-Host "    $_" }
                if ($LASTEXITCODE -ne 0) {
                    $failures += "conan consumer build"
                    Write-Host "FAIL - build against the Conan package" -ForegroundColor Red
                } else {
                    $exe = Join-Path $conanConsumer "$Config\pi_consumer.exe"
                    # the deployed dependency tree carries the runtime DLL; take the
                    # CORE one (piplugind.dll), not an adapter kit's
                    $coreNames = @("piplugind.dll", "piplugin.dll")
                    $dll = Get-ChildItem $deployRoot -Recurse -File -ErrorAction SilentlyContinue |
                           Where-Object { $coreNames -contains $_.Name } |
                           Select-Object -First 1
                    if ($dll) {
                        Write-Host ("    runtime: {0}" -f $dll.FullName)
                        Copy-Item $dll.FullName (Split-Path $exe) -Force
                    } else {
                        Write-Host "    WARNING: no runtime DLL found in the deployed tree" -ForegroundColor Yellow
                    }
                    # ... and put every deployed bin dir on PATH too: the core is SHARED
                    # and may need further DLLs that live next to it.
                    $binDirs = Get-ChildItem $deployRoot -Recurse -Directory -Filter "bin" -ErrorAction SilentlyContinue |
                               ForEach-Object { $_.FullName }
                    $env:PATH = (($binDirs + (Split-Path $exe)) -join ";") + ";" + $env:PATH
                    & $exe
                    if ($LASTEXITCODE -ne 0) {
                        $failures += "conan consumer run"
                        Write-Host "FAIL - running against the Conan package" -ForegroundColor Red
                    } else {
                        Write-Host "conan package: PASS" -ForegroundColor Green
                    }
                }
            }
        }
    }
}

# ---------------------------------------------------------------------------
# Verdict
# ---------------------------------------------------------------------------
Write-Section "verdict"
if (-not $KeepTemp) { Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue; if ($deployRoot) { Remove-Item $deployRoot -Recurse -Force -ErrorAction SilentlyContinue } }
else { Write-Host ("temp kept: {0}" -f $work) }
if ($failures.Count -gt 0) {
    Write-Host ("RESULT: FAIL - {0}" -f ($failures -join ', ')) -ForegroundColor Red
    exit 1
}
Write-Host "RESULT: PASS" -ForegroundColor Green
exit 0
