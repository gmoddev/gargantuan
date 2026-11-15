param(
	[Parameter(Mandatory = $true)][string]$Packager,
	[Parameter(Mandatory = $true)][string]$ServerRuntimeDistribution,
	[Parameter(Mandatory = $true)][string]$ProjectRoot,
	[Parameter(Mandatory = $true)][string]$ManagedHostTests
)

$ErrorActionPreference = 'Stop'
$TestRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("gmsh-{0}-{1}" -f $PID, [Guid]::NewGuid().ToString('N').Substring(0, 8))
$PackageRoot = Join-Path $TestRoot 'ServerPackage'
try {
	New-Item -ItemType Directory -Path $TestRoot | Out-Null
	# Use the ordinary packager and authored sample, with no direct project edits.
	& $Packager build --project $ProjectRoot --output $PackageRoot --runtime $ServerRuntimeDistribution --configuration Release
	if ($LASTEXITCODE -ne 0) { throw 'Managed host fixture package build failed' }
	$TestExecutable = Join-Path $PackageRoot 'ManagedServerHostTests.exe'
	Copy-Item -LiteralPath $ManagedHostTests -Destination $TestExecutable
	$CaseIndex = 0
	foreach ($Case in @('ReadyDrain', 'AuthorityLoss', 'NeverReady', 'PreReady', 'MissingProvider', 'MissingLifecycle', 'InsecureOverride')) {
		$Port = 41000 + ($PID % 1000) + $CaseIndex
		$CaseIndex++
		$StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
		$StartInfo.FileName = $TestExecutable
		$StartInfo.WorkingDirectory = $PackageRoot
		$StartInfo.UseShellExecute = $false
		$StartInfo.CreateNoWindow = $true
		$StartInfo.RedirectStandardOutput = $true
		$StartInfo.RedirectStandardError = $true
		$StartInfo.Arguments = ('"{0}" --bind "127.0.0.1:{1}" --max-ticks 400' -f $Case, $Port)
		if ($Case -eq 'InsecureOverride') { $StartInfo.Arguments += ' --allow-insecure-development-network' }
		$Process = [System.Diagnostics.Process]::Start($StartInfo)
		try {
			$StandardOutput = $Process.StandardOutput.ReadToEndAsync()
			$StandardError = $Process.StandardError.ReadToEndAsync()
			if (-not $Process.WaitForExit(20000)) {
				$Process.Kill()
				[void]$Process.WaitForExit(5000)
				throw 'Managed host fixture timed out'
			}
			$Output = $StandardOutput.GetAwaiter().GetResult()
			$ErrorOutput = $StandardError.GetAwaiter().GetResult()
			if ($Process.ExitCode -ne 0 -or -not $Output.Contains('[Runtime:ManagedServerTest] lifecycle case passed')) {
				throw ("Managed host fixture {0} failed: Exit={1} Out={2} Err={3}" -f $Case, $Process.ExitCode, $Output, $ErrorOutput)
			}
		} finally {
			$Process.Dispose()
		}
	}
	Write-Output '[Runtime:ManagedServerTest] all packaged lifecycle cases passed'
} finally {
	if (Test-Path -LiteralPath $TestRoot) { Remove-Item -LiteralPath $TestRoot -Recurse -Force }
}
