#requires -Version 7.0
# Harmless local package/TLS fixture and an owned mock Node child. No physical
# endpoint, real provider, capture service, or installed tool is touched.
param([string]$OfficialNodeBinary, [string]$GoExecutable = 'go')
$ErrorActionPreference = 'Stop'
$OriginalSource = Join-Path $PSScriptRoot 'PhysicalGameSessionFarmNode.ps1'
$Tokens = $null; $Errors = $null
[void][Management.Automation.Language.Parser]::ParseFile($OriginalSource, [ref]$Tokens, [ref]$Errors)
if ($Errors.Count -ne 0) { throw "Node helper syntax failed: $($Errors[0].Message)" }

function Get-Pin { param([string]$Path)
	return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Expect-Rejection { param([scriptblock]$Case, [string]$Reason)
	$Rejected = $false
	try { & $Case | Out-Null } catch { $Rejected = $true }
	if (-not $Rejected) { throw "expected rejection: $Reason" }
}
function Write-TestCertificate {
	param([string]$CertificatePath, [string]$KeyPath, [string]$IpAddress)
	$Rsa = [Security.Cryptography.RSA]::Create(2048)
	try {
		$Request = [Security.Cryptography.X509Certificates.CertificateRequest]::new(
			'CN=FarmNodeMock', $Rsa, [Security.Cryptography.HashAlgorithmName]::SHA256,
			[Security.Cryptography.RSASignaturePadding]::Pkcs1)
		$Request.CertificateExtensions.Add(
			[Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new(
				$true, $false, 0, $true))
		$Request.CertificateExtensions.Add(
			[Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
				[Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature -bor
				[Security.Cryptography.X509Certificates.X509KeyUsageFlags]::KeyCertSign, $true))
		$San = [Security.Cryptography.X509Certificates.SubjectAlternativeNameBuilder]::new()
		$San.AddIpAddress([Net.IPAddress]::Parse($IpAddress))
		$Request.CertificateExtensions.Add($San.Build())
		$Certificate = $Request.CreateSelfSigned([DateTimeOffset]::UtcNow.AddMinutes(-5),
			[DateTimeOffset]::UtcNow.AddHours(2))
		try {
			[IO.File]::WriteAllText($CertificatePath, $Certificate.ExportCertificatePem())
			[IO.File]::WriteAllText($KeyPath, $Rsa.ExportPkcs8PrivateKeyPem())
		} finally { $Certificate.Dispose() }
	} finally { $Rsa.Dispose() }
}

$Root = Join-Path ([IO.Path]::GetTempPath()) ('farm-node-test-' + [Guid]::NewGuid().ToString('N'))
$Package = Join-Path $Root 'Server'
[void][IO.Directory]::CreateDirectory((Join-Path $Package 'content'))
$OldToken = [Environment]::GetEnvironmentVariable('GARGANTUAN_ENGINE_ADAPTER_TOKEN')
try {
	$Tools = Join-Path $Root 'Tools'
	[void][IO.Directory]::CreateDirectory($Tools)
	$PinnedGo = Join-Path $Tools 'go.exe'
	Copy-Item -LiteralPath (Get-Command $GoExecutable -CommandType Application).Source -Destination $PinnedGo
	$Source = Join-Path $Tools 'PhysicalGameSessionFarmNode.ps1'
	$Validator = Join-Path $Tools 'PhysicalGameSessionFarmEndpoint.ps1'
	Copy-Item -LiteralPath $OriginalSource -Destination $Source
	Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'PhysicalGameSessionFarmEndpoint.ps1') `
		-Destination $Validator
	$ProjectId = 'fedcba9876543210fedcba9876543210'
	$Revision = 23L
	$Files = @('GargantuanServer.exe', 'game.package.json', 'content/content.manifest.json')
	[IO.File]::WriteAllText((Join-Path $Package 'GargantuanServer.exe'), 'server mock')
	[IO.File]::WriteAllText((Join-Path $Package 'game.package.json'),
		(@{ ProjectId = $ProjectId; Revision = $Revision } | ConvertTo-Json))
	[IO.File]::WriteAllText((Join-Path $Package 'content/content.manifest.json'),
		(@{ ProjectId = $ProjectId; PackageVersion = $Revision } | ConvertTo-Json))
	$Deployment = [ordered]@{
		Format = 'GargantuanFarmDeployment'; Version = 1; SourceCommit = 'a' * 40
		Files = @($Files | ForEach-Object { $File = Join-Path $Package $_
			[ordered]@{ Path = $_; Bytes = ([IO.FileInfo]$File).Length; Sha256 = (Get-Pin $File) }
		})
	}
	[IO.File]::WriteAllText((Join-Path $Package 'deployment-sha256.json'),
		($Deployment | ConvertTo-Json -Depth 6))
	$DescriptorPath = Join-Path $Root 'qualification-descriptor.json'
	$Descriptor = [ordered]@{
		format = 'GargantuanQualifiedScalePackage'; version = 1
		project_id = $ProjectId; revision = $Revision; server_package = 'Server'
		expected_clients = 32
	}
	[IO.File]::WriteAllText($DescriptorPath, ($Descriptor | ConvertTo-Json -Depth 5))
	$Certificate = Join-Path $Root 'node-cert.pem'
	$PrivateKey = Join-Path $Root 'node-key.pem'
	Write-TestCertificate -CertificatePath $Certificate -KeyPath $PrivateKey -IpAddress '127.0.0.1'
	$WrongCertificate = Join-Path $Root 'wrong-cert.pem'
	$WrongKey = Join-Path $Root 'wrong-key.pem'
	Write-TestCertificate -CertificatePath $WrongCertificate -KeyPath $WrongKey -IpAddress '127.0.0.2'

	$MockSourceRoot = Join-Path $Root 'mock-source'
	[void][IO.Directory]::CreateDirectory($MockSourceRoot)
	$MockSource = Join-Path $MockSourceRoot 'mock-node.go'
	$MockExecutable = Join-Path $Root 'mock-node.exe'
	[IO.File]::WriteAllText((Join-Path $MockSourceRoot 'go.mod'),
		"module example.com/farm-node-mock`n`ngo 1.22`n")
	[IO.File]::WriteAllText($MockSource, @'
package main
import (
 "net"
 "os"
 "regexp"
)
func main() {
 if len(os.Args) != 4 || os.Args[1] != "serve" || os.Args[2] != "--config" { os.Exit(2) }
 b, e := os.ReadFile(os.Args[3]); if e != nil { os.Exit(3) }
 m := regexp.MustCompile(`(?m)^listen = "([^"]+)"$`).FindSubmatch(b)
 if len(m) != 2 { os.Exit(4) }
 l, e := net.Listen("tcp", string(m[1])); if e != nil { os.Exit(5) }
 defer l.Close()
 for { c, e := l.Accept(); if e != nil { return }; c.Close() }
}
'@)
	& git -C $MockSourceRoot init -q
	if ($LASTEXITCODE -ne 0) { throw 'mock Node Git initialization failed' }
	& git -C $MockSourceRoot add -- go.mod mock-node.go
	if ($LASTEXITCODE -ne 0) { throw 'mock Node source staging failed' }
	$CommitEmail = (& git config --get user.email 2>$null)
	if ([string]::IsNullOrWhiteSpace($CommitEmail)) {
		$CommitEmail = 'farm-node-test@example.invalid'
	}
	& git -C $MockSourceRoot -c user.name=FarmNodeTest -c "user.email=$CommitEmail" `
		commit -qm 'Create mock Node source'
	if ($LASTEXITCODE -ne 0) { throw 'mock Node source commit failed' }
	$MockCommit = (& git -C $MockSourceRoot rev-parse HEAD).Trim()
	Push-Location $MockSourceRoot
	try { & $GoExecutable build -buildvcs=true -trimpath -o $MockExecutable . }
	finally { Pop-Location }
	if ($LASTEXITCODE -ne 0) { throw 'mock Node build failed' }

	$Listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
	$Listener.Start()
	$Port = ([Net.IPEndPoint]$Listener.LocalEndpoint).Port
	$Listener.Stop()
	$Nonces = @(for ($Slot = 1; $Slot -le 32; $Slot++) { [string]([UInt64]4294967296 + $Slot) })
	$Manifest = [ordered]@{
		Format = 'GargantuanPhysicalFarmEndpoint'; Version = 1
		RunId = '12345678-1234-4234-8234-123456789abc'
		SourceCommit = 'a' * 40; Endpoint = '127.0.0.1:39450'
		Provider = 'Node'; ScaleWorkload = $true
		ClientFrames = 9000; ServerTicks = 10000; Nonces = $Nonces
		ServerSha256 = Get-Pin (Join-Path $Package 'GargantuanServer.exe')
		ServerPackageSha256 = Get-Pin (Join-Path $Package 'game.package.json')
		ServerContentManifestSha256 = Get-Pin (Join-Path $Package 'content/content.manifest.json')
		ServerDeploymentSha256 = Get-Pin (Join-Path $Package 'deployment-sha256.json')
		PlayerSha256 = 'b' * 64; PlayerPackageSha256 = 'c' * 64
		PlayerContentManifestSha256 = 'd' * 64; PlayerDeploymentSha256 = 'e' * 64
		NodeEndpoint = "127.0.0.1:$Port"
		NodeRootCertificateSha256 = Get-Pin $Certificate
		NodeTokenEnvironment = 'GARGANTUAN_ENGINE_ADAPTER_TOKEN'
	}
	$ManifestPath = Join-Path $Root 'run-manifest.json'
	[IO.File]::WriteAllText($ManifestPath, ($Manifest | ConvertTo-Json -Depth 6))
	[Environment]::SetEnvironmentVariable('GARGANTUAN_ENGINE_ADAPTER_TOKEN', 'test-only-value')
	$Stage = Join-Path $Root 'Stage'
	$Prepare = @{
		Mode = 'Prepare'; StageRoot = $Stage; RunManifestPath = $ManifestPath
		RunManifestSha256 = Get-Pin $ManifestPath; ServerPackageRoot = $Package
		DescriptorPath = $DescriptorPath; DescriptorSha256 = Get-Pin $DescriptorPath
		NodeExecutablePath = $MockExecutable; NodeExecutableSha256 = Get-Pin $MockExecutable
		NodeSourceCommit = $MockCommit; GoExecutablePath = $PinnedGo
		GoExecutableSha256 = Get-Pin $PinnedGo; CertificatePath = $Certificate
		PrivateKeyPath = $PrivateKey; RootCertificatePath = $Certificate
	}
	$Bad = $Prepare.Clone(); $Bad.StageRoot = Join-Path $Root 'BadRevision'
	$Bad.NodeSourceCommit = 'f' * 40
	Expect-Rejection { & $Source @Bad } 'binary VCS revision differs from declared Node source commit'
	$Bad = $Prepare.Clone(); $Bad.StageRoot = Join-Path $Root 'BadBuildMetadata'
	$Bad.NodeExecutablePath = Join-Path $Root 'not-a-go-binary.exe'
	[IO.File]::WriteAllText($Bad.NodeExecutablePath, 'not a Go binary')
	$Bad.NodeExecutableSha256 = Get-Pin $Bad.NodeExecutablePath
	Expect-Rejection { & $Source @Bad } 'hash-pinned binary lacks Go VCS metadata'
	$StageOutput = & $Source @Prepare
	if ($StageOutput -notmatch 'STAGED') { throw 'Node stage was not created' }
	$Proof = Get-Content -LiteralPath (Join-Path $Stage 'node-stage.json') -Raw | ConvertFrom-Json -AsHashtable
	$ConfigText = Get-Content -LiteralPath (Join-Path $Stage 'node.toml') -Raw
	if ($Proof.ProjectId -cne $ProjectId -or $Proof.Revision -ne $Revision -or
		$Proof.NodeSourceCommit -cne $MockCommit -or
		$Proof.NodeBinaryVcsStatus -cne 'MATCHED_CLEAN' -or
		$Proof.GoExecutableSha256 -cne (Get-Pin $PinnedGo) -or
		$ConfigText -notmatch [regex]::Escape($ProjectId) -or
		$ConfigText -notmatch 'package_version = 23' -or
		$ConfigText -notmatch '(?m)^level = "info"$' -or
		$ConfigText -match 'test-only-value' -or $Proof.Contains('TokenValue') -or
		$Proof.Contains('PrivateKeyPem')) {
		throw 'Node config identity or secret separation failed'
	}
	if ($OfficialNodeBinary) {
		& $OfficialNodeBinary validate-config --config (Join-Path $Stage 'node.toml') | Out-Null
		if ($LASTEXITCODE -ne 0) { throw 'official Node rejected generated farm config' }
	}
	$Claim = Join-Path $Stage 'node-run.claim'
	$StagePin = Get-Pin (Join-Path $Stage 'node-stage.json')
	Expect-Rejection {
		& $Source -Mode Run -StageRoot $Stage -StageSha256 ('0' * 64)
	} 'out-of-band Node stage pin mismatch'
	if (Test-Path -LiteralPath $Claim) { throw 'rejected stage consumed its run claim' }
	[IO.File]::AppendAllText($Validator, "`n# tampered after stage`n")
	Expect-Rejection {
		& $Source -Mode Run -StageRoot $Stage -StageSha256 $StagePin
	} 'imported endpoint validator pin mismatch'
	if (Test-Path -LiteralPath $Claim) { throw 'tampered validator consumed its run claim' }
	Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'PhysicalGameSessionFarmEndpoint.ps1') `
		-Destination $Validator -Force
	[IO.File]::AppendAllText($PinnedGo, 'tampered inspector')
	Expect-Rejection {
		& $Source -Mode Run -StageRoot $Stage -StageSha256 $StagePin
	} 'pinned Go metadata inspector changed after stage'
	if (Test-Path -LiteralPath $Claim) { throw 'tampered inspector consumed its run claim' }
	Copy-Item -LiteralPath (Get-Command $GoExecutable -CommandType Application).Source -Destination $PinnedGo -Force
	$Info = [Diagnostics.ProcessStartInfo]::new()
	$Info.FileName = (Get-Command pwsh).Source
	$Info.UseShellExecute = $false
	$Info.CreateNoWindow = $true
	$Info.RedirectStandardOutput = $true
	$Info.RedirectStandardError = $true
	foreach ($Argument in @('-NoProfile', '-File', $Source, '-Mode', 'Run', '-StageRoot',
		$Stage, '-StageSha256', $StagePin, '-MaximumRuntimeSeconds', '60')) {
		[void]$Info.ArgumentList.Add($Argument)
	}
	$Runner = [Diagnostics.Process]::new()
	$Runner.StartInfo = $Info
	try {
		if (-not $Runner.Start()) { throw 'Node test supervisor failed to start' }
		$Ready = $false
		for ($Attempt = 0; $Attempt -lt 100 -and -not $Ready; $Attempt++) {
			if ($Runner.HasExited) { break }
			$Socket = [Net.Sockets.TcpClient]::new()
			try {
				$Connected = $Socket.ConnectAsync('127.0.0.1', $Port)
				$Ready = $Connected.Wait(100) -and $Socket.Connected
			} catch { } finally { $Socket.Dispose() }
			if (-not $Ready) { Start-Sleep -Milliseconds 100 }
		}
		if (-not $Ready) {
			$Failure = if ($Runner.HasExited) {
				"exit=$($Runner.ExitCode) stdout=$($Runner.StandardOutput.ReadToEnd()) stderr=$($Runner.StandardError.ReadToEnd())"
			} else { 'runner remains active' }
			throw "mock Node did not become TCP ready: $Failure"
		}
		$ReadyMarker = Join-Path $Stage 'node-tcp-ready.json'
		for ($Attempt = 0; $Attempt -lt 50 -and -not (Test-Path -LiteralPath $ReadyMarker); $Attempt++) {
			Start-Sleep -Milliseconds 100
		}
		if (-not (Test-Path -LiteralPath $ReadyMarker)) { throw 'Node supervisor did not record TCP readiness' }
		[IO.File]::WriteAllText((Join-Path $Stage 'stop.request'), $Manifest.RunId)
		if (-not $Runner.WaitForExit(10000)) { throw 'Node supervisor did not reap in time' }
		$Output = $Runner.StandardOutput.ReadToEnd()
		$ErrorText = $Runner.StandardError.ReadToEnd()
		if ($Runner.ExitCode -ne 0) { throw "Node supervisor failed: $ErrorText $Output" }
	} finally {
		if (-not $Runner.HasExited) { $Runner.Kill($true); [void]$Runner.WaitForExit(10000) }
		$Runner.Dispose()
	}
	$Receipt = Get-Content -LiteralPath (Join-Path $Stage 'node-run.json') -Raw |
		ConvertFrom-Json -AsHashtable
	if (-not $Receipt.ChildReaped -or -not $Receipt.TcpReady -or $Receipt.TlsProven -or
		$Receipt.Reason -cne 'STOP_REQUESTED' -or -not (Test-Path -LiteralPath $Claim)) {
		throw 'Node child lifecycle or honest TLS classification failed'
	}
	if ($Receipt.LogsDiscarded -or $Receipt.StdoutBytes -gt 8MB -or
		$Receipt.StderrBytes -gt 8MB -or
		$Receipt.StdoutSha256 -cne (Get-Pin $Receipt.StdoutPath) -or
		$Receipt.StderrSha256 -cne (Get-Pin $Receipt.StderrPath)) {
		throw 'Node child log retention is not bounded and hash-pinned'
	}
	Expect-Rejection { & $Source -Mode Run -StageRoot $Stage -StageSha256 $StagePin } 'single-use Node stage'
	$BadDescriptor = Join-Path $Root 'bad-descriptor.json'
	$Descriptor.revision = 24
	[IO.File]::WriteAllText($BadDescriptor, ($Descriptor | ConvertTo-Json -Depth 5))
	$Bad = $Prepare.Clone(); $Bad.StageRoot = Join-Path $Root 'BadIdentity'
	$Bad.DescriptorPath = $BadDescriptor; $Bad.DescriptorSha256 = Get-Pin $BadDescriptor
	Expect-Rejection { & $Source @Bad } 'package/descriptor mismatch'
	$Bad = $Prepare.Clone(); $Bad.StageRoot = Join-Path $Root 'BadSan'
	$Bad.CertificatePath = $WrongCertificate; $Bad.PrivateKeyPath = $WrongKey
	$Bad.RootCertificatePath = $WrongCertificate
	$ChangedManifest = $Manifest | ConvertTo-Json -Depth 6 | ConvertFrom-Json -AsHashtable
	$ChangedManifest.NodeRootCertificateSha256 = Get-Pin $WrongCertificate
	$ChangedManifestPath = Join-Path $Root 'bad-san-manifest.json'
	[IO.File]::WriteAllText($ChangedManifestPath, ($ChangedManifest | ConvertTo-Json -Depth 6))
	$Bad.RunManifestPath = $ChangedManifestPath
	$Bad.RunManifestSha256 = Get-Pin $ChangedManifestPath
	Expect-Rejection { & $Source @Bad } 'certificate SAN mismatch'
	$Bad = $Prepare.Clone(); $Bad.StageRoot = Join-Path $Root 'BadBinary'
	$Bad.NodeExecutableSha256 = '0' * 64
	Expect-Rejection { & $Source @Bad } 'unapproved Node binary'
	$Bad = $Prepare.Clone(); $Bad.StageRoot = Join-Path $Root 'BadGoInspector'
	$Bad.GoExecutableSha256 = '0' * 64
	Expect-Rejection { & $Source @Bad } 'unapproved Go metadata inspector'
	$DirtyExecutable = Join-Path $Root 'dirty-node.exe'
	[IO.File]::AppendAllText($MockSource, "`n// intentionally dirty at build`n")
	Push-Location $MockSourceRoot
	try { & $GoExecutable build -buildvcs=true -trimpath -o $DirtyExecutable . }
	finally { Pop-Location }
	if ($LASTEXITCODE -ne 0) { throw 'dirty mock Node build failed' }
	$Bad = $Prepare.Clone(); $Bad.StageRoot = Join-Path $Root 'BadDirtyBuild'
	$Bad.NodeExecutablePath = $DirtyExecutable
	$Bad.NodeExecutableSha256 = Get-Pin $DirtyExecutable
	Expect-Rejection { & $Source @Bad } 'Node binary was built from a dirty source tree'
	$Bad = $Prepare.Clone(); $Bad.StageRoot = Join-Path $Root 'BadKey'
	$Bad.PrivateKeyPath = $WrongKey
	Expect-Rejection { & $Source @Bad } 'certificate/private-key mismatch'
	$Bad = $Prepare.Clone(); $Bad.StageRoot = Join-Path $Root 'MissingToken'
	[Environment]::SetEnvironmentVariable('GARGANTUAN_ENGINE_ADAPTER_TOKEN', $null)
	Expect-Rejection { & $Source @Bad } 'missing environment-backed token'
	Write-Output '[Qualification:FarmNode] MOCK_TEST_OK'
} finally {
	[Environment]::SetEnvironmentVariable('GARGANTUAN_ENGINE_ADAPTER_TOKEN', $OldToken)
	$Resolved = [IO.Path]::GetFullPath($Root)
	$Temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
	if (-not $Resolved.StartsWith($Temp + [IO.Path]::DirectorySeparatorChar,
		[StringComparison]::OrdinalIgnoreCase) -or
		[IO.Path]::GetFileName($Resolved) -cnotmatch '^farm-node-test-[a-f0-9]{32}$') {
		throw 'refusing recursive cleanup outside this Node test root'
	}
	Remove-Item -LiteralPath $Resolved -Recurse -Force
}
