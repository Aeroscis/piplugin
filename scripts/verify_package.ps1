# verify_package.ps1 - prove the PACKAGE works for an outside consumer (roadmap ECO-04).
#
# Unit tests and the conformance harness say the framework works; neither says
# anything about what somebody else receives. This script builds the consumer in
# examples/conan_consumer/ against three shapes of the distribution, and runs it:
#
#   A. install tree   -- cmake --install into a temporary prefix, then
#                        find_package(piplugin) + link pi::plugin / _host /
#                        _events / _imgui and run the result.
#   B. Conan package  -- conan create (builds the recipe into the local cache),
#                        then a consumer that installs piplugin/<version> as a
#                        Conan requirement, configures with the generated
#                        toolchain and runs.
#   C. cpack archive  -- cpack builds the ZIP somebody actually downloads (W-08).
#                        It is unzipped into a clean directory and has to stand on
#                        its own: the consumer is configured there WITHOUT the
#                        Conan toolchain (no build tree, no Qt environment on
#                        PATH) and the resulting host program must run.
#
# Phase B is the slow one (it rebuilds the whole project inside Conan's cache).
# Use -SkipConan to run A + C only.
#
# Requirements for the consumer: imgui (a real Conan dependency) and, for the Qt
# components, a local Qt5 - the package deliberately does NOT require Qt5 (see
# src/cmake/piConfig.cmake.in), so the consumer here links the core, the two host
# kits and the imgui adapter.
#
# Usage: pwsh -NoProfile -File scripts\verify_package.ps1 [-SkipConan] [-SkipCpack] [-KeepTemp]
# Exit code 0 = every phase that ran passed.
#
# Keep this file ASCII-only: it has no BOM, and Windows PowerShell 5.1 decodes a
# BOM-less script as ANSI, where non-ASCII bytes mis-decode and can eat the
# following line.

param(
    [string]$BuildDir = "",
    [string]$Config   = "Debug",
    [switch]$SkipConan,
    [switch]$SkipCpack,
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

# Debug artifacts carry the global "d" suffix (GLOBAL_PROJECT_BUILD_TYPE_SUFFIX)
$libSuffix = if ($Config -eq "Debug") { "d" } else { "" }

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

$hasQtKit = Test-Path (Join-Path $prefix "lib\$Config\piplugin_qt$libSuffix.lib")
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
        $exe = Join-Path $installConsumer "$Config\pi_plugin_consumer.exe"
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
    & conan create . --build=missing -s build_type=$Config -o PI_PLUGIN_BUILD_TESTS=False 2>&1 |
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
        #
        # imgui is declared HERE, and that is not a workaround for laziness: under
        # Conan 2.10 + CMakeDeps a component-level EXTERNAL require is only
        # propagated when the consumer itself also requires that package. Without
        # it CMakeDeps silently drops it (conan/tools/cmake/cmakedeps/templates/
        # target_configuration.py::get_deps_targets_names resolves component
        # requires against the consumer's requirements and just `pass`es on
        # KeyError): no imgui-config.cmake is generated at all, the component's
        # DEPENDENCIES list keeps only pi::plugin, and the consumer fails to link
        # with 29 unresolved imgui symbols. Declaring it makes CMakeDeps emit
        # "piplugin_FIND_DEPENDENCY_NAMES imgui" and
        # "piplugin_pi_plugin_imgui_DEPENDENCIES_DEBUG pi::plugin imgui::imgui".
        # Version is taken from the recipe so the two can never drift.
        $imguiVersion = (Select-String -Path (Join-Path $repoRoot "conanfile.py") `
                            -Pattern 'self\.requires\("imgui/([^"]+)"\)' |
                         Select-Object -First 1).Matches[0].Groups[1].Value
        Set-Content -Path (Join-Path $conanWork "conanfile.txt") -Encoding Ascii -Value @"
[requires]
piplugin/$version
imgui/$imguiVersion

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
                -DPI_CONSUMER_LINK_IMGUI=ON 2>&1 |
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
                    $exe = Join-Path $conanConsumer "$Config\pi_plugin_consumer.exe"
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
# C. cpack archive (W-08)
#
# The third shape of the distribution, and the one somebody actually downloads.
# It has to stand on its own OUTSIDE this repository: unzipped into a clean
# directory, with no build tree, no Conan toolchain and no Qt runtime on PATH, the
# archive must still be enough to build a host against it and RUN that host.
#
# The consumer is configured WITHOUT -DCMAKE_TOOLCHAIN_FILE on purpose: phases A
# and B hand it Conan's dependency paths, this one may not. Everything the build
# needs has to come from the unpacked archive (imgui, the one external
# dependency, is therefore absent here - the umbrella config skips a component
# whose dependency is missing, so the consumer links core + host kits only).
# ---------------------------------------------------------------------------
Write-Section "C. cpack archive"

if ($SkipCpack) {
    Write-Host "SKIP - -SkipCpack was given" -ForegroundColor Yellow
} else {
    $cpackOut = Join-Path $work "cpack-out"
    New-Item -ItemType Directory -Force -Path $cpackOut | Out-Null

    & cpack --config (Join-Path $BuildDir "CPackConfig.cmake") -C $Config -B $cpackOut 2>&1 |
        ForEach-Object { Write-Host "    $_" }
    $cpackExit = $LASTEXITCODE

    $archive = $null
    if ($cpackExit -eq 0) {
        $archive = Get-ChildItem $cpackOut -Filter *.zip -File -ErrorAction SilentlyContinue |
                   Select-Object -First 1
    }

    if ($cpackExit -ne 0 -or -not $archive) {
        $failures += "cpack"
        Write-Host "FAIL - cpack produced no archive" -ForegroundColor Red
    } else {
        Write-Host ("archive: {0} ({1:N0} bytes)" -f $archive.Name, $archive.Length)

        $extract = Join-Path $work "cpack-extract"
        Expand-Archive -Path $archive.FullName -DestinationPath $extract -Force
        # CPack archives carry one top-level directory (CPACK_INCLUDE_TOPLEVEL_DIRECTORY)
        $pkgRoot = (Get-ChildItem $extract -Directory | Select-Object -First 1).FullName
        if (-not $pkgRoot) { $pkgRoot = $extract }
        Write-Host ("unpacked into {0}" -f $pkgRoot)

        # -- the archive promises a layout: assert it, do not eyeball it -------
        $required = @(
            "bin\$Config\piplugin$libSuffix.dll",
            "lib\$Config\piplugin$libSuffix.lib",
            "include\piplugin\pi_plugin.h",
            "lib\cmake\piplugin\pipluginConfig.cmake",
            "LICENSE"
        )
        $missing = @()
        foreach ($rel in $required) {
            if (-not (Test-Path (Join-Path $pkgRoot $rel))) { $missing += $rel }
        }
        if ($missing.Count -gt 0) {
            $failures += "cpack layout"
            Write-Host ("FAIL - not in the archive: {0}" -f ($missing -join ', ')) -ForegroundColor Red
        } else {
            Write-Host "archive layout: PASS" -ForegroundColor Green
        }

        # -- the archive alone must be enough to build and RUN a host ----------
        $zipConsumer = Join-Path $work "consumer-cpack"
        & cmake -S $consumerSrc -B $zipConsumer -G "Visual Studio 17 2022" -A x64 `
            "-DCMAKE_PREFIX_PATH=$pkgRoot" "-DCMAKE_BUILD_TYPE=$Config" 2>&1 |
            ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -ne 0) {
            $failures += "cpack consumer configure"
            Write-Host "FAIL - configure against the unpacked archive" -ForegroundColor Red
        } else {
            & cmake --build $zipConsumer --config $Config --parallel 2>&1 |
                ForEach-Object { Write-Host "    $_" }
            if ($LASTEXITCODE -ne 0) {
                $failures += "cpack consumer build"
                Write-Host "FAIL - build against the unpacked archive" -ForegroundColor Red
            } else {
                $exe = Join-Path $zipConsumer "$Config\pi_plugin_consumer.exe"
                $pkgBin = Join-Path $pkgRoot "bin\$Config"
                # the host finds the SHARED core either next to itself (copy, as in
                # phase A) or on PATH; take the copy so the run cannot silently pick
                # up a DLL from somewhere else.
                Copy-Item (Join-Path $pkgBin "*.dll") (Split-Path $exe) -Force

                # No build tree on PATH: prove the run does not depend on this
                # repository (phase B appended Conan deploy dirs to PATH).
                $savedPath = $env:PATH
                $env:PATH = ((($savedPath -split ';') |
                              Where-Object { $_ -and ($_ -notlike "$repoRoot*") }) -join ';')
                $env:PATH = "$pkgBin;" + $env:PATH
                & $exe
                $runExit = $LASTEXITCODE
                $env:PATH = $savedPath

                if ($runExit -ne 0) {
                    $failures += "cpack consumer run"
                    Write-Host "FAIL - running a host built from the unpacked archive" -ForegroundColor Red
                } else {
                    Write-Host "cpack archive: PASS" -ForegroundColor Green
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
