param(
	[Parameter(Mandatory = $true)][string]$Path,
	[Parameter(Mandatory = $true)][string]$RunId,
	[string[]]$ExpectedConnections = @()
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'AdmissionFairnessEvidence.ps1')
$Result = Read-AdmissionFairnessEvidence -Path $Path -RunId $RunId `
	-ExpectedConnections $ExpectedConnections
$Result | ConvertTo-Json -Depth 6
