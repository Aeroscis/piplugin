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
#                        It is unzipped into a clean directory and the consumer is
#                        configured there WITHOUT the Conan toolchain (no build tree,
#                        no Qt environment on PATH), and the resulting host program
#                        must run. The archive is packaged from its own build tree,
#                        configured with PI_PLUGIN_PIBASE_PROVIDER=fetch (see below).
#
#                        "Stands on its own" means something narrower than it used
#                        to, now that piplugin depends on the family root layer: the
#                        archive must be built from a tree where pibase came from
#                        source (PI_PLUGIN_PIBASE_PROVIDER=fetch), so that pibase's
#                        headers and config install into the same prefix and travel
#                        inside the ZIP. An archive built against an externally
#                        installed pibase would look fine here and then fail on the
#                        downloader's machine, which is the one failure this phase
#                        exists to catch.
#
#                        The checkout in external\pibase must BE the pinned commit,
#                        not merely exist: this phase calls fetch_pibase.ps1 with
#                        -RequirePin, which moves a checkout that sits at some other
#                        revision back to the pin (or re-fetches when the commit is
#                        not in the clone). A pin is only worth something if every
#                        green run was produced against it.
#
# Phase B is the slow one (it rebuilds the whole project inside Conan's cache),
# and phase C builds the project a second time in its own tree. Use -SkipConan to
# run A + C only, -SkipCpack to run A + B only.
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

# Runs a native command and echoes its output - stdout AND stderr - as plain text.
#
# Why not `2>&1` at the call site: PowerShell turns every stderr line of a native
# command into an ErrorRecord, and with $ErrorActionPreference "Stop" the first
# harmless cmake / conan WARNING on stderr becomes a terminating error - the run
# died there twice, before anybody could read the reason. Lowering the preference
# for the duration of the call keeps those lines as text; the caller still decides
# from $LASTEXITCODE (which is global, so it survives the function boundary).
function Invoke-Native([scriptblock]$command) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $command 2>&1 | ForEach-Object { Write-Host "    $_" }
    } finally {
        $ErrorActionPreference = $previous
    }
}

# ---------------------------------------------------------------------------
# A. install tree
# ---------------------------------------------------------------------------
Write-Section "A. install tree"

$prefix = Join-Path $work "install"
Invoke-Native { cmake --install $BuildDir --config $Config --prefix $prefix }
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL - cmake --install" -ForegroundColor Red; exit 1 }
Write-Host ("installed into {0}" -f $prefix)

$hasQtKit = Test-Path (Join-Path $prefix "lib\$Config\piplugin_qt$libSuffix.lib")
Write-Host ("Qt adapter kit installed: {0} (the consumer does not need it)" -f $hasQtKit)

$conanToolchain = Join-Path $BuildDir "generators\conan_toolchain.cmake"
$installConsumer = Join-Path $work "consumer-install"
Invoke-Native { cmake -S $consumerSrc -B $installConsumer -G "Visual Studio 17 2022" -A x64 `
    "-DCMAKE_TOOLCHAIN_FILE=$conanToolchain" `
    "-DCMAKE_PREFIX_PATH=$prefix" -DPI_PLUGIN_CONSUMER_LINK_IMGUI=ON }
if ($LASTEXITCODE -ne 0) {
    $failures += "install-tree configure"
    Write-Host "FAIL - configure against the install tree" -ForegroundColor Red
} else {
    Invoke-Native { cmake --build $installConsumer --config $Config --parallel }
    if ($LASTEXITCODE -ne 0) {
        $failures += "install-tree build"
        Write-Host "FAIL - build against the install tree" -ForegroundColor Red
    } else {
        $exe = Join-Path $installConsumer "$Config\pi_plugin_consumer.exe"
        # the core is SHARED: put the installed runtime next to the exe
        Copy-Item (Join-Path $prefix "bin\$Config\*.dll") (Split-Path $exe) -Force
        Invoke-Native { & $exe }
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
    # Scoped on purpose: an unscoped -o is "ambiguous" to Conan 2 (it cannot tell
    # which package the option belongs to) and only buys a warning on stderr.
    Invoke-Native { conan create . --build=missing -s build_type=$Config `
        -o "piplugin/*:PI_PLUGIN_BUILD_TESTS=False" }
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
        # piplugin is the ONLY requirement here, and that is the point: both of its
        # own dependencies have to travel with it. They do because the recipe says
        # so - pibase with transitive_headers (its headers are named by our own
        # public headers) and imgui with transitive_libs (the imgui adapter kit's
        # component links imgui::imgui). Conan's default propagation drops both
        # (conan/internal/model/requires.py, Requirement.transform_downstream),
        # which is how this phase used to fail: pibase-config.cmake never
        # generated, no target carried pibase's include dir, and the consumer
        # died on #include <pibase/pi_base.h>. A consumer that has to declare our
        # dependencies by hand is not a self-contained package, so the probe here
        # must not do it either.
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
        Invoke-Native { conan install . -s build_type=$Config --build=missing `
            --deployer=full_deploy "--deployer-folder=$deployRoot" }
        $genExit = $LASTEXITCODE
        Pop-Location

        if ($genExit -ne 0) {
            $failures += "conan install (consumer)"
            Write-Host "FAIL - conan install for the consumer" -ForegroundColor Red
        } else {
            $conanConsumer = Join-Path $work "consumer-conan"
            Invoke-Native { cmake -S $consumerSrc -B $conanConsumer -G "Visual Studio 17 2022" -A x64 `
                "-DCMAKE_TOOLCHAIN_FILE=$conanWork\conan_toolchain.cmake" -DCMAKE_BUILD_TYPE=$Config `
                -DPI_PLUGIN_CONSUMER_LINK_IMGUI=ON }
            if ($LASTEXITCODE -ne 0) {
                $failures += "conan consumer configure"
                Write-Host "FAIL - configure against the Conan package" -ForegroundColor Red
            } else {
                Invoke-Native { cmake --build $conanConsumer --config $Config --parallel }
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
                    Invoke-Native { & $exe }
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
    # -- the tree the archive is packaged from -------------------------------
    # Not the tree the other phases used: that one takes the family root layer
    # (pibase) from an installed package, so pibase's headers and CMake config
    # stay outside the install prefix and never enter the ZIP. Such an archive
    # looks fine HERE - where pibase happens to be installed - and then fails
    # on the downloader's machine, which is the one failure this phase exists
    # to catch. So the archive is built from a tree configured with
    # PI_PLUGIN_PIBASE_PROVIDER=fetch: pibase comes in with add_subdirectory
    # and installs into the same prefix, headers and config included.
    $pibaseSrc = Join-Path $repoRoot "external\pibase"
    # -RequirePin, not just "fetch when absent": the pin is compared against the
    # checkout, and one sitting at some other revision is moved back (or re-fetched
    # when the pin is not in that clone). Fetching only when the directory was
    # missing meant this phase passed against whatever revision the machine
    # happened to hold - the one thing a pin is there to rule out. The script also
    # prints the revision it settled on, so the log names what was packaged.
    Invoke-Native { powershell -NoProfile -File (Join-Path $repoRoot "scripts\fetch_pibase.ps1") -RequirePin }
    if (-not (Test-Path (Join-Path $pibaseSrc "CMakeLists.txt"))) {
        $failures += "pibase sources"
        Write-Host "FAIL - external\pibase is missing, or not at the pin; scripts\fetch_pibase.ps1 failed" -ForegroundColor Red
    }

    $cpackTree = Join-Path $work "cpack-tree"
    # Qt is deliberately off: without a Conan toolchain there is no imgui here
    # either, so the tree is core + host kits - the same subset the archive
    # must be able to serve on its own.
    Invoke-Native { cmake -S $repoRoot -B $cpackTree -G "Visual Studio 17 2022" -A x64 `
        -DPI_PLUGIN_PIBASE_PROVIDER=fetch "-DPI_PLUGIN_PIBASE_SOURCE_DIR=$pibaseSrc" `
        -DPI_PLUGIN_BUILD_TESTS=False -DPI_PLUGIN_BUILD_EXAMPLES=False `
        -DPI_PLUGIN_BUILD_ADAPTER_QT=False }
    $treeExit = $LASTEXITCODE
    if ($treeExit -ne 0) {
        $failures += "archive tree configure"
        Write-Host "FAIL - configure the fetch-provider tree" -ForegroundColor Red
    } else {
        # cpack does not build for a multi-config generator: build first.
        Invoke-Native { cmake --build $cpackTree --config $Config --parallel }
        $treeExit = $LASTEXITCODE
        if ($treeExit -ne 0) {
            $failures += "archive tree build"
            Write-Host "FAIL - build the fetch-provider tree" -ForegroundColor Red
        }
    }

    $cpackOut = Join-Path $work "cpack-out"
    New-Item -ItemType Directory -Force -Path $cpackOut | Out-Null

    $cpackExit = 1
    if ($treeExit -eq 0 -and -not ($failures -contains "pibase sources")) {
        Invoke-Native { cpack --config (Join-Path $cpackTree "CPackConfig.cmake") -C $Config -B $cpackOut }
        $cpackExit = $LASTEXITCODE
    }

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
        # The base layer is in this list because its ABSENCE is the defect this
        # phase was written for: added with EXCLUDE_FROM_ALL, pibase's install
        # rules never ran (see src/piplugin/CMakeLists.txt), the ZIP held piplugin
        # alone, and the downloader's build died on #include <pibase/pi_base.h>.
        # The consumer below fails in that case too, but naming the missing
        # artifact in the layout check says which one went missing instead of
        # leaving it to a compiler error two steps later.
        $required = @(
            "bin\$Config\piplugin$libSuffix.dll",
            "lib\$Config\piplugin$libSuffix.lib",
            "include\piplugin\pi_plugin.h",
            "lib\cmake\piplugin\pipluginConfig.cmake",
            "include\pibase\pi_base.h",
            "lib\cmake\pibase\pibaseConfig.cmake",
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
        Invoke-Native { cmake -S $consumerSrc -B $zipConsumer -G "Visual Studio 17 2022" -A x64 `
            "-DCMAKE_PREFIX_PATH=$pkgRoot" "-DCMAKE_BUILD_TYPE=$Config" }
        if ($LASTEXITCODE -ne 0) {
            $failures += "cpack consumer configure"
            Write-Host "FAIL - configure against the unpacked archive" -ForegroundColor Red
        } else {
            Invoke-Native { cmake --build $zipConsumer --config $Config --parallel }
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
                Invoke-Native { & $exe }
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
