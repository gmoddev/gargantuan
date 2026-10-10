#requires -Version 7.0
# Failure-only cleanup must allow the server to seal diagnostic evidence before
# the bounded force-kill fallback. This uses two harmless local mock processes.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PhysicalFarmPipeDrain.ps1')
$Runner = Join-Path $PSScriptRoot 'PhysicalGameSessionFarm.ps1'
$Tokens = $null
$Errors = $null
$Ast = [Management.Automation.Language.Parser]::ParseFile($Runner, [ref]$Tokens, [ref]$Errors)
if ($Errors.Count) { throw "farm runner syntax failed: $($Errors[0].Message)" }
$Needed = @('Start-LoggedProcess', 'Stop-RunProcess', 'Stop-FarmProcesses',
	'Test-SealedFarmPublicationTrace')
foreach ($Function in $Ast.FindAll({ param($Node)
	$Node -is [Management.Automation.Language.FunctionDefinitionAst]
}, $true)) {
	if ($Function.Name -in $Needed) { . ([scriptblock]::Create($Function.Extent.Text)) }
}
$RunDirectory = Join-Path ([IO.Path]::GetTempPath()) ('physical-farm-cleanup-' + [Guid]::NewGuid().ToString('N'))
$RootDirectory = $RunDirectory
[void][IO.Directory]::CreateDirectory($RunDirectory)
$AllProcesses = [Collections.Generic.List[object]]::new()
$CleanupErrors = [Collections.Generic.List[string]]::new()
$ClientScript = Join-Path $RunDirectory 'mock-client.ps1'
$ServerScript = Join-Path $RunDirectory 'mock-server.ps1'
$Trace = Join-Path $RunDirectory 'publication-service.bin'
[IO.File]::WriteAllText($ClientScript, 'while ($true) { Start-Sleep -Milliseconds 50 }')
[IO.File]::WriteAllText($ServerScript, @'
param([string]$EventName, [string]$TracePath, [string]$RunId)
$Event = [Threading.EventWaitHandle]::OpenExisting($EventName)
if (-not $Event.WaitOne(5000)) { exit 9 }
[IO.File]::WriteAllText($TracePath,
	"format=GargantuanFarmPublicationV1`trun=$RunId`trole=SERVER`tslot=-1`tnonce=0`tcount=0`tdropped=0`tdecode_failures=0`n",
	[Text.UTF8Encoding]::new($false))
$Event.Dispose()
'@)
$PowerShell = Join-Path $PSHOME 'pwsh.exe'
try {
	$ScaleWorkload = $true
	$RunId = 'mock-run'
	$CreatedNew = $false
	$EventName = 'Local\GargantuanFarmStop-' + [Guid]::NewGuid().ToString('N')
	$FarmStopEvent = [Threading.EventWaitHandle]::new($false,
		[Threading.EventResetMode]::ManualReset, $EventName, [ref]$CreatedNew)
	if (-not $CreatedNew) { throw 'mock diagnostic event was not new' }
	$Client = Start-LoggedProcess -Executable $PowerShell -WorkingDirectory $RunDirectory `
		-Arguments @('-NoProfile', '-File', $ClientScript) -Label 'client00'
	$Server = Start-LoggedProcess -Executable $PowerShell -WorkingDirectory $RunDirectory `
		-Arguments @('-NoProfile', '-File', $ServerScript, $EventName, $Trace, $RunId) -Label 'server'
	$Failure = 'client00 exited 15'
	$Result = [ordered]@{ Status = 'FAIL'; Reason = $Failure }
	Stop-FarmProcesses -Failed $true
	if ($Result.Status -cne 'FAIL' -or $Result.Reason -cne $Failure) {
		throw 'diagnostic grace changed the original failure verdict'
	}
	if ($CleanupErrors.Count) { throw "cleanup failed: $($CleanupErrors -join '; ')" }
	if (-not (Test-SealedFarmPublicationTrace -Path $Trace -ExpectedRunId $RunId)) {
		throw 'server was force-cleaned before sealing its diagnostic trace'
	}
	[IO.File]::AppendAllText($Trace, 'incomplete')
	if (Test-SealedFarmPublicationTrace -Path $Trace -ExpectedRunId $RunId) {
		throw 'incomplete diagnostic trace was accepted as sealed'
	}
	# A server that ignores the stop signal must still be force-cleaned on time,
	# with an explicit diagnostic-seal failure and the original FAIL preserved.
	$FarmStopEvent.Dispose()
	$AllProcesses.Clear()
	$CleanupErrors.Clear()
	$RunDirectory = Join-Path $RunDirectory 'hung'
	[void][IO.Directory]::CreateDirectory($RunDirectory)
	$RunId = 'hung-run'
	$EventName = 'Local\GargantuanFarmStop-' + [Guid]::NewGuid().ToString('N')
	$FarmStopEvent = [Threading.EventWaitHandle]::new($false,
		[Threading.EventResetMode]::ManualReset, $EventName)
	$HungScript = Join-Path $RunDirectory 'hung-server.ps1'
	[IO.File]::WriteAllText($HungScript, 'while ($true) { Start-Sleep -Milliseconds 50 }')
	$Server = Start-LoggedProcess -Executable $PowerShell -WorkingDirectory $RunDirectory `
		-Arguments @('-NoProfile', '-File', $HungScript) -Label 'server'
	$Clock = [Diagnostics.Stopwatch]::StartNew()
	Stop-FarmProcesses -Failed $true
	if ($Clock.ElapsedMilliseconds -gt 7000 -or $Result.Status -cne 'FAIL' -or
		-not @($CleanupErrors | Where-Object { $_ -match 'did not seal within 3000 ms' }).Count -or
		-not @($CleanupErrors | Where-Object { $_ -match 'trace was not sealed or is incomplete' }).Count) {
		throw 'bounded force cleanup did not report the diagnostic-seal failure'
	}
	Write-Output 'PhysicalGameSessionFarmCleanupTests PASS'
} finally {
	if ($FarmStopEvent) { $FarmStopEvent.Dispose() }
	foreach ($Owner in $AllProcesses) {
		try { if (-not $Owner.Process.HasExited) { $Owner.Process.Kill($true) } } catch {}
	}
	$ResolvedTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
	$ResolvedRoot = [IO.Path]::GetFullPath($RootDirectory).TrimEnd('\', '/')
	if ([IO.Path]::GetDirectoryName($ResolvedRoot) -ceq $ResolvedTemp) {
		Remove-Item -LiteralPath $ResolvedRoot -Recurse -Force -ErrorAction SilentlyContinue
	}
}
