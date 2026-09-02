param(
	[Parameter(Mandatory = $true)][string]$Packager,
	[Parameter(Mandatory = $true)][string]$RuntimeDistribution,
	[Parameter(Mandatory = $true)][string]$ProjectRoot,
	[string]$GraphicalClient = 'OFF'
)

$ErrorActionPreference = 'Stop'
$TestRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("gps-{0}-{1}" -f $PID, [Guid]::NewGuid().ToString('N').Substring(0, 8))
$PackageRoot = Join-Path $TestRoot 'Package'
$ServerProcess = $null
$ClientProcess = $null

function Start-PlayerProcess {
	param([string[]]$Arguments)
	try {
		$StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
		$StartInfo.FileName = $Player
		$StartInfo.WorkingDirectory = $PackageRoot
		$StartInfo.UseShellExecute = $false
		$StartInfo.CreateNoWindow = $true
		$StartInfo.RedirectStandardOutput = $true
		$StartInfo.RedirectStandardError = $true
		$StartInfo.Arguments = ($Arguments | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }) -join ' '
		return [System.Diagnostics.Process]::Start($StartInfo)
	} catch {
		throw "Packaged player process start failed: $($_.Exception.Message) $($_.ScriptStackTrace)"
	}
}

try {
	New-Item -ItemType Directory -Path $TestRoot | Out-Null
	& $Packager build --project $ProjectRoot --output $PackageRoot --runtime $RuntimeDistribution --configuration Release
	if ($LASTEXITCODE -ne 0) {
		throw "Packager exited with $LASTEXITCODE"
	}

	$Player = Join-Path $PackageRoot 'GargantuanPlayer.exe'
	$Port = 40000 + ($PID % 1000)
	$Endpoint = "127.0.0.1:$Port"
	$ServerProcess = Start-PlayerProcess -Arguments @('--headless', '--server-bind', $Endpoint)
	Start-Sleep -Milliseconds 300
	$ClientArguments = @('--connect', $Endpoint, '--max-frames', '60')
	if ($GraphicalClient -ne 'ON') {
		$ClientArguments = @('--headless') + $ClientArguments
	}
	$ClientProcess = Start-PlayerProcess -Arguments $ClientArguments
	if (-not $ClientProcess.WaitForExit(20000)) {
		Stop-Process -Id $ClientProcess.Id -Force
		throw 'Packaged game-session client timed out'
	}
	if ($ClientProcess.ExitCode -ne 0 -or $ServerProcess.HasExited) {
		$ClientDiagnostic = if ($ClientProcess.HasExited) { $ClientProcess.StandardError.ReadToEnd() } else { '' }
		$ServerDiagnostic = if ($ServerProcess.HasExited) { $ServerProcess.StandardError.ReadToEnd() } else { '' }
		throw "Packaged session failed: client=$($ClientProcess.ExitCode) clientError=$ClientDiagnostic serverError=$ServerDiagnostic"
	}
	Write-Output ("[Package:Session] packaged GNS server/{0} client bootstrap passed" -f $(
		if ($GraphicalClient -eq 'ON') { 'graphical' } else { 'headless' }
	))
} finally {
	if ($ClientProcess -and -not $ClientProcess.HasExited) {
		Stop-Process -Id $ClientProcess.Id -Force
	}
	if ($ServerProcess -and -not $ServerProcess.HasExited) {
		Stop-Process -Id $ServerProcess.Id -Force
	}
	$ResolvedTemp = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
	$ResolvedTest = [System.IO.Path]::GetFullPath($TestRoot)
	if ($ResolvedTest.StartsWith($ResolvedTemp, [System.StringComparison]::OrdinalIgnoreCase) -and
		[System.IO.Path]::GetFileName($ResolvedTest).StartsWith('gps-')) {
		Remove-Item -LiteralPath $ResolvedTest -Recurse -Force -ErrorAction SilentlyContinue
	}
}
