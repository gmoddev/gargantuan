# Test-only layout fixture. Content bodies are deliberately harmless mock bytes;
# no native host, package semantic loader, socket, or capture is invoked.
function Write-ProjectionTestDeployment {
	param([string]$Root)
	$Game = Get-Content -LiteralPath (Join-Path $Root 'game.package.json') -Raw | ConvertFrom-Json -AsHashtable
	$Names = @('game.package.json') + @($Game.Content | ForEach-Object Path)
	$Files = @($Names | ForEach-Object {
		$Path = Join-Path $Root $_
		[ordered]@{ Path = $_; Bytes = (Get-Item -LiteralPath $Path).Length;
			Sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
	})
	[IO.File]::WriteAllText((Join-Path $Root 'deployment-sha256.json'),
		([ordered]@{ Format = 'GargantuanFarmDeployment'; Version = 1; SourceCommit = 'a' * 40; Files = $Files } |
		ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
}

function Write-ProjectionTestEnvelope {
	param([string]$Root, [ValidateSet('Server', 'Player')][string]$Role)
	[void][IO.Directory]::CreateDirectory($Root)
	$Binary = "Gargantuan$Role.exe"
	$Names = [string[]]@($Binary, 'content/game.instance.json', 'content/assets/catalog.json', 'content/content.manifest.json',
		'runtime/DefaultActionMap.luau', 'runtime/DefaultInteractionRuntime.luau', 'runtime/DefaultCharacterRuntime.luau',
		'runtime/DefaultLocomotion.luau', 'runtime/DefaultCamera.luau', 'runtime/DefaultPlayerRuntime.luau',
		'runtime/GargantuanSans.ttf', 'shaders/gui.frag.spv', 'shaders/gui.vert.spv',
		'shaders/opaque.frag.spv', 'shaders/opaque.vert.spv', 'shaders/shadow.frag.spv', 'shaders/shadow.vert.spv',
		'shaders/sky.frag.spv', 'shaders/sky.vert.spv', 'notices/Gargantuan.txt', 'notices/SDL3.txt',
		'notices/SDL3_image.txt', 'notices/SDL3_ttf.txt')
	[Array]::Sort($Names, [StringComparer]::Ordinal)
	$Content = @($Names | ForEach-Object {
		$Path = Join-Path $Root $_
		[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path))
		[IO.File]::WriteAllText($Path, "mock-$Role/$_", [Text.UTF8Encoding]::new($false))
		[ordered]@{ Path = $_; Size = (Get-Item -LiteralPath $Path).Length;
			Sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant(); Category = 'Runtime' }
	})
	$Table = [Text.UTF8Encoding]::new($false).GetBytes((ConvertTo-Json -InputObject $Content -Compress -Depth 8))
	$Game = [ordered]@{ Format = 'GargantuanGamePackage'; PackageFormatVersion = 2; RuntimeCompatibility = 1;
		ProjectId = 'b' * 32; DisplayName = 'HarmlessProjectionFixture'; Configuration = 'Release'; Revision = 1;
		UnsavedChanges = $false; Player = $Binary; Startup = [ordered]@{ Project = 'content/game.instance.json';
			ContentManifest = 'content/content.manifest.json'; AssetCatalog = 'content/assets/catalog.json'; PreRun = $null };
		ContentTableSha256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Table)).ToLowerInvariant(); Content = $Content }
	[IO.File]::WriteAllText((Join-Path $Root 'game.package.json'), ($Game | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
	Write-ProjectionTestDeployment -Root $Root
}
