# drag_measure.ps1 — Automated reproduction / verification for the "ImGui panel
# gets transiently scaled while dragging the window border" bug
# (docs/todo/tests.md item 7).
#
# What it does:
#   1. kills any running pi_test_host_imgui instance,
#   2. starts the host with the Qt test plugin auto-loaded,
#   3. parks the window at a known geometry, measures the static panel width,
#   4. synthesizes a real border drag (mouse_event) on the RIGHT border,
#      grabbing one screen scan line per moment (two shots per drag step),
#   5. drags the border back (shrink direction),
#   6. writes per-frame CSV + a summary report; a frame whose measured panel
#      width deviates from the static baseline by more than 3 px is counted as
#      a DEFORM frame (that is the DWM-scaled frame the bug is about).
#
# Output: <OutDir>\<Tag>_frames.csv and <OutDir>\<Tag>_report.txt
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\drag_measure.ps1 -Tag baseline
#   powershell -ExecutionPolicy Bypass -File scripts\drag_measure.ps1 -Tag fixed

param(
    [string]$ExePath    = "",
    [string]$PluginPath = "",
    [string]$Tag        = "run",
    [int]$Steps         = 10,
    [int]$StepPx        = 36,
    [int]$SettleMs      = 2000,
    [string]$OutDir     = ""
)

$ErrorActionPreference = 'Stop'

# Defaults are derived from the repository root (this script lives in
# <root>\scripts\) instead of hardcoding absolute paths.
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $ExePath)    { $ExePath    = Join-Path $repoRoot "bin\Debug\pi_test_host_imgui.exe" }
if (-not $PluginPath) { $PluginPath = Join-Path $repoRoot "bin\Debug\pi_test_plugin_qt.dll" }
if (-not $OutDir) { $OutDir = Join-Path $PSScriptRoot "out" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

trap {
    $_ | Out-String | Set-Content -Path (Join-Path $OutDir "${Tag}_error.txt")
    if ($script:hostProc -and -not $script:hostProc.HasExited) {
        Stop-Process -Id $script:hostProc.Id -Force -ErrorAction SilentlyContinue
    }
    Get-Process -Name pi_test_host_imgui -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    exit 3
}

Add-Type -TypeDefinition (Get-Content -Raw (Join-Path $PSScriptRoot 'drag_measure.cs')) -Language CSharp
[Dm]::SetProcessDPIAware() | Out-Null

$exeDir = Split-Path -Parent $ExePath
$logPath = Join-Path $exeDir 'pi_plugin_test_host.log'

function Get-SharedText([string]$path) {
    try {
        $fs = New-Object System.IO.FileStream($path, [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read,
            ([System.IO.FileShare]::Read -bor [System.IO.FileShare]::Write -bor [System.IO.FileShare]::Delete))
        try {
            $sr = New-Object System.IO.StreamReader($fs)
            try { return $sr.ReadToEnd() } finally { $sr.Dispose() }
        } finally { $fs.Dispose() }
    } catch { return $null }
}

$csv  = New-Object System.Collections.Generic.List[string]
$csv.Add("seq,step,shot,clientW,clientH,panelW,pluginLeft,delta,verdict")
$script:hostProc = $null
$script:hwnd     = [IntPtr]::Zero

try {
    # ---- 1. clean slate ----------------------------------------------------
    Get-Process -Name pi_test_host_imgui -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 600

    # ---- 2. start host with plugin ------------------------------------------
    $oldLen = 0
    if (Test-Path $logPath) { $oldLen = (Get-Item $logPath).Length }

    $script:hostProc = Start-Process -FilePath $ExePath -ArgumentList ('"' + $PluginPath + '"') `
        -WorkingDirectory $exeDir -PassThru

    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline -and $script:hwnd -eq [IntPtr]::Zero) {
        $script:hwnd = [Dm]::FindHostWindow()
        if ($script:hwnd -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 150 }
    }
    if ($script:hwnd -eq [IntPtr]::Zero) { throw "host window not found" }

    # wait for the plugin to be attached (log grows and mentions attach)
    $attached = $false
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline) {
        $txt = Get-SharedText $logPath
        if ($txt -and $txt.Length -ge $oldLen) {
            $fresh = ""
            if ($txt.Length -gt $oldLen) { $fresh = $txt.Substring($oldLen) }
            elseif ($txt.Length -eq $oldLen -and $oldLen -eq 0) { $fresh = $txt }
            if ($fresh -match 'attach: plugin hwnd') { $attached = $true; break }
            if ($fresh -match 'auto-load:') { }
        }
        if ($script:hostProc.HasExited) { throw "host exited prematurely" }
        Start-Sleep -Milliseconds 200
    }

    # ---- 3. park the window at a known geometry -----------------------------
    # keep the grow path on the primary monitor: window at (40,40), ~1280x720 client
    $screenW = [Dm]::GetSystemMetrics(0)
    $growBudget = [Math]::Max(160, $screenW - 40 - 1300 - 40)
    $steps = [Math]::Min($Steps, [int]($growBudget / $StepPx))
    if ($steps -lt 4) { $steps = 4 }

    [Dm]::SetWindowPos($script:hwnd, [IntPtr]::Zero, 40, 40, 1296, 759, 0x0040) | Out-Null # SWP_SHOWWINDOW
    [Dm]::SetForegroundWindow($script:hwnd) | Out-Null
    Start-Sleep -Milliseconds $SettleMs

    # ---- 4. static baseline (panel width while nothing moves) ---------------
    $staticPw = New-Object System.Collections.Generic.List[int]
    for ($i = 0; $i -lt 5; $i++) {
        $cw = 0; $ch = 0; $pw = 0; $pl = 0
        $rc = [Dm]::CaptureRow($script:hwnd, 0.5, [ref]$cw, [ref]$ch, [ref]$pw, [ref]$pl)
        if ($rc -eq 0 -and $pw -gt 0) { $staticPw.Add($pw) }
        Start-Sleep -Milliseconds 70
    }
    $sorted = $staticPw.ToArray(); [Array]::Sort($sorted)
    $baseline = $sorted[[int](($sorted.Length - 1) / 2)]
    if ($staticPw.Count -lt 3) { throw "static panel not detected (plugin loaded: $attached)" }

    # ---- helper: one shot ----------------------------------------------------
    function Add-Shot([string]$seq, [int]$step, [int]$shot) {
        $cw = 0; $ch = 0; $pw = 0; $pl = 0
        $rc = [Dm]::CaptureRow($script:hwnd, 0.5, [ref]$cw, [ref]$ch, [ref]$pw, [ref]$pl)
        $delta = ""
        $verdict = "MISS"
        if ($rc -eq 0 -and $pw -gt 0) {
            $delta = $pw - $baseline
            if ([Math]::Abs($pw - $baseline) -le 3) { $verdict = "OK" } else { $verdict = "DEFORM" }
        }
        $script:csv.Add(("$seq,$step,$shot,$cw,$ch,$pw,$pl,$delta,$verdict"))
        return $verdict
    }

    # ---- 5. drag right border OUT (grow) -------------------------------------
    function Invoke-Drag([int]$dxTotal, [int]$nSteps, [string]$seqName) {
        $wr = New-Object 'Dm+RECT'
        [Dm]::GetWindowRect($script:hwnd, [ref]$wr) | Out-Null
        $ax = $wr.R - 5
        $ay = [int](($wr.T + $wr.B) / 2)
        [Dm]::SetCursorPos($ax, $ay) | Out-Null
        Start-Sleep -Milliseconds 150
        [Dm]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # LEFTDOWN on the border
        Start-Sleep -Milliseconds 180                          # host size loop takes over
        for ($i = 1; $i -le $nSteps; $i++) {
            $tx = $ax + [int]($dxTotal * $i / $nSteps)
            [Dm]::SetCursorPos($tx, $ay) | Out-Null
            Start-Sleep -Milliseconds 8
            for ($k = 0; $k -lt 2; $k++) {
                Add-Shot $seqName $i $k | Out-Null
                Start-Sleep -Milliseconds 6
            }
        }
        [Dm]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # LEFTUP
        Start-Sleep -Milliseconds 300
    }

    Invoke-Drag  ($steps * $StepPx) $steps "grow"
    Invoke-Drag -($steps * $StepPx) $steps "shrink"

    # ---- 6. summary ------------------------------------------------------------
    $dataRows = @($csv) | Select-Object -Skip 1
    $deform = 0; $ok = 0; $miss = 0; $maxAbs = 0
    foreach ($line in $dataRows) {
        $f = $line -split ','
        if ($f[8] -eq 'DEFORM') { $deform++ }
        elseif ($f[8] -eq 'OK') { $ok++ }
        else { $miss++ }
        if ($f[7] -ne '' -and $f[7] -match '^-?\d+$') {
            $a = [Math]::Abs([int]$f[7]); if ($a -gt $maxAbs) { $maxAbs = $a }
        }
    }
    $report = @()
    $report += "drag-measure report  tag=$Tag  time=$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    $report += "exe=$ExePath"
    $report += "plugin-attached=$attached  steps=$steps stepPx=$StepPx"
    $report += "static baseline panelW=$baseline px (median of $($staticPw.Count) shots)"
    $report += "drag shots: ok=$ok deform=$deform miss=$miss  max|delta|=${maxAbs}px"
    if ($deform -gt 0) {
        $report += "RESULT: FAIL - $deform frame(s) show the panel scaled (bug reproduced)"
    } elseif ($miss -gt 0 -and $ok -eq 0) {
        $report += "RESULT: INCONCLUSIVE - no usable shots"
    } else {
        $report += "RESULT: PASS - panel width stable through the whole drag"
    }
    Set-Content -Path (Join-Path $OutDir "${Tag}_frames.csv") -Value $csv -Encoding ASCII
    Set-Content -Path (Join-Path $OutDir "${Tag}_report.txt") -Value $report -Encoding ASCII
}
finally {
    if ($script:hostProc -and -not $script:hostProc.HasExited) {
        Stop-Process -Id $script:hostProc.Id -Force -ErrorAction SilentlyContinue
    }
    Get-Process -Name pi_test_host_imgui -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
