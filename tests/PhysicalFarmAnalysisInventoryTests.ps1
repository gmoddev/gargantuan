#requires -Version 7.0
$ErrorActionPreference = 'Stop'
$Root = Join-Path ([IO.Path]::GetTempPath()) ('farm-analysis-inventory-' + [Guid]::NewGuid().ToString('N'))
$Checkout = Join-Path $Root 'source'
$Script = Join-Path $PSScriptRoot 'PhysicalFarmAnalysisInventory.ps1'
$Source = [IO.File]::ReadAllText($Script)
$Paths = @([regex]::Matches($Source, "'((?:tests|tools)/[^']+\.(?:ps1|py))'") | ForEach-Object { $_.Groups[1].Value })
$Python = @(Get-Command python -CommandType Application -ErrorAction Stop)[0].Source
$PythonHash = (Get-FileHash -LiteralPath $Python -Algorithm SHA256).Hash
$Cases = 0
function Reject([string]$Name, [scriptblock]$Action) {
	$Failed = $false
	try { & $Action | Out-Null } catch { $Failed = $true }
	if (-not $Failed) { throw "invalid analysis inventory accepted: $Name" }
	$script:Cases++
}
try {
	foreach ($Path in $Paths) {
		$Destination = Join-Path $Checkout $Path
		[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Destination))
		Copy-Item -LiteralPath (Join-Path (Join-Path $PSScriptRoot '..') $Path) -Destination $Destination
	}
	& git -C $Checkout init --quiet
	& git -C $Checkout add --all
	$FixtureEmail = & git -C (Join-Path $PSScriptRoot '..') config user.email
	if (-not $FixtureEmail) { $FixtureEmail = 'fixture@users.noreply.github.com' }
	& git -C $Checkout -c user.name=Fixture -c "user.email=$FixtureEmail" commit --quiet -m fixture
	if ($LASTEXITCODE -ne 0) { throw 'fixture source commit failed' }
	$Head = & git -C $Checkout rev-parse HEAD
	$CopiedScript = Join-Path $Checkout 'tests/PhysicalFarmAnalysisInventory.ps1'
	$Output = Join-Path $Root 'inventory.json'
	& $CopiedScript -SourceCommit $Head -PythonPath $Python -PythonSha256 $PythonHash -OutputPath $Output | Out-Null
	$Result = Get-Content -LiteralPath $Output -Raw | ConvertFrom-Json
	if ($Result.Files.Count -ne 21 -or $Result.SourceCommit -cne $Head -or
		$Result.PythonPath -ine $Python -or @($Result.Files | Where-Object Path -ceq 'tests/PhysicalFarmClockEvidence.ps1').Count -ne 1 -or
		@($Result.Files | Where-Object Path -ceq 'tests/PhysicalGameSessionFarmLifecycle.ps1').Count -ne 1) {
		throw 'analysis dependency/runtime inventory incomplete'
	}
	foreach ($File in $Result.Files) {
		if ($File.Sha256 -cne (Get-FileHash -LiteralPath (Join-Path $Checkout $File.Path)).Hash.ToLowerInvariant()) {
			throw 'analysis file pin mismatch'
		}
	}
	$Cases++
	Reject 'output overwrite' { & $CopiedScript -SourceCommit $Head -PythonPath $Python -PythonSha256 $PythonHash -OutputPath $Output }
	Reject 'wrong source' { & $CopiedScript -SourceCommit ('0' * 40) -PythonPath $Python -PythonSha256 $PythonHash -OutputPath (Join-Path $Root 'bad-source.json') }
	Reject 'wrong Python pin' { & $CopiedScript -SourceCommit $Head -PythonPath $Python -PythonSha256 ('0' * 64) -OutputPath (Join-Path $Root 'bad-python.json') }
	$OtherPython = Join-Path $Root 'python.exe'
	Copy-Item -LiteralPath $Python -Destination $OtherPython
	Reject 'unselected Python copy' { & $CopiedScript -SourceCommit $Head -PythonPath $OtherPython -PythonSha256 $PythonHash -OutputPath (Join-Path $Root 'wrong-resolution.json') }
	$Clock = Join-Path $Checkout 'tests/PhysicalFarmClockEvidence.ps1'
	[IO.File]::AppendAllText($Clock, "`n# modified`n")
	Reject 'modified helper' { & $CopiedScript -SourceCommit $Head -PythonPath $Python -PythonSha256 $PythonHash -OutputPath (Join-Path $Root 'dirty.json') }
	Remove-Item -LiteralPath $Clock -Force
	Reject 'missing helper' { & $CopiedScript -SourceCommit $Head -PythonPath $Python -PythonSha256 $PythonHash -OutputPath (Join-Path $Root 'missing.json') }
	Write-Output "[Qualification:FarmAnalysisInventory] PASS cases=$Cases"
} finally {
	$Resolved = [IO.Path]::GetFullPath($Root)
	$Temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
	if (-not $Resolved.StartsWith($Temp + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
		[IO.Path]::GetFileName($Resolved) -cnotmatch '^farm-analysis-inventory-[a-f0-9]{32}$') {
		throw 'inventory fixture cleanup escaped expected temporary root'
	}
	if (Test-Path -LiteralPath $Resolved) { Remove-Item -LiteralPath $Resolved -Recurse -Force }
}
