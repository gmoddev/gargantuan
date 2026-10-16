#requires -Version 7.0
# Synthetic parser tests only. No game process, socket, or physical run.
$ErrorActionPreference = 'Stop'
$RunId = '12345678-1234-4234-8234-123456789abc'
$Root = Join-Path ([IO.Path]::GetTempPath()) ('farm-cadence-test-' + [Guid]::NewGuid().ToString('N'))
$Evidence = Join-Path $Root 'clients'
[void][IO.Directory]::CreateDirectory($Evidence)
$Script = Join-Path $PSScriptRoot 'PhysicalGameSessionFarmCadence.ps1'
$Phases = @('baseline', 'load', 'resident', 'evict', 'reload')
try {
	for ($Slot = 0; $Slot -lt 32; $Slot++) {
		$Nonce = [string](4294967297L + $Slot)
		$Lines = [Collections.Generic.List[string]]::new()
		$Lines.Add("[Qualification:Client] event=ready run_id=$RunId slot=$Slot nonce=$Nonce connection_slot=1")
		for ($Index = 0; $Index -lt 5; $Index++) {
			$Count = ($Index + 1) * 60
			$Lines.Add("[Qualification:Callback] event=beat run_id=$RunId slot=$Slot nonce=$Nonce phase=$($Phases[$Index]) callbacks=$Count simulation_tick=$Count steady_ns=$($Count * 16667000)")
			$Lines.Add("[Qualification:Callback] event=phase_result phase=$($Phases[$Index]) callbacks=60 total=$Count")
		}
		[IO.File]::WriteAllLines((Join-Path $Evidence ('client-{0:D2}.stdout.log' -f $Slot)), $Lines)
	}
	$Report = Join-Path $Root 'accepted.json'
	& $Script -RunId $RunId -ClientEvidenceRoot $Evidence -ReportPath $Report | Out-Null
	$Parsed = Get-Content -LiteralPath $Report -Raw | ConvertFrom-Json
	if ($Parsed.Status -cne 'OBSERVED' -or $Parsed.Clients.Count -ne 32 -or
		$Parsed.Clients[0].CallbackCount -ne 300 -or $Parsed.IndividualCallbackIntervals -cne 'NOT_MEASURED') {
		throw 'valid callback cadence receipt was not preserved with its limited scope'
	}
	$BadLog = Join-Path $Evidence 'client-00.stdout.log'
	$Text = [IO.File]::ReadAllText($BadLog).Replace('callbacks=120 simulation_tick=120', 'callbacks=180 simulation_tick=120')
	[IO.File]::WriteAllText($BadLog, $Text)
	$Rejected = $false
	try { & $Script -RunId $RunId -ClientEvidenceRoot $Evidence -ReportPath (Join-Path $Root 'rejected.json') | Out-Null }
	catch { $Rejected = $true }
	if (-not $Rejected) { throw 'skipped callback beat was accepted' }
	Write-Output '[Qualification:Callback] MOCK_TEST_OK'
} finally {
	$ResolvedRoot = [IO.Path]::GetFullPath($Root)
	$ResolvedTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
	if (-not $ResolvedRoot.StartsWith($ResolvedTemp + [IO.Path]::DirectorySeparatorChar,
		[StringComparison]::OrdinalIgnoreCase) -or
		[IO.Path]::GetFileName($ResolvedRoot) -cnotmatch '^farm-cadence-test-[a-f0-9]{32}$') {
		throw 'refusing callback test cleanup outside its temporary root'
	}
	if (Test-Path -LiteralPath $ResolvedRoot) { Remove-Item -LiteralPath $ResolvedRoot -Recurse -Force }
}
