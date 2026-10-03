param(
    [Parameter(Mandatory=$true)][string]$Label,
    [Parameter(Mandatory=$true)][string]$RunId
)
$ErrorActionPreference = 'Stop'
if ($Label -cnotmatch '^[0-9a-f]{16}$' -or $RunId -cne ([guid]::Parse($RunId)).ToString()) {
    throw '[Qualification:Socket] Invalid broker cleanup identity.'
}
$Repository = 'C:\Sandbox\Codex\Workspaces\agent-coordinator-foundation-2-server'
$Root = "C:\Sandbox\Codex\Artifacts\gargantuan-3l-physical\$Label"
$TaskName = "Gargantuan3L-SocketBroker-$Label"
$Cancel = Join-Path $Repository ".lifecycle\physical-broker-$Label.cancel.json"
$Request = @{ RunId = $RunId; Action = 'CANCEL' } | ConvertTo-Json -Compress
if (-not (Test-Path -LiteralPath $Cancel)) {
    [System.IO.File]::WriteAllText($Cancel, $Request)
}
$Task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
if ($Task) {
    $Deadline = (Get-Date).AddSeconds(5)
    while ((Get-ScheduledTask -TaskName $TaskName).State -eq 'Running' -and (Get-Date) -lt $Deadline) {
        Start-Sleep -Milliseconds 100
    }
    if ((Get-ScheduledTask -TaskName $TaskName).State -eq 'Running') {
        $Marker = Join-Path $Root 'worker-endpoint-process.json'
        if (Test-Path -LiteralPath $Marker) {
            $Owned = Get-Content -LiteralPath $Marker -Raw | ConvertFrom-Json
            if ($Owned.RunId -cne $RunId -or $Owned.Executable -cne 'C:\Sandbox\Codex\Tools\physical-qualifier\runtime\python.exe' -or
                $Owned.Config -cne (Join-Path $Root 'server-broker.json')) {
                throw '[Qualification:Socket] Broker process ownership changed.'
            }
            $Process = Get-CimInstance Win32_Process -Filter "ProcessId=$($Owned.Pid)" -ErrorAction SilentlyContinue
            if ($Process -and ($Process.ExecutablePath -cne $Owned.Executable -or
                -not $Process.CommandLine.Contains($Owned.Config))) {
                throw '[Qualification:Socket] Endpoint PID ownership changed.'
            }
            if ($Process) {
                & taskkill /T /F /PID $Owned.Pid | Out-Null
                if ($LASTEXITCODE -ne 0) { throw '[Qualification:Socket] Unable to stop owned endpoint tree.' }
            }
        }
        Stop-ScheduledTask -TaskName $TaskName
    }
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
}
Write-Output '[Qualification:Socket] Fixed worker broker cleaned.'
