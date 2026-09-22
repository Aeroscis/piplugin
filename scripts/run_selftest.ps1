# run_selftest.ps1 - run the imgui test host self-test cycles (plugin lifetime
# regression) and dump the resulting log next to it.
#
# The host keeps pi_test_host.log open with an exclusive fopen(...,"a") while
# it lives, so the log is read through FileShare.ReadWrite.

param(
    [int]$Cycles = 3,
    [string]$Plugin = "pi_test_plugin_qt.dll",
    [int]$IdleFrames = 12,
    [string]$BinDir = "D:\Flora\ProgramProjects\pipluginframework\bin\Debug",
    [string]$OutFile = "D:\Flora\ProgramProjects\pipluginframework\build\selftest.txt"
)

$exe = Join-Path $BinDir "pi_test_host_imgui.exe"
$log = Join-Path $BinDir "pi_test_host.log"
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
