#requires -Version 7.0
# Create one short-lived Farm32 CA and loopback Node server identity inside an
# already protected, one-run worker directory. Only the public CA is exported.
param(
	[Parameter(Mandatory = $true)][string]$RunRoot,
	[Parameter(Mandatory = $true)][ValidatePattern('^[a-f0-9-]{36}$')][string]$RunId
)
$ErrorActionPreference = 'Stop'

function Write-NewBytes {
	param([string]$Path, [byte[]]$Bytes)
	$Stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew,
		[IO.FileAccess]::Write, [IO.FileShare]::None)
	try { $Stream.Write($Bytes) } finally { $Stream.Dispose() }
}
function Get-Pin {
	param([string]$Path)
	return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

$Root = [IO.Path]::GetFullPath($RunRoot)
if ([IO.Path]::GetFileName($Root.TrimEnd('\', '/')) -cne $RunId -or
	[Guid]::Parse($RunId).ToString() -cne $RunId) {
	throw 'Node TLS output is not a confined one-run worker root'
}
$Item = Get-Item -LiteralPath $Root -ErrorAction Stop
if (-not $Item.PSIsContainer -or ($Item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
	throw 'Node TLS one-run root is not a regular directory'
}
$Acl = Get-Acl -LiteralPath $Root
$Sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$Allowed = @($Sid, 'S-1-5-18', 'S-1-5-32-544', 'S-1-3-4')
$Observed = @($Acl.Access | ForEach-Object {
	$_.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value
})
if (-not $Acl.AreAccessRulesProtected -or
	$Acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -cne $Sid -or
	$Observed.Count -lt 1 -or $Sid -notin $Observed -or
	@($Observed | Where-Object { $_ -notin $Allowed }).Count -ne 0) {
	throw 'Node TLS one-run root is not private to this worker identity'
}
$CertificatePath = Join-Path $Root 'node-cert.pem'
$KeyPath = Join-Path $Root 'node-key.pem'
$CaPath = Join-Path $Root 'node-root-ca.pem'
$ReceiptPath = Join-Path $Root 'node-tls-stage.json'
foreach ($Path in @($CertificatePath, $KeyPath, $CaPath, $ReceiptPath)) {
	if (Test-Path -LiteralPath $Path) { throw 'Node TLS one-run identity already exists' }
}
$Encoding = [Text.UTF8Encoding]::new($false)
$CaKey = $null; $LeafKey = $null; $Ca = $null; $Leaf = $null; $LeafWithKey = $null
try {
	$NotBefore = [DateTimeOffset]::UtcNow.AddMinutes(-1)
	$NotAfter = [DateTimeOffset]::UtcNow.AddHours(2)
	$CaKey = [Security.Cryptography.RSA]::Create(3072)
	$CaRequest = [Security.Cryptography.X509Certificates.CertificateRequest]::new(
		"CN=Gargantuan Farm32 CA $RunId", $CaKey,
		[Security.Cryptography.HashAlgorithmName]::SHA256,
		[Security.Cryptography.RSASignaturePadding]::Pkcs1)
	$CaRequest.CertificateExtensions.Add(
		[Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new(
			$true, $false, 0, $true))
	$CaRequest.CertificateExtensions.Add(
		[Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
			[Security.Cryptography.X509Certificates.X509KeyUsageFlags]::KeyCertSign -bor
			[Security.Cryptography.X509Certificates.X509KeyUsageFlags]::CrlSign, $true))
	$Ca = $CaRequest.CreateSelfSigned($NotBefore, $NotAfter)
	$LeafKey = [Security.Cryptography.RSA]::Create(3072)
	$LeafRequest = [Security.Cryptography.X509Certificates.CertificateRequest]::new(
		'CN=127.0.0.1', $LeafKey, [Security.Cryptography.HashAlgorithmName]::SHA256,
		[Security.Cryptography.RSASignaturePadding]::Pkcs1)
	$LeafRequest.CertificateExtensions.Add(
		[Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new(
			$false, $false, 0, $true))
	$LeafRequest.CertificateExtensions.Add(
		[Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
			[Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature, $true))
	$Usages = [Security.Cryptography.OidCollection]::new()
	[void]$Usages.Add([Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.1'))
	$LeafRequest.CertificateExtensions.Add(
		[Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($Usages, $true))
	$San = [Security.Cryptography.X509Certificates.SubjectAlternativeNameBuilder]::new()
	$San.AddIpAddress([Net.IPAddress]::Parse('127.0.0.1'))
	$LeafRequest.CertificateExtensions.Add($San.Build())
	$Serial = [Security.Cryptography.RandomNumberGenerator]::GetBytes(16)
	$Leaf = $LeafRequest.Create($Ca, $NotBefore, $NotAfter, $Serial)
	$LeafWithKey = [Security.Cryptography.X509Certificates.RSACertificateExtensions]::CopyWithPrivateKey(
		$Leaf, $LeafKey)
	if ($LeafWithKey.NotAfter.ToUniversalTime() -le [DateTime]::UtcNow.AddMinutes(20)) {
		throw 'Node TLS certificate validity is below the 20-minute floor'
	}
	Write-NewBytes -Path $CaPath -Bytes $Encoding.GetBytes($Ca.ExportCertificatePem())
	Write-NewBytes -Path $CertificatePath -Bytes $Encoding.GetBytes($LeafWithKey.ExportCertificatePem())
	Write-NewBytes -Path $KeyPath -Bytes $Encoding.GetBytes($LeafKey.ExportPkcs8PrivateKeyPem())
	$Proof = [ordered]@{
		Format = 'GargantuanFarmNodeTlsStage'; Version = 1; RunId = $RunId
		RootCertificatePath = $CaPath; RootCertificateSha256 = Get-Pin $CaPath
		CertificatePath = $CertificatePath; CertificateSha256 = Get-Pin $CertificatePath
		PrivateKeyPath = $KeyPath; PrivateKeySha256 = Get-Pin $KeyPath
		CertificateThumbprint = $LeafWithKey.Thumbprint.ToLowerInvariant()
		NotAfterUtc = $LeafWithKey.NotAfter.ToUniversalTime().ToString('O')
		Status = 'GENERATED_NOT_TLS_PROVEN'
	}
	Write-NewBytes -Path $ReceiptPath -Bytes $Encoding.GetBytes(($Proof | ConvertTo-Json -Depth 4))
} catch {
	foreach ($Path in @($ReceiptPath, $KeyPath, $CertificatePath, $CaPath)) {
		if (Test-Path -LiteralPath $Path -PathType Leaf) { [IO.File]::Delete($Path) }
	}
	throw
} finally {
	if ($LeafWithKey) { $LeafWithKey.Dispose() }
	if ($Leaf) { $Leaf.Dispose() }
	if ($Ca) { $Ca.Dispose() }
	if ($LeafKey) { $LeafKey.Dispose() }
	if ($CaKey) { $CaKey.Dispose() }
}
Write-Output "[Qualification:FarmNodeTls] GENERATED run=$RunId root_sha256=$(Get-Pin $CaPath)"
