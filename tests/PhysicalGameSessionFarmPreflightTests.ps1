#requires -Version 7.0
# Pure, read-only validation of role-local resource inventory semantics.
$ErrorActionPreference = 'Stop'
$Source = Join-Path $PSScriptRoot 'PhysicalGameSessionFarmPreflight.ps1'
$Tokens = $null; $Errors = $null
$Ast = [Management.Automation.Language.Parser]::ParseFile($Source, [ref]$Tokens, [ref]$Errors)
if ($Errors.Count -ne 0) { throw "preflight syntax failed: $($Errors[0].Message)" }
$Function = @($Ast.FindAll({ param($Node)
	$Node -is [Management.Automation.Language.FunctionDefinitionAst] -and
	$Node.Name -eq 'Assert-PreflightInventory'
}, $true))
if ($Function.Count -ne 1) { throw 'preflight inventory validator is missing or duplicated' }
. ([scriptblock]::Create($Function[0].Extent.Text))

function Expect-Rejection {
	param([scriptblock]$Case, [string]$Reason)
	$Rejected = $false
	try { & $Case } catch { $Rejected = $true }
	if (-not $Rejected) { throw "expected preflight rejection: $Reason" }
}

$Manifest = @{ Endpoint = '10.253.3.2:39450'; Provider = 'Local' }
$Good = [ordered]@{
	LogicalProcessors = 16; AvailableMemoryBytes = 16GB
	EvidenceDriveFreeBytes = 10GB; PackageDriveFreeBytes = 10GB
	ProcessCount = 100; ThreadCount = 1500
	GameInterfaceStatus = 'Up'; GameInterfaceLinkSpeed = '10 Gbps'
	GameInterfaceMtu = 1500; GameInterfaceIndex = 19
	GameInterfaceAddress = '10.253.3.2'; GameUdpPortOccupied = $false
	NodeRootCertificateMatches = $false; NodeTokenPresent = $false
}
Assert-PreflightInventory -Inventory $Good -LocalRole Server -RunManifest $Manifest
$Client = [ordered]@{} + $Good
$Client.GameInterfaceAddress = '10.253.3.1'
Assert-PreflightInventory -Inventory $Client -LocalRole Clients -RunManifest $Manifest
$Client.GameInterfaceAddress = '10.253.3.2'
Expect-Rejection { Assert-PreflightInventory -Inventory $Client -LocalRole Clients -RunManifest $Manifest } `
	'client cannot claim the worker fiber address'
$Client.GameInterfaceAddress = '10.253.3.1'
$Client.AvailableMemoryBytes = 4GB
Expect-Rejection { Assert-PreflightInventory -Inventory $Client -LocalRole Clients -RunManifest $Manifest } `
	'32-client worker lacks 8 GiB free memory'
$Client.AvailableMemoryBytes = 16GB
$Client.EvidenceDriveFreeBytes = 1GB
Expect-Rejection { Assert-PreflightInventory -Inventory $Client -LocalRole Clients -RunManifest $Manifest } `
	'evidence drive lacks bounded-capture headroom'
$Client.EvidenceDriveFreeBytes = 10GB
$Client.GameInterfaceLinkSpeed = '1 Gbps'
Expect-Rejection { Assert-PreflightInventory -Inventory $Client -LocalRole Clients -RunManifest $Manifest } `
	'wrong client link speed'
$Server = [ordered]@{} + $Good
$Server.GameUdpPortOccupied = $true
Expect-Rejection { Assert-PreflightInventory -Inventory $Server -LocalRole Server -RunManifest $Manifest } `
	'game UDP port already bound'
$Server.GameUdpPortOccupied = $false
$NodeManifest = @{ Endpoint = '10.253.3.2:39450'; Provider = 'Node' }
Expect-Rejection { Assert-PreflightInventory -Inventory $Server -LocalRole Server -RunManifest $NodeManifest } `
	'Node root or token absent'
$Server.NodeRootCertificateMatches = $true
$Server.NodeTokenPresent = $true
Assert-PreflightInventory -Inventory $Server -LocalRole Server -RunManifest $NodeManifest
Write-Output '[Qualification:FarmPreflight] MOCK_TEST_OK'
