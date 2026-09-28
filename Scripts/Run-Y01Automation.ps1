param([string]$Label = 'Wave-Green', [string]$Tests = 'Y01')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = 'G:\UE_Engine\g01_engine\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$args = @("`"$root\Y01.uproject`"", '-unattended', '-nop4', '-nullrhi', '-nosound', "`"-ExecCmds=Automation RunTests $Tests`"", '"-TestExit=Automation Test Queue Empty"', "-ReportExportPath=$root\Saved\$Label", "-abslog=$root\Saved\$Label.log")
Start-Transcript -Path "$root\Saved\$Label-runner.log" -NoClobber
$p = Start-Process -FilePath $exe -ArgumentList $args -PassThru
try {
    Wait-Process -Id $p.Id -Timeout 150 -ErrorAction Stop
    Write-Output "Automation PID=$($p.Id) Exit=$($p.ExitCode) Report=$root\Saved\$Label"
    if ($p.ExitCode -ne 0) { exit $p.ExitCode }
    $report = Get-Content "$root\Saved\$Label\index.json" -Raw -Encoding UTF8 | ConvertFrom-Json
    Write-Output "Results Succeeded=$($report.succeeded) WithWarnings=$($report.succeededWithWarnings) Failed=$($report.failed) NotRun=$($report.notRun)"
    if ($report.failed -ne 0 -or $report.notRun -ne 0 -or $report.inProcess -ne 0) { exit 1 }
    exit 0
} finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Stop-Transcript
}
