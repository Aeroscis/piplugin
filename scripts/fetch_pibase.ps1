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
# A pin that is compared against nothing is decoration, so an existing checkout
# is checked against it: the pin is resolved inside that clone and compared with
# HEAD, and the two are printed when they differ. -RequirePin goes further and
# makes the checkout match - it moves it to the pin (no network, when the commit
# is already in the clone) or re-fetches when it is not. That is what the package
# verification calls: every green run has to be a run against the named revision.
#
# Keep this file ASCII-only: it has no BOM, and Windows PowerShell 5.1 decodes a
# BOM-less script as ANSI, where non-ASCII bytes mis-decode and can eat the
# following line.

param(
    [string]$Ref = "",
    [string]$Remote = "",
    [string]$Dest = "",
    [switch]$Force,
    [switch]$RequirePin
)

$ErrorActionPreference = "Stop"

# Reads git's answer without letting git's stderr end the run. Same trap as the
# one scripts/verify_package.ps1 documents for cmake and conan: a native command's
# stderr line becomes an ErrorRecord, and under $ErrorActionPreference "Stop" the
# first one is a terminating error - here it would be a harmless "unknown
# revision" from rev-parse. Empty output means "no answer".
function Invoke-Git([string[]]$arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & git @arguments 2>$null
        if ($null -eq $output) { return "" }
        return ($output | Out-String).Trim()
    } finally {
        $ErrorActionPreference = $previous
    }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Dest)   { $Dest   = Join-Path $repoRoot "external\pibase" }
# gitee by default: it is this repository's origin, and GitHub over HTTPS is not
# reachable from every machine that builds this project (SSH is, HTTPS is not).
if (-not $Remote) { $Remote = "https://gitee.com/Aeroscis/pibase.git" }
# Pinned commit: pibase has no tags yet, so a commit is the only stable pin.
if (-not $Ref)    { $Ref    = "65de807" }

if (Test-Path (Join-Path $Dest ".git")) {
    $head = Invoke-Git @("-C", $Dest, "rev-parse", "HEAD")
    # Resolve the pin inside this clone, so a tag, a full sha and an abbreviated
    # sha all compare as the commit they name. Empty = not present here.
    $want = Invoke-Git @("-C", $Dest, "rev-parse", "--verify", "--quiet", "$Ref^{commit}")
    $atPin = [bool]($head -and $want -and ($head -eq $want))

    if ($atPin -and -not $Force) {
        Write-Host "pibase already fetched at $Dest"
        Write-Host "  at the pin: $head"
        exit 0
    }

    if (-not $Force -and -not $RequirePin) {
        Write-Host "pibase already fetched at $Dest"
        if ($head) { Write-Host "  checkout: $head" }
        if ($want) { Write-Host "  pin     : $want" }
        else       { Write-Host "  pin     : $Ref (not in this checkout)" }
        Write-Host "  pass -Force to re-fetch, or -RequirePin to move the checkout to the pin"
        exit 0
    }

    if ($Force) {
        Write-Host "re-fetching pibase (-Force)"
    } elseif ($want) {
        # -RequirePin with the pinned commit already in this clone: move the
        # checkout. No network, and nothing is deleted.
        Write-Host "pibase checkout is not at the pin; moving it"
        Write-Host "  checkout: $head"
        Write-Host "  pin     : $want"
        $null = Invoke-Git @("-C", $Dest, "checkout", "--quiet", $Ref)
        $moved = Invoke-Git @("-C", $Dest, "rev-parse", "HEAD")
        if ($moved -ne $want) { throw "-RequirePin could not move $Dest to $Ref" }
        Write-Host "pibase ready: $moved"
        exit 0
    } else {
        # -RequirePin with a pin this clone does not contain: only a fetch can
        # supply it, and a fetch means starting over.
        Write-Host "re-fetching pibase (-RequirePin: $Ref is not in this checkout)"
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
