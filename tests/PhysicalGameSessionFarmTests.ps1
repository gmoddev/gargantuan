#requires -Version 7.0
# Focused no-process tests for the farm's typed identity and evidence parser.

$ErrorActionPreference = 'Stop'
$Farm = Join-Path $PSScriptRoot 'PhysicalGameSessionFarm.ps1'
$Tokens = $null
$Errors = $null
$Ast = [Management.Automation.Language.Parser]::ParseFile($Farm, [ref]$Tokens, [ref]$Errors)
if ($Errors.Count -ne 0) { throw "farm syntax failed: $($Errors[0].Message)" }
foreach ($Function in $Ast.FindAll({ param($Node)
	$Node -is [Management.Automation.Language.FunctionDefinitionAst]
}, $true)) {
	. ([scriptblock]::Create($Function.Extent.Text))
}

$RunId = 'farm-parser-test'
$Peers = 1
$Provider = 'Local'
$ScaleWorkload = $false
$Directory = Join-Path ([IO.Path]::GetTempPath()) ('physical-farm-test-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($Directory)
$ServerPath = Join-Path $Directory 'server.log'
$ClientPath = Join-Path $Directory 'client.log'
try {
	[IO.File]::WriteAllLines($ServerPath, @(
		'[Qualification:Server] event=start run=farm-parser-test provider=local expected=1',
		'[Qualification:Server] event=ready run=farm-parser-test nonce=123 connection_slot=5 connection_generation=1 session_epoch=7 player_id=9 monotonic_us=5',
		'[Qualification:Server] event=result run=farm-parser-test provider=local expected=1 ready_high_water=1 unique_ready=1 identity_conflict=0 exit=0'
	))
	[IO.File]::WriteAllLines($ClientPath, @(
		'[Qualification:Client] event=start run_id=farm-parser-test slot=0 nonce=123 steady_ns=1',
		'[Qualification:Client] event=ready run_id=farm-parser-test slot=0 nonce=123 connection_slot=1 connection_generation=1 steady_ns=2',
		'[Qualification:Client] event=result run_id=farm-parser-test slot=0 nonce=123 status=PASS exit_code=0 reason=completed local_player=1 character=1 steady_ns=3'
	))
	$Expected = [Collections.Generic.List[string]]::new()
	$Expected.Add('123')
	$Identity = Assert-Records -Clients @([pscustomobject]@{ OutputPath = $ClientPath }) `
		-Server ([pscustomobject]@{ OutputPath = $ServerPath }) -ExpectedNonces $Expected
	if ($Identity.Ready -ne 1 -or $Identity.UniqueConnections -ne 1 -or $Identity.UniquePlayers -ne 1) {
		throw 'valid typed evidence was rejected'
	}
	$GoodClient = [IO.File]::ReadAllText($ClientPath)
	[IO.File]::WriteAllText($ClientPath, $GoodClient.Replace('local_player=1', 'local_player=0'))
	$Rejected = $false
	try {
		[void](Assert-Records -Clients @([pscustomobject]@{ OutputPath = $ClientPath }) `
			-Server ([pscustomobject]@{ OutputPath = $ServerPath }) -ExpectedNonces $Expected)
	} catch { $Rejected = $true }
	if (-not $Rejected) { throw 'client without a local Player was accepted' }
	$ScaleWorkload = $true
	[IO.File]::WriteAllText($ClientPath, $GoodClient.Replace('reason=completed', 'reason=scale_complete'))
	[void](Assert-Records -Clients @([pscustomobject]@{ OutputPath = $ClientPath }) `
		-Server ([pscustomobject]@{ OutputPath = $ServerPath }) -ExpectedNonces $Expected)
	$ScaleWorkload = $false
	[IO.File]::WriteAllText($ClientPath, $GoodClient)
	# The endpoint-local GNS IDs intentionally differ. The nonce is the join key.
	$BadClient = [IO.File]::ReadAllText($ClientPath).Replace('nonce=123', 'nonce=124')
	[IO.File]::WriteAllText($ClientPath, $BadClient)
	$Rejected = $false
	try {
		[void](Assert-Records -Clients @([pscustomobject]@{ OutputPath = $ClientPath }) `
			-Server ([pscustomobject]@{ OutputPath = $ServerPath }) -ExpectedNonces $Expected)
	} catch { $Rejected = $true }
	if (-not $Rejected) { throw 'mismatched client nonce was accepted' }
	[IO.File]::WriteAllText($ClientPath, $BadClient.Replace('nonce=124', 'nonce=123'))
	$BadServer = [IO.File]::ReadAllText($ServerPath).Replace('ready_high_water=1', 'ready_high_water=0')
	[IO.File]::WriteAllText($ServerPath, $BadServer)
	$Rejected = $false
	try {
		[void](Assert-Records -Clients @([pscustomobject]@{ OutputPath = $ClientPath }) `
			-Server ([pscustomobject]@{ OutputPath = $ServerPath }) -ExpectedNonces $Expected)
	} catch { $Rejected = $true }
	if (-not $Rejected) { throw 'non-simultaneous readiness was accepted' }
	$ScaleLines = [Collections.Generic.List[string]]::new()
	foreach ($Index in 0..4) {
		$Phase = @('baseline', 'load', 'resident', 'evict', 'reload')[$Index]
		$StartedUs = 1000000 + $Index * 14000000
		$StartedTick = 1 + $Index * 840
		$ScaleLines.Add("[Qualification:Scale] event=phase_start run=$RunId phase=$Phase tick=$StartedTick monotonic_us=$StartedUs")
		$ScaleLines.Add("[Qualification:Scale] event=phase_acks run=$RunId phase=$Phase count=32 tick=$($StartedTick + 1)")
		$ScaleLines.Add("[Qualification:Scale] event=producer_ack run=$RunId phase=$Phase tick=$($StartedTick + 2)")
		$ScaleLines.Add("[Qualification:Scale] event=phase_end run=$RunId phase=$Phase ticks=781 elapsed_us=13000001 monotonic_us=$($StartedUs + 13000001) producer_done=1 phase_acks=32 tick=$($StartedTick + 781)")
	}
	$ScaleLines.Add("[Qualification:Scale] event=result run=$RunId status=PASS phases=5 peers=32 tick=4201")
	[IO.File]::WriteAllLines($ServerPath, $ScaleLines)
	$ScaleClients = [Collections.Generic.List[object]]::new()
	$ScaleNonces = [Collections.Generic.List[string]]::new()
	foreach ($Slot in 0..31) {
		$Nonce = [string](1000 + $Slot)
		$ScaleNonces.Add($Nonce)
		$Path = Join-Path $Directory ("scale-client-$Slot.log")
		$RootSlot = 10 + $Slot
		$ClientLines = @(
			"[Qualification:Client] event=phase_observed run_id=$RunId slot=$Slot nonce=$Nonce phase=baseline objects=0 root_slot=0 root_generation=0 receive_to_observed_us=1",
			"[Qualification:Client] event=phase_observed run_id=$RunId slot=$Slot nonce=$Nonce phase=load objects=512 root_slot=$RootSlot root_generation=1 receive_to_observed_us=2",
			"[Qualification:Client] event=phase_observed run_id=$RunId slot=$Slot nonce=$Nonce phase=resident objects=512 root_slot=$RootSlot root_generation=1 receive_to_observed_us=3",
			"[Qualification:Client] event=phase_observed run_id=$RunId slot=$Slot nonce=$Nonce phase=evict objects=0 root_slot=0 root_generation=0 receive_to_observed_us=4",
			"[Qualification:Client] event=phase_observed run_id=$RunId slot=$Slot nonce=$Nonce phase=reload objects=512 root_slot=$RootSlot root_generation=2 receive_to_observed_us=5",
			"[Qualification:Client] event=scale_complete run_id=$RunId slot=$Slot nonce=$Nonce observed_phases=5 producer_phases=$(if ($Slot -eq 0) { 5 } else { 0 })"
		)
		if ($Slot -eq 0) {
			foreach ($Phase in @('baseline', 'load', 'resident', 'evict', 'reload')) {
				$ClientLines += "[Qualification:Producer] event=phase_metrics run_id=$RunId slot=0 nonce=$Nonce phase=$Phase status=PASS remote_samples=100 remote_p95_us=1000 remote_p99_us=2000 remote_max_us=3000 remote_errors=0 event_acks=1 event_max_gap_us=1000 action_resolutions=1 action_endings=1 action_max_result_us=1000 submission_failures=0 action_rejections=0"
			}
		}
		[IO.File]::WriteAllLines($Path, $ClientLines)
		$ScaleClients.Add([pscustomobject]@{ OutputPath = $Path })
	}
	Assert-ScaleRecords -Server ([pscustomobject]@{ OutputPath = $ServerPath }) -Clients $ScaleClients -ExpectedNonces $ScaleNonces
	[IO.File]::WriteAllText($ServerPath, ([IO.File]::ReadAllText($ServerPath)).Replace(
		'phase=baseline ticks=781 elapsed_us=13000001', 'phase=baseline ticks=781 elapsed_us=12000000'))
	$Rejected = $false
	try { Assert-ScaleRecords -Server ([pscustomobject]@{ OutputPath = $ServerPath }) -Clients $ScaleClients -ExpectedNonces $ScaleNonces } catch { $Rejected = $true }
	if (-not $Rejected) { throw 'short scale phase was accepted' }
	[IO.File]::WriteAllLines($ServerPath, $ScaleLines)
	$ProducerPath = $ScaleClients[0].OutputPath
	$ProducerBaseline = [IO.File]::ReadAllText($ProducerPath)
	[IO.File]::WriteAllText($ProducerPath, $ProducerBaseline.Replace(' remote_errors=0', ''))
	$Rejected = $false
	try { Assert-ScaleRecords -Server ([pscustomobject]@{ OutputPath = $ServerPath }) -Clients $ScaleClients -ExpectedNonces $ScaleNonces } catch { $Rejected = $true }
	if (-not $Rejected) { throw 'producer receipt with a missing zero-valued metric was accepted' }
	[IO.File]::WriteAllText($ProducerPath, $ProducerBaseline)
	[IO.File]::WriteAllLines($ServerPath, @($ScaleLines | Where-Object { $_ -notmatch 'event=phase_acks .*phase=reload' }))
	$Rejected = $false
	try { Assert-ScaleRecords -Server ([pscustomobject]@{ OutputPath = $ServerPath }) -Clients $ScaleClients -ExpectedNonces $ScaleNonces } catch { $Rejected = $true }
	if (-not $Rejected) { throw 'scale result with missing peer phase ACK was accepted' }
	[IO.File]::WriteAllLines($ServerPath, $ScaleLines)
	$BadClient = [IO.File]::ReadAllText($ScaleClients[31].OutputPath).Replace('phase=reload objects=512', 'phase=reload objects=511')
	[IO.File]::WriteAllText($ScaleClients[31].OutputPath, $BadClient)
	$Rejected = $false
	try { Assert-ScaleRecords -Server ([pscustomobject]@{ OutputPath = $ServerPath }) -Clients $ScaleClients -ExpectedNonces $ScaleNonces } catch { $Rejected = $true }
	if (-not $Rejected) { throw 'scale result with incomplete client reload was accepted' }
	$CurrentProcess = [Diagnostics.Process]::GetCurrentProcess()
	$AllProcesses = [Collections.Generic.List[object]]::new()
	$AllProcesses.Add([pscustomobject]@{ Label = 'sampler-test'; Pid = $CurrentProcess.Id; Process = $CurrentProcess })
	$ResourceSamples = [Collections.Generic.List[object]]::new()
	$ResourceClock = [Diagnostics.Stopwatch]::StartNew()
	$LastResourceSampleMilliseconds = -2000L
	Sample-RunResources
	if ($ResourceSamples.Count -ne 1 -or $ResourceSamples[0].Label -ne 'sampler-test' -or
		$ResourceSamples[0].WorkingSetBytes -le 0 -or $ResourceSamples[0].Threads -le 0) {
		throw 'bounded process resource sample was not recorded'
	}
	Write-Output '[Qualification:Farm] TYPED_EVIDENCE_TEST_OK'
} finally {
	foreach ($Path in @($ServerPath, $ClientPath) + @($ScaleClients | ForEach-Object OutputPath)) {
		if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path -Force }
	}
	Remove-Item -LiteralPath $Directory -Force
}
