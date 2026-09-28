param(
    [ValidateSet('Success','Failure','Disabled','Production')][string]$Scenario = 'Success',
    [int]$Port = 17777,
    [string]$Label = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = 'G:\UE_Engine\g01_engine\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
if (-not $Label) { $Label = 'Smoke-' + $Scenario + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss') }
$prefix = "$root\Saved\$Label"
$owned = @()
Start-Transcript -Path "$prefix-runner.log" -NoClobber
try {
    $busy = & "$env:SystemRoot\System32\netstat.exe" -ano -p UDP | Select-String -Pattern "^\s*UDP\s+\S+:$Port\s"
    if ($busy) { throw "UDP端口已占用: $Port" }
    $url = '/Engine/Maps/Entry?game=/Script/Y01Tests.Y01SmokeGameMode'
    if ($Scenario -eq 'Production') { $url = '/Engine/Maps/Entry' }
    $serverArgs = @("`"$root\Y01.uproject`"", $url, '-server', "-port=$Port", '-nullrhi', '-unattended', '-nop4', '-nosound', "-abslog=$prefix-server.log")
    if ($Scenario -eq 'Success' -or $Scenario -eq 'Failure') { $serverArgs += @('-Y01Smoke', "-Y01SmokeScenario=$Scenario") }
    else { $serverArgs += '-ExecCmds=Quit' }
    $server = Start-Process -FilePath $exe -ArgumentList $serverArgs -PassThru
    $owned += $server
    Write-Output "START Server PID=$($server.Id) Scenario=$Scenario Port=$Port"
    if ($Scenario -eq 'Disabled' -or $Scenario -eq 'Production') {
        Wait-Process -Id $server.Id -Timeout 60
        Write-Output "EXIT Server PID=$($server.Id) Code=$($server.ExitCode)"
        if ($server.ExitCode -ne 0) { throw '负场景进程异常退出' }
        $log = Get-Content "$prefix-server.log" -Raw
        if ($log -notmatch 'Y01Flow MissingConfiguration Waiting' -or $log -match 'Y01Smoke (ENABLED|EXPECT|ACK|PASS)') { throw '禁用smoke验证失败' }
        Write-Output "NEGATIVE PASS Scenario=$Scenario MissingConfiguration Waiting NoSmoke"
    } else {
        $ready = $false
        for ($i = 0; $i -lt 45; $i++) {
            if ($server.HasExited) { throw '服务器在监听前退出' }
            if ((Test-Path "$prefix-server.log") -and (Select-String -Path "$prefix-server.log" -Pattern 'listening on port' -Quiet)) { $ready = $true; break }
            Start-Sleep -Seconds 1
        }
        if (-not $ready) { throw '服务器未在45秒内监听' }
        $clients = @()
        foreach ($id in 1,2) {
            $clientArgs = @("`"$root\Y01.uproject`"", "127.0.0.1:$Port", '-game', '-Y01Smoke', "-Y01SmokeClient=$id", '-nullrhi', '-unattended', '-nop4', '-nosound', "-abslog=$prefix-client$id.log")
            $client = Start-Process -FilePath $exe -ArgumentList $clientArgs -PassThru
            $owned += $client; $clients += $client
            Write-Output "START Client$id PID=$($client.Id)"
        }
        Wait-Process -Id $server.Id -Timeout 120
        Write-Output "EXIT Server PID=$($server.Id) Code=$($server.ExitCode)"
        foreach ($client in $clients) {
            if (-not $client.HasExited) { Wait-Process -Id $client.Id -Timeout 15 }
            Write-Output "EXIT Client PID=$($client.Id) Code=$($client.ExitCode)"
        }
        foreach ($p in $owned) { if ($p.ExitCode -ne 0) { throw "进程失败 PID=$($p.Id) Exit=$($p.ExitCode)" } }
        $serverLog = Get-Content "$prefix-server.log" -Raw
        if ($serverLog -notmatch "Y01Smoke PASS Scenario=$Scenario Clients=2" -or $serverLog -match 'Y01Smoke FAIL') { throw '服务器缺少成功证据' }
        $ack = Select-String -Path "$prefix-server.log" -Pattern "Y01Smoke ACK Scenario=$Scenario"
        $expectedCount = 20
        if ($Scenario -eq 'Failure') { $expectedCount = 10 }
        if ($ack.Count -ne $expectedCount) { throw "ACK数量错误 Actual=$($ack.Count) Expected=$expectedCount" }
        $sequences = @{}
        foreach ($entry in $ack) {
            if ($entry.Line -notmatch 'Peer=(\d+).*Checkpoint=\w+ (Seq=(\d+) .*)') { throw 'ACK格式无效' }
            $peer = $Matches[1]; $observation = $Matches[2]; $seq = [int]$Matches[3]
            if (-not $sequences.ContainsKey($peer)) { $sequences[$peer] = 0 }
            if ($seq -ne $sequences[$peer] + 1) { throw "Peer=$peer阶段ACK乱序" }
            $sequences[$peer] = $seq
            $needle = "Y01Smoke CLIENT_OBS Peer=$peer $observation"
            if (-not (Select-String -Path "$prefix-client1.log","$prefix-client2.log" -Pattern $needle -SimpleMatch -Quiet)) { throw "Peer=$peer Seq=$seq缺少对应客户端实测日志" }
            if ($observation -notmatch 'FloorReady=1') { throw '客户端未收到碰撞地面' }
        }
        if ($sequences.Count -ne 2) { throw '不是两个唯一客户端' }
        foreach ($id in 1,2) {
            $clientLog = Get-Content "$prefix-client$id.log" -Raw
            if ($clientLog -notmatch 'Y01Smoke CLIENT_OBS.*Phase=Ended' -or $clientLog -notmatch 'Y01Smoke CLIENT_FINISHED') { throw "Client$id缺少复制终态证据" }
            if ($clientLog -match 'could not resolve the new relative movement base actor|Y01Smoke FAIL|Fatal error:') { throw "Client$id存在基座或运行时错误" }
        }
        Write-Output "VERIFIED Scenario=$Scenario ACKs=$($ack.Count) AllExit=0 Evidence=$prefix"
    }
} finally {
    foreach ($p in $owned) {
        if (-not $p.HasExited) { Write-Output "CLEANUP owned PID=$($p.Id)"; Stop-Process -Id $p.Id -Force }
    }
    Stop-Transcript
}
