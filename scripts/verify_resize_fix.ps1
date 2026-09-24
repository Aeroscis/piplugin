# verify_resize_fix.ps1 - automated regression for the ImGui-panel resize fix.
#
# Everything is programmatic (SetWindowPos steps + PrintWindow captures); no
# real mouse dragging is involved, so this can run unattended.
#
#   1. Start the imgui test host with the Qt plugin auto-loaded.
#   2. Screenshot A: steady state at the initial size.
#   3. Grow the window fast via SetWindowPos steps (WM_SIZE path; the swap
#      chain must grow once and then stay big).
#   4. Screenshot B: steady state at the larger size.
#   5. Shrink the window below the grown size (the swap chain must NOT shrink
#      - this is the state where DXGI_SCALING_NONE has to clip the oversized
#      frame 1:1 instead of letting DWM rescale it).
#   6. Screenshot C: steady state at the smaller size (buffer > window).
#   7. Smoke-test the host's own size-loop entry (WM_NCLBUTTONDOWN with no
#      button held must exit immediately and cleanly).
#   8. Pixel-measure the panel right edge and the plugin left edge on all three
#      screenshots, relative to the client-area left edge: they must stay at
#      ~400 / ~410 physical px. If DWM ever rescaled the frame, C would show
#      them shrunk by the window/buffer ratio.
#   9. Assert the log shows scale:none, the buffer growth, zero failures.
#
# NOTE on DPI: this script runs DPI-unaware, so GetWindowRect/GetClientRect
# return virtualised (logical) pixels, while PrintWindow returns physical
# pixels. The scale factor is derived from the two and applied to the
# client-area offset before measuring.
#
# Run: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify_resize_fix.ps1

param(
    [string]$BinDir  = "",
    [string]$ShotDir = "",
    [double]$GrowFactor  = 1.5,   # target client size while growing
    [double]$ShrinkFactor = 0.62  # target client size while shrinking (relative to initial)
)

$ErrorActionPreference = "Stop"

# Defaults are derived from the repository root (this script lives in
# <root>\scripts\), so another machine, another checkout directory or another
# build configuration only needs -BinDir. The old hardcoded absolute paths
# broke when the repository was renamed to piplugin.
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BinDir)  { $BinDir  = Join-Path $repoRoot "bin\Debug" }
if (-not $ShotDir) { $ShotDir = Join-Path $repoRoot "build\verify_shots" }

# ------------------------- Win32 + screenshot helpers (one compilation) ------
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Drawing;
using System.Drawing.Imaging;
public class PiWin32 {
    // NOTE: the second parameter is IntPtr, not string - PowerShell turns
    // $null into an empty string, and FindWindowW("cls", "") only matches a
    // window whose title is empty. IntPtr.Zero means "ignore the title".
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowW(string cls, IntPtr title);
    public delegate bool EnumWindowsCb(IntPtr h, IntPtr l);
    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsCb cb, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")]
    public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")]
    public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int left, top, right, bottom; }
    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int x, y; }

    // Fall back to the title if the class lookup fails.
    public static IntPtr FindHostWindow(string cls, string titlePrefix) {
        IntPtr found = FindWindowW(cls, IntPtr.Zero);
        if (found != IntPtr.Zero) return found;
        IntPtr hit = IntPtr.Zero;
        EnumWindows((h, l) => {
            System.Text.StringBuilder sb = new System.Text.StringBuilder(256);
            if (GetWindowTextW(h, sb, 256) > 0 && sb.ToString().StartsWith(titlePrefix)) {
                hit = h;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return hit;
    }

    public static int[] WindowRect(IntPtr h) {
        RECT r; GetWindowRect(h, out r);
        return new int[] { r.left, r.top, r.right - r.left, r.bottom - r.top };
    }
    public static int[] ClientRect(IntPtr h) {
        RECT r; GetClientRect(h, out r);
        return new int[] { r.right, r.bottom };
    }
    public static int[] ClientOffset(IntPtr h) {
        RECT wr; GetWindowRect(h, out wr);
        POINT p = new POINT(); ClientToScreen(h, ref p);
        return new int[] { p.x - wr.left, p.y - wr.top };
    }
    public static void Resize(IntPtr h, int left, int top, int winW, int winH) {
        SetWindowPos(h, IntPtr.Zero, left, top, winW, winH, 0x0004 | 0x0010);  // NOZORDER|NOACTIVATE
    }
}

public class PiShot {
    // Capture the composited window (D3D content included) into a PNG.
    public static int[] Capture(IntPtr hwnd, string path) {
        PiWin32.RECT wr;
        PiWin32.GetWindowRect(hwnd, out wr);
        int w = wr.right - wr.left, h = wr.bottom - wr.top;
        if (w <= 0 || h <= 0) return null;
        using (Bitmap bmp = new Bitmap(w, h)) {
            using (Graphics g = Graphics.FromImage(bmp)) {
                IntPtr hdc = g.GetHdc();
                bool ok = PiWin32.PrintWindow(hwnd, hdc, 2 /*PW_RENDERFULLCONTENT*/);
                g.ReleaseHdc(hdc);
                if (!ok) return null;
            }
            bmp.Save(path, ImageFormat.Png);
        }
        return new int[] { w, h };
    }

    // Measure the ImGui panel right edge and the plugin area left edge, both
    // returned RELATIVE to the client-area left edge (physical px):
    //   panel background ~ (16,16,16), host clear ~ (38,38,38),
    //   plugin light background ~ (241,241,241)
    // Returns null when no scan line has a clean panel->gap->plugin profile.
    public static int[] Measure(string path, int cx, int cy, int cw, int ch) {
        using (Bitmap bmp = new Bitmap(path)) {
            if (cx + cw > bmp.Width || cy + ch > bmp.Height) return null;
            BitmapData bd = bmp.LockBits(new Rectangle(0, 0, bmp.Width, bmp.Height),
                                         ImageLockMode.ReadOnly,
                                         PixelFormat.Format32bppRgb);
            int stride = Math.Abs(bd.Stride);
            byte[] row = new byte[stride];
            int[] best = null;
            int y0 = cy + (int)(ch * 0.15);
            int y1 = cy + (int)(ch * 0.95);
            if (y1 >= bmp.Height) y1 = bmp.Height - 1;
            for (int y = y0; y < y1 && best == null; y += 2) {
                Marshal.Copy(new IntPtr(bd.Scan0.ToInt64() + (long)y * stride), row, 0, stride);
                int x0 = cx, x1 = cx + cw;
                // 1. plugin left edge: start of the longest run of light pixels
                int runStart = -1, runLen = 0, bestStart = -1, bestLen = 0;
                for (int x = x0; x < x1; x++) {
                    int b = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2];
                    bool light = (r > 230 && g > 230 && b > 230);
                    if (light) {
                        if (runLen == 0) runStart = x;
                        runLen++;
                        if (runLen > bestLen) { bestLen = runLen; bestStart = runStart; }
                    } else {
                        runLen = 0;
                    }
                }
                if (bestStart < 0 || bestLen < 40) continue;
                int pluginLeft = bestStart;
                // 2. gap: walk left from the plugin edge over the clear colour
                int x2 = pluginLeft - 1;
                while (x2 >= x0) {
                    int b = row[x2 * 4], g = row[x2 * 4 + 1], r = row[x2 * 4 + 2];
                    if (Math.Abs(r - 38) <= 6 && Math.Abs(g - 38) <= 6 && Math.Abs(b - 38) <= 6) x2--;
                    else break;
                }
                int gapLen = pluginLeft - 1 - x2;
                if (gapLen < 3 || gapLen > 40) continue;
                int panelRight = x2 + 1;
                // 3. left of the gap must be the dark panel
                int probe = panelRight - 5;
                if (probe < x0) continue;
                int pb = row[probe * 4], pg = row[probe * 4 + 1], pr = row[probe * 4 + 2];
                if (!(pr <= 40 && pg <= 40 && pb <= 40)) continue;
                best = new int[] { panelRight - cx, pluginLeft - cx };
            }
            bmp.UnlockBits(bd);
            return best;
        }
    }
}
"@

# ------------------------------------------------------------------ start --
New-Item -ItemType Directory -Force -Path $ShotDir | Out-Null
$exe = Join-Path $BinDir "pi_plugin_test_host_imgui.exe"
if (-not (Test-Path $exe)) { throw "host exe not found: $exe" }
$log = Join-Path $BinDir "pi_plugin_test_host.log"
if (Test-Path $log) { Remove-Item $log -Force }
foreach ($f in @("A_initial.png", "B_grown.png", "C_shrunk.png")) {
    $p = Join-Path $ShotDir $f
    if (Test-Path $p) { Remove-Item $p -Force }
}

Write-Host "[1/8] starting host with Qt plugin..."
$proc = Start-Process -FilePath $exe -ArgumentList "pi_plugin_test_plugin_qt.dll" -WorkingDirectory $BinDir -PassThru
Start-Sleep -Seconds 3

$hwnd = [PiWin32]::FindHostWindow("PiPluginTestHost", "piplugin")
if ($hwnd -eq [IntPtr]::Zero) { throw "host window not found" }

$wr   = [PiWin32]::WindowRect($hwnd)      # left, top, w, h  (logical px)
$cr   = [PiWin32]::ClientRect($hwnd)      # client w, h      (logical px)
$coff = [PiWin32]::ClientOffset($hwnd)    # client offset    (logical px)
$frameW = $wr[2] - $cr[0]
$frameH = $wr[3] - $cr[1]
$baseW = $cr[0]; $baseH = $cr[1]
Write-Host ("       client (logical) = {0}x{1}, frame = {2}x{3}" -f $baseW, $baseH, $frameW, $frameH)

function Set-ClientSize([int]$cw, [int]$ch) {
    [PiWin32]::Resize($hwnd, $wr[0], $wr[1], $cw + $frameW, $ch + $frameH)
}
function Resize-InSteps([int]$fromW, [int]$fromH, [int]$toW, [int]$toH, [int]$steps, [int]$msPause) {
    for ($i = 1; $i -le $steps; $i++) {
        $cw = [int]($fromW + ($toW - $fromW) * $i / $steps)
        $ch = [int]($fromH + ($toH - $fromH) * $i / $steps)
        Set-ClientSize $cw $ch
        Start-Sleep -Milliseconds $msPause
    }
}
function Shot([string]$file) {
    $p = Join-Path $ShotDir $file
    $size = [PiShot]::Capture($hwnd, $p)
    if ($null -eq $size) { throw "capture $file failed" }
    # logical window size -> physical bitmap size ratio (DPI)
    $wrNow = [PiWin32]::WindowRect($hwnd)
    $scale = [double]$size[0] / [double]$wrNow[2]
    $crNow = [PiWin32]::ClientRect($hwnd)
    [PSCustomObject]@{
        Path   = $p
        Scale  = $scale
        BmpW   = $size[0]; BmpH = $size[1]
        Cx     = [int]($coff[0] * $scale)
        Cy     = [int]($coff[1] * $scale)
        Cw     = [int]($crNow[0] * $scale)
        Ch     = [int]($crNow[1] * $scale)
    }
}

# ------------------------------------------------------------- screenshot A --
Write-Host "[2/8] screenshot A: steady state at initial size"
$A = Shot "A_initial.png"

Write-Host "[3/8] growing window to $([int]($baseW*$GrowFactor))x$([int]($baseH*$GrowFactor)) logical, in 10 fast steps"
Resize-InSteps $baseW $baseH ([int]($baseW * $GrowFactor)) ([int]($baseH * $GrowFactor)) 10 25
Start-Sleep -Milliseconds 800

Write-Host "[4/8] screenshot B: steady state, grown"
$B = Shot "B_grown.png"

Write-Host "[5/8] shrinking window to $([int]($baseW*$ShrinkFactor))x$([int]($baseH*$ShrinkFactor)) logical, in 10 fast steps (buffer must stay big)"
$grownW = [int]($baseW * $GrowFactor); $grownH = [int]($baseH * $GrowFactor)
Resize-InSteps $grownW $grownH ([int]($baseW * $ShrinkFactor)) ([int]($baseH * $ShrinkFactor)) 10 25
Start-Sleep -Milliseconds 800

Write-Host "[6/8] screenshot C: steady state, shrunk (buffer larger than window)"
$C = Shot "C_shrunk.png"

Write-Host "[7/8] smoke: WM_NCLBUTTONDOWN(HTBOTTOMRIGHT) with no button held (must exit at once)"
$wrNow = [PiWin32]::WindowRect($hwnd)
$lx = $wrNow[0] + 20; $ly = $wrNow[1] + 20
$lparam = [IntPtr]((($ly -shl 16) -bor ($lx -band 0xFFFF)))
[void][PiWin32]::PostMessageW($hwnd, 0x00A1, [IntPtr]17, $lparam)   # WM_NCLBUTTONDOWN, HTBOTTOMRIGHT
Start-Sleep -Milliseconds 700

Write-Host "[8/8] closing host and analysing..."
[void][PiWin32]::PostMessageW($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE
try { Wait-Process -Id $proc.Id -Timeout 20 -ErrorAction SilentlyContinue } catch { }
if (-not $proc.HasExited) { try { Stop-Process -Id $proc.Id -Force } catch { } }

# ---------------------------------------------------------------- analysis --
$fail = $false
foreach ($s in @(
    @{ Name = "A_initial"; Data = $A },
    @{ Name = "B_grown";   Data = $B },
    @{ Name = "C_shrunk";  Data = $C })) {
    $d = $s.Data
    $m = [PiShot]::Measure($d.Path, $d.Cx, $d.Cy, $d.Cw, $d.Ch)
    if ($null -eq $m) {
        Write-Host ("  {0}: MEASURE FAILED (no clean panel->gap->plugin row)" -f $s.Name)
        $fail = $true
        continue
    }
    $panelW = $m[0]; $pluginL = $m[1]; $gap = $pluginL - $panelW
    $ok = ($panelW -ge 392 -and $panelW -le 408 -and $pluginL -ge 400 -and $pluginL -le 422)
    $line = ("  {0}: panel right = {1}px, plugin left = {2}px, gap = {3}px  ({4}x{5} bmp, dpi {6:N2}) -> {7}" -f `
        $s.Name, $panelW, $pluginL, $gap, $d.BmpW, $d.BmpH, $d.Scale, $(if ($ok) { "OK" } else { "OUT OF RANGE" }))
    if ($ok) { Write-Host $line -ForegroundColor Green }
    else { Write-Host $line -ForegroundColor Red; $fail = $true }
}

# ------------------------------------------------------------------- log ----
if (Test-Path $log) {
    # The host keeps the log open with an exclusive fopen(...,"a") while it
    # lives; read it with FileShare.ReadWrite and retry while it is still held.
    $logText = $null
    for ($i = 0; $i -lt 5 -and $null -eq $logText; $i++) {
        try {
            $fs = [System.IO.File]::Open($log, [System.IO.FileMode]::Open,
                                          [System.IO.FileAccess]::Read,
                                          [System.IO.FileShare]::ReadWrite)
            $sr = New-Object System.IO.StreamReader($fs)
            $logText = $sr.ReadToEnd()
            $sr.Close(); $fs.Close()
        } catch {
            Start-Sleep -Milliseconds 800
        }
    }
    if ($null -eq $logText) { Write-Host "  log could not be read (still locked)"; $fail = $true }
    foreach ($c in @(
        @{ Pattern = "scale:none";                                 Desc = "swap chain uses DXGI_SCALING_NONE" },
        @{ Pattern = "swap chain grows to";                        Desc = "buffer grew on demand" },
        @{ Pattern = "host size loop ended after 0 steps";         Desc = "size-loop smoke exited at once" })) {
        if ($logText -match $c.Pattern) { Write-Host ("  log OK: {0}" -f $c.Desc) -ForegroundColor Green }
        else { Write-Host ("  log MISSING: {0}" -f $c.Desc); $fail = $true }
    }
    # case-sensitive: "FAILED hr=0x..." is a real failure, "failed=0" is not
    if ($logText -cmatch "FAILED") {
        Write-Host "  log has FAILED entries:" -ForegroundColor Red
        ($logText -split "`n" | Where-Object { $_ -cmatch "FAILED" }) | ForEach-Object { Write-Host "    $_" }
        $fail = $true
    } else { Write-Host "  log OK: no FAILED entries" -ForegroundColor Green }
} else {
    Write-Host "  log not produced" -ForegroundColor Red
    $fail = $true
}

Write-Host ""
if ($fail) { Write-Host "RESULT: FAIL"; exit 1 }
Write-Host "RESULT: PASS - geometry stable across programmatic resizes" -ForegroundColor Green
exit 0
