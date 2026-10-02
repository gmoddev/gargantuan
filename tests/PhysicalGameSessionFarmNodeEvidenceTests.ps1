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
		" run=$($Run.RunId) project=$Project revision=$Revision endpoint=$($Run.NodeEndpoint)" +
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
		$Validated.TlsSessionDetails -cne 'NOT_MEASURED') {
		throw 'Node request was mistaken for full TLS/provider proof'
	}
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
