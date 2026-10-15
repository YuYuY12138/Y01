param([string]$Label = ('Damage-Net-' + (Get-Date -Format 'yyyyMMdd-HHmmss')), [int]$Port = 18097, [ValidateSet(8,128)][int]$BurstEvents = 8)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$prefix = "$root\Saved\$Label"
$exe = 'G:\UE_Engine\g01_engine\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$script:owned = @()
$result = [ordered]@{ RunnerPID=$PID; Started=(Get-Date).ToString('o'); Finished=$null; Passed=$false; Error=$null; BurstEvents=$BurstEvents; Processes=@(); Expected=0; Sent=0; Received=0; Missing=@(); Unexpected=@(); Duplicates=@(); ContentChanged=@() }
function Save-State { $result | ConvertTo-Json -Depth 8 | Set-Content "$prefix-result.json" -Encoding UTF8 }
function Content-Hashes {
    $hashes = @{}
    Get-ChildItem "$root\Content" -File -Recurse | ForEach-Object { $hashes[$_.FullName] = (Get-FileHash $_.FullName -Algorithm SHA256).Hash }
    return $hashes
}
function Launch([string]$Role, [string[]]$Arguments) {
    $log = "$prefix-$Role.log"
    $common = @("`"$root\Y01.uproject`"", '-unattended', '-nop4', '-nosound', '-nullrhi', '-Y01DamageTrace', '"-LogCmds=LogNetTraffic Log,LogNetPartialBunch VeryVerbose"', "`"-abslog=$log`"")
    $p = Start-Process $exe -ArgumentList ($common + $Arguments) -PassThru
    $script:owned += $p
    $result.Processes += [ordered]@{ Role=$Role; PID=$p.Id; Started=$p.StartTime.ToString('o'); Finished=$null; Command=($common + $Arguments) -join ' '; Log=$log; StoppedByRunner=$false; ExitCode=$null }
    Save-State
    return $p
}
function Stop-Owned {
    for ($i=0; $i -lt $owned.Count; $i++) {
        $p = $owned[$i]
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; $result.Processes[$i].StoppedByRunner=$true }
        Wait-Process -Id $p.Id -Timeout 10 -ErrorAction SilentlyContinue
        $result.Processes[$i].ExitCode=$p.ExitCode
        if (-not $result.Processes[$i].Finished) { $result.Processes[$i].Finished=(Get-Date).ToString('o') }
    }
    Save-State
}
function Events([string]$Path, [string]$Kind) {
    $events = @()
    foreach ($line in Select-String -Path $Path -Pattern "Y01Damage $Kind ") {
        $fields = @{}
        foreach ($m in [regex]::Matches($line.Line, '(Player|Id|Epoch|Region|Actual|Generation)=([^ ]+)')) { $fields[$m.Groups[1].Value] = $m.Groups[2].Value }
        $events += (($fields.Player, $fields.Epoch, $fields.Id, $fields.Region, $fields.Actual) -join '|')
    }
    return $events
}
Save-State
$before = Content-Hashes
try {
    if (Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue) { throw "UDP port $Port is occupied" }
    $server = Launch 'Server' @('/Game/Y01Playground/Maps/L_Y01Playground', '-server', "-port=$Port", '-Y01DamageNetVerify', "-Y01DamageBurstEvents=$BurstEvents")
    $deadline = (Get-Date).AddSeconds(45)
    $ready = $false
    while ((Get-Date) -lt $deadline -and -not $server.HasExited) {
        if ((Test-Path "$prefix-Server.log") -and (Select-String "$prefix-Server.log" -Pattern 'GameNetDriver.*listening on port' -Quiet)) { $ready=$true; break }
        Start-Sleep -Milliseconds 250
    }
    if (-not $ready) { throw 'DS did not open game port' }
    $null = Launch 'Client1' @("127.0.0.1:$Port", '-game', '-ExecCmds="t.MaxFPS 60"')
    $null = Launch 'Client2' @("127.0.0.1:$Port", '-game', '-ExecCmds="t.MaxFPS 60"')
    $deadline = (Get-Date).AddSeconds(90)
    $done = $false
    while ((Get-Date) -lt $deadline) {
        if (@($owned | Where-Object { $_.HasExited }).Count -gt 0) { throw 'Owned UE process exited before verification completed' }
        if (Select-String "$prefix-Server.log" -Pattern 'Y01Damage NET_FAIL|Fatal error|Ensure condition failed' -Quiet) { throw 'Server verification failed' }
        if (Select-String "$prefix-Server.log" -Pattern 'Y01Damage NET_DONE' -Quiet) { $done=$true; break }
        Start-Sleep -Milliseconds 250
    }
    if (-not $done) { throw 'Network verification timed out' }
    Start-Sleep -Seconds 2
    Stop-Owned
    $expected = @(Events "$prefix-Server.log" 'EXPECT')
    $sent = @(Events "$prefix-Server.log" 'SEND')
    $received = @(Events "$prefix-Client1.log" 'RECEIVE') + @(Events "$prefix-Client2.log" 'RECEIVE')
    $result.Expected=$expected.Count; $result.Sent=$sent.Count; $result.Received=$received.Count
    $result.Missing = @($expected | Where-Object { $_ -notin $sent -or $_ -notin $received })
    $result.Unexpected = @($sent + $received | Where-Object { $_ -notin $expected })
    $result.Duplicates = @($received | Group-Object | Where-Object { $_.Count -ne 1 } | ForEach-Object { $_.Name })
    $result.SentDuplicates = @($sent | Group-Object | Where-Object { $_.Count -ne 1 } | ForEach-Object { $_.Name })
    $result.ServerLocalReceives = @(Events "$prefix-Server.log" 'RECEIVE').Count
    $result.ClientCounts = @(@(Events "$prefix-Client1.log" 'RECEIVE').Count, @(Events "$prefix-Client2.log" 'RECEIVE').Count)
    $total = if ($BurstEvents -eq 128) { 1024 } else { 80 }
    $result.OrderChanged = @()
    foreach ($player in @($expected | ForEach-Object { ($_ -split '\|')[0] } | Select-Object -Unique)) {
        $e = @($expected | Where-Object { $_.StartsWith("$player|") }) -join ','
        $s = @($sent | Where-Object { $_.StartsWith("$player|") }) -join ','
        $r = @($received | Where-Object { $_.StartsWith("$player|") }) -join ','
        if ($e -cne $s -or $e -cne $r) { $result.OrderChanged += $player }
    }
    $serverLines = @(Select-String -Path "$prefix-Server.log" -Pattern 'Y01Damage (NET_CONFIG|CHUNK|BATCH|DEFER)|Sent RPC:|LogNetPartialBunch:' | ForEach-Object { $_.Line })
    $result.NetworkConfig = @($serverLines | Where-Object { $_ -match 'Y01Damage NET_CONFIG ' })
    $result.RPCBytes = @($serverLines | ForEach-Object { if ($_ -match 'Sent RPC: .*::ClientDamageBatch \[([0-9.]+) bytes\]') { [double]$Matches[1] } })
    $result.Chunks = @($serverLines | ForEach-Object {
        if ($_ -match 'Y01Damage CHUNK Controller=([^ ]+) Time=([0-9.]+) Offset=(\d+) Count=(\d+) BytesBudget=(\d+)') {
            [ordered]@{ Controller=$Matches[1]; Time=[double]$Matches[2]; Offset=[int]$Matches[3]; Count=[int]$Matches[4]; BytesBudget=[int]$Matches[5]; RPCBytes=$null }
        }
    })
    $result.Deferred = @($serverLines | Where-Object { $_ -match 'Y01Damage DEFER ' })
    $result.FullBatches = @($serverLines | Where-Object { $_ -match 'Y01Damage BATCH .*Count=128$' }).Count
    $result.ReliablePromotions = @($serverLines | Where-Object { $_ -match 'Making bunches reliable|reliable partial bunch overflows|exceeds reliable threashold' })
    $result.PartialLogs = @($serverLines | Where-Object { $_ -match 'LogNetPartialBunch:' })
    $result.DamagePartialLogs = @(); $insideDamageRPC = $false
    foreach ($line in $serverLines) {
        if ($line -match 'Sent RPC:') { $insideDamageRPC = $line -match '::ClientDamageBatch ' }
        if ($insideDamageRPC -and $line -match 'LogNetPartialBunch:') { $result.DamagePartialLogs += $line }
    }
    $result.ClientPartialLogs = @(Select-String -Path "$prefix-Client1.log","$prefix-Client2.log" -Pattern 'LogNetPartialBunch:' | ForEach-Object { $_.Line })
    $result.MaxRPCBytes = ($result.RPCBytes | Measure-Object -Maximum).Maximum
    $result.BudgetViolations = @()
    for ($i=0; $i -lt $result.Chunks.Count; $i++) {
        $chunk = $result.Chunks[$i]
        if ($i -lt $result.RPCBytes.Count) { $chunk.RPCBytes = $result.RPCBytes[$i] }
        if ($chunk.Count -lt 1 -or $chunk.Count -gt 8 -or $chunk.BytesBudget -ge 768 -or $i -ge $result.RPCBytes.Count -or $result.RPCBytes[$i] -gt $chunk.BytesBudget) { $result.BudgetViolations += $i }
    }
    $result.RPCCount = $result.RPCBytes.Count
    $result.ChunkEventTotal = ($result.Chunks | ForEach-Object { $_.Count } | Measure-Object -Sum).Sum
    Save-State
    if ($expected.Count -ne $total -or $sent.Count -ne $total -or $received.Count -ne $total -or $result.Missing.Count -or $result.Unexpected.Count -or $result.Duplicates.Count -or $result.SentDuplicates.Count -or $result.ServerLocalReceives -or $result.OrderChanged.Count -or @($result.ClientCounts | Where-Object { $_ -ne ($total / 2) }).Count) { throw 'Per-event audience/identity/order comparison failed' }
    if ($result.NetworkConfig.Count -ne 2 -or @($result.NetworkConfig | Where-Object { $_ -notmatch 'MaxPacket=1024 .*ReliableThreshold=8 Iris=0 CustomReplication=0' }).Count) { throw 'Generic replication / default packet configuration evidence failed' }
    if ($result.ReliablePromotions.Count -or $result.DamagePartialLogs.Count) { throw 'Damage RPC triggered engine partial bunch / reliable promotion' }
    if (-not $result.RPCBytes.Count -or $result.RPCBytes.Count -ne $result.Chunks.Count -or $result.ChunkEventTotal -ne $total -or $result.BudgetViolations.Count) { throw 'RPC byte budget / chunk evidence failed' }
    if ($BurstEvents -eq 128 -and ($result.FullBatches -ne 8 -or $result.RPCBytes.Count -ne 128)) { throw '128-event full-load coverage failed' }
    $result.Passed = $true
} catch { $result.Error = $_.ToString(); $result.Passed = $false }
finally {
    Stop-Owned
    $after = Content-Hashes
    $result.ContentChanged = @($before.Keys | Where-Object { -not $after.ContainsKey($_) -or $before[$_] -ne $after[$_] }) + @($after.Keys | Where-Object { -not $before.ContainsKey($_) })
    if ($result.ContentChanged.Count) { $result.Passed=$false; $result.Error='Content hash changed during run' }
    $result.Finished=(Get-Date).ToString('o'); Save-State
}
if (-not $result.Passed) { exit 1 }
exit 0
