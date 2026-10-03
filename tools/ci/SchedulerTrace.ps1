# Wrap exactly the existing FULL workload once. No trace or workload retries.
[CmdletBinding()]
param([switch]$SelfTest)
$ErrorActionPreference = 'Stop'
function ResolveExit([object]$ChildExit, [int]$ControllerExit, [bool]$TimedOut, [int]$CleanupExit) {
    if ($null -ne $ChildExit -and $ChildExit -ne 0) { return [int]$ChildExit }
    if ($TimedOut -or $CleanupExit -ne 0) { return 125 }
    return $ControllerExit
}
if ($SelfTest) {
    foreach ($Case in @(
        @(17, 125, $true, 125, 17), @(0, 0, $false, 0, 0), @(0, 125, $false, 0, 125),
        @($null, 125, $false, 0, 125), @(0, 0, $true, 0, 125), @(0, 0, $false, 125, 125)
    )) {
        if ((ResolveExit $Case[0] $Case[1] $Case[2] $Case[3]) -ne $Case[4]) { throw 'Exit precedence test failed' }
    }
    Write-Output '[Qualification:SchedulerTrace] wrapper-self-test=PASS no-session-or-child-created'
    exit 0
}
$Root = (Get-Location).ProviderPath
$Helper = Join-Path $Root 'build-ci/scheduler-trace.exe'
$Workload = Join-Path $Root 'build-ci/gargantuan_game_session_real_transport_tests.exe'
$Output = Join-Path $Root 'build-ci/scheduler-trace'
$Etl = Join-Path $Output 'scheduler.etl'
$SessionGuid = [Guid]::NewGuid().ToString('D')
$ControllerExit = 125
$ControllerTimedOut = $false
$CleanupExit = 125
$CoverageExit = 125
$ChildExit = $null
$Started = [DateTime]::UtcNow.ToString('o')
if (-not (Test-Path -LiteralPath $Helper -PathType Leaf) -or -not (Test-Path -LiteralPath $Workload -PathType Leaf)) {
    throw 'Scheduler trace helper or fixed workload executable missing'
}
if (Test-Path -LiteralPath $Output) { throw 'Scheduler trace output already exists; refusing replay' }
$HelperHash = (Get-FileHash -LiteralPath $Helper -Algorithm SHA256).Hash.ToLowerInvariant()
$WorkloadHash = (Get-FileHash -LiteralPath $Workload -Algorithm SHA256).Hash.ToLowerInvariant()
function StartOwnedProcess([string[]]$Arguments) {
    $Info = [Diagnostics.ProcessStartInfo]::new($Helper)
    $Info.UseShellExecute = $false
    $Info.CreateNoWindow = $true
    $Info.WorkingDirectory = $Root
    foreach ($Argument in $Arguments) { [void]$Info.ArgumentList.Add($Argument) }
    return [Diagnostics.Process]::Start($Info)
}
try {
    $Controller = StartOwnedProcess @('--run', $Root, $SessionGuid)
    try {
        if (-not $Controller.WaitForExit(960000)) {
            $ControllerTimedOut = $true
            $Controller.Kill($true)
            [void]$Controller.WaitForExit(10000)
        } else { $ControllerExit = $Controller.ExitCode }
    } finally { $Controller.Dispose() }
} catch {
    Write-Output ('[Qualification:SchedulerTrace] controller-error=' + $_.Exception.Message)
    $ControllerExit = 125
} finally {
    # This command only queries/stops the fixed session if BOTH GUID and ETL path match.
    try {
        $Cleanup = StartOwnedProcess @('--cleanup', $SessionGuid, $Etl)
        try {
            if ($Cleanup.WaitForExit(30000)) { $CleanupExit = $Cleanup.ExitCode }
            else { $Cleanup.Kill($true); [void]$Cleanup.WaitForExit(10000) }
        } finally { $Cleanup.Dispose() }
    } catch { Write-Output ('[Qualification:SchedulerTrace] cleanup-error=' + $_.Exception.Message) }
    if (Test-Path -LiteralPath $Output -PathType Container) {
        foreach ($Name in @('child-result.json', 'metadata.json')) {
            try {
                $MetadataPath = Join-Path $Output $Name
                if (Test-Path -LiteralPath $MetadataPath -PathType Leaf) {
                    $Metadata = Get-Content -LiteralPath $MetadataPath -Raw | ConvertFrom-Json
                    if ($Metadata.ChildResumed -eq $true) { $ChildExit = $Metadata.ChildExitCode; break }
                }
            } catch { Write-Output ('[Qualification:SchedulerTrace] result-read-error=' + $_.Exception.Message) }
        }
        try {
        & python (Join-Path $Root 'tools/ci/SchedulerTraceValidate.py') --root $Output
        $CoverageExit = $LASTEXITCODE
        $Files = @()
        foreach ($File in Get-ChildItem -LiteralPath $Output -File) {
            $Files += [ordered]@{ Name=$File.Name; Bytes=$File.Length; Sha256=(Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
        }
        $Receipt = [ordered]@{
            Format='GargantuanSchedulerTraceWrapper'; Version=1; StartedUtc=$Started
            CompletedUtc=[DateTime]::UtcNow.ToString('o'); SessionGuid=$SessionGuid
            HelperSha256=$HelperHash; WorkloadSha256=$WorkloadHash
            WorkloadArguments=@('--reliable-workload'); ControllerExitCode=$ControllerExit
            ControllerTimedOut=$ControllerTimedOut; CleanupExitCode=$CleanupExit; ChildExitCode=$ChildExit
            ControllerDeadlineMs=960000; CleanupDeadlineMs=30000
            CoverageExitCode=$CoverageExit
            Files=$Files; CausalVerdict='NOT_CLAIMED'
        }
        $Text = $Receipt | ConvertTo-Json -Depth 8
        $ReceiptPath = Join-Path $Output 'wrapper.json'
        $Stream = [IO.File]::Open($ReceiptPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
        try { $Bytes = [Text.Encoding]::UTF8.GetBytes($Text); $Stream.Write($Bytes, 0, $Bytes.Length) } finally { $Stream.Dispose() }
        foreach ($Name in @('workload.stdout.txt', 'workload.stderr.txt')) {
            $Path = Join-Path $Output $Name
            if (Test-Path -LiteralPath $Path -PathType Leaf) { Get-Content -LiteralPath $Path }
        }
        } catch {
            $CoverageExit = 125
            Write-Output ('[Qualification:SchedulerTrace] artifact-error=' + $_.Exception.Message)
        }
    }
}
if ($CoverageExit -ne 0 -and $ControllerExit -eq 0) { $ControllerExit = 125 }
exit (ResolveExit $ChildExit $ControllerExit $ControllerTimedOut $CleanupExit)
