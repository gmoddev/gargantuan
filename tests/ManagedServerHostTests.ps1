param(
	[Parameter(Mandatory = $true)][string]$Packager,
	[Parameter(Mandatory = $true)][string]$ServerRuntimeDistribution,
	[Parameter(Mandatory = $true)][string]$ProjectRoot,
	[Parameter(Mandatory = $true)][string]$ManagedHostTests
)

$ErrorActionPreference = 'Stop'
$TestRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("gmsh-{0}-{1}" -f $PID, [Guid]::NewGuid().ToString('N').Substring(0, 8))
$PackageRoot = Join-Path $TestRoot 'ServerPackage'
$RuntimeFixture = Join-Path $TestRoot 'ServerRuntime'
try {
	New-Item -ItemType Directory -Path $TestRoot | Out-Null
	# The package loader rejects undeclared files. Put the test host in a private
	# copy of the ordinary runtime distribution, then let Packager hash it.
	Copy-Item -LiteralPath $ServerRuntimeDistribution -Destination $RuntimeFixture -Recurse
	$RuntimeHost = Join-Path $RuntimeFixture 'GargantuanServer.exe'
	if (-not (Test-Path -LiteralPath $RuntimeHost)) { throw 'Server runtime executable is missing' }
	Copy-Item -LiteralPath $ManagedHostTests -Destination $RuntimeHost -Force
	& $Packager build --project $ProjectRoot --output $PackageRoot --runtime $RuntimeFixture --configuration Release
	if ($LASTEXITCODE -ne 0) { throw 'Managed host fixture package build failed' }
	$TestExecutable = Join-Path $PackageRoot 'GargantuanServer.exe'
	$CaseIndex = 0
	foreach ($Case in @('ReadyDrain', 'AuthorityLoss', 'NeverReady', 'ListenerUnavailable', 'PreReady', 'MissingProvider', 'MissingLifecycle', 'InsecureOverride')) {
		$Port = 41000 + ($PID % 1000) + $CaseIndex
		$CaseIndex++
		$ListenerSocket = $null
		try {
			if ($Case -eq 'ListenerUnavailable') {
				$ListenerSocket = [Net.Sockets.Socket]::new(
					[Net.Sockets.AddressFamily]::InterNetwork,
					[Net.Sockets.SocketType]::Dgram,
					[Net.Sockets.ProtocolType]::Udp)
				$ListenerSocket.ExclusiveAddressUse = $true
				$ListenerSocket.Bind([Net.IPEndPoint]::new([Net.IPAddress]::Loopback, 0))
				$Port = ([Net.IPEndPoint]$ListenerSocket.LocalEndPoint).Port
			}
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
				if ($Case -eq 'NeverReady' -and
					@($ErrorOutput -split '\r?\n') -cnotcontains 'GargantuanServer stopped because packaged runtime startup failed: managed server authority or readiness expired') {
					throw ("Managed host fixture NeverReady did not report the readiness deadline: Exit={0} Out={1} Err={2}" -f
						$Process.ExitCode, $Output, $ErrorOutput)
				}
				if ($Case -eq 'ListenerUnavailable' -and
					@($ErrorOutput -split '\r?\n') -cnotcontains 'GargantuanServer stopped because packaged runtime startup failed: server game-session endpoint failed to start') {
					throw ("Managed host fixture ListenerUnavailable did not report the bind failure: Exit={0} Out={1} Err={2}" -f
						$Process.ExitCode, $Output, $ErrorOutput)
				}
				if ($Process.ExitCode -ne 0 -or -not $Output.Contains('[Runtime:ManagedServerTest] lifecycle case passed')) {
					throw ("Managed host fixture {0} failed: Exit={1} Out={2} Err={3}" -f $Case, $Process.ExitCode, $Output, $ErrorOutput)
				}
			} finally {
				$Process.Dispose()
			}
		} finally {
			if ($null -ne $ListenerSocket) { $ListenerSocket.Dispose() }
		}
	}
	Write-Output '[Runtime:ManagedServerTest] all packaged lifecycle cases passed'
} finally {
	if (Test-Path -LiteralPath $TestRoot) { Remove-Item -LiteralPath $TestRoot -Recurse -Force }
}
