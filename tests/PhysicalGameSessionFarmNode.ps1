#requires -Version 7.0
# Source-only candidate for the 32-client Node provider. Prepare creates a new,
# pinned local config; Run owns one bounded child. TCP readiness is not TLS proof.
param(
	[Parameter(Mandatory = $true)][ValidateSet('Prepare', 'Run')][string]$Mode,
	[Parameter(Mandatory = $true)][string]$StageRoot,
	[string]$RunManifestPath,
	[ValidatePattern('^[a-fA-F0-9]{64}$')][string]$RunManifestSha256,
	[string]$ServerPackageRoot,
	[string]$DescriptorPath,
	[ValidatePattern('^[a-fA-F0-9]{64}$')][string]$DescriptorSha256,
	[string]$NodeExecutablePath,
	[ValidatePattern('^[a-fA-F0-9]{64}$')][string]$NodeExecutableSha256,
	[ValidatePattern('^[a-fA-F0-9]{40}$')][string]$NodeSourceCommit,
	[string]$CertificatePath,
	[string]$PrivateKeyPath,
	[string]$RootCertificatePath,
	[ValidatePattern('^[a-fA-F0-9]{64}$')][string]$StageSha256,
	[ValidateRange(60, 1200)][int]$MaximumRuntimeSeconds = 900
)

$ErrorActionPreference = 'Stop'

function Assert-FilePin {
	param([string]$Path, [string]$Sha256, [long]$MaximumBytes = 0)
	$Resolved = [IO.Path]::GetFullPath($Path)
	$Item = Get-Item -LiteralPath $Resolved -ErrorAction Stop
	if ($Item.PSIsContainer -or ($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
		($MaximumBytes -gt 0 -and $Item.Length -gt $MaximumBytes)) {
		throw 'pinned Node input is not a bounded regular file'
	}
	if ($Sha256 -and (Get-FileHash -LiteralPath $Resolved -Algorithm SHA256).Hash -ine $Sha256) {
		throw 'pinned Node input SHA-256 mismatch'
	}
	return $Resolved
}

function Import-EndpointValidators {
	param([string]$ExpectedSha256)
	$Source = Assert-FilePin -Path (Join-Path $PSScriptRoot 'PhysicalGameSessionFarmEndpoint.ps1') `
		-Sha256 $ExpectedSha256 -MaximumBytes 1048576
	$Bytes = [IO.File]::ReadAllBytes($Source)
	$Actual = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant()
	if ($Actual -cne $ExpectedSha256) { throw 'imported endpoint validator changed while reading' }
	$Text = [Text.UTF8Encoding]::new($false, $true).GetString($Bytes)
	$Tokens = $null; $Errors = $null
	$Ast = [Management.Automation.Language.Parser]::ParseInput($Text, [ref]$Tokens, [ref]$Errors)
	if ($Errors.Count -ne 0) { throw 'role-local endpoint validator source has a syntax error' }
	$Names = @('Assert-RunManifest', 'Read-PinnedRunManifest', 'Assert-LocalPackagePins',
		'Assert-DeploymentManifest')
	$Definitions = @($Ast.FindAll({ param($Node)
		$Node -is [Management.Automation.Language.FunctionDefinitionAst] -and
		$Node.Name -in $Names
	}, $true))
	if ($Definitions.Count -ne $Names.Count) { throw 'role-local package validator set is incomplete' }
	return $Definitions
}

function Quote-Toml {
	param([string]$Value)
	if ($Value -match '[\x00-\x1f\x7f]') { throw 'Node TOML field contains control characters' }
	return '"' + $Value.Replace('\', '/').Replace('"', '\"') + '"'
}

function Assert-Certificate {
	param([string]$Certificate, [string]$PrivateKey, [string]$Root, [string]$HostName)
	$Leaf = [Security.Cryptography.X509Certificates.X509Certificate2]::CreateFromPemFile(
		$Certificate, $PrivateKey)
	$Ca = [Security.Cryptography.X509Certificates.X509Certificate2]::new($Root)
	try {
		$Now = [DateTime]::UtcNow
		if (-not $Leaf.HasPrivateKey -or $Leaf.NotBefore.ToUniversalTime() -gt $Now -or
			$Leaf.NotAfter.ToUniversalTime() -le $Now.AddMinutes(20)) {
			throw 'Node TLS certificate/key is invalid or too close to expiry'
		}
		$San = $Leaf.Extensions | Where-Object {
			$_ -is [Security.Cryptography.X509Certificates.X509SubjectAlternativeNameExtension]
		} | Select-Object -First 1
		$Address = $null
		$Matches = if ([Net.IPAddress]::TryParse($HostName, [ref]$Address)) {
			$San -and @($San.EnumerateIPAddresses() | Where-Object { $_.Equals($Address) }).Count -gt 0
		} else {
			$San -and @($San.EnumerateDnsNames() | Where-Object {
				$_.Equals($HostName, [StringComparison]::OrdinalIgnoreCase)
			}).Count -gt 0
		}
		if (-not $Matches) { throw 'Node TLS certificate SAN does not match the endpoint' }
		$Chain = [Security.Cryptography.X509Certificates.X509Chain]::new()
		try {
			$Chain.ChainPolicy.TrustMode = [Security.Cryptography.X509Certificates.X509ChainTrustMode]::CustomRootTrust
			[void]$Chain.ChainPolicy.CustomTrustStore.Add($Ca)
			$Chain.ChainPolicy.RevocationMode = [Security.Cryptography.X509Certificates.X509RevocationMode]::NoCheck
			if (-not $Chain.Build($Leaf)) { throw 'Node TLS certificate does not chain to the pinned root CA' }
		} finally { $Chain.Dispose() }
		return $Leaf.Thumbprint.ToLowerInvariant()
	} finally { $Leaf.Dispose(); $Ca.Dispose() }
}

function Assert-PackageIdentity {
	param([string]$PackageRoot, [string]$DescriptorFile)
	$Descriptor = Get-Content -LiteralPath $DescriptorFile -Raw | ConvertFrom-Json -AsHashtable
	$Package = Get-Content -LiteralPath (Join-Path $PackageRoot 'game.package.json') -Raw |
		ConvertFrom-Json -AsHashtable
	$Content = Get-Content -LiteralPath (Join-Path $PackageRoot 'content/content.manifest.json') -Raw |
		ConvertFrom-Json -AsHashtable
	if ($Descriptor.format -cne 'GargantuanQualifiedScalePackage' -or $Descriptor.version -ne 1 -or
		$Descriptor.server_package -cne 'Server' -or $Descriptor.expected_clients -ne 32 -or
		[string]$Descriptor.project_id -cnotmatch '^[a-f0-9]{32}$' -or
		$Descriptor.revision -isnot [long] -or $Descriptor.revision -le 0 -or
		$Package.ProjectId -cne $Descriptor.project_id -or
		$Content.ProjectId -cne $Descriptor.project_id -or
		$Package.Revision -ne $Descriptor.revision -or
		$Content.PackageVersion -ne $Descriptor.revision) {
		throw 'qualified scale descriptor, Server package, and content manifest identity differ'
	}
	return [pscustomobject]@{ ProjectId = $Descriptor.project_id; Revision = [long]$Descriptor.revision }
}

function Write-NewFile {
	param([string]$Path, [byte[]]$Bytes)
	$Stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write,
		[IO.FileShare]::None)
	try { $Stream.Write($Bytes) } finally { $Stream.Dispose() }
}

function Get-Sha256 {
	param([string]$Path)
	return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-NodeEndpoint {
	param([string]$Endpoint)
	$Parts = $Endpoint -split ':'
	$Port = 0
	# The Node child is owned on the Server endpoint. A remote listener cannot be
	# reaped by this supervisor and must use a separate reviewed profile.
	if ($Parts.Count -ne 2 -or $Parts[0] -cne '127.0.0.1' -or
		-not [int]::TryParse($Parts[1], [ref]$Port) -or $Port -lt 1024 -or
		$Port -gt 65535) { throw 'owned Node endpoint must be 127.0.0.1:port (1024..65535)' }
	return $Port
}

function Assert-DisjointStage {
	param([string]$Stage, [string[]]$Inputs)
	$Resolved = [IO.Path]::GetFullPath($Stage).TrimEnd('\', '/')
	$Ancestor = [IO.Path]::GetDirectoryName($Resolved)
	while ($Ancestor) {
		if (Test-Path -LiteralPath $Ancestor) {
			$Item = Get-Item -LiteralPath $Ancestor -Force
			if ($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
				throw 'Node stage ancestor is a reparse point'
			}
		}
		$Next = [IO.Path]::GetDirectoryName($Ancestor)
		if ($Next -eq $Ancestor) { break }
		$Ancestor = $Next
	}
	foreach ($Input in $Inputs) {
		$Path = [IO.Path]::GetFullPath($Input).TrimEnd('\', '/')
		if ($Resolved.Equals($Path, [StringComparison]::OrdinalIgnoreCase) -or
			$Resolved.StartsWith($Path + [IO.Path]::DirectorySeparatorChar,
				[StringComparison]::OrdinalIgnoreCase) -or
			$Path.StartsWith($Resolved + [IO.Path]::DirectorySeparatorChar,
				[StringComparison]::OrdinalIgnoreCase)) {
			throw 'Node stage must be disjoint from its inputs'
		}
	}
	if (Test-Path -LiteralPath $Resolved) { throw 'Node stage root already exists' }
	return $Resolved
}

if ($Mode -eq 'Prepare') {
	$ValidatorPath = Join-Path $PSScriptRoot 'PhysicalGameSessionFarmEndpoint.ps1'
	$ValidatorSha256 = Get-Sha256 (Assert-FilePin -Path $ValidatorPath -MaximumBytes 1048576)
	foreach ($Definition in @(Import-EndpointValidators -ExpectedSha256 $ValidatorSha256)) {
		. ([scriptblock]::Create($Definition.Extent.Text))
	}
	foreach ($Required in @($RunManifestPath, $RunManifestSha256, $ServerPackageRoot,
		$DescriptorPath, $DescriptorSha256, $NodeExecutablePath, $NodeExecutableSha256,
		$NodeSourceCommit, $CertificatePath, $PrivateKeyPath, $RootCertificatePath)) {
		if ([string]::IsNullOrWhiteSpace($Required)) { throw 'Prepare needs every pinned Node input' }
	}
	$Manifest = Read-PinnedRunManifest -Path $RunManifestPath -ExpectedSha256 $RunManifestSha256
	if ($Manifest.Provider -cne 'Node') { throw 'Node staging requires a Node farm manifest' }
	$Port = Assert-NodeEndpoint -Endpoint $Manifest.NodeEndpoint
	$Server = [IO.Path]::GetFullPath($ServerPackageRoot)
	[void](Assert-LocalPackagePins -LocalRoot $Server -LocalRole Server -RunManifest $Manifest)
	$Descriptor = Assert-FilePin -Path $DescriptorPath -Sha256 $DescriptorSha256 -MaximumBytes 65536
	$Identity = Assert-PackageIdentity -PackageRoot $Server -DescriptorFile $Descriptor
	$Executable = Assert-FilePin -Path $NodeExecutablePath -Sha256 $NodeExecutableSha256
	$Certificate = Assert-FilePin -Path $CertificatePath -MaximumBytes 65536
	$Key = Assert-FilePin -Path $PrivateKeyPath -MaximumBytes 65536
	$Root = Assert-FilePin -Path $RootCertificatePath -Sha256 $Manifest.NodeRootCertificateSha256 `
		-MaximumBytes 65536
	$CertificateThumbprint = Assert-Certificate -Certificate $Certificate -PrivateKey $Key `
		-Root $Root -HostName '127.0.0.1'
	if ([string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable(
		$Manifest.NodeTokenEnvironment))) { throw 'Node token environment is empty' }
	$Stage = Assert-DisjointStage -Stage $StageRoot -Inputs @($Server, $Descriptor, $Executable,
		$Certificate, $Key, $Root, $RunManifestPath)
	$ConfigText = @(
		'schema_version = 1', '', '[host]',
		('id = ' + (Quote-Toml "farm32-$($Manifest.RunId)")),
		('listen = ' + (Quote-Toml $Manifest.NodeEndpoint)), '',
		'[tls]', 'enabled = true', 'insecure_remote = false',
		('cert_file = ' + (Quote-Toml $Certificate)),
		('key_file = ' + (Quote-Toml $Key)), '',
		'[auth]', 'provider = "static"', '',
		'[[auth.static_principals]]', 'id = "farm-server"', 'kind = "game_server"',
		'tenant_id = "farm32"', 'host_id = "farm32-server"',
		'node_instance_id = "farm32-node"',
		('token_env = ' + (Quote-Toml $Manifest.NodeTokenEnvironment)),
		'capabilities = ["content.manifest.read", "content.blob.read"]', '',
		'[core]', 'enabled = false', '', '[diagnostics]', 'enabled = false', '',
		'[entitlements]', 'enabled = false', '', '[content]', 'enabled = true',
		'version = "v1"', 'provider = "filesystem"',
		'max_manifest_bytes = 524288', 'max_content_bytes = 1048576',
		'max_content_key_bytes = 128', 'max_packages = 1', '',
		'[[content.packages]]', 'tenant_id = "farm32"',
		('project_id = ' + (Quote-Toml $Identity.ProjectId)),
		("package_version = $($Identity.Revision)"),
		('directory = ' + (Quote-Toml $Server)), '',
		'[limits]', 'max_send_bytes = 1049088', '', '[logging]', 'level = "error"', ''
	) -join "`n"
	[void][IO.Directory]::CreateDirectory($Stage)
	$ConfigPath = Join-Path $Stage 'node.toml'
	Write-NewFile -Path $ConfigPath -Bytes ([Text.UTF8Encoding]::new($false).GetBytes($ConfigText))
	$Proof = [ordered]@{
		Format = 'GargantuanFarmNodeStage'; Version = 1
		RunId = $Manifest.RunId; SourceCommit = $Manifest.SourceCommit
		RunManifestPath = [IO.Path]::GetFullPath($RunManifestPath)
		RunManifestSha256 = $RunManifestSha256.ToLowerInvariant()
		ServerPackageRoot = $Server
		DescriptorPath = $Descriptor; DescriptorSha256 = $DescriptorSha256.ToLowerInvariant()
		ProjectId = $Identity.ProjectId; Revision = $Identity.Revision
		NodeSourceCommit = $NodeSourceCommit.ToLowerInvariant()
		NodeExecutablePath = $Executable; NodeExecutableSha256 = $NodeExecutableSha256.ToLowerInvariant()
		NodeEndpoint = $Manifest.NodeEndpoint; NodeTokenEnvironment = $Manifest.NodeTokenEnvironment
		RootCertificatePath = $Root; RootCertificateSha256 = (Get-Sha256 $Root)
		CertificatePath = $Certificate; CertificateSha256 = (Get-Sha256 $Certificate)
		CertificateThumbprint = $CertificateThumbprint
		PrivateKeyPath = $Key; ConfigSha256 = (Get-Sha256 $ConfigPath)
		HelperSha256 = (Get-Sha256 $PSCommandPath)
		EndpointValidatorSha256 = $ValidatorSha256
		PreparedUtc = [DateTimeOffset]::UtcNow.ToString('O')
		Status = 'STAGED_NOT_TLS_PROVEN'
	}
	$StagePath = Join-Path $Stage 'node-stage.json'
	Write-NewFile -Path $StagePath -Bytes ([Text.UTF8Encoding]::new($false).GetBytes(
		($Proof | ConvertTo-Json -Depth 5)))
	Write-Output "[Qualification:FarmNode] STAGED run=$($Manifest.RunId) stage_sha256=$(Get-Sha256 $StagePath)"
	return
}

if (-not $StageSha256) { throw 'Run needs the out-of-band Node stage SHA-256 pin' }
$Stage = [IO.Path]::GetFullPath($StageRoot)
$StagePath = Assert-FilePin -Path (Join-Path $Stage 'node-stage.json') -Sha256 $StageSha256 `
	-MaximumBytes 65536
$Proof = Get-Content -LiteralPath $StagePath -Raw | ConvertFrom-Json -AsHashtable
if ($Proof.Format -cne 'GargantuanFarmNodeStage' -or $Proof.Version -ne 1 -or
	$Proof.Status -cne 'STAGED_NOT_TLS_PROVEN' -or
	$Proof.HelperSha256 -cne (Get-Sha256 $PSCommandPath) -or
	[string]$Proof.EndpointValidatorSha256 -cnotmatch '^[a-f0-9]{64}$' -or
	$Proof.NodeEndpoint -cnotmatch '^127\.0\.0\.1:[0-9]{4,5}$') {
	throw 'Node stage schema or helper pin is invalid'
}
$ValidatorPath = Join-Path $PSScriptRoot 'PhysicalGameSessionFarmEndpoint.ps1'
[void](Assert-FilePin -Path $ValidatorPath -Sha256 $Proof.EndpointValidatorSha256 -MaximumBytes 1048576)
foreach ($Definition in @(Import-EndpointValidators -ExpectedSha256 $Proof.EndpointValidatorSha256)) {
	. ([scriptblock]::Create($Definition.Extent.Text))
}
$Port = Assert-NodeEndpoint -Endpoint $Proof.NodeEndpoint
$Manifest = Read-PinnedRunManifest -Path $Proof.RunManifestPath `
	-ExpectedSha256 $Proof.RunManifestSha256
if ($Manifest.Provider -cne 'Node' -or $Manifest.RunId -cne $Proof.RunId -or
	$Manifest.NodeEndpoint -cne $Proof.NodeEndpoint -or
	$Manifest.NodeTokenEnvironment -cne $Proof.NodeTokenEnvironment -or
	$Manifest.NodeRootCertificateSha256 -cne $Proof.RootCertificateSha256) {
	throw 'Node stage and pinned farm run manifest disagree'
}
[void](Assert-LocalPackagePins -LocalRoot $Proof.ServerPackageRoot -LocalRole Server `
	-RunManifest $Manifest)
[void](Assert-FilePin -Path $Proof.DescriptorPath -Sha256 $Proof.DescriptorSha256 -MaximumBytes 65536)
$Identity = Assert-PackageIdentity -PackageRoot $Proof.ServerPackageRoot `
	-DescriptorFile $Proof.DescriptorPath
if ($Identity.ProjectId -cne $Proof.ProjectId -or $Identity.Revision -ne $Proof.Revision) {
	throw 'Node stage package identity changed'
}
$Executable = Assert-FilePin -Path $Proof.NodeExecutablePath -Sha256 $Proof.NodeExecutableSha256
$Config = Assert-FilePin -Path (Join-Path $Stage 'node.toml') -Sha256 $Proof.ConfigSha256 `
	-MaximumBytes 65536
$Root = Assert-FilePin -Path $Proof.RootCertificatePath -Sha256 $Proof.RootCertificateSha256 `
	-MaximumBytes 65536
$Certificate = Assert-FilePin -Path $Proof.CertificatePath -Sha256 $Proof.CertificateSha256 `
	-MaximumBytes 65536
$Key = Assert-FilePin -Path $Proof.PrivateKeyPath -MaximumBytes 65536
if ((Assert-Certificate -Certificate $Certificate -PrivateKey $Key -Root $Root `
	-HostName '127.0.0.1') -cne $Proof.CertificateThumbprint -or
	[string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable(
		$Proof.NodeTokenEnvironment))) {
	throw 'Node TLS inputs or token environment changed'
}
$ReceiptPath = Join-Path $Stage 'node-run.json'
if (Test-Path -LiteralPath $ReceiptPath) { throw 'Node stage was already run' }
$ClaimPath = Join-Path $Stage 'node-run.claim'
Write-NewFile -Path $ClaimPath -Bytes ([Text.UTF8Encoding]::new($false).GetBytes($Proof.RunId))
$Info = [Diagnostics.ProcessStartInfo]::new()
$Info.FileName = $Executable
$Info.WorkingDirectory = $Stage
$Info.UseShellExecute = $false
$Info.CreateNoWindow = $true
$Info.RedirectStandardOutput = $true
$Info.RedirectStandardError = $true
foreach ($Argument in @('serve', '--config', $Config)) { [void]$Info.ArgumentList.Add($Argument) }
$Child = [Diagnostics.Process]::new()
$Child.StartInfo = $Info
$StartedUtc = [DateTimeOffset]::UtcNow
$Reason = 'START_FAILED'; $Ready = $false; $ReadyRecorded = $false; $ExitCode = $null
$Started = $false; $ChildProcessId = 0
try {
	if (-not $Child.Start()) { throw 'Node child did not start' }
	$Started = $true; $ChildProcessId = $Child.Id
	$Stdout = $Child.StandardOutput.BaseStream.CopyToAsync([IO.Stream]::Null)
	$Stderr = $Child.StandardError.BaseStream.CopyToAsync([IO.Stream]::Null)
	$Deadline = $StartedUtc.AddSeconds($MaximumRuntimeSeconds)
	$ReadyDeadline = $StartedUtc.AddSeconds(15)
	while (-not $Child.HasExited -and [DateTimeOffset]::UtcNow -lt $Deadline) {
		if (-not $Ready -and [DateTimeOffset]::UtcNow -lt $ReadyDeadline) {
			$Socket = [Net.Sockets.TcpClient]::new()
			try {
				$Connect = $Socket.ConnectAsync('127.0.0.1', $Port)
				$Ready = $Connect.Wait(200) -and $Socket.Connected
			} catch { } finally { $Socket.Dispose() }
		}
		if (-not $Ready -and [DateTimeOffset]::UtcNow -ge $ReadyDeadline) {
			$Reason = 'TCP_READY_TIMEOUT'; break
		}
		if ($Ready -and -not $ReadyRecorded) {
			Write-NewFile -Path (Join-Path $Stage 'node-tcp-ready.json') -Bytes `
				([Text.UTF8Encoding]::new($false).GetBytes((
					[ordered]@{ Format = 'GargantuanFarmNodeTcpReady'; Version = 1
						RunId = $Proof.RunId; Pid = $Child.Id; TlsProven = $false
						ObservedUtc = [DateTimeOffset]::UtcNow.ToString('O') } |
					ConvertTo-Json -Depth 4)))
			$ReadyRecorded = $true
		}
		$StopPath = Join-Path $Stage 'stop.request'
		if (Test-Path -LiteralPath $StopPath -PathType Leaf) {
			$StopPath = Assert-FilePin -Path $StopPath -MaximumBytes 128
			if ((Get-Content -LiteralPath $StopPath -Raw).Trim() -cne $Proof.RunId) {
				$Reason = 'INVALID_STOP_REQUEST'; break
			}
			$Reason = 'STOP_REQUESTED'; break
		}
		Start-Sleep -Milliseconds 100
	}
	if ($Child.HasExited) { $Reason = 'CHILD_EXITED'; $ExitCode = $Child.ExitCode }
	elseif ([DateTimeOffset]::UtcNow -ge $Deadline) { $Reason = 'HARD_DEADLINE' }
} catch {
	$Reason = 'SUPERVISOR_ERROR'
	throw
} finally {
	if ($Started) {
		if (-not $Child.HasExited) { try { $Child.Kill($true) } catch { } }
		try { [void]$Child.WaitForExit(10000) } catch { }
		if ($Child.HasExited) { $ExitCode = $Child.ExitCode }
	}
	if ($Stdout) { try { [void]$Stdout.Wait(10000) } catch { } }
	if ($Stderr) { try { [void]$Stderr.Wait(10000) } catch { } }
	$Receipt = [ordered]@{
		Format = 'GargantuanFarmNodeRun'; Version = 1
		RunId = $Proof.RunId; StageSha256 = $StageSha256.ToLowerInvariant()
		NodeExecutableSha256 = $Proof.NodeExecutableSha256
		ConfigSha256 = $Proof.ConfigSha256; CertificateSha256 = $Proof.CertificateSha256
		RootCertificateSha256 = $Proof.RootCertificateSha256
		Pid = $ChildProcessId
		StartedUtc = $StartedUtc.ToString('O')
		EndedUtc = [DateTimeOffset]::UtcNow.ToString('O')
		TcpReady = $Ready; TlsProven = $false; Reason = $Reason
		ExitCode = $ExitCode; ChildReaped = [bool]($Started -and $Child.HasExited)
		LogsDiscarded = $true
	}
	Write-NewFile -Path $ReceiptPath -Bytes ([Text.UTF8Encoding]::new($false).GetBytes(
		($Receipt | ConvertTo-Json -Depth 5)))
	$Child.Dispose()
}
Write-Output "[Qualification:FarmNode] STOPPED run=$($Proof.RunId) reason=$Reason tcp_ready=$Ready receipt=$ReceiptPath"
if ($Reason -cne 'STOP_REQUESTED' -or -not $Ready -or -not $Receipt.ChildReaped) { exit 1 }
