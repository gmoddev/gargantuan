#requires -Version 7.0
# Pure temporary-file qualification only. Never executes copied mock binaries.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PhysicalFarmPackageTestFixture.ps1')
$Tokens = $null; $Errors = $null
$Ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'PhysicalGameSessionFarmEndpoint.ps1'), [ref]$Tokens, [ref]$Errors)
if ($Errors.Count) { throw $Errors[0].Message }
foreach ($Definition in $Ast.FindAll({ param($Node) $Node -is [Management.Automation.Language.FunctionDefinitionAst] }, $true)) {
	. ([scriptblock]::Create($Definition.Extent.Text))
}
$Cases = 0
function Expect-ProjectionRejection {
	param([scriptblock]$Case, [string]$Label)
	$Rejected = $false
	try { & $Case | Out-Null } catch { $Rejected = $true }
	if (-not $Rejected) { throw "expected projection rejection: $Label" }
	$script:Cases++
}
$Root = Join-Path ([IO.Path]::GetTempPath()) ('farm-projection-test-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($Root)
try {
	$Source = Join-Path $Root 'Envelope'; $Registry = Join-Path $Root 'Registry'
	Write-ProjectionTestEnvelope -Root $Source -Role Server
	$Run = '12345678-1234-4234-8234-123456789abc'
	$Pin = (Get-FileHash -LiteralPath (Join-Path $Source 'deployment-sha256.json')).Hash
	$Plan = Get-NativeRuntimePlan -LocalRoot $Source -DeploymentSha256 $Pin -SourceCommit ('a' * 40) -LocalRole Server
	$Original = @($Plan.Files | ForEach-Object { (Get-FileHash -LiteralPath (Join-Path $Source $_.Path)).Hash })
	$Runtime = New-RuntimeProjection -LocalRoot $Source -Registry $Registry -LocalRunId $Run -LocalRole Server -Plan $Plan
	[void](Assert-PreparedRuntimeProjection -LocalRoot $Source -Registry $Registry -LocalRunId $Run -LocalRole Server -Plan $Plan)
	if ((Test-Path -LiteralPath (Join-Path $Runtime 'deployment-sha256.json')) -or
		(Get-ChildItem -LiteralPath $Runtime -File -Recurse).Count -ne $Plan.Files.Count -or
		(@($Plan.Files | ForEach-Object { (Get-FileHash -LiteralPath (Join-Path $Source $_.Path)).Hash }) -join ',') -cne ($Original -join ',')) {
		throw 'projection changed source bytes or did not exclude sidecar'
	}
	$Cases++
	Expect-ProjectionRejection { New-RuntimeProjection -LocalRoot $Source -Registry $Registry -LocalRunId $Run -LocalRole Server -Plan $Plan } 'successful destination stale'
	Expect-ProjectionRejection { New-RuntimeProjection -LocalRoot $Source -Registry $Source -LocalRunId $Run -LocalRole Server -Plan $Plan } 'projection within source'
	Expect-ProjectionRejection { Get-RuntimeProjectionPath -Registry $Registry -LocalRunId '../escape' -LocalRole Server } 'unsafe identity'
	Expect-ProjectionRejection { Assert-RuntimeProjection -LocalRoot $Source -RuntimeRoot (Join-Path $Root 'absent') -Plan $Plan } 'absent runtime'
	$Extra = Join-Path $Runtime 'extra.txt'; [IO.File]::WriteAllText($Extra, 'unlisted')
	Expect-ProjectionRejection { Assert-RuntimeProjection -LocalRoot $Source -RuntimeRoot $Runtime -Plan $Plan } 'extra runtime file'
	Remove-Item -LiteralPath $Extra
	$Victim = Join-Path $Runtime $Plan.Binary; $Bytes = [IO.File]::ReadAllBytes($Victim)
	[IO.File]::WriteAllText($Victim, 'tampered')
	Expect-ProjectionRejection { Assert-RuntimeProjection -LocalRoot $Source -RuntimeRoot $Runtime -Plan $Plan } 'copied content tamper'
	Remove-Item -LiteralPath $Victim
	Expect-ProjectionRejection { Assert-RuntimeProjection -LocalRoot $Source -RuntimeRoot $Runtime -Plan $Plan } 'copied content absent'
	[IO.File]::WriteAllBytes($Victim, $Bytes)
	$Receipt = $Runtime + '.json'; $ReceiptBytes = [IO.File]::ReadAllBytes($Receipt)
	$Bad = Get-Content -LiteralPath $Receipt -Raw | ConvertFrom-Json -AsHashtable; $Bad.Extra = 1
	[IO.File]::WriteAllText($Receipt, ($Bad | ConvertTo-Json -Depth 8))
	Expect-ProjectionRejection { Assert-PreparedRuntimeProjection -LocalRoot $Source -Registry $Registry -LocalRunId $Run -LocalRole Server -Plan $Plan } 'receipt extra field'
	[IO.File]::WriteAllBytes($Receipt, $ReceiptBytes)
	$Bad = Get-Content -LiteralPath $Receipt -Raw | ConvertFrom-Json -AsHashtable; $Bad.Version = $true
	[IO.File]::WriteAllText($Receipt, ($Bad | ConvertTo-Json -Depth 8))
	Expect-ProjectionRejection { Assert-PreparedRuntimeProjection -LocalRoot $Source -Registry $Registry -LocalRunId $Run -LocalRole Server -Plan $Plan } 'receipt boolean version'
	[IO.File]::WriteAllBytes($Receipt, $ReceiptBytes)
	$GamePath = Join-Path $Source 'game.package.json'; $GameBytes = [IO.File]::ReadAllBytes($GamePath)
	$Bad = Get-Content -LiteralPath $GamePath -Raw | ConvertFrom-Json -AsHashtable; $Bad.ContentTableSha256 = '0' * 64
	[IO.File]::WriteAllText($GamePath, ($Bad | ConvertTo-Json -Depth 8)); Write-ProjectionTestDeployment -Root $Source
	$BadPin = (Get-FileHash -LiteralPath (Join-Path $Source 'deployment-sha256.json')).Hash
	Expect-ProjectionRejection { Get-NativeRuntimePlan -LocalRoot $Source -DeploymentSha256 $BadPin -SourceCommit ('a' * 40) -LocalRole Server } 'ordered table hash mismatch'
	[IO.File]::WriteAllBytes($GamePath, $GameBytes); Write-ProjectionTestDeployment -Root $Source
	$DeploymentPath = Join-Path $Source 'deployment-sha256.json'
	$DeploymentBytes = [IO.File]::ReadAllBytes($DeploymentPath)
	foreach ($Variant in @('unsafe-path', 'duplicate-entry', 'boolean-size', 'missing-required')) {
		$Bad = [Text.Encoding]::UTF8.GetString($GameBytes) | ConvertFrom-Json -AsHashtable
		switch ($Variant) {
			'unsafe-path' { $Bad.Content[0].Path = '../escape' }
			'duplicate-entry' { $Bad.Content[1].Path = $Bad.Content[0].Path }
			'boolean-size' { $Bad.Content[0].Size = $true }
			'missing-required' { $Bad.Startup.ContentManifest = 'content/absent.json' }
		}
		[IO.File]::WriteAllText($GamePath, ($Bad | ConvertTo-Json -Depth 8))
		$Deployment = [Text.Encoding]::UTF8.GetString($DeploymentBytes) | ConvertFrom-Json -AsHashtable
		$Descriptor = @($Deployment.Files | Where-Object Path -CEQ 'game.package.json')[0]
		$Descriptor.Bytes = (Get-Item -LiteralPath $GamePath).Length
		$Descriptor.Sha256 = (Get-FileHash -LiteralPath $GamePath).Hash.ToLowerInvariant()
		[IO.File]::WriteAllText($DeploymentPath, ($Deployment | ConvertTo-Json -Depth 8))
		$BadPin = (Get-FileHash -LiteralPath $DeploymentPath).Hash
		Expect-ProjectionRejection { Get-NativeRuntimePlan -LocalRoot $Source -DeploymentSha256 $BadPin -SourceCommit ('a' * 40) -LocalRole Server } $Variant
	}
	[IO.File]::WriteAllBytes($GamePath, $GameBytes); [IO.File]::WriteAllBytes($DeploymentPath, $DeploymentBytes)
	Expect-ProjectionRejection { Get-NativeRuntimePlan -LocalRoot $Source -DeploymentSha256 ('0' * 64) -SourceCommit ('a' * 40) -LocalRole Server } 'changed original deployment pin'
	$Link = Join-Path $Root 'RedirectedRegistry'
	[void](New-Item -ItemType Junction -Path $Link -Target $Registry)
	Expect-ProjectionRejection { Get-RuntimeProjectionPath -Registry $Link -LocalRunId $Run -LocalRole Server } 'redirected registry ancestor'
	Remove-Item -LiteralPath $Link
	$Extra = Join-Path $Source 'extra.txt'; [IO.File]::WriteAllText($Extra, 'unlisted')
	$Pin = (Get-FileHash -LiteralPath (Join-Path $Source 'deployment-sha256.json')).Hash
	Expect-ProjectionRejection { Get-NativeRuntimePlan -LocalRoot $Source -DeploymentSha256 $Pin -SourceCommit ('a' * 40) -LocalRole Server } 'extra envelope file'
	Remove-Item -LiteralPath $Extra
	$PartialRun = '12345678-1234-4234-8234-123456789abd'
	$Partial = Get-RuntimeProjectionPath -Registry $Registry -LocalRunId $PartialRun -LocalRole Server
	[IO.File]::WriteAllText(($Partial + '.attempt'), 'previous incomplete copy')
	Expect-ProjectionRejection { New-RuntimeProjection -LocalRoot $Source -Registry $Registry -LocalRunId $PartialRun -LocalRole Server -Plan $Plan } 'partial attempt cannot retry'
	$Malformed = Join-Path $Root 'bad.json'; [IO.File]::WriteAllText($Malformed, '{"value":1,"value":2}')
	Expect-ProjectionRejection { Read-ProjectionJson -Path $Malformed } 'duplicate JSON properties'
	# Self-consistent content/deployment identities isolate the native UTF8 path bound.
	# Each component is within Windows component limits; UTF16 length alone is insufficient.
	$Multibyte = 'x/' + ([string][char]0x4E00 * 85) + '/' + ([string][char]0x4E00 * 85)
	if ($Multibyte.Length -ge 512 -or [Text.Encoding]::UTF8.GetByteCount($Multibyte) -ne 513) {
		throw 'multibyte bound witness invalid'
	}
	$UnicodeFile = Join-Path $Source $Multibyte
	[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($UnicodeFile))
	[IO.File]::WriteAllText($UnicodeFile, 'mock-unicode', [Text.UTF8Encoding]::new($false))
	$Bad = [Text.Encoding]::UTF8.GetString($GameBytes) | ConvertFrom-Json -AsHashtable
	$Bad.Content += [ordered]@{ Path = $Multibyte; Size = (Get-Item -LiteralPath $UnicodeFile).Length;
		Sha256 = (Get-FileHash -LiteralPath $UnicodeFile).Hash.ToLowerInvariant(); Category = 'Asset' }
	$Canonical = @($Bad.Content | ForEach-Object { [ordered]@{ Path = $_.Path; Size = $_.Size; Sha256 = $_.Sha256; Category = $_.Category } })
	$TableBytes = [Text.Encoding]::UTF8.GetBytes((ConvertTo-Json -InputObject $Canonical -Depth 8 -Compress))
	$Bad.ContentTableSha256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($TableBytes)).ToLowerInvariant()
	[IO.File]::WriteAllText($GamePath, ($Bad | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
	Write-ProjectionTestDeployment -Root $Source
	$UnicodePin = (Get-FileHash -LiteralPath $DeploymentPath).Hash
	# Prove the deployment itself is complete and all declared source bytes match first.
	Assert-DeploymentManifest -LocalRoot $Source -ExpectedSha256 $UnicodePin -ExpectedSourceCommit ('a' * 40)
	$CurrentDefinition = @($Ast.FindAll({ param($Node)
		$Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -eq 'Get-NativeRuntimePlan'
	}, $true))[0].Extent.Text
	$OldPredicate = $CurrentDefinition.Replace('[Text.Encoding]::UTF8.GetByteCount($Entry.Path)', '$Entry.Path.Length')
	if ($OldPredicate -ceq $CurrentDefinition) { throw 'former character predicate witness absent' }
	$PreviouslyAccepted = & {
		. ([scriptblock]::Create($OldPredicate))
		Get-NativeRuntimePlan -LocalRoot $Source -DeploymentSha256 $UnicodePin -SourceCommit ('a' * 40) -LocalRole Server
	}
	if ($PreviouslyAccepted.Files.Count -ne $Plan.Files.Count + 1) { throw 'former predicate did not accept complete witness' }
	Expect-ProjectionRejection { Get-NativeRuntimePlan -LocalRoot $Source -DeploymentSha256 $UnicodePin -SourceCommit ('a' * 40) -LocalRole Server } '513 UTF8 byte path despite short UTF16 length'
	# Native std::string orders UTF8 bytes. BMP E000 precedes astral 10000 in UTF8,
	# but UTF16 Ordinal has the opposite order; both source names remain legal.
	$LexicalSource = Join-Path $Root 'LexicalEnvelope'
	Write-ProjectionTestEnvelope -Root $LexicalSource -Role Server
	$LexicalGamePath = Join-Path $LexicalSource 'game.package.json'
	$LexicalGame = Get-Content -LiteralPath $LexicalGamePath -Raw | ConvertFrom-Json -AsHashtable
	$FirstName = 'x/' + [char]0xE000; $SecondName = 'x/' + [char]::ConvertFromUtf32(0x10000)
	if ([StringComparer]::Ordinal.Compare($FirstName, $SecondName) -le 0) { throw 'UTF16 inverse-order witness invalid' }
	foreach ($Name in @($FirstName, $SecondName)) {
		$Path = Join-Path $LexicalSource $Name
		[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path))
		[IO.File]::WriteAllText($Path, 'mock-lexical', [Text.UTF8Encoding]::new($false))
		$LexicalGame.Content += [ordered]@{ Path = $Name; Size = (Get-Item -LiteralPath $Path).Length;
			Sha256 = (Get-FileHash -LiteralPath $Path).Hash.ToLowerInvariant(); Category = 'Asset' }
	}
	function Write-LexicalTestManifest {
		$Table = @($LexicalGame.Content | ForEach-Object { [ordered]@{ Path = $_.Path; Size = $_.Size; Sha256 = $_.Sha256; Category = $_.Category } })
		$Bytes = [Text.Encoding]::UTF8.GetBytes((ConvertTo-Json -InputObject $Table -Depth 8 -Compress))
		$LexicalGame.ContentTableSha256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant()
		[IO.File]::WriteAllText($LexicalGamePath, ($LexicalGame | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
		Write-ProjectionTestDeployment -Root $LexicalSource
	}
	Write-LexicalTestManifest
	$LexicalPin = (Get-FileHash -LiteralPath (Join-Path $LexicalSource 'deployment-sha256.json')).Hash
	$LexicalPlan = Get-NativeRuntimePlan -LocalRoot $LexicalSource -DeploymentSha256 $LexicalPin -SourceCommit ('a' * 40) -LocalRole Server
	if ($LexicalPlan.Files.Count -ne $Plan.Files.Count + 2) { throw 'native UTF8 lexical witness did not preserve all content' }
	$Cases++
	$Last = $LexicalGame.Content.Count - 1
	$Temporary = $LexicalGame.Content[$Last - 1]
	$LexicalGame.Content[$Last - 1] = $LexicalGame.Content[$Last]
	$LexicalGame.Content[$Last] = $Temporary
	Write-LexicalTestManifest
	$InversePin = (Get-FileHash -LiteralPath (Join-Path $LexicalSource 'deployment-sha256.json')).Hash
	Assert-DeploymentManifest -LocalRoot $LexicalSource -ExpectedSha256 $InversePin -ExpectedSourceCommit ('a' * 40)
	Expect-ProjectionRejection { Get-NativeRuntimePlan -LocalRoot $LexicalSource -DeploymentSha256 $InversePin -SourceCommit ('a' * 40) -LocalRole Server } 'inverse native UTF8 lexical ordering'
	Write-Output "[Qualification:RuntimeProjection] PASS cases=$Cases scope=pure-temporary-files"
} finally {
	$Resolved = [IO.Path]::GetFullPath($Root)
	if (-not $Resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase) -or
		[IO.Path]::GetFileName($Resolved) -cnotmatch '^farm-projection-test-[a-f0-9]{32}$') { throw 'temporary test cleanup confinement failed' }
	Remove-Item -LiteralPath $Resolved -Recurse -Force
}
