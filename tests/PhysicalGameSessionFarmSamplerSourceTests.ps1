#requires -Version 7.0
# Pure AST checks only: no Add-Type, native counters, sampler or owned children.
$ErrorActionPreference = 'Stop'
function Read-SamplerAst {
	param([string]$Name)
	$Tokens = $null; $Errors = $null
	$Tree = [Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $Name),[ref]$Tokens,[ref]$Errors)
	if ($Errors.Count) { throw "sampler source syntax failed: $Name" }
	return $Tree
}
$Endpoint = Read-SamplerAst 'PhysicalGameSessionFarmEndpoint.ps1'
$Fixture = Read-SamplerAst 'PhysicalGameSessionFarmSamplerTests.ps1'
$Main = @($Endpoint.EndBlock.Statements | Where-Object { $_ -is [Management.Automation.Language.TryStatementAst] })
if ($Main.Count -ne 1) { throw 'endpoint production Main must remain explicit' }
$ProductionPreparation = @($Main[0].Body.FindAll({param($Node)
	$Node -is [Management.Automation.Language.CommandAst] -and $Node.GetCommandName() -ceq 'Initialize-HostResourceCounters'},$true))
$RunClock = @($Main[0].Body.FindAll({param($Node)
	$Node -is [Management.Automation.Language.AssignmentStatementAst] -and $Node.Left.Extent.Text -ceq '$RunStartedTicks'},$true))
if ($ProductionPreparation.Count -ne 1 -or $RunClock.Count -ne 1 -or
	$ProductionPreparation[0].Extent.StartOffset -ge $RunClock[0].Extent.StartOffset) { throw 'production counter preparation must precede run clock' }
$FixturePreparation = @($Fixture.FindAll({param($Node)
	$Node -is [Management.Automation.Language.CommandAst] -and $Node.GetCommandName() -ceq 'Initialize-HostResourceCounters'},$true))
$TimedFixture = @($Fixture.FindAll({param($Node)
	$Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -ceq 'Start-TestSampler'},$true))
if ($FixturePreparation.Count -ne 1 -or $TimedFixture.Count -ne 1 -or
	$FixturePreparation[0].Extent.StartOffset -ge $TimedFixture[0].Extent.StartOffset) { throw 'fixture must establish the production precondition before timed initialization' }
$Initializer = @($Endpoint.FindAll({param($Node)
	$Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -ceq 'Initialize-HostResourceCounters'},$true))
$Body = $Initializer[0].Body.EndBlock.Statements
if ($Initializer.Count -ne 1 -or $Body[0] -isnot [Management.Automation.Language.IfStatementAst] -or
	$Body[0].Extent.Text -cnotmatch "FarmHostNativeCounters" -or $Body[0].Extent.Text -cnotmatch '\breturn\b') { throw 'prepared parent must retain cached-type return' }
$Start = @($Endpoint.FindAll({param($Node)
	$Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -ceq 'Start-EndpointResourceSampler'},$true))[0]
$Checks = @($Start.Body.FindAll({param($Node)
	$Node -is [Management.Automation.Language.BinaryExpressionAst] -and $Node.Extent.Text -ceq '$InitializationClock.ElapsedMilliseconds -ge 5000'},$true))
if ($Checks.Count -ne 2) { throw 'both actual five-second initialization checks must remain intact' }
$Child = @($Endpoint.FindAll({param($Node)
	$Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -ceq 'Invoke-EndpointResourceSampler'},$true))[0]
if (@($Child.Body.FindAll({param($Node)
	$Node -is [Management.Automation.Language.CommandAst] -and $Node.GetCommandName() -ceq 'Initialize-HostResourceCounters'},$true)).Count -ne 1) { throw 'child must still prepare its own counters within real initialization' }
Write-Output '[Qualification:ResourceSamplerSource] PASS cases=6 scope=AST_ONLY actualInitialization=NOT_MEASURED'
