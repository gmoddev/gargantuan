#requires -Version 7.0
# Offline reconciliation of two role-local Foundation 3L farm receipts. This
# intentionally does not pronounce Local or Node provider qualification.

param(
	[Parameter(Mandatory = $true)][string]$RunManifestPath,
	[Parameter(Mandatory = $true)][ValidatePattern('^[a-fA-F0-9]{64}$')][string]$ManifestSha256,
	[Parameter(Mandatory = $true)][string]$ServerEvidenceRoot,
	[Parameter(Mandatory = $true)][string]$ClientEvidenceRoot,
	[Parameter(Mandatory = $true)][string]$ReportPath
)

$ErrorActionPreference = 'Stop'

function Get-RequiredJson {
	param([string]$Path)
	if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or
		(Get-Item -LiteralPath $Path).Length -gt 1048576) { throw "missing or oversized JSON evidence: $Path" }
	return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -AsHashtable
}

function Assert-Hash {
	param([string]$Path, [string]$Expected, [string]$Label)
	if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or
		(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ine $Expected) {
		throw "$Label SHA-256 does not match its pin"
	}
}

function Assert-EvidenceRoot {
	param([string]$Root, [string]$ExpectedRole, [string]$ExpectedRunId, [string]$ExpectedManifestSha256)
	$Root = [IO.Path]::GetFullPath($Root)
	if (-not (Test-Path -LiteralPath $Root -PathType Container) -or
		(Get-Item -LiteralPath $Root).Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) {
		throw "$ExpectedRole evidence root is missing or redirected"
	}
	$IndexPath = Join-Path $Root 'evidence-sha256.json'
	$Index = Get-RequiredJson -Path $IndexPath
	if ($Index.RunId -cne $ExpectedRunId -or $Index.Role -cne $ExpectedRole -or
		$Index.Files -isnot [array] -or $Index.Files.Count -lt 4 -or
		$Index.Files.Count -gt 80) { throw "$ExpectedRole evidence index schema or identity is invalid" }
	$Expected = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
	foreach ($File in $Index.Files) {
		$Name = [string]$File.Name
		if ($Name -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$' -or
			$Name -ieq 'evidence-sha256.json' -or -not $Expected.Add($Name) -or
			[long]$File.Bytes -lt 0 -or [long]$File.Bytes -gt 16777216 -or
			[string]$File.Sha256 -cnotmatch '^[a-fA-F0-9]{64}$') {
			throw "$ExpectedRole evidence index contains an invalid file entry"
		}
		$Path = Join-Path $Root $Name
		$Item = Get-Item -LiteralPath $Path -ErrorAction Stop
		if ($Item.PSIsContainer -or $Item.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint) -or
			$Item.Length -ne [long]$File.Bytes) {
			throw "$ExpectedRole evidence file $Name is missing, redirected, or has wrong size"
		}
		Assert-Hash -Path $Path -Expected $File.Sha256 -Label "$ExpectedRole evidence $Name"
	}
	$Actual = @(Get-ChildItem -LiteralPath $Root -Force)
	foreach ($Item in $Actual) {
		if ($Item.Name -ine 'evidence-sha256.json' -and -not $Expected.Contains($Item.Name)) {
			throw "$ExpectedRole evidence has an unindexed file or directory: $($Item.Name)"
		}
	}
	if ($Actual.Count -ne $Index.Files.Count + 1) {
		throw "$ExpectedRole evidence index does not cover the complete root"
	}
	$RunCopy = Join-Path $Root 'run-manifest.json'
	Assert-Hash -Path $RunCopy -Expected $ExpectedManifestSha256 -Label "$ExpectedRole run manifest copy"
	$Result = Get-RequiredJson -Path (Join-Path $Root 'result.json')
	if ($Result.RunId -cne $ExpectedRunId -or $Result.Role -cne $ExpectedRole -or
		$Result.Status -cne 'PASS' -or $Result.ManifestSha256 -ine $ExpectedManifestSha256 -or
		$Result.CleanupErrors -or $Result.EvidenceError) {
		throw "$ExpectedRole role-local result failed, was not cleaned up, or belongs to another run"
	}
	return [pscustomobject]@{ Root = $Root; Result = $Result; Index = $Index;
		IndexSha256 = (Get-FileHash -LiteralPath $IndexPath -Algorithm SHA256).Hash.ToLowerInvariant() }
}

function Assert-ResourceRows {
	param([string]$Path, [string]$ExpectedRunId, [string[]]$Labels, [long]$ExpectedCount)
	$Rows = @(Import-Csv -LiteralPath $Path)
	if ($Rows.Count -ne $ExpectedCount -or $Rows.Count -lt $Labels.Count -or
		$Rows.Count -gt 20000) { throw 'role-local resource sample count is invalid' }
	$Seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	$LastByLabel = @{}
	foreach ($Row in $Rows) {
		if ($Row.RunId -cne $ExpectedRunId -or $Row.Label -cnotin $Labels -or
			[long]$Row.Pid -le 0 -or [long]$Row.WorkingSetBytes -le 0 -or
			[long]$Row.PrivateBytes -lt 0 -or [double]$Row.CpuMilliseconds -lt 0 -or
			[long]$Row.Threads -le 0 -or [long]$Row.Handles -lt 0 -or
			[long]$Row.SupervisorElapsedMilliseconds -lt 0 -or
			[long]$Row.MonotonicTicks -le 0 -or [long]$Row.MonotonicFrequency -le 0) {
			throw 'role-local resource sample is invalid'
		}
		$Timestamp = [DateTimeOffset]::MinValue
		if (-not [DateTimeOffset]::TryParse($Row.Utc, [ref]$Timestamp)) {
			throw 'role-local resource timestamp is invalid'
		}
		if ($LastByLabel.ContainsKey($Row.Label)) {
			$Prior = $LastByLabel[$Row.Label]
			if ([long]$Row.Pid -ne $Prior.Pid -or
				[long]$Row.MonotonicFrequency -ne $Prior.Frequency -or
				[long]$Row.MonotonicTicks -le $Prior.Ticks -or
				[long]$Row.SupervisorElapsedMilliseconds -lt $Prior.Elapsed) {
				throw 'role-local resource sample order or process identity is invalid'
			}
		}
		$LastByLabel[$Row.Label] = [pscustomobject]@{
			Pid = [long]$Row.Pid; Frequency = [long]$Row.MonotonicFrequency
			Ticks = [long]$Row.MonotonicTicks; Elapsed = [long]$Row.SupervisorElapsedMilliseconds
		}
		[void]$Seen.Add($Row.Label)
	}
	foreach ($Label in $Labels) {
		if (-not $Seen.Contains($Label)) { throw "role-local resource sample missing $Label" }
	}
	return $Rows.Count
}

function Get-UnsignedField {
	param([System.Collections.IDictionary]$Record, [string]$Name)
	$Value = [UInt64]0
	if (-not $Record.ContainsKey($Name) -or [string]$Record[$Name] -cnotmatch '^(0|[1-9][0-9]*)$' -or
		-not [UInt64]::TryParse([string]$Record[$Name], [ref]$Value)) {
		throw "admission evidence lacks a valid $Name counter"
	}
	return $Value
}

function Assert-AdmissionConservation {
	param([string]$Path, [string]$ExpectedRunId)
	$Lines = @([IO.File]::ReadAllLines($Path) | Where-Object {
		$_.StartsWith('[Qualification:Admission] ', [StringComparison]::Ordinal)
	})
	if ($Lines.Count -ne 1) { throw 'server lacks one final native admission receipt' }
	$Record = Get-Fields -Line $Lines[0]
	if ($Record.event -cne 'result' -or $Record.run -cne $ExpectedRunId) {
		throw 'native admission receipt belongs to another run or event'
	}
	$Names = @('accepted', 'retired', 'terminal_release', 'outstanding', 'outstanding_high',
		'active_grants', 'grants_high', 'grant_deferrals', 'funded_deferrals',
		'credit_deferrals', 'fairness_deferrals', 'max_wait_us', 'peer_backlog_high',
		'global_backlog_high', 'peer_credit_high', 'global_credit_high',
		'fairness_rotations', 'pending_enters', 'pending_leaves',
		'materialization_backlog', 'journal_backlog', 'structural_active_peers',
		'oldest_pending_ticks', 'backlog_failures', 'journal_failures')
	$Values = @{}
	foreach ($Name in $Names) { $Values[$Name] = Get-UnsignedField -Record $Record -Name $Name }
	if ($Values.accepted -eq 0 -or $Values.retired -ne $Values.accepted -or
		$Values.terminal_release -ne 0 -or $Values.outstanding -ne 0 -or
		$Values.active_grants -ne 0 -or $Values.grants_high -eq 0 -or
		$Values.grants_high -gt 4 -or $Values.outstanding_high -gt 2097152 -or
		$Values.peer_credit_high -gt 524288 -or $Values.global_credit_high -gt 2097152 -or
		$Values.pending_enters -ne 0 -or $Values.pending_leaves -ne 0 -or
		$Values.materialization_backlog -ne 0 -or $Values.journal_backlog -ne 0 -or
		$Values.structural_active_peers -ne 0 -or
		$Values.backlog_failures -ne 0 -or $Values.journal_failures -ne 0) {
		throw 'native admission conservation or canonical pooled bounds failed'
	}
	return [pscustomobject]$Values
}

function Get-Ledger {
	param([bool]$ScaleValidated)
	# These are the independent gates in PooledPhysicalQualification3L.md.
	# A phase-control receipt proves only the listed subset. Other gates need
	# native trace/capture/provider analyses rather than inferred success.
	return @(
		[ordered]@{ Gate = '32 actual GameSession clients and connection identity'; State = 'MEASURED'; Evidence = 'typed start/ready/result and nonce join' },
		[ordered]@{ Gate = 'Five structural content phases and client observations'; State = $(if ($ScaleValidated) { 'MEASURED' } else { 'NOT MEASURED' }); Evidence = 'phase, content ACK and 32 client observation records' },
		[ordered]@{ Gate = 'One-producer RPC/Event/action metrics'; State = $(if ($ScaleValidated) { 'MEASURED' } else { 'NOT MEASURED' }); Evidence = 'five typed producer metric receipts' },
		[ordered]@{ Gate = 'Role-local CPU/RSS/thread/handle sampling'; State = 'MEASURED'; Evidence = 'pinned process-resources.csv on both hosts' },
		[ordered]@{ Gate = 'Authoritative server tick, network and full Character cadence'; State = 'NOT MEASURED'; Evidence = 'requires bounded native application trace analysis' },
		[ordered]@{ Gate = 'RPC handler and response queue bounds'; State = 'NOT MEASURED'; Evidence = 'requires native queue trace analysis' },
		[ordered]@{ Gate = 'Fixed 20-second service recovery'; State = 'NOT MEASURED'; Evidence = 'requires independently timed recovery workload and trace' },
		[ordered]@{ Gate = 'Exact accepted/retired/terminal/debt/grant/journal conservation'; State = 'MEASURED'; Evidence = 'final native admission receipt, exact byte equality, zero debt/grants and zero journal failures' },
		[ordered]@{ Gate = 'Fairness, backlog and backpressure'; State = 'NOT MEASURED'; Evidence = 'requires admission/scheduler trace and overload case' },
		[ordered]@{ Gate = 'Bidirectional physical capture and transport reserve'; State = 'NOT MEASURED'; Evidence = 'requires both independently qualified capture manifests and packet analysis' },
		[ordered]@{ Gate = 'Content provider provenance and real TLS when Node'; State = 'NOT MEASURED'; Evidence = 'requires provider/TLS transcript and Node service evidence' },
		[ordered]@{ Gate = 'Client callback intervals and missed observations'; State = 'NOT MEASURED'; Evidence = 'requires client callback timing trace' },
		[ordered]@{ Gate = 'Logical lifecycle, readers, content and debt cleanup'; State = 'NOT MEASURED'; Evidence = 'role-local owned PID cleanup does not prove Engine lifetime cleanup' }
	)
}

function Import-FarmParser {
	$Path = Join-Path $PSScriptRoot 'PhysicalGameSessionFarm.ps1'
	$Tokens = $null
	$Errors = $null
	$Ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$Tokens, [ref]$Errors)
	if ($Errors.Count -ne 0) { throw 'canonical farm evidence parser has a syntax error' }
	$Needed = @('Get-Fields', 'Get-Records', 'Assert-Records', 'Assert-ScaleRecords')
	foreach ($Function in $Ast.FindAll({ param($Node)
		$Node -is [Management.Automation.Language.FunctionDefinitionAst]
	}, $true)) {
		if ($Function.Name -in $Needed) { $Function.Extent.Text }
	}
}

function Import-EndpointManifestValidator {
	$Path = Join-Path $PSScriptRoot 'PhysicalGameSessionFarmEndpoint.ps1'
	$Tokens = $null
	$Errors = $null
	$Ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$Tokens, [ref]$Errors)
	if ($Errors.Count -ne 0) { throw 'role-local endpoint manifest validator has a syntax error' }
	$Definitions = @($Ast.FindAll({ param($Node)
		$Node -is [Management.Automation.Language.FunctionDefinitionAst] -and
		$Node.Name -eq 'Assert-RunManifest'
	}, $true))
	if ($Definitions.Count -ne 1) { throw 'role-local endpoint has no unique run manifest validator' }
	return $Definitions[0].Extent.Text
}

$RunManifestPath = [IO.Path]::GetFullPath($RunManifestPath)
Assert-Hash -Path $RunManifestPath -Expected $ManifestSha256 -Label 'canonical run manifest'
$Manifest = Get-RequiredJson -Path $RunManifestPath
. ([scriptblock]::Create((Import-EndpointManifestValidator)))
[void](Assert-RunManifest -Value $Manifest)
if ($Manifest.ScaleWorkload -ne $true) {
	throw 'run manifest is not a 32-client five-phase physical farm manifest'
}
$RunId = [string]$Manifest.RunId
$Server = Assert-EvidenceRoot -Root $ServerEvidenceRoot -ExpectedRole 'Server' -ExpectedRunId $RunId `
	-ExpectedManifestSha256 $ManifestSha256
$Clients = Assert-EvidenceRoot -Root $ClientEvidenceRoot -ExpectedRole 'Clients' -ExpectedRunId $RunId `
	-ExpectedManifestSha256 $ManifestSha256
if ($Server.Root -ieq $Clients.Root -or $Server.Result.Provider -cne $Manifest.Provider -or
	$Clients.Result.Provider -cne $Manifest.Provider -or
	$Server.Result.Endpoint -cne $Manifest.Endpoint -or $Clients.Result.Endpoint -cne $Manifest.Endpoint) {
	throw 'role-local evidence roots or provider/endpoint identities do not match'
}
$ClientLabels = @(0..31 | ForEach-Object { 'client-{0:D2}' -f $_ })
$ServerSamples = Assert-ResourceRows -Path (Join-Path $Server.Root 'process-resources.csv') `
	-ExpectedRunId $RunId -Labels @('server') -ExpectedCount ([long]$Server.Result.ResourceSamples)
$ClientSamples = Assert-ResourceRows -Path (Join-Path $Clients.Root 'process-resources.csv') `
	-ExpectedRunId $RunId -Labels $ClientLabels -ExpectedCount ([long]$Clients.Result.ResourceSamples)

foreach ($Definition in @(Import-FarmParser)) { . ([scriptblock]::Create($Definition)) }
foreach ($Name in @('Get-Fields', 'Get-Records', 'Assert-Records', 'Assert-ScaleRecords')) {
	if (-not (Get-Command $Name -CommandType Function -ErrorAction SilentlyContinue)) {
		throw "canonical farm evidence parser lacks $Name"
	}
}
$Peers = 32
$Provider = [string]$Manifest.Provider
$ScaleWorkload = $true
$ExpectedNonces = [Collections.Generic.List[string]]::new()
foreach ($Nonce in $Manifest.Nonces) { $ExpectedNonces.Add([string]$Nonce) }
$ServerLog = [pscustomobject]@{ OutputPath = (Join-Path $Server.Root 'server.stdout.log') }
$ClientLogs = @(0..31 | ForEach-Object {
	[pscustomobject]@{ OutputPath = (Join-Path $Clients.Root ('client-{0:D2}.stdout.log' -f $_)) }
})
$Identity = Assert-Records -Server $ServerLog -Clients $ClientLogs -ExpectedNonces $ExpectedNonces
Assert-ScaleRecords -Server $ServerLog -Clients $ClientLogs -ExpectedNonces $ExpectedNonces
$Admission = Assert-AdmissionConservation -Path $ServerLog.OutputPath -ExpectedRunId $RunId
$Ledger = Get-Ledger -ScaleValidated $true
$Report = [ordered]@{
	Format = 'GargantuanPhysicalFarmReconciliation'; Version = 1
	RunId = $RunId; Provider = $Provider; ManifestSha256 = $ManifestSha256.ToLowerInvariant()
	ServerEvidenceSha256 = $Server.IndexSha256; ClientEvidenceSha256 = $Clients.IndexSha256
	Status = 'INCOMPLETE'; RoleLocalEvidence = 'VALIDATED'; ProviderQualification = 'NOT CLAIMED'
	Identity = $Identity; Admission = $Admission
	ServerResourceSamples = $ServerSamples; ClientResourceSamples = $ClientSamples
	Ledger = $Ledger
	MeasuredGateCount = @($Ledger | Where-Object State -eq 'MEASURED').Count
	MissingGateCount = @($Ledger | Where-Object State -eq 'NOT MEASURED').Count
}
$ReportPath = [IO.Path]::GetFullPath($ReportPath)
if ($ReportPath.StartsWith($Server.Root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
	$ReportPath.StartsWith($Clients.Root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
	throw 'reconciliation report must remain outside immutable role-local evidence roots'
}
if (Test-Path -LiteralPath $ReportPath) { throw 'reconciliation report path already exists' }
$ReportBytes = [Text.UTF8Encoding]::new($false).GetBytes(($Report | ConvertTo-Json -Depth 8))
$Stream = [IO.File]::Open($ReportPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try { $Stream.Write($ReportBytes) } finally { $Stream.Dispose() }
Write-Output "[Qualification:FarmReconcile] INCOMPLETE run=$RunId measured=$($Report.MeasuredGateCount) missing=$($Report.MissingGateCount) report=$ReportPath"
