# Run only fixed diagnostic cases, once each. No trace or workload retries.
[CmdletBinding(DefaultParameterSetName='Case')]
param(
    [Parameter(ParameterSetName='Case')][ValidateSet('Full','AckStats','Aggregate32','Aggregate32Structural','PooledAggregate32Structural','PooledFull','AckCycleFunded')][string]$Case='Full',
    [Parameter(ParameterSetName='Pair')][switch]$Pair,
    [Parameter(ParameterSetName='SelfTest')][switch]$SelfTest
)
$ErrorActionPreference = 'Stop'
function GetFixedCase([ValidateSet('Full','AckStats','Aggregate32','Aggregate32Structural','PooledAggregate32Structural','PooledFull','AckCycleFunded')][string]$Case) {
    switch ($Case) {
        'Full' { return [pscustomobject]@{ Binary='build-ci/gargantuan_game_session_real_transport_tests.exe'; Output='build-ci/scheduler-trace'; Argument='--reliable-workload'; Command='--run' } }
        'AckStats' { return [pscustomobject]@{ Binary='build-ci/gargantuan_real_transport_tests.exe'; Output='build-ci/scheduler-trace-ack-stats'; Argument='--ack-stats-boundary'; Command='--run-ack-stats' } }
        'AckCycleFunded' { return [pscustomobject]@{ Binary='build-ci/gargantuan_real_transport_tests.exe'; Output='build-ci/scheduler-trace-ack-cycle-funded'; Argument=@('--ack-cycle-funded','1348'); Command='--run-ack-cycle-funded' } }
        'Aggregate32' { return [pscustomobject]@{ Binary='build-ci/gargantuan_game_session_real_transport_tests.exe'; Output='build-ci/scheduler-trace-aggregate32'; Argument='--reliable-workload-32'; Command='--run-aggregate32' } }
        'Aggregate32Structural' { return [pscustomobject]@{ Binary='build-ci/gargantuan_game_session_real_transport_tests.exe'; Output='build-ci/scheduler-trace-aggregate32-structural'; Argument='--reliable-workload-32-structural'; Command='--run-aggregate32-structural' } }
        'PooledAggregate32Structural' { return [pscustomobject]@{ Binary='build-ci/gargantuan_game_session_real_transport_tests.exe'; Output='build-ci/scheduler-trace-pooled-aggregate32-structural'; Argument=@('--pooled','--reliable-workload-32-structural'); Command='--run-pooled-aggregate32-structural' } }
        'PooledFull' { return [pscustomobject]@{ Binary='build-ci/gargantuan_game_session_real_transport_tests.exe'; Output='build-ci/scheduler-trace-pooled-full'; Argument=@('--pooled','--reliable-workload'); Command='--run-pooled-full' } }
    }
    throw 'Unsupported fixed workload case'
}
function NormalizeChildExit([object]$ChildExit) {
    # GetExitCodeProcess emits a DWORD; PowerShell exit consumes signed Int32.
    # Normalize only the returned process status. Raw metadata/receipts retain
    # the original unsigned value and its exact failure evidence.
    if ($ChildExit -isnot [int] -and $ChildExit -isnot [long] -and
        $ChildExit -isnot [uint32] -and $ChildExit -isnot [uint64]) { return 125 }
    $Value = [decimal]$ChildExit
    if ($Value -lt -2147483648 -or $Value -gt 4294967295) { return 125 }
    if ($Value -gt 2147483647) { return [int]([long]$Value - 4294967296L) }
    return [int]$Value
}
function ResolveExit([object]$ChildExit, [int]$ControllerExit, [bool]$TimedOut, [int]$CleanupExit) {
    if ($null -ne $ChildExit -and $ChildExit -ne 0) { return NormalizeChildExit $ChildExit }
    if ($TimedOut -or $CleanupExit -ne 0) { return 125 }
    return $ControllerExit
}
function InvokePair([scriptblock]$InvokeCase) {
    $Results = @()
    $First = & $InvokeCase 'Full'
    $Results += $First
    if ($First.CleanupVerified -eq $true) { $Results += (& $InvokeCase 'AckStats') }
    else { $Results += [pscustomobject]@{ Case='AckStats'; State='NOT_STARTED'; ExitCode=$null; ChildExitCode=$null; CleanupVerified=$false } }
    $ExitCode = 0
    foreach ($Result in $Results) {
        if ($null -ne $Result.ChildExitCode -and $Result.ChildExitCode -ne 0) { $ExitCode = NormalizeChildExit $Result.ChildExitCode; break }
        if ($Result.State -ne 'COMPLETED' -or $Result.ExitCode -ne 0) { $ExitCode = 125 }
    }
    return [pscustomobject]@{ Format='GargantuanSchedulerTracePair'; Version=1; Cases=$Results; ExitCode=$ExitCode
        NativeCIQualification=$false; CausalVerdict='NOT_CLAIMED' }
}
if ($SelfTest) {
    $AckCycle = GetFixedCase 'AckCycleFunded'
    if ($AckCycle.Binary -ne 'build-ci/gargantuan_real_transport_tests.exe' -or
        $AckCycle.Output -ne 'build-ci/scheduler-trace-ack-cycle-funded' -or
        $AckCycle.Argument.Count -ne 2 -or $AckCycle.Argument[0] -ne '--ack-cycle-funded' -or
        $AckCycle.Argument[1] -ne '1348' -or $AckCycle.Command -ne '--run-ack-cycle-funded') {
        throw 'Fixed ACK cycle case binding test failed'
    }
    $PooledFull = GetFixedCase 'PooledFull'
    if ($PooledFull.Binary -ne 'build-ci/gargantuan_game_session_real_transport_tests.exe' -or
        $PooledFull.Output -ne 'build-ci/scheduler-trace-pooled-full' -or
        $PooledFull.Argument.Count -ne 2 -or $PooledFull.Argument[0] -ne '--pooled' -or
        $PooledFull.Argument[1] -ne '--reliable-workload' -or
        $PooledFull.Command -ne '--run-pooled-full') { throw 'Fixed pooled Full case binding test failed' }
    $Pooled = GetFixedCase 'PooledAggregate32Structural'
    if ($Pooled.Binary -ne 'build-ci/gargantuan_game_session_real_transport_tests.exe' -or
        $Pooled.Output -ne 'build-ci/scheduler-trace-pooled-aggregate32-structural' -or
        $Pooled.Argument.Count -ne 2 -or $Pooled.Argument[0] -ne '--pooled' -or
        $Pooled.Argument[1] -ne '--reliable-workload-32-structural' -or
        $Pooled.Command -ne '--run-pooled-aggregate32-structural') { throw 'Fixed pooled case binding test failed' }
    $Selection = GetFixedCase 'Aggregate32Structural'
    if ($Selection.Binary -ne 'build-ci/gargantuan_game_session_real_transport_tests.exe' -or
        $Selection.Output -ne 'build-ci/scheduler-trace-aggregate32-structural' -or
        $Selection.Argument -ne '--reliable-workload-32-structural' -or
        $Selection.Command -ne '--run-aggregate32-structural' -or
        (GetFixedCase 'Aggregate32').Binary -ne 'build-ci/gargantuan_game_session_real_transport_tests.exe' -or
        (GetFixedCase 'Aggregate32').Output -ne 'build-ci/scheduler-trace-aggregate32' -or
        (GetFixedCase 'Aggregate32').Argument -ne '--reliable-workload-32' -or
        (GetFixedCase 'Aggregate32').Command -ne '--run-aggregate32' -or
        (GetFixedCase 'Full').Argument -ne '--reliable-workload' -or
        (GetFixedCase 'AckStats').Argument -ne '--ack-stats-boundary') { throw 'Fixed case binding test failed' }
    foreach ($ExitCase in @(
        @(17, 125, $true, 125, 17), @(0, 0, $false, 0, 0), @(0, 125, $false, 0, 125),
        @($null, 125, $false, 0, 125), @(0, 0, $true, 0, 125), @(0, 0, $false, 125, 125),
        @(3221226505L, 125, $true, 125, -1073740791), @(4294967295L, 125, $true, 125, -1),
        @(-1073740791, 125, $true, 125, -1073740791), @(-2147483648, 125, $true, 125, -2147483648)
    )) {
        if ((ResolveExit $ExitCase[0] $ExitCase[1] $ExitCase[2] $ExitCase[3]) -ne $ExitCase[4]) { throw 'Exit precedence test failed' }
    }
    foreach ($ExitCase in @(@(0, 0), @(17, 17), @(2147483647, 2147483647), @(2147483648L, -2147483648),
                            @(3221226505L, -1073740791), @(4294967295L, -1), @(-1, -1),
                            @(-2147483648, -2147483648), @(4294967296L, 125), @(-2147483649L, 125),
                            @('3221226505', 125), @(17.5, 125), @($null, 125))) {
        if ((NormalizeChildExit $ExitCase[0]) -ne $ExitCase[1]) { throw 'DWORD exit normalization test failed' }
    }
    foreach ($FirstExit in @(0, 17)) {
        foreach ($Clean in @($false, $true)) {
            $Calls = [Collections.Generic.List[string]]::new()
            $PairResult = InvokePair {
                param($Name)
                $Calls.Add($Name)
                [pscustomobject]@{ Case=$Name; State='COMPLETED'; ExitCode=$FirstExit; ChildExitCode=$FirstExit; CleanupVerified=$Clean }
            }
            if ($Calls.Count -ne $(if ($Clean) { 2 } else { 1 }) -or $Calls[0] -ne 'Full' -or
                ($Clean -and $Calls[1] -ne 'AckStats') -or ($FirstExit -ne 0 -and $PairResult.ExitCode -ne $FirstExit)) {
                throw 'Pair ordering/cleanup/child-failure retention test failed'
            }
        }
    }
    $PairResult = InvokePair {
        param($Name)
        $Code = $(if ($Name -eq 'Full') { 0 } else { 23 })
        [pscustomobject]@{ Case=$Name; State='COMPLETED'; ExitCode=$Code; ChildExitCode=$Code; CleanupVerified=$true }
    }
    if ($PairResult.ExitCode -ne 23 -or $PairResult.Cases.Count -ne 2 -or
        $PairResult.Cases[0].ChildExitCode -ne 0 -or $PairResult.Cases[1].ChildExitCode -ne 23) {
        throw 'Pair second-child failure retention test failed'
    }
    $PairResult = InvokePair {
        param($Name)
        [pscustomobject]@{ Case=$Name; State='COMPLETED'; ExitCode=-1073740791; ChildExitCode=3221226505L; CleanupVerified=$true }
    }
    if ($PairResult.ExitCode -ne -1073740791 -or $PairResult.Cases.Count -ne 2 -or
        $PairResult.Cases[0].ChildExitCode -ne 3221226505L -or $PairResult.Cases[1].ChildExitCode -ne 3221226505L) {
        throw 'Pair unsigned native status retention test failed'
    }
    Write-Output '[Qualification:SchedulerTrace] dword-exit-normalization=PASS raw-status-preserved'
    Write-Output '[Qualification:SchedulerTrace] wrapper-self-test=PASS no-session-or-child-created'
    exit 0
}
function InvokeFixedCase([ValidateSet('Full','AckStats','Aggregate32','Aggregate32Structural','PooledAggregate32Structural','PooledFull','AckCycleFunded')][string]$Case) {
$Root = (Get-Location).ProviderPath
$Helper = Join-Path $Root 'build-ci/scheduler-trace.exe'
$Selection = GetFixedCase $Case
$Workload = Join-Path $Root $Selection.Binary
$Output = Join-Path $Root $Selection.Output
$Arguments = $Selection.Argument
$RunCommand = $Selection.Command
$Etl = Join-Path $Output 'scheduler.etl'
$SessionGuid = [Guid]::NewGuid().ToString('D')
$ControllerExit = 125
$ControllerTimedOut = $false
$CleanupExit = 125
$CoverageExit = 125
$ChildExit = $null
$ChildScheduling = $null
$ChildTreeReaped = $false
$ZeroLaunch = $false
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
    if ($Case -eq 'AckStats') { $Info.Environment['GARGANTUAN_SCHEDULER_ACK_STATS'] = '1' }
    if ($Case -eq 'AckCycleFunded') { $Info.Environment['GARGANTUAN_SCHEDULER_ACK_CYCLE'] = '1' }
    foreach ($Argument in $Arguments) { [void]$Info.ArgumentList.Add($Argument) }
    return [Diagnostics.Process]::Start($Info)
}
try {
    $Controller = StartOwnedProcess @($RunCommand, $Root, $SessionGuid)
    try {
        if (-not $Controller.WaitForExit(960000)) {
            $ControllerTimedOut = $true
            $Controller.Kill($true)
            [void]$Controller.WaitForExit(10000)
        } else { $ControllerExit = $Controller.ExitCode }
    } finally { $Controller.Dispose() }
} catch {
    Write-Host ('[Qualification:SchedulerTrace] controller-error=' + $_.Exception.Message)
    $ControllerExit = 125
} finally {
    # This command only queries/stops the fixed session if BOTH GUID and ETL path match.
    try {
        $Cleanup = StartOwnedProcess @('--cleanup', $SessionGuid, $Etl)
        try {
            if ($Cleanup.WaitForExit(30000)) { $CleanupExit = $Cleanup.ExitCode }
            else { $Cleanup.Kill($true); [void]$Cleanup.WaitForExit(10000) }
        } finally { $Cleanup.Dispose() }
    } catch { Write-Host ('[Qualification:SchedulerTrace] cleanup-error=' + $_.Exception.Message) }
    if (Test-Path -LiteralPath $Output -PathType Container) {
        foreach ($Name in @('child-result.json', 'metadata.json')) {
            try {
                $MetadataPath = Join-Path $Output $Name
                if (Test-Path -LiteralPath $MetadataPath -PathType Leaf) {
                    $Metadata = Get-Content -LiteralPath $MetadataPath -Raw | ConvertFrom-Json
                    if ($Metadata.PSObject.Properties.Name -contains 'RequestedChildCreationFlags') {
                        $ChildScheduling = [ordered]@{}
                        foreach ($Field in @('RequestedChildCreationFlags', 'ControllerPriorityClass', 'ChildPriorityClass',
                                             'ChildThreadPriority', 'PriorityQueryError', 'PriorityVerifiedBeforeResume')) {
                            $ChildScheduling[$Field] = $Metadata.$Field
                        }
                    }
                    if ($Metadata.ChildTreeReaped -is [bool] -and $Metadata.ChildTreeReaped) { $ChildTreeReaped = $true }
                    if ($Metadata.ChildLaunchAttempts -is [long] -and $Metadata.ChildLaunchAttempts -eq 0) { $ZeroLaunch = $true }
                    if ($Metadata.ChildResumed -eq $true) { $ChildExit = $Metadata.ChildExitCode; break }
                }
            } catch { Write-Host ('[Qualification:SchedulerTrace] result-read-error=' + $_.Exception.Message) }
        }
        try {
        & python (Join-Path $Root 'tools/ci/SchedulerTraceValidate.py') --root $Output --case $Case
        $CoverageExit = $LASTEXITCODE
        $Files = @()
        foreach ($File in Get-ChildItem -LiteralPath $Output -File) {
            $Files += [ordered]@{ Name=$File.Name; Bytes=$File.Length; Sha256=(Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
        }
        $Receipt = [ordered]@{
            Format='GargantuanSchedulerTraceWrapper'; Version=1; StartedUtc=$Started
            CompletedUtc=[DateTime]::UtcNow.ToString('o'); SessionGuid=$SessionGuid
            HelperSha256=$HelperHash; WorkloadSha256=$WorkloadHash
            WorkloadArguments=@($Arguments); WorkloadCase=$Case; ControllerExitCode=$ControllerExit
            ControllerTimedOut=$ControllerTimedOut; CleanupExitCode=$CleanupExit; ChildExitCode=$ChildExit
            ChildScheduling=$ChildScheduling
            CleanupVerified=($CleanupExit -eq 0 -and ($ChildTreeReaped -or $ZeroLaunch))
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
            if (Test-Path -LiteralPath $Path -PathType Leaf) { Get-Content -LiteralPath $Path | ForEach-Object { Write-Host $_ } }
        }
        } catch {
            $CoverageExit = 125
            Write-Host ('[Qualification:SchedulerTrace] artifact-error=' + $_.Exception.Message)
        }
    }
}
if ($CoverageExit -ne 0 -and $ControllerExit -eq 0) { $ControllerExit = 125 }
return [pscustomobject]@{ Case=$Case; State='COMPLETED'; ChildExitCode=$ChildExit
    ExitCode=(ResolveExit $ChildExit $ControllerExit $ControllerTimedOut $CleanupExit)
    CleanupVerified=($CleanupExit -eq 0 -and ($ChildTreeReaped -or $ZeroLaunch)); WrapperPath=(Join-Path $Output 'wrapper.json') }
}
if ($Pair) {
    $Root = (Get-Location).ProviderPath
    $PairPath = Join-Path $Root 'build-ci/scheduler-trace-pair.json'
    foreach ($Path in @($PairPath, (Join-Path $Root 'build-ci/scheduler-trace'), (Join-Path $Root 'build-ci/scheduler-trace-ack-stats'))) {
        if (Test-Path -LiteralPath $Path) { throw 'Pair output exists; refusing replay' }
    }
    $Result = InvokePair {
        param($Name)
        try { InvokeFixedCase $Name }
        catch {
            Write-Host ('[Qualification:SchedulerTrace] fixed-case-error=' + $_.Exception.Message)
            [pscustomobject]@{ Case=$Name; State='CONTROLLER_ERROR'; ExitCode=125; ChildExitCode=$null; CleanupVerified=$false }
        }
    }
    foreach ($Entry in $Result.Cases) {
        if ($Entry.PSObject.Properties.Name -contains 'WrapperPath' -and (Test-Path -LiteralPath $Entry.WrapperPath)) {
            $Entry | Add-Member WrapperSha256 (Get-FileHash -LiteralPath $Entry.WrapperPath -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $Stream = [IO.File]::Open($PairPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
    try { $Bytes = [Text.Encoding]::UTF8.GetBytes(($Result | ConvertTo-Json -Depth 8)); $Stream.Write($Bytes, 0, $Bytes.Length) }
    finally { $Stream.Dispose() }
    exit $Result.ExitCode
}
$Result = InvokeFixedCase $Case
exit $Result.ExitCode
