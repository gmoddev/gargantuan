#requires -Version 7.0
# Controller-only preparation receipt. Does not stage files or launch endpoints.
param(
	[Parameter(Mandatory)][ValidatePattern('^[a-f0-9]{40}$')][string]$SourceCommit,
	[Parameter(Mandatory)][string]$PythonPath,
	[Parameter(Mandatory)][ValidatePattern('^[a-fA-F0-9]{64}$')][string]$PythonSha256,
	[Parameter(Mandatory)][string]$OutputPath
)
$ErrorActionPreference = 'Stop'
$Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$Head = (& git -C $Root rev-parse HEAD)
if ($LASTEXITCODE -ne 0 -or $Head -cne $SourceCommit) { throw 'analysis source commit is not checkout HEAD' }
# Imports and AST-extracted canonical parsers used by offline Reconcile/Acceptance.
# Preserve this repository-relative layout; endpoint package.py is not this path.
$Paths = @(
	'tests/PhysicalFarmAnalysisInventory.ps1',
	'tests/PhysicalGameSessionFarmReconcile.ps1',
	'tests/PhysicalGameSessionFarmAcceptance.ps1',
	'tests/PhysicalGameSessionFarm.ps1',
	'tests/PhysicalGameSessionFarmEndpoint.ps1',
	'tests/PhysicalGameSessionFarmNodeTls.ps1',
	'tests/AdmissionFairnessEvidence.ps1',
	'tests/RecoveryCausalEvidence.ps1',
	'tests/PhysicalFarmPublicationEvidence.ps1',
	'tests/PhysicalFarmClockEvidence.ps1',
	'tests/PhysicalGameSessionFarmLifecycle.ps1',
	'tests/PhysicalGameSessionFarmRemoteOwnership.ps1',
	'tests/PhysicalGameSessionFarmF1.ps1',
	'tests/PhysicalFarmNodeResources.ps1',
	'tools/physical-qualifier/farm_publication_join.py',
	'tools/physical-qualifier/farm_publication_trace.py',
	'tools/physical-qualifier/farm_ordinary_demand.py',
	'tools/physical-qualifier/farm_clock_exchange.py',
	'tools/physical-qualifier/farm_server_tick.py',
	'tools/physical-qualifier/farm_remote_cadence.py',
	'tools/physical-qualifier/farm_capture_acceptance.py',
	'tools/physical-qualifier/farm_capture_directions.py'
)
$Dirty = @(& git -C $Root status --porcelain --untracked-files=all -- $Paths)
if ($LASTEXITCODE -ne 0 -or $Dirty.Count) { throw 'analysis dependencies differ from committed source' }
$Python = Get-Item -LiteralPath $PythonPath -ErrorAction Stop
$ResolvedPython = @(Get-Command python -CommandType Application -ErrorAction Stop)[0].Source
if ([IO.Path]::GetFullPath($ResolvedPython) -ine $Python.FullName -or
	(Get-FileHash -LiteralPath $Python.FullName -Algorithm SHA256).Hash -ine $PythonSha256) {
	throw 'offline analyzer Python selection or executable hash mismatch'
}
$PowerShell = (Get-Process -Id $PID).Path
$Files = @($Paths | ForEach-Object {
	$File = Get-Item -LiteralPath (Join-Path $Root $_) -ErrorAction Stop
	if ($File.PSIsContainer -or $File.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) {
		throw 'analysis dependency missing or redirected'
	}
	[ordered]@{ Path = $_; Bytes = $File.Length;
		Sha256 = (Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
$Receipt = [ordered]@{ Format = 'GargantuanFarmOfflineAnalysisInventory'; Version = 1;
	SourceCommit = $SourceCommit; SourceRoot = $Root;
	PythonPath = $Python.FullName; PythonSha256 = $PythonSha256.ToLowerInvariant();
	PowerShellPath = $PowerShell;
	PowerShellSha256 = (Get-FileHash -LiteralPath $PowerShell -Algorithm SHA256).Hash.ToLowerInvariant();
	Files = $Files }
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if ($OutputPath.StartsWith($Root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
	throw 'analysis inventory must be retained outside the source checkout'
}
$Bytes = [Text.UTF8Encoding]::new($false).GetBytes(($Receipt | ConvertTo-Json -Depth 6))
$Stream = [IO.File]::Open($OutputPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try { $Stream.Write($Bytes) } finally { $Stream.Dispose() }
Write-Output "[Qualification:FarmAnalysis] source=$SourceCommit files=$($Files.Count) inventory=$OutputPath"
