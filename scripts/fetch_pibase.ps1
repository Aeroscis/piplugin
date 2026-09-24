# fetch_pibase.ps1 - obtain the family root layer (pibase) as source.
#
# Why a script and not FetchContent: cloning at configure time means a build can
# fail, or quietly use a different revision, depending on the network. Fetching
# is an explicit step here, so configure stays offline and reproducible, and CI
# can cache the checkout.
#
# The checkout lands in <repo>/external/pibase, which is git-ignored, and is
# pinned to a commit - a branch would make builds unreproducible. Override with
# -Ref once pibase starts tagging releases (a tag is the better pin).
#
# Keep this file ASCII-only: it has no BOM, and Windows PowerShell 5.1 decodes a
# BOM-less script as ANSI, where non-ASCII bytes mis-decode and can eat the
# following line.
param(
    [string]$Ref = "",
    [string]$Remote = "",
    [string]$Dest = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Dest)   { $Dest   = Join-Path $repoRoot "external\pibase" }
# gitee by default: it is this repository's origin, and GitHub over HTTPS is not
# reachable from every machine that builds this project (SSH is, HTTPS is not).
if (-not $Remote) { $Remote = "https://gitee.com/Aeroscis/pibase.git" }
# Pinned commit: pibase has no tags yet, so a commit is the only stable pin.
if (-not $Ref)    { $Ref    = "9985d3f" }

if (Test-Path (Join-Path $Dest ".git")) {
    if (-not $Force) {
        Write-Host "pibase already fetched at $Dest"
        Write-Host "  pass -Force to re-fetch, or -Ref <commit|tag> to change the pin"
        exit 0
    }
    Remove-Item -Recurse -Force $Dest
}

Write-Host "fetching pibase"
Write-Host "  remote: $Remote"
Write-Host "  ref   : $Ref"
Write-Host "  dest  : $Dest"

New-Item -ItemType Directory -Path (Split-Path -Parent $Dest) -Force | Out-Null

# Clone, then check out the exact pin. --no-checkout + checkout keeps the pin
# honest: a shallow single-branch clone cannot always reach an arbitrary commit.
& git clone --quiet --no-checkout $Remote $Dest
if ($LASTEXITCODE -ne 0) { throw "git clone failed ($LASTEXITCODE)" }

& git -C $Dest checkout --quiet $Ref
if ($LASTEXITCODE -ne 0) {
    throw "git checkout $Ref failed - is the commit reachable from the cloned refs? Try -Ref main for a floating checkout."
}

$sha = (& git -C $Dest rev-parse HEAD).Trim()
Write-Host "pibase ready: $sha"
Write-Host ""
Write-Host "Now configure piplugin as usual. The provider defaults to 'auto', which"
Write-Host "finds an installed pibase first and falls back to this checkout:"
Write-Host "  cmake --preset conan-default"
