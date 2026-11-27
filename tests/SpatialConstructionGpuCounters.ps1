# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
param(
    [Parameter(Mandatory)][string]$Benchmark,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][string]$Source,
    [ValidateRange(1,20)][int]$Trials = 3
)
$ErrorActionPreference = 'Stop'
$Executable = (Resolve-Path -LiteralPath $Benchmark).ProviderPath
$Output = (New-Item -ItemType Directory -Force -Path $OutputDirectory).FullName
$Stages = [Collections.Generic.List[object]]::new()
$Frames = [Collections.Generic.List[object]]::new()

function Read-ProcessGpuCounters([int]$GpuPid) {
    $Stem = 'pid_' + $GpuPid + '_*'
    $Paths = @("\GPU Engine($Stem)\Running Time", "\GPU Process Memory($Stem)\Dedicated Usage",
               "\GPU Process Memory($Stem)\Shared Usage")
    $Samples = @(Get-Counter -Counter $Paths -ErrorAction Stop | Select-Object -ExpandProperty CounterSamples)
    $Run = @($Samples | Where-Object { $_.Path.EndsWith('\running time',[StringComparison]::OrdinalIgnoreCase) })
    $Dedicated = @($Samples | Where-Object { $_.Path.EndsWith('\dedicated usage',[StringComparison]::OrdinalIgnoreCase) })
    $Shared = @($Samples | Where-Object { $_.Path.EndsWith('\shared usage',[StringComparison]::OrdinalIgnoreCase) })
    if (!$Run.Count -or !$Dedicated.Count -or !$Shared.Count -or @($Samples | Where-Object Status -GT 1).Count) {
        throw 'Missing or invalid WDDM process engine or memory counter'
    }
    $Bases = @($Run | Select-Object -ExpandProperty TimeBase -Unique)
    if ($Bases.Count -ne 1) { throw 'GPU engine counters report different time bases' }
    $Graphics = @($Run | Where-Object { $_.InstanceName.EndsWith('_engtype_3d',[StringComparison]::OrdinalIgnoreCase) })
    return [ordered]@{
        EngineRunningRaw = [decimal](($Run | Measure-Object RawValue -Sum).Sum)
        GraphicsRunningRaw = [decimal](($Graphics | Measure-Object RawValue -Sum).Sum)
        EngineCounterTimeBase = $Bases[0]
        EngineInstances = $Run.Count
        DedicatedBytes = [decimal](($Dedicated | Measure-Object RawValue -Sum).Sum)
        SharedBytes = [decimal](($Shared | Measure-Object RawValue -Sum).Sum)
    }
}

for ($Trial=1; $Trial -le $Trials; $Trial++) {
    foreach ($Layout in @('checkerboard','solid')) {
        $Representations = if ($Trial % 2) { @('grid','parts') } else { @('parts','grid') }
        foreach ($Rep in $Representations) {
            $Start = [Diagnostics.ProcessStartInfo]::new()
            $Start.FileName = $Executable
            $Start.WorkingDirectory = [IO.Path]::GetDirectoryName($Executable)
            $Start.Arguments = "$Layout $Rep --sample-counters"
            $Start.UseShellExecute = $false
            $Start.CreateNoWindow = $true
            $Start.RedirectStandardInput = $Start.RedirectStandardOutput = $Start.RedirectStandardError = $true
            $Process = [Diagnostics.Process]::Start($Start)
            $Errors = $Process.StandardError.ReadToEndAsync()
            try {
                foreach ($Expected in @('Empty','Cold','Warm','Edit','Retired')) {
                    do {
                        $Read = $Process.StandardOutput.ReadLineAsync()
                        if (!$Read.Wait(30000)) { throw "GPU benchmark stalled before $Expected" }
                        $Line = $Read.Result
                        if ($null -eq $Line) { throw "GPU benchmark ended before $Expected. $($Errors.Result)" }
                    } until ($Line.StartsWith('{'))
                    $Stage = $Line | ConvertFrom-Json
                    if ($Stage.Stage -ne $Expected) { throw "Expected $Expected, received $($Stage.Stage)" }
                    $Record = [ordered]@{ Source=$Source; Trial=$Trial; Layout=$Stage.Layout; Representation=$Stage.Representation;
                        Stage=$Stage.Stage; CoveredPixels=$Stage.CoveredPixels; CoverageHash=$Stage.CoverageHash;
                        DrawCalls=$Stage.DrawCalls; UploadedBytes=$Stage.UploadedBytes; VertexBufferCreates=$Stage.VertexBufferCreates }
                    foreach ($Pair in (Read-ProcessGpuCounters $Process.Id).GetEnumerator()) { $Record[$Pair.Key]=$Pair.Value }
                    $Stages.Add([pscustomobject]$Record)
                    $Index=0
                    foreach ($Frame in $Stage.Frames) {
                        $Index++
                        $Frames.Add([pscustomobject][ordered]@{Source=$Source; Trial=$Trial; Layout=$Stage.Layout;
                            Representation=$Stage.Representation; Stage=$Stage.Stage; Frame=$Index;
                            PublishUs=$Frame.PublishUs; SubmissionUs=$Frame.SubmissionUs;
                            FenceReadbackUs=$Frame.FenceReadbackUs; ExtractUs=$Frame.ExtractUs; CaptureUs=$Frame.CaptureUs})
                    }
                    $Process.StandardInput.WriteLine('continue')
                    $Process.StandardInput.Flush()
                }
                if (!$Process.WaitForExit(15000) -or $Process.ExitCode) { throw "GPU benchmark failed. $($Errors.Result)" }
            } finally {
                if (!$Process.HasExited) { $Process.Kill(); $Process.WaitForExit() }
                $Process.Dispose()
            }
        }
    }
}
$Stages | Export-Csv -LiteralPath (Join-Path $Output 'SpatialConstructionGpuCounters.csv') -NoTypeInformation
$Frames | Export-Csv -LiteralPath (Join-Path $Output 'SpatialConstructionGpuFrames.csv') -NoTypeInformation
Write-Output "[Spatial:GpuCounters] stages=$($Stages.Count) frames=$($Frames.Count)"
