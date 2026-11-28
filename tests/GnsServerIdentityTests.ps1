param(
    [Parameter(Mandatory = $true)]
    [string] $Executable
)

$ErrorActionPreference = 'Stop'
$TempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$RunRoot = [IO.Path]::GetFullPath((Join-Path $TempRoot ('GargantuanGnsIdentity-' + [guid]::NewGuid().ToString('N'))))
if (-not $RunRoot.StartsWith($TempRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Identity fixture path escaped the temporary directory.'
}
if (-not [IO.File]::Exists($Executable) -or $Executable.Contains('"')) {
    throw 'Identity fixture executable is unavailable.'
}
[IO.Directory]::CreateDirectory($RunRoot) | Out-Null
$Owned = New-Object 'System.Collections.Generic.List[System.Diagnostics.Process]'
$Passed = $false

function Start-Owned {
    param([string[]] $ProcessArguments)
    foreach ($Value in $ProcessArguments) {
        if ($Value.Contains('"')) { throw 'Identity fixture argument is invalid.' }
    }
    $Info = New-Object System.Diagnostics.ProcessStartInfo
    $Info.FileName = $Executable
    $Info.Arguments = ($ProcessArguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $Info.UseShellExecute = $false
    $Info.CreateNoWindow = $true
    $Info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $Process = New-Object System.Diagnostics.Process
    $Process.StartInfo = $Info
    if (-not $Process.Start()) { throw 'Identity fixture child did not start.' }
    $Owned.Add($Process)
    return $Process
}

function Wait-File {
    param([string] $Path, [int] $Seconds)
    $Deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    while ([DateTime]::UtcNow -lt $Deadline) {
        if ([IO.File]::Exists($Path)) { return }
        Start-Sleep -Milliseconds 20
    }
    throw 'Identity fixture evidence deadline expired.'
}

function Wait-Count {
    param([string] $Path, [int] $Expected, [int] $Seconds)
    $Deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    while ([DateTime]::UtcNow -lt $Deadline) {
        if ([IO.File]::Exists($Path) -and [IO.File]::ReadAllText($Path).Trim() -eq [string]$Expected) { return }
        Start-Sleep -Milliseconds 20
    }
    throw 'Identity fixture message count did not match.'
}

function Run-Client {
    param([int] $Port, [string] $Key, [string] $Mode)
    $Process = Start-Owned -ProcessArguments @('client', [string]$Port, $Key, $Mode)
    if (-not $Process.WaitForExit(7000)) { throw 'Identity fixture client deadline expired.' }
    if ($Process.ExitCode -ne 0) { throw 'Identity fixture client result failed.' }
}

function Start-Server {
    param([string] $Directory)
    [IO.Directory]::CreateDirectory($Directory) | Out-Null
    $Process = Start-Owned -ProcessArguments @('server', $Directory)
    Wait-File (Join-Path $Directory 'ready') 5
    $Lines = [IO.File]::ReadAllLines((Join-Path $Directory 'ready'))
    if ($Lines.Count -ne 2 -or $Lines[0] -notmatch '^[0-9]{5}$' -or
        $Lines[1] -cnotmatch '^[0-9a-f]{64}$') {
        throw 'Identity fixture server readiness is malformed.'
    }
    Wait-File (Join-Path $Directory 'messages') 5
    return @{ Process = $Process; Port = [int]$Lines[0]; Key = $Lines[1]; Directory = $Directory }
}

function Stop-Server {
    param($Server)
    [IO.File]::WriteAllText((Join-Path $Server.Directory 'stop'), 'stop')
    if (-not $Server.Process.WaitForExit(5000) -or $Server.Process.ExitCode -ne 0) {
        throw 'Identity fixture server did not stop cleanly.'
    }
}

try {
    $First = Start-Server (Join-Path $RunRoot 'first')
    $Wrong = if ($First.Key[0] -eq '0') { '1' + $First.Key.Substring(1) } else { '0' + $First.Key.Substring(1) }
    Run-Client $First.Port $Wrong 'reject'
    Wait-Count (Join-Path $First.Directory 'messages') 0 2
    Run-Client $First.Port $First.Key 'accept'
    Wait-Count (Join-Path $First.Directory 'messages') 1 3
    Stop-Server $First

    $Second = Start-Server (Join-Path $RunRoot 'second')
    if ($Second.Key -ceq $First.Key) { throw 'Identity fixture reused the prior server key.' }
    Run-Client $Second.Port $First.Key 'reject'
    Wait-Count (Join-Path $Second.Directory 'messages') 0 2
    Run-Client $Second.Port $Second.Key 'accept'
    Wait-Count (Join-Path $Second.Directory 'messages') 1 3
    Stop-Server $Second
    $Passed = $true
    Write-Output '[Network:ServerIdentity] real GNS pin, wrong key, prior-session key and pre-send denial passed.'
} finally {
    foreach ($Process in $Owned) {
        try {
            if (-not $Process.HasExited) { $Process.Kill() }
            $Process.Dispose()
        } catch {}
    }
    if ($Passed -and $RunRoot.StartsWith($TempRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $RunRoot -Recurse -Force
    }
}
