# run_selftest.ps1 - run the imgui test host's conformance cycles (plugin lifetime
# + resize regression) over one or more plugin DLLs, and dump the log next to it.
#
# This is the ECO-02 conformance harness entry point: each listed plugin gets
#   load -> attach -> idle frames -> resize grow -> resize restore -> detach -> unload
# repeated $Cycles times. Exit code 0 = every plugin passed, 2 = something failed.
#
# The host keeps pi_plugin_test_host.log open with an exclusive fopen(...,"a") while
# it lives, so the log is read through FileShare.ReadWrite.
#
# -BinDir / -OutFile default to paths derived from the repository root (this
# script lives in <root>\scripts\), so another machine or build configuration
# only needs -BinDir.
#
# Keep this file ASCII-only: it has no BOM, and Windows PowerShell 5.1 decodes a
# BOM-less script as ANSI, where non-ASCII bytes mis-decode and can even eat the
# following line (a mangled byte turning into a backtick/backslash continuation).

param(
    [int]$Cycles = 3,
    [string]$Plugin = "pi_test_plugin_qt.dll,pi_test_plugin_imgui.dll",
    [int]$IdleFrames = 12,
    [string]$BinDir = "",
    [string]$OutFile = ""
)

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BinDir)  { $BinDir  = Join-Path $repoRoot "bin\Debug" }
if (-not $OutFile) { $OutFile = Join-Path $repoRoot "build\selftest.txt" }

$exe = Join-Path $BinDir "pi_test_host_imgui.exe"
if (-not (Test-Path $exe)) {
    Write-Host ("RESULT: FAIL - host not found: {0}" -f $exe) -ForegroundColor Red
    Write-Host "  hint: pass -BinDir <repo>\bin\<CONFIG> matching the build configuration"
    exit 1
}
$log = Join-Path $BinDir "pi_plugin_test_host.log"
Remove-Item $log -Force -ErrorAction SilentlyContinue

$p = Start-Process -FilePath $exe `
    -ArgumentList "--cycles", "$Cycles", "--plugin", $Plugin, "--idle-frames", "$IdleFrames" `
    -WorkingDirectory $BinDir -PassThru -Wait

Start-Sleep -Milliseconds 500
$text = "exitcode=" + $p.ExitCode + [Environment]::NewLine
$text += "-------------------------------- log --------------------------------" + [Environment]::NewLine
for ($i = 0; $i -lt 5; $i++) {
    try {
        $fs = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
        $sr = New-Object System.IO.StreamReader($fs)
        $text += $sr.ReadToEnd()
        $sr.Close(); $fs.Close()
        break
    } catch {
        Start-Sleep -Milliseconds 700
    }
}
[System.IO.File]::WriteAllText($OutFile, $text, [System.Text.Encoding]::UTF8)

# ---------------------------- verdict (conformance exit) ----------------------------
# exit code 0 = every listed plugin completed every cycle; 2 = something failed
# (see the "selftest: FAIL" lines in the log).
if ($p.ExitCode -ne 0) {
    Write-Host ("RESULT: FAIL - exitcode={0} (2 = at least one plugin did not pass)" -f $p.ExitCode) -ForegroundColor Red
    Write-Host ("  log: {0}" -f $OutFile)
    exit 1
}
if ($text -notmatch "selftest: PASS") {
    Write-Host "RESULT: FAIL - no 'selftest: PASS' in the log (exitcode was 0 but the run did not complete)" -ForegroundColor Red
    Write-Host ("  log: {0}" -f $OutFile)
    exit 1
}
Write-Host ("RESULT: PASS - {0} ({1} cycles each)" -f $Plugin, $Cycles) -ForegroundColor Green
exit 0
