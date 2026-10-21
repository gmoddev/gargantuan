#requires -Version 7.0
. (Join-Path $PSScriptRoot 'PhysicalFarmPipeDrain.ps1')
$ErrorActionPreference = 'Stop'
$TempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$Root = [IO.Path]::GetFullPath((Join-Path $TempRoot ('GargantuanFarmPipeDrain-' + [Guid]::NewGuid().ToString('N'))))
if (-not $Root.StartsWith($TempRoot, [StringComparison]::OrdinalIgnoreCase)) {
	throw 'pipe-drain test root escaped the temporary directory'
}
[void](New-Item -ItemType Directory -Path $Root)
$Owners = [Collections.Generic.List[object]]::new()
try {
	function Start-Probe {
		param([string]$Label, [string]$Command)
		$Info = [Diagnostics.ProcessStartInfo]::new()
		$Info.FileName = $env:ComSpec
		$Info.UseShellExecute = $false
		$Info.CreateNoWindow = $true
		$Info.RedirectStandardOutput = $true
		$Info.RedirectStandardError = $true
		foreach ($Argument in @('/d', '/c', $Command)) { [void]$Info.ArgumentList.Add($Argument) }
		$Process = [Diagnostics.Process]::new()
		$Process.StartInfo = $Info
		$OutPath = Join-Path $Root "$Label.stdout.log"
		$ErrPath = Join-Path $Root "$Label.stderr.log"
		$Out = [IO.File]::Open($OutPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
		$Err = [IO.File]::Open($ErrPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
		try {
			if (-not $Process.Start()) { throw "probe $Label did not start" }
			$Owner = [pscustomobject]@{
				Label = $Label; Process = $Process; Out = $Out; Err = $Err
				OutPath = $OutPath; ErrPath = $ErrPath
				OutTask = [PhysicalFarmPipeDrain]::Start($Process.StandardOutput.BaseStream, $Out)
				ErrTask = [PhysicalFarmPipeDrain]::Start($Process.StandardError.BaseStream, $Err)
			}
			$Owners.Add($Owner)
			return $Owner
		} catch {
			if (-not $Process.HasExited) { $Process.Kill($true) }
			$Out.Dispose(); $Err.Dispose(); $Process.Dispose()
			throw
		}
	}

	$Clock = [Diagnostics.Stopwatch]::StartNew()
	$Server = Start-Probe 'server' 'ping -n 3 127.0.0.1 >nul & for /L %i in (1,1,2500) do @echo xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx & echo FARM-ERR 1>&2'
	for ($Slot = 0; $Slot -lt 32; $Slot++) {
		[void](Start-Probe ('idle-{0:D2}' -f $Slot) 'ping -n 6 127.0.0.1 >nul & echo IDLE-ERR 1>&2')
	}
	$Last = $Clock.ElapsedMilliseconds
	$MaximumGap = 0L
	for ($Sample = 0; $Sample -lt 20; $Sample++) {
		Start-Sleep -Milliseconds 100
		$Now = $Clock.ElapsedMilliseconds
		$MaximumGap = [Math]::Max($MaximumGap, $Now - $Last)
		$Last = $Now
	}
	if ($MaximumGap -gt 2000) { throw "pipe-drain sampling stalled for $MaximumGap ms" }
	if ([Threading.ThreadPool]::PendingWorkItemCount -gt 8) {
		throw 'pipe drains queued on the shared ThreadPool'
	}
	if (-not $Server.Process.WaitForExit(15000)) { throw 'output probe did not exit' }
	if (-not $Server.OutTask.Wait(5000) -or -not $Server.ErrTask.Wait(5000)) {
		throw 'output probe drains did not complete'
	}
	$Server.Out.Dispose()
	$Server.Err.Dispose()
	$Output = [IO.File]::ReadAllLines($Server.OutPath)
	$Errors = [IO.File]::ReadAllLines($Server.ErrPath)
	if ($Output.Count -ne 2500 -or $Output[0].Length -ne 79 -or
		$Errors.Count -ne 2500 -or $Errors[0].TrimEnd() -cne 'FARM-ERR') {
		throw "pipe drains lost, truncated, or misrouted output: lines=$($Output.Count) first_length=$($Output[0].Length) stderr_lines=$($Errors.Count) first_error=[$($Errors[0])]"
	}
	Write-Output "Physical farm pipe-drain test PASS: 33 children, 66 streams, 2500 lines, max gap $MaximumGap ms"
} finally {
	foreach ($Owner in $Owners) {
		try {
			if (-not $Owner.Process.HasExited) { $Owner.Process.Kill($true) }
			if (-not $Owner.Process.WaitForExit(5000) -or
				-not $Owner.OutTask.Wait(5000) -or -not $Owner.ErrTask.Wait(5000)) {
				throw "probe $($Owner.Label) did not drain or exit"
			}
		} finally {
			$Owner.Out.Dispose(); $Owner.Err.Dispose(); $Owner.Process.Dispose()
		}
	}
	if (-not $Root.StartsWith($TempRoot, [StringComparison]::OrdinalIgnoreCase)) {
		throw 'pipe-drain test cleanup escaped the temporary directory'
	}
	Remove-Item -LiteralPath $Root -Recurse -Force
}
