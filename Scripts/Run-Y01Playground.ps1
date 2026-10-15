param(
    [ValidateSet('Dedicated','Standalone')][string]$Mode = 'Dedicated',
    [ValidateRange(1,2)][int]$Clients = 1,
    [ValidateRange(1024,65535)][int]$Port = 17977,
    [string]$Engine = 'G:\UE_Engine\g01_engine',
    [string]$Map = '/Game/Developers/Y01PlaygroundTests/R36/Runs/S4_AddedRows_20261001_01/Maps/L_Y01R36',
    [ValidateSet('','content','profile','evidence','compare')][string]$R36Profile = '',
    [switch]$Verify,
    [switch]$Headless,
    [int]$TimeoutSeconds = 120,
    [string]$Label = ('Playground-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
)
$ErrorActionPreference = 'Stop'
function Resolve-PlaygroundMap([string]$Selection, [bool]$Verification) {
    $legacy = '/Game/Developers/Y01PlaygroundTests/R33U2Legacy/Maps/L_Y01Playground'
    $resolved = switch ($Selection) {
        'main' { '/Game/Y01Playground/Maps/L_Y01Playground' }
        'r36' { '/Game/Developers/Y01PlaygroundTests/R36/Runs/S4_AddedRows_20261001_01/Maps/L_Y01R36' }
        'legacy' { $legacy }
        'compare' { '/Game/Developers/Y01PlaygroundTests/R33U2Compare/Maps/L_Y01Playground' }
        'u2baseline' { '/Game/Developers/Y01PlaygroundTests/R33U2Compare/Baseline/Maps/L_Y01Playground' }
        'growthcompare' { '/Game/Developers/Y01PlaygroundTests/R33U3GrowthCompare/Maps/L_Y01Playground' }
        'r33growth' { '/Game/Developers/Y01PlaygroundTests/R33Growth/Maps/L_Y01Playground' }
        'trainingcompare' { '/Game/Developers/Y01PlaygroundTests/R33TrainingCompare/Maps/L_Y01Playground' }
        'r34compare' { '/Game/Developers/Y01PlaygroundTests/R34Compare/Maps/L_Y01Playground' }
        'r34ranged' { '/Game/Developers/Y01PlaygroundTests/R34Ranged/Maps/L_Y01Playground' }
        default { $Selection }
    }
    if ($Verification -and $resolved -ne $legacy) { throw '旧即时Verify仅支持显式 -Map legacy；新技能不能复用此验收。' }
    return $resolved
}
$Map = Resolve-PlaygroundMap $Map ([bool]$Verify)
$root = Split-Path $PSScriptRoot -Parent
$project = Join-Path $root 'Y01.uproject'
$exe = Join-Path $Engine 'Engine\Binaries\Win64\UnrealEditor.exe'
$cmd = Join-Path $Engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
if ($Headless -and -not $Verify) { throw 'Headless is only allowed with explicit -Verify.' }
if ($Mode -eq 'Standalone' -and $Clients -ne 1) { throw 'Standalone supports one local player. Use Dedicated for two clients.' }
if ($Map -notmatch '^/Game/[A-Za-z0-9_/]+$') { throw 'Map must be a /Game/ package path.' }
$diskMap = Join-Path $root ('Content\' + $Map.Substring(6).Replace('/','\') + '.umap')
if (-not (Test-Path $diskMap)) { throw "Saved map missing: $diskMap. This launcher never generates or overwrites assets." }
if (-not (Test-Path $exe)) { throw "UnrealEditor missing: $exe" }
$script:owned = @()
$script:logs = @()
$common = @("`"$project`"", '-nop4', '-nosound', '-NoSplash', '-NoLoadingScreen', '-NoScreenMessages')
if ($R36Profile) { $common += "-R36Profile=$R36Profile" }
$verifyArgs = @()
if ($Verify) { $verifyArgs = @('-Y01PlaygroundVerify', "-Y01PlaygroundPeers=$Clients") }
function Launch-Owned([string]$binary, [string[]]$arguments, [string]$role) {
    $log = Join-Path $root "Saved\$Label-$role.log"
    $script:logs += $log
    $process = Start-Process -FilePath $binary -ArgumentList ($arguments + "`"-abslog=$log`"") -PassThru
    $script:owned += $process
    Write-Host "Playground $role PID=$($process.Id) Log=$log"
    return $process
}
try {
    Write-Host "Map=$Map"
    if ($Verify) { Write-Host 'LEGACY_VERIFY_ONLY：旧即时技能回归，不代表新范围/飞行物验收。' }
    Write-Host 'Enter: start/next wave | WASD: move | 1/2: attack | F5: reload instructions'
    Write-Host 'Save DataTables, Ctrl+C this launcher, then run again to reload. Only owned processes are stopped.'
    if ($Mode -eq 'Dedicated') {
        if (Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue) { throw "UDP port $Port is already occupied." }
        $server = Launch-Owned $cmd ($common + @($Map, '-server', '-nullrhi', "-port=$Port") + $verifyArgs) 'Server'
        $readyDeadline = (Get-Date).AddSeconds(40)
        $ready = $false
        while ((Get-Date) -lt $readyDeadline -and -not $server.HasExited) {
            $serverLog = $logs[0]
            if ((Test-Path $serverLog) -and (Select-String -Path $serverLog -Pattern 'GameNetDriver.*listening on port' -Quiet)) { $ready = $true; break }
            Start-Sleep -Milliseconds 300
        }
        if (-not $ready) { throw 'Dedicated server did not open its game port; inspect Server log.' }
    }
    for ($i = 1; $i -le $Clients; $i++) {
        $destination = if ($Mode -eq 'Dedicated') { "127.0.0.1:$Port" } else { $Map }
        $clientArgs = $common + @($destination, '-game', '-windowed', '-ResX=1280', '-ResY=800', '-ForceRes', "-WinX=$($i * 40)", "-WinY=$($i * 40)", '-ExecCmds="t.MaxFPS 30"') + $verifyArgs
        $binary = $exe
        if ($Headless) { $clientArgs += '-nullrhi'; $binary = $cmd }
        elseif ($Verify) { $clientArgs += '-Y01PlaygroundScreenshot' }
        $null = Launch-Owned $binary $clientArgs "Client$i"
    }
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while (@($owned | Where-Object { -not $_.HasExited }).Count -gt 0) {
        if ($Verify -and (Get-Date) -gt $deadline) { throw "Verification timeout after $TimeoutSeconds seconds. No success ACK." }
        if (-not $Verify -and @($owned | Where-Object { $_.HasExited }).Count -gt 0) { break }
        Start-Sleep -Milliseconds 250
    }
    if ($Verify) {
        foreach ($p in $owned) { if ($p.ExitCode -ne 0) { throw "PID=$($p.Id) Exit=$($p.ExitCode)" } }
        foreach ($log in $logs) {
            $marker = if ($log.EndsWith('-Server.log')) { 'Y01Playground VERIFY_SERVER_PASS' } else { 'Y01Playground VERIFY_CLIENT_PASS' }
            if (-not (Select-String -Path $log -Pattern $marker -Quiet)) { throw "Missing real verification evidence: $marker in $log" }
            if (-not $log.EndsWith('-Server.log') -and -not (Select-String -Path $log -Pattern 'Y01Playground INPUT_PASS' -Quiet)) { throw "Missing real keyboard input-path evidence in $log" }
        }
        Write-Host "LEGACY VERIFY PASS: $Mode / $Clients client(s). Logs: Saved\$Label-*"
    }
} finally {
    foreach ($p in $owned) {
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    }
}
