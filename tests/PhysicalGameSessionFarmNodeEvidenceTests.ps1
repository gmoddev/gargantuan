#requires -Version 7.0
# Tests the authenticated Node evidence boundary without a network or process.
$ErrorActionPreference = 'Stop'

function Import-Functions {
	param([string]$Path, [string[]]$Names)
	$Tokens = $null; $Errors = $null
	$Ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$Tokens, [ref]$Errors)
	if ($Errors.Count -ne 0) { throw "source syntax failed: $Path" }
	$Definitions = @($Ast.FindAll({ param($Node)
		$Node -is [Management.Automation.Language.FunctionDefinitionAst] -and
		$Node.Name -in $Names
	}, $true))
	if ($Definitions.Count -ne $Names.Count) { throw "source function set is incomplete: $Path" }
	return $Definitions
}
function Expect-Rejection { param([scriptblock]$Case, [string]$Reason)
	$Rejected = $false
	try { & $Case | Out-Null } catch { $Rejected = $true }
	if (-not $Rejected) { throw "expected rejection: $Reason" }
}
function Save-NodeRunReceipt {
	param([string]$Path, [string]$LogPath, [System.Collections.IDictionary]$Receipt)
	$Receipt.StdoutBytes = [long](Get-Item -LiteralPath $LogPath).Length
	$Receipt.StdoutSha256 = (Get-FileHash -LiteralPath $LogPath -Algorithm SHA256).Hash.ToLowerInvariant()
	[IO.File]::WriteAllText($Path, ($Receipt | ConvertTo-Json -Depth 4))
	return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

$Endpoint = Join-Path $PSScriptRoot 'PhysicalGameSessionFarmEndpoint.ps1'
$Reconciler = Join-Path $PSScriptRoot 'PhysicalGameSessionFarmReconcile.ps1'
foreach ($Definition in @(Import-Functions -Path $Endpoint -Names @(
	'Get-TypedFields', 'Get-TypedRecords', 'Get-AuthenticatedNodeManifestReceipt',
	'Write-AuthenticatedNodeManifestReceipt'))) {
	. ([scriptblock]::Create($Definition.Extent.Text))
}
foreach ($Definition in @(Import-Functions -Path $Reconciler -Names @(
	'Get-RequiredJson', 'Assert-AuthenticatedNodeManifestReceipt'))) {
	. ([scriptblock]::Create($Definition.Extent.Text))
}
$TlsAnalyzer = Join-Path $PSScriptRoot 'PhysicalGameSessionFarmNodeTls.ps1'
foreach ($Definition in @(Import-Functions -Path $TlsAnalyzer -Names @(
	'Get-NodeTlsLogMatchReceipt', 'Write-NodeTlsLogMatchReceipt'))) {
	. ([scriptblock]::Create($Definition.Extent.Text))
}

$Root = Join-Path ([IO.Path]::GetTempPath()) ('farm-node-evidence-test-' + [Guid]::NewGuid().ToString('N'))
$Server = Join-Path $Root 'Server'
$Package = Join-Path $Root 'Package'
[void][IO.Directory]::CreateDirectory($Server)
[void][IO.Directory]::CreateDirectory((Join-Path $Package 'content'))
try {
	$Project = '0123456789abcdef0123456789abcdef'
	$Revision = 37L
	$PackageJson = Join-Path $Package 'game.package.json'
	$ManifestJson = Join-Path $Package 'content/content.manifest.json'
	[IO.File]::WriteAllText($PackageJson,
		(@{ ProjectId = $Project; Revision = $Revision } | ConvertTo-Json))
	[IO.File]::WriteAllText($ManifestJson,
		(@{ ProjectId = $Project; PackageVersion = $Revision } | ConvertTo-Json))
	$ManifestHash = (Get-FileHash -LiteralPath $ManifestJson -Algorithm SHA256).Hash.ToLowerInvariant()
	$RootHash = 'a' * 64
	$Run = [ordered]@{
		Provider = 'Node'; RunId = '12345678-1234-4234-8234-123456789abc'
		NodeEndpoint = '127.0.0.1:50051'; NodeRootCertificateSha256 = $RootHash
		ServerContentManifestSha256 = $ManifestHash
	}
	$Line = '[Qualification:NodeProvider] event=authenticated_manifest' +
		" run=$($Run.RunId) request_id=server-content-1 project=$Project revision=$Revision endpoint=$($Run.NodeEndpoint)" +
		" root_sha256=$RootHash manifest_sha256=$ManifestHash" +
		" manifest_bytes=$(([IO.FileInfo]$ManifestJson).Length) rpc_count=1" +
		' channel=grpc_ssl_credentials authenticated_rpc=1 tls_session_details=not_measured'
	$Log = Join-Path $Root 'server.stdout.log'
	[IO.File]::WriteAllText($Log, "$Line`n")
	$Receipt = Get-AuthenticatedNodeManifestReceipt -LogPath $Log -PackageRoot $Package -RunManifest $Run
	$ReceiptPath = Join-Path $Server 'node-provider.json'
	Write-AuthenticatedNodeManifestReceipt -Path $ReceiptPath -Receipt $Receipt
	if ((Get-Item -LiteralPath $ReceiptPath).Length -gt 4096 -or
		(Get-Content -LiteralPath $ReceiptPath -Raw) -match 'Bearer|token|PRIVATE KEY') {
		throw 'Node receipt is oversized or contains secret material'
	}
	$Validated = Assert-AuthenticatedNodeManifestReceipt -ServerRoot $Server -RunManifest $Run
	if ($Validated.State -cne 'AUTHENTICATED_MANIFEST_RPC_MEASURED' -or
		$Validated.RequestId -cne 'server-content-1' -or
		$Validated.TlsSessionDetails -cne 'NOT_MEASURED') {
		throw 'Node request was mistaken for full TLS/provider proof'
	}
	$NodeLog = Join-Path $Root 'node.stdout.log'
	$TlsLine = [ordered]@{
		time = '2026-10-01T00:00:00Z'; level = 'INFO'
		msg = '[Content:TLS] authenticated manifest RPC'
		request_id = 'server-content-1'; project_id = $Project
		package_version = $Revision; principal_id = 'farm-server'
		tls_version = 'TLSv1.3'; cipher_suite = 'TLS_AES_128_GCM_SHA256'
		transport = 'grpc_tls'
	} | ConvertTo-Json -Compress
	[IO.File]::WriteAllText($NodeLog, "$TlsLine`n")
	$StagePath = Join-Path $Root 'node-stage.json'
	$Stage = [ordered]@{
		Format = 'GargantuanFarmNodeStage'; Version = 1
		RunId = $Run.RunId; ProjectId = $Project; Revision = $Revision
		NodeEndpoint = $Run.NodeEndpoint; RootCertificateSha256 = $RootHash
		NodeExecutableSha256 = 'd' * 64; ConfigSha256 = 'e' * 64
		CertificateSha256 = 'f' * 64
	}
	[IO.File]::WriteAllText($StagePath, ($Stage | ConvertTo-Json -Depth 4))
	$StageHash = (Get-FileHash -LiteralPath $StagePath -Algorithm SHA256).Hash.ToLowerInvariant()
	$NodeRunPath = Join-Path $Root 'node-run.json'
	$NodeRun = [ordered]@{
		Format = 'GargantuanFarmNodeRun'; Version = 1
		RunId = $Run.RunId; StageSha256 = $StageHash
		NodeExecutableSha256 = 'd' * 64; ConfigSha256 = 'e' * 64
		CertificateSha256 = 'f' * 64; RootCertificateSha256 = $RootHash
		Pid = 1234; StartedUtc = '2026-10-01T00:00:00Z'
		EndedUtc = '2026-10-01T00:00:01Z'; TcpReady = $true
		ChildReaped = $true; Reason = 'STOP_REQUESTED'
		StdoutPath = $NodeLog; StdoutSha256 = ''; StdoutBytes = 0
		StderrPath = (Join-Path $Root 'node.stderr.log'); StderrSha256 = 'a' * 64
		StderrBytes = 0
	}
	$RunPin = Save-NodeRunReceipt -Path $NodeRunPath -LogPath $NodeLog -Receipt $NodeRun
	$TlsReceipt = Get-NodeTlsLogMatchReceipt -ServerReceiptPath $ReceiptPath -NodeStagePath $StagePath `
		-NodeRunReceiptPath $NodeRunPath -NodeRunReceiptSha256 $RunPin -NodeStageSha256 $StageHash
	$TlsReceiptPath = Join-Path $Root 'node-tls-match.json'
	Write-NodeTlsLogMatchReceipt -Path $TlsReceiptPath -Receipt $TlsReceipt
	if ($TlsReceipt.State -cne 'OFFLINE_LOG_MATCH_BOUND_TO_PINNED_NODE_RUN' -or
		$TlsReceipt.RequestId -cne 'server-content-1' -or
		$TlsReceipt.CipherSuite -cne 'TLS_AES_128_GCM_SHA256') {
		throw 'offline Node TLS match was lost or promoted to physical provenance'
	}
	Expect-Rejection {
		Get-NodeTlsLogMatchReceipt -ServerReceiptPath $ReceiptPath -NodeStagePath $StagePath `
			-NodeRunReceiptPath $NodeRunPath -NodeRunReceiptSha256 ('0' * 64) `
			-NodeStageSha256 $StageHash
	} 'wrong independent run receipt pin'
	Expect-Rejection {
		Get-NodeTlsLogMatchReceipt -ServerReceiptPath $ReceiptPath -NodeStagePath $StagePath `
			-NodeRunReceiptPath $NodeRunPath -NodeRunReceiptSha256 $RunPin `
			-NodeStageSha256 ('0' * 64)
	} 'wrong stage pin'
	foreach ($Replacement in @(
		@('server-content-1', 'server-content-2'),
		@('TLSv1.3', 'TLSv1.1'),
		@('TLS_AES_128_GCM_SHA256', 'TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256'),
		@($Project, ('b' * 32))
	)) {
		[IO.File]::WriteAllText($NodeLog, ($TlsLine.Replace($Replacement[0], $Replacement[1]) + "`n"))
		$RunPin = Save-NodeRunReceipt -Path $NodeRunPath -LogPath $NodeLog -Receipt $NodeRun
		Expect-Rejection {
			Get-NodeTlsLogMatchReceipt -ServerReceiptPath $ReceiptPath -NodeStagePath $StagePath `
				-NodeRunReceiptPath $NodeRunPath -NodeRunReceiptSha256 $RunPin -NodeStageSha256 $StageHash
		} "mismatched Node TLS field $($Replacement[0])"
	}
	[IO.File]::WriteAllText($NodeLog, "$TlsLine`n$TlsLine`n")
	$RunPin = Save-NodeRunReceipt -Path $NodeRunPath -LogPath $NodeLog -Receipt $NodeRun
	Expect-Rejection {
		Get-NodeTlsLogMatchReceipt -ServerReceiptPath $ReceiptPath -NodeStagePath $StagePath `
			-NodeRunReceiptPath $NodeRunPath -NodeRunReceiptSha256 $RunPin -NodeStageSha256 $StageHash
	} 'duplicate matching TLS records'
	[IO.File]::WriteAllText($NodeLog, "$TlsLine`n")
	$RunPin = Save-NodeRunReceipt -Path $NodeRunPath -LogPath $NodeLog -Receipt $NodeRun
	$BadLine = $Line.Replace('request_id=server-content-1', 'request_id=bad-id')
	[IO.File]::WriteAllText($Log, "$BadLine`n")
	Expect-Rejection {
		Get-AuthenticatedNodeManifestReceipt -LogPath $Log -PackageRoot $Package -RunManifest $Run
	} 'unbounded or malformed native request ID'
	$BadLine = $Line.Replace("root_sha256=$RootHash", ('root_sha256=' + ('b' * 64)))
	[IO.File]::WriteAllText($Log, "$BadLine`n")
	Expect-Rejection {
		Get-AuthenticatedNodeManifestReceipt -LogPath $Log -PackageRoot $Package -RunManifest $Run
	} 'wrong CA hash'
	$BadLine = $Line.Replace("manifest_sha256=$ManifestHash", ('manifest_sha256=' + ('b' * 64)))
	[IO.File]::WriteAllText($Log, "$BadLine`n")
	Expect-Rejection {
		Get-AuthenticatedNodeManifestReceipt -LogPath $Log -PackageRoot $Package -RunManifest $Run
	} 'wrong package manifest digest'
	[IO.File]::WriteAllText($Log, "$Line`n$Line`n")
	Expect-Rejection {
		Get-AuthenticatedNodeManifestReceipt -LogPath $Log -PackageRoot $Package -RunManifest $Run
	} 'duplicate authenticated manifest records'
	$BadReceipt = Get-Content -LiteralPath $ReceiptPath -Raw | ConvertFrom-Json -AsHashtable
	$BadReceipt.TlsSessionDetails = 'TLSv1.3'
	[IO.File]::WriteAllText($ReceiptPath, ($BadReceipt | ConvertTo-Json -Depth 5))
	Expect-Rejection {
		Assert-AuthenticatedNodeManifestReceipt -ServerRoot $Server -RunManifest $Run
	} 'fabricated TLS negotiation detail'
	$Local = $Run | ConvertTo-Json -Depth 4 | ConvertFrom-Json -AsHashtable
	$Local.Provider = 'Local'
	Expect-Rejection {
		Assert-AuthenticatedNodeManifestReceipt -ServerRoot $Server -RunManifest $Local
	} 'Local run with Node evidence'
	Remove-Item -LiteralPath $ReceiptPath -Force
	if ((Assert-AuthenticatedNodeManifestReceipt -ServerRoot $Server -RunManifest $Local).State -cne
		'NOT_APPLICABLE') { throw 'Local provider behavior changed' }
	Write-Output '[Qualification:FarmNodeEvidence] MOCK_TEST_OK'
} finally {
	$Resolved = [IO.Path]::GetFullPath($Root)
	$Temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
	if (-not $Resolved.StartsWith($Temp + [IO.Path]::DirectorySeparatorChar,
		[StringComparison]::OrdinalIgnoreCase) -or
		[IO.Path]::GetFileName($Resolved) -cnotmatch '^farm-node-evidence-test-[a-f0-9]{32}$') {
		throw 'refusing recursive cleanup outside this Node evidence test root'
	}
	Remove-Item -LiteralPath $Resolved -Recurse -Force
}
