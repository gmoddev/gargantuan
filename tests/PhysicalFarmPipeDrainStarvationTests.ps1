#requires -Version 7.0
# Run in a fresh owned PowerShell process. Actual Windows anonymous pipes
# distinguish the unchanged five-second stop bound under a deliberately busy
# shared worker pool; this is a causal fixture, not a measured physical farm.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PhysicalFarmPipeDrain.ps1')
Add-Type @'
using System;
using System.Threading;
using System.Threading.Tasks;
public static class EndpointPipePoolBlocker {
    public static readonly ManualResetEventSlim Entered = new ManualResetEventSlim();
    public static readonly ManualResetEventSlim Released = new ManualResetEventSlim();
    public static Task Start() { return Task.Run(() => { Entered.Set(); Released.Wait(); }); }
}
'@
$MinimumWorker = $MinimumIo = $MaximumWorker = $MaximumIo = 0
[Threading.ThreadPool]::GetMinThreads([ref]$MinimumWorker, [ref]$MinimumIo)
[Threading.ThreadPool]::GetMaxThreads([ref]$MaximumWorker, [ref]$MaximumIo)
$Owners = [Collections.Generic.List[object]]::new()
$Blocker = $null
$TempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
$Root = Join-Path $TempRoot ('GargantuanPipeStarvation-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($Root)
try {
    foreach ($Label in @('old', 'dedicated')) {
        $Info = [Diagnostics.ProcessStartInfo]::new()
        $Info.FileName = $env:ComSpec
        $Info.UseShellExecute = $false
        $Info.CreateNoWindow = $true
        $Info.RedirectStandardOutput = $true
        $Info.RedirectStandardError = $true
        $Info.RedirectStandardInput = $true
        $ReadyPath = Join-Path $Root "$Label.ready"
        # cmd's own stdin read blocks without spawning a descendant. Readiness
        # is published only after both exact pipe markers have been written.
        $Command = 'echo PIPE-OUT & echo PIPE-ERR 1>&2 & echo READY >"' + $ReadyPath + '" & set /p "Unused="'
        # cmd parses its command string rather than CRT argv quoting. /s
        # removes only the enclosing command quotes and preserves path quotes.
        $Info.Arguments = '/d /s /c "' + $Command + '"'
        $Process = [Diagnostics.Process]::new()
        $Process.StartInfo = $Info
        if (-not $Process.Start()) { throw 'owned probe did not start' }
        $Owners.Add([pscustomobject]@{ Label = $Label; Process = $Process; Out = [IO.MemoryStream]::new(); Err = [IO.MemoryStream]::new(); OutTask = $null; ErrTask = $null })
        $ReadyClock = [Diagnostics.Stopwatch]::StartNew()
        while (-not [IO.File]::Exists($ReadyPath)) {
            if ($Process.HasExited -or $ReadyClock.ElapsedMilliseconds -ge 5000) { throw 'owned probe did not publish its pipe-write readiness' }
            [Threading.Thread]::Sleep(10)
        }
    }
    if (-not [Threading.ThreadPool]::SetMinThreads(1, $MinimumIo) -or
        -not [Threading.ThreadPool]::SetMaxThreads(1, $MaximumIo)) { throw 'isolated worker-pool bound unavailable' }
    $Blocker = [EndpointPipePoolBlocker]::Start()
    if (-not [EndpointPipePoolBlocker]::Entered.Wait(5000)) { throw 'worker-pool occupation was not established' }
    $Owners[0].OutTask = $Owners[0].Process.StandardOutput.BaseStream.CopyToAsync($Owners[0].Out)
    $Owners[0].ErrTask = $Owners[0].Process.StandardError.BaseStream.CopyToAsync($Owners[0].Err)
    $Owners[1].OutTask = [PhysicalFarmPipeDrain]::Start($Owners[1].Process.StandardOutput.BaseStream, $Owners[1].Out)
    $Owners[1].ErrTask = [PhysicalFarmPipeDrain]::Start($Owners[1].Process.StandardError.BaseStream, $Owners[1].Err)
    foreach ($Owner in $Owners) {
        $Owner.Process.Kill($true)
        if (-not $Owner.Process.WaitForExit(5000)) { throw 'owned probe did not exit' }
    }
    $OldCompleted = $Owners[0].OutTask.Wait(5000) -and $Owners[0].ErrTask.IsCompleted
    $NewCompleted = $Owners[1].OutTask.Wait(5000) -and $Owners[1].ErrTask.Wait(5000)
    if ($OldCompleted -or -not $NewCompleted) { throw "counterexample did not discriminate: old=$OldCompleted dedicated=$NewCompleted" }
    [EndpointPipePoolBlocker]::Released.Set()
    if (-not $Blocker.Wait(5000) -or -not $Owners[0].OutTask.Wait(5000) -or -not $Owners[0].ErrTask.Wait(5000)) { throw 'released legacy drain did not complete' }
    foreach ($Owner in $Owners) {
        if ([Text.Encoding]::UTF8.GetString($Owner.Out.ToArray()).Trim() -cne 'PIPE-OUT' -or
            [Text.Encoding]::UTF8.GetString($Owner.Err.ToArray()).Trim() -cne 'PIPE-ERR') { throw 'owned pipe bytes lost or misrouted' }
    }
    Write-Output '[Qualification:EndpointPipeStarvation] PASS OldCopyWait5000=False DedicatedWait5000=True BothChildrenExited=True BothStreamsExact=True PoolPremise=OneOccupiedWorker Scope=IsolatedProcess'
} finally {
    [EndpointPipePoolBlocker]::Released.Set()
    [void][Threading.ThreadPool]::SetMaxThreads($MaximumWorker, $MaximumIo)
    [void][Threading.ThreadPool]::SetMinThreads($MinimumWorker, $MinimumIo)
    if ($null -ne $Blocker) { [void]$Blocker.Wait(5000) }
    foreach ($Owner in $Owners) {
        if (-not $Owner.Process.HasExited) { $Owner.Process.Kill($true) }
        [void]$Owner.Process.WaitForExit(5000)
        if ($null -ne $Owner.OutTask) { [void]$Owner.OutTask.Wait(5000) }
        if ($null -ne $Owner.ErrTask) { [void]$Owner.ErrTask.Wait(5000) }
        $Owner.Out.Dispose(); $Owner.Err.Dispose(); $Owner.Process.Dispose()
    }
    $Resolved = [IO.Path]::GetFullPath($Root)
    if (-not $Resolved.StartsWith($TempRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($Resolved) -cnotmatch '^GargantuanPipeStarvation-[a-f0-9]{32}$') { throw 'pipe-starvation cleanup escaped its owned temporary root' }
    Remove-Item -LiteralPath $Resolved -Recurse -Force
}
