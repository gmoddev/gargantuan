#requires -Version 7.0
# Offline correlation only. The independently pinned Node run receipt binds
# the bounded JSON log to the owned child; this still does not prove the full
# physical content-provider gate.
param(
	[Parameter(Mandatory = $true)][string]$ServerReceiptPath,
	[Parameter(Mandatory = $true)][string]$NodeStagePath,
	[Parameter(Mandatory = $true)][string]$NodeRunReceiptPath,
	[Parameter(Mandatory = $true)][ValidatePattern('^[a-fA-F0-9]{64}$')][string]$NodeRunReceiptSha256,
	[Parameter(Mandatory = $true)][ValidatePattern('^[a-fA-F0-9]{64}$')][string]$NodeStageSha256,
	[Parameter(Mandatory = $true)][string]$OutputPath
)

$ErrorActionPreference = 'Stop'

function Get-NodeTlsLogMatchReceipt {
	param([string]$ServerReceiptPath, [string]$NodeStagePath, [string]$NodeRunReceiptPath,
		[string]$NodeRunReceiptSha256, [string]$NodeStageSha256)
	$ServerFile = Get-Item -LiteralPath $ServerReceiptPath -ErrorAction Stop
	$StageFile = Get-Item -LiteralPath $NodeStagePath -ErrorAction Stop
	$RunFile = Get-Item -LiteralPath $NodeRunReceiptPath -ErrorAction Stop
	if ($ServerFile.Length -lt 1 -or $ServerFile.Length -gt 4096 -or
		$StageFile.Length -lt 1 -or $StageFile.Length -gt 65536 -or
		$RunFile.Length -lt 1 -or $RunFile.Length -gt 4096) {
		throw 'Node TLS correlation input exceeds its fixed byte bound'
	}
	$Server = Get-Content -LiteralPath $ServerReceiptPath -Raw | ConvertFrom-Json -AsHashtable
	$StageDigest = (Get-FileHash -LiteralPath $StageFile.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
	$RunDigest = (Get-FileHash -LiteralPath $RunFile.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
	if ($StageDigest -ine $NodeStageSha256 -or $RunDigest -ine $NodeRunReceiptSha256) {
		throw 'Node stage or run receipt hash differs from independent pin'
	}
	$Stage = Get-Content -LiteralPath $StageFile.FullName -Raw | ConvertFrom-Json -AsHashtable
	$NodeRun = Get-Content -LiteralPath $RunFile.FullName -Raw | ConvertFrom-Json -AsHashtable
	if ($Server.Format -cne 'GargantuanFarmNodeAuthenticatedManifest' -or
		$Server.Version -ne 2 -or $Server.Provider -cne 'Node' -or
		$Server.RunId -cnotmatch '^[a-f0-9]{8}(-[a-f0-9]{4}){3}-[a-f0-9]{12}$' -or
		$Server.RequestId -cnotmatch '^server-content-[1-9][0-9]{0,19}$' -or
		$Server.ProjectId -cnotmatch '^[a-f0-9]{32}$' -or
		$Server.PackageVersion -isnot [long] -or $Server.PackageVersion -le 0 -or
		$Server.ManifestSha256 -cnotmatch '^[a-f0-9]{64}$' -or
		$Server.RootCertificateSha256 -cnotmatch '^[a-f0-9]{64}$' -or
		$Server.ChannelCredentials -cne 'grpc_ssl_credentials') {
		throw 'server Node receipt is not a bounded authenticated manifest record'
	}
	$RunFields = @('Format', 'Version', 'RunId', 'StageSha256', 'NodeExecutableSha256',
		'ConfigSha256', 'CertificateSha256', 'RootCertificateSha256', 'Pid',
		'StartedUtc', 'EndedUtc', 'TcpReady', 'ChildReaped', 'Reason',
		'StdoutPath', 'StdoutSha256', 'StdoutBytes', 'StderrPath', 'StderrSha256', 'StderrBytes')
	$StageDirectory = [IO.Path]::GetFullPath($StageFile.DirectoryName)
	if ([IO.Path]::GetDirectoryName($RunFile.FullName) -ine $StageDirectory -or
		[IO.Path]::GetFileName($StageFile.FullName) -cne 'node-stage.json' -or
		[IO.Path]::GetFileName($RunFile.FullName) -cne 'node-run.json' -or
		$Stage.Format -cne 'GargantuanFarmNodeStage' -or $Stage.Version -ne 1 -or
		$Stage.RunId -cne $Server.RunId -or $Stage.ProjectId -cne $Server.ProjectId -or
		$Stage.Revision -ne $Server.PackageVersion -or
		$Stage.NodeEndpoint -cne $Server.NodeEndpoint -or
		$Stage.RootCertificateSha256 -ine $Server.RootCertificateSha256 -or
		@($RunFields | Where-Object { -not $NodeRun.Contains($_) }).Count -ne 0 -or
		$NodeRun.Format -cne 'GargantuanFarmNodeRun' -or $NodeRun.Version -ne 1 -or
		$NodeRun.RunId -cne $Server.RunId -or $NodeRun.StageSha256 -ine $NodeStageSha256 -or
		$NodeRun.RootCertificateSha256 -ine $Server.RootCertificateSha256 -or
		$NodeRun.TcpReady -cne $true -or $NodeRun.ChildReaped -cne $true -or
		$NodeRun.Reason -cne 'STOP_REQUESTED' -or
		$NodeRun.Pid -isnot [long] -or $NodeRun.Pid -le 0 -or
		$NodeRun.StdoutBytes -isnot [long] -or $NodeRun.StdoutBytes -lt 1 -or
		$NodeRun.StdoutBytes -gt 8388608 -or
		$NodeRun.StdoutSha256 -cnotmatch '^[a-fA-F0-9]{64}$' -or
		$NodeRun.NodeExecutableSha256 -cnotmatch '^[a-fA-F0-9]{64}$' -or
		$NodeRun.ConfigSha256 -cnotmatch '^[a-fA-F0-9]{64}$' -or
		$NodeRun.CertificateSha256 -cnotmatch '^[a-fA-F0-9]{64}$' -or
		$NodeRun.NodeExecutableSha256 -ine $Stage.NodeExecutableSha256 -or
		$NodeRun.ConfigSha256 -ine $Stage.ConfigSha256 -or
		$NodeRun.CertificateSha256 -ine $Stage.CertificateSha256) {
		throw 'Node run receipt is not a successful pinned owned-child record'
	}
	$NodeLogPath = [IO.Path]::GetFullPath([string]$NodeRun.StdoutPath)
	if ([IO.Path]::GetDirectoryName($NodeLogPath) -ine $StageDirectory -or
		[IO.Path]::GetFileName($NodeLogPath) -cne 'node.stdout.log') {
		throw 'Node stdout log is outside the pinned run stage'
	}
	$NodeFile = Get-Item -LiteralPath $NodeLogPath -ErrorAction Stop
	if ($NodeFile.Length -ne $NodeRun.StdoutBytes -or
		(Get-FileHash -LiteralPath $NodeLogPath -Algorithm SHA256).Hash -ine $NodeRun.StdoutSha256) {
		throw 'Node stdout log differs from owned-child receipt'
	}
	$TlsRecords = [Collections.Generic.List[object]]::new()
	foreach ($Line in [IO.File]::ReadAllLines($NodeFile.FullName)) {
		if ($Line.Length -gt 4096) { throw 'Node JSON log line exceeds fixed bound' }
		if (-not $Line.Contains('[Content:TLS] authenticated manifest RPC')) { continue }
		try { $Record = $Line | ConvertFrom-Json -AsHashtable -ErrorAction Stop }
		catch { throw 'Node TLS record is not valid JSON' }
		if ($Record.msg -cne '[Content:TLS] authenticated manifest RPC') { continue }
		if ($Record.request_id -cne $Server.RequestId) { continue }
		$TlsRecords.Add([pscustomobject]@{ Record = $Record; Line = $Line })
	}
	if ($TlsRecords.Count -ne 1) { throw 'expected one Node TLS record for the exact authenticated request ID' }
	$Record = $TlsRecords[0].Record
	$Cipher = [string]$Record.cipher_suite
	$Version = [string]$Record.tls_version
	$CipherValid = switch -CaseSensitive ($Version) {
		'TLSv1.3' { $Cipher -cin @('TLS_AES_128_GCM_SHA256', 'TLS_AES_256_GCM_SHA384',
			'TLS_CHACHA20_POLY1305_SHA256'); break }
		'TLSv1.2' { $Cipher -cmatch '^TLS_ECDHE_(RSA|ECDSA)_WITH_(AES_128_GCM_SHA256|AES_256_GCM_SHA384|CHACHA20_POLY1305_SHA256)$'; break }
		default { $false }
	}
	if ($Record.level -cne 'INFO' -or
		$Record.project_id -cne $Server.ProjectId -or
		$Record.package_version -isnot [long] -or
		$Record.package_version -ne $Server.PackageVersion -or
		$Record.principal_id -cne 'farm-server' -or
		$Record.transport -cne 'grpc_tls' -or -not $CipherValid) {
		throw 'Node TLS record differs from the request, package or accepted TLS suite'
	}
	$LineBytes = [Text.UTF8Encoding]::new($false).GetBytes($TlsRecords[0].Line)
	$LineHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($LineBytes)).ToLowerInvariant()
	return [ordered]@{
		Format = 'GargantuanFarmNodeTlsLogMatch'; Version = 1
		State = 'OFFLINE_LOG_MATCH_BOUND_TO_PINNED_NODE_RUN'
		RunId = $Server.RunId; RequestId = $Server.RequestId
		ProjectId = $Server.ProjectId; PackageVersion = $Server.PackageVersion
		RootCertificateSha256 = $Server.RootCertificateSha256
		ManifestSha256 = $Server.ManifestSha256
		TlsVersion = $Version; CipherSuite = $Cipher
		PrincipalId = $Record.principal_id
		NodeLogSha256 = (Get-FileHash -LiteralPath $NodeLogPath -Algorithm SHA256).Hash.ToLowerInvariant()
		NodeRecordSha256 = $LineHash
		NodeRunReceiptSha256 = $RunDigest; NodeStageSha256 = $NodeStageSha256.ToLowerInvariant()
		Source = 'gargantuan-node/ContentStreaming.GetManifest'
	}
}

function Write-NodeTlsLogMatchReceipt {
	param([string]$Path, [System.Collections.IDictionary]$Receipt)
	$Bytes = [Text.UTF8Encoding]::new($false).GetBytes(($Receipt | ConvertTo-Json -Depth 4))
	if ($Bytes.Length -gt 4096) { throw 'Node TLS match receipt exceeds fixed byte bound' }
	$Stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write,
		[IO.FileShare]::None)
	try { $Stream.Write($Bytes) } finally { $Stream.Dispose() }
}

$Receipt = Get-NodeTlsLogMatchReceipt -ServerReceiptPath $ServerReceiptPath -NodeStagePath $NodeStagePath `
	-NodeRunReceiptPath $NodeRunReceiptPath -NodeRunReceiptSha256 $NodeRunReceiptSha256 `
	-NodeStageSha256 $NodeStageSha256
Write-NodeTlsLogMatchReceipt -Path $OutputPath -Receipt $Receipt
Write-Output '[Qualification:FarmNodeTls] OFFLINE_LOG_MATCH_BOUND_TO_PINNED_NODE_RUN'
