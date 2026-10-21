#requires -Version 7.0
# Local one-run certificate fixture; no worker, endpoint, or physical service.
$ErrorActionPreference = 'Stop'
$Generator = Join-Path $PSScriptRoot 'NewPhysicalGameSessionFarmNodeTls.ps1'
$TestRoot = Join-Path ([IO.Path]::GetTempPath()) ('farm-node-tls-test-' + [Guid]::NewGuid().ToString('N'))
$RunId = [Guid]::NewGuid().ToString()
$RunRoot = Join-Path $TestRoot $RunId
try {
	[void][IO.Directory]::CreateDirectory($RunRoot)
	$Sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
	& icacls.exe $RunRoot /grant:r "*$($Sid):(OI)(CI)F" '*S-1-5-18:(OI)(CI)F' `
		'*S-1-5-32-544:(OI)(CI)F' | Out-Null
	if ($LASTEXITCODE -ne 0) { throw 'TLS test ACL grant failed' }
	& icacls.exe $RunRoot /inheritance:r | Out-Null
	if ($LASTEXITCODE -ne 0) { throw 'TLS test ACL protection failed' }
	& icacls.exe $RunRoot /setowner "*$Sid" | Out-Null
	if ($LASTEXITCODE -ne 0) { throw 'TLS test owner failed' }
	$Output = & $Generator -RunRoot $RunRoot -RunId $RunId
	$Receipt = Get-Content -LiteralPath (Join-Path $RunRoot 'node-tls-stage.json') -Raw |
		ConvertFrom-Json -AsHashtable
	if ($Receipt.RunId -cne $RunId -or $Receipt.Status -cne 'GENERATED_NOT_TLS_PROVEN' -or
		$Output -notmatch 'GENERATED') { throw 'one-run TLS receipt is invalid' }
	foreach ($Name in @('RootCertificate', 'Certificate', 'PrivateKey')) {
		if ((Get-FileHash -LiteralPath $Receipt[$Name + 'Path'] -Algorithm SHA256).Hash -ine
			$Receipt[$Name + 'Sha256']) { throw 'one-run TLS file pin mismatch' }
	}
	$Leaf = [Security.Cryptography.X509Certificates.X509Certificate2]::CreateFromPemFile(
		$Receipt.CertificatePath, $Receipt.PrivateKeyPath)
	$Ca = [Security.Cryptography.X509Certificates.X509Certificate2]::new($Receipt.RootCertificatePath)
	try {
		$San = $Leaf.Extensions | Where-Object {
			$_ -is [Security.Cryptography.X509Certificates.X509SubjectAlternativeNameExtension]
		} | Select-Object -First 1
		if (-not $Leaf.HasPrivateKey -or -not $San -or
			@($San.EnumerateIPAddresses() | Where-Object {
				$_.Equals([Net.IPAddress]::Parse('127.0.0.1'))
			}).Count -ne 1 -or
			$Leaf.NotAfter.ToUniversalTime() -le [DateTime]::UtcNow.AddMinutes(20)) {
			throw 'generated Node identity lacks key, loopback SAN, or validity'
		}
		$Chain = [Security.Cryptography.X509Certificates.X509Chain]::new()
		try {
			$Chain.ChainPolicy.TrustMode = [Security.Cryptography.X509Certificates.X509ChainTrustMode]::CustomRootTrust
			[void]$Chain.ChainPolicy.CustomTrustStore.Add($Ca)
			$Chain.ChainPolicy.RevocationMode = [Security.Cryptography.X509Certificates.X509RevocationMode]::NoCheck
			if (-not $Chain.Build($Leaf)) { throw 'generated Node certificate does not chain to its CA' }
		} finally { $Chain.Dispose() }
	} finally { $Leaf.Dispose(); $Ca.Dispose() }
	$Tokens = $null; $Errors = $null
	$NodeAst = [Management.Automation.Language.Parser]::ParseFile(
		(Join-Path $PSScriptRoot 'PhysicalGameSessionFarmNode.ps1'), [ref]$Tokens, [ref]$Errors)
	if ($Errors.Count -ne 0) { throw 'Node consumer helper has a syntax error' }
	$Consumer = @($NodeAst.FindAll({ param($Node)
		$Node -is [Management.Automation.Language.FunctionDefinitionAst] -and
		$Node.Name -eq 'Assert-Certificate'
	}, $true))
	if ($Consumer.Count -ne 1) { throw 'Node consumer certificate validator is absent' }
	. ([scriptblock]::Create($Consumer[0].Extent.Text))
	if ((Assert-Certificate -Certificate $Receipt.CertificatePath `
		-PrivateKey $Receipt.PrivateKeyPath -Root $Receipt.RootCertificatePath `
		-HostName '127.0.0.1') -cne $Receipt.CertificateThumbprint) {
		throw 'generated TLS identity is incompatible with the Node consumer'
	}
	$Before = (Get-FileHash -LiteralPath $Receipt.PrivateKeyPath -Algorithm SHA256).Hash
	$Rejected = $false
	try { & $Generator -RunRoot $RunRoot -RunId $RunId | Out-Null } catch { $Rejected = $true }
	if (-not $Rejected -or
		(Get-FileHash -LiteralPath $Receipt.PrivateKeyPath -Algorithm SHA256).Hash -cne $Before) {
		throw 'TLS generator reused or changed the one-run private key'
	}
	$Rejected = $false
	try { & $Generator -RunRoot $RunRoot -RunId ([Guid]::NewGuid().ToString()) | Out-Null }
	catch { $Rejected = $true }
	if (-not $Rejected) { throw 'TLS generator accepted a different run identity' }
	$Inherited = Join-Path $TestRoot ([Guid]::NewGuid().ToString())
	[void][IO.Directory]::CreateDirectory($Inherited)
	$Rejected = $false
	try { & $Generator -RunRoot $Inherited -RunId ([IO.Path]::GetFileName($Inherited)) | Out-Null }
	catch { $Rejected = $true }
	if (-not $Rejected) { throw 'TLS generator accepted inherited ACLs' }
	Write-Output '[Qualification:FarmNodeTls] MOCK_TEST_OK'
} finally {
	if (Test-Path -LiteralPath $TestRoot) { Remove-Item -LiteralPath $TestRoot -Recurse -Force }
}
