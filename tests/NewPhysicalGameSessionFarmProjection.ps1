#requires -Version 7.0
# Fixed, non-operational preparation: copy only the verified native package
# closure into a fresh registry-owned run directory. Originals are never edited.
param(
	[Parameter(Mandatory = $true)][string]$PackageRoot,
	[Parameter(Mandatory = $true)][string]$RunRegistryRoot,
	[Parameter(Mandatory = $true)][string]$RunId,
	[Parameter(Mandatory = $true)][ValidateSet('Server', 'Clients')][string]$Role,
	[Parameter(Mandatory = $true)][ValidatePattern('^[a-fA-F0-9]{64}$')][string]$DeploymentSha256,
	[Parameter(Mandatory = $true)][ValidatePattern('^[a-f0-9]{40}$')][string]$SourceCommit
)
$ErrorActionPreference = 'Stop'
$Tokens = $null; $Errors = $null
$Ast = [Management.Automation.Language.Parser]::ParseFile(
	(Join-Path $PSScriptRoot 'PhysicalGameSessionFarmEndpoint.ps1'), [ref]$Tokens, [ref]$Errors)
if ($Errors.Count -ne 0) { throw 'endpoint validator syntax invalid' }
$Names = @('Assert-DeploymentManifest', 'Assert-ProjectionPlainPath', 'Read-ProjectionJson', 'Get-NativeRuntimePlan',
	'Get-RuntimeProjectionPath', 'Assert-RuntimeProjection', 'New-RuntimeProjection')
$Definitions = @($Ast.FindAll({ param($Node)
	$Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -in $Names
}, $true))
if ($Definitions.Count -ne $Names.Count) { throw 'runtime projection function set incomplete' }
foreach ($Definition in $Definitions) { . ([scriptblock]::Create($Definition.Extent.Text)) }
$Plan = Get-NativeRuntimePlan -LocalRoot $PackageRoot -DeploymentSha256 $DeploymentSha256 `
	-SourceCommit $SourceCommit -LocalRole $Role
$Runtime = New-RuntimeProjection -LocalRoot $PackageRoot -Registry $RunRegistryRoot -LocalRunId $RunId -LocalRole $Role -Plan $Plan
Write-Output "[Qualification:RuntimeProjection] PREPARED run=$RunId role=$Role runtime=$Runtime"
