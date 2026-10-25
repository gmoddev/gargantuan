#requires -Version 7.0
# Socket-free host sampler contract test. No server, Player, capture, or NIC traffic starts.
$ErrorActionPreference = 'Stop'
$Source = Join-Path $PSScriptRoot 'PhysicalGameSessionFarmEndpoint.ps1'
$Tokens = $null; $Errors = $null
$Ast = [Management.Automation.Language.Parser]::ParseFile($Source, [ref]$Tokens, [ref]$Errors)
if ($Errors.Count -ne 0) { throw 'farm endpoint syntax is invalid' }
$Names = @('Initialize-HostResourceCounters', 'Add-HostResourceSample', 'Write-EndpointSamplerJson')
$Definitions = @($Ast.FindAll({ param($Node)
	$Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -in $Names
}, $true))
if ($Definitions.Count -ne $Names.Count) { throw 'host resource sampler functions are missing' }
foreach ($Definition in $Definitions) { . ([scriptblock]::Create($Definition.Extent.Text)) }

function Get-NetAdapter {
	param([int]$InterfaceIndex)
	if ($InterfaceIndex -ne 22) { throw 'unrecognized mock interface' }
	return [pscustomobject]@{ Status = 'Up'; LinkSpeed = '10 Gbps';
		MacAddress = 'AA-BB-CC-DD-EE-02'; Name = 'Mellanox Mock' }
}
function Get-NetAdapterStatistics {
	param([Parameter(ValueFromPipeline = $true)]$InputObject)
	process {
		return [pscustomobject]@{ SentBytes = 1234; ReceivedBytes = 5678
			OutboundDiscardedPackets = 0; OutboundPacketErrors = 0
			ReceivedDiscardedPackets = 0; ReceivedPacketErrors = 0 }
	}
}

$PreparationRoot = Join-Path ([IO.Path]::GetTempPath()) ('farm-host-resource-test-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($PreparationRoot)
$CounterPreparation = Initialize-HostResourceCounters -PreparationRoot (Join-Path $PreparationRoot 'CounterRegistry') -EvidenceDirectory $PreparationRoot
$Owner = [pscustomobject]@{ Process = (Get-Process -Id $PID) }
$Interface = [pscustomobject]@{ Index = 22; MacAddress = 'AA-BB-CC-DD-EE-02'
	Address = '10.253.3.2'; Name = 'Mellanox Mock'; HostName = 'WORKER' }
$Rows = [Collections.Generic.List[object]]::new()
try {
	Add-HostResourceSample -Owners @($Owner) `
		-LocalRunId '7c93e53d-0e0c-4b8d-8a3b-9a761a406ebd' `
		-LocalRole 'Server' -LocalProvider 'Local' -Interface $Interface -Samples $Rows `
		-SupervisorElapsedMilliseconds 2000
	if ($Rows.Count -ne 1 -or $Rows[0].LiveOwnedProcessCount -ne 1 -or
		$Rows[0].HostTotalPhysicalBytes -lt $Rows[0].HostAvailablePhysicalBytes -or
		$Rows[0].HostCpuKernel100ns -lt $Rows[0].HostCpuIdle100ns -or
		$Rows[0].SampleEndTicks -lt $Rows[0].SampleStartTicks -or
		$Rows[0].NicSentBytes -ne 1234) {
		throw 'native host sampler did not produce a coherent bounded observation'
	}
	$Interface.MacAddress = 'AA-BB-CC-DD-EE-03'
	$Rejected = $false
	try {
		Add-HostResourceSample -Owners @($Owner) `
			-LocalRunId '7c93e53d-0e0c-4b8d-8a3b-9a761a406ebd' `
			-LocalRole 'Server' -LocalProvider 'Local' -Interface $Interface -Samples $Rows `
			-SupervisorElapsedMilliseconds 4000
	} catch { $Rejected = $true }
	if (-not $Rejected -or $Rows.Count -ne 1) { throw 'changed interface identity was sampled' }
	Write-Output '[Qualification:FarmHostResources] MOCK_TEST_OK'
} finally {
	$Owner.Process.Dispose()
	$Resolved = [IO.Path]::GetFullPath($PreparationRoot)
	$Temporary = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/')
	if (-not $Resolved.StartsWith($Temporary+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or
		[IO.Path]::GetFileName($Resolved) -cnotmatch '^farm-host-resource-test-[a-f0-9]{32}$') { throw 'host resource cleanup outside owned root' }
	Remove-Item -LiteralPath $Resolved -Recurse -Force
}
