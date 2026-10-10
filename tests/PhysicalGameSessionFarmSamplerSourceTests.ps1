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
	$Node -is [Management.Automation.Language.CommandAst] -and $Node.GetCommandName() -ceq 'Initialize-HostResourceCounters' -and
	$Node.Extent.Text -cmatch '-PreparationRoot '},$true))
$TimedFixture = @($Fixture.FindAll({param($Node)
	$Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -ceq 'Start-TestSampler'},$true))
if ($FixturePreparation.Count -ne 1 -or $TimedFixture.Count -ne 1 -or
	$FixturePreparation[0].Extent.StartOffset -ge $TimedFixture[0].Extent.StartOffset) { throw 'fixture must establish the production precondition before timed initialization' }
$Initializer = @($Endpoint.FindAll({param($Node)
	$Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -ceq 'Initialize-HostResourceCounters'},$true))
$Emit = @($Initializer[0].Body.FindAll({param($Node)
	$Node -is [Management.Automation.Language.CommandAst] -and $Node.GetCommandName() -ceq 'Add-Type'},$true))
if ($Initializer.Count -ne 1 -or $Emit.Count -ne 1 -or $Emit[0].Extent.Text -cnotmatch '-OutputAssembly \$AssemblyPath' -or
	$Emit[0].Parent.Extent.Text -cnotmatch 'Add-Type' -or
	$Initializer[0].Extent.Text -cnotmatch '\[Reflection.Assembly\]::Load\(\$Bytes\)' -or
	$Initializer[0].Extent.Text -cnotmatch '\$Held.ReadExactly\(\$Bytes,0,\$Bytes.Length\)' -or
	$Initializer[0].Extent.Text -cnotmatch 'ReferenceEquals\(\$CounterType.Assembly,\$Assembly\)') { throw 'fixed emitted assembly must load exact held bytes with type-origin proof' }
$Start = @($Endpoint.FindAll({param($Node)
	$Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -ceq 'Start-EndpointResourceSampler'},$true))[0]
$Checks = @($Start.Body.FindAll({param($Node)
	$Node -is [Management.Automation.Language.BinaryExpressionAst] -and $Node.Extent.Text -ceq '$InitializationClock.ElapsedMilliseconds -ge 5000'},$true))
if ($Checks.Count -ne 2) { throw 'both actual five-second initialization checks must remain intact' }
$Child = @($Endpoint.FindAll({param($Node)
	$Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -ceq 'Invoke-EndpointResourceSampler'},$true))[0]
$ChildLoad = @($Child.Body.FindAll({param($Node)
	$Node -is [Management.Automation.Language.CommandAst] -and $Node.GetCommandName() -ceq 'Initialize-HostResourceCounters'},$true))
if ($ChildLoad.Count -ne 1 -or $ChildLoad[0].Extent.Text -cnotmatch '-Preparation ' -or
	$ChildLoad[0].Extent.Text -cmatch '-PreparationRoot' -or $ChildLoad[0].Extent.Text -cnotmatch '-ExpectedRoot \$Root') {
	throw 'child must load its exact assembly without compilation within real initialization'
}
$HostSample = @($Child.Body.FindAll({param($Node)
	$Node -is [Management.Automation.Language.CommandAst] -and $Node.GetCommandName() -ceq 'Add-HostResourceSample'},$true))[0]
$Ready = @($Child.Body.FindAll({param($Node)
	$Node -is [Management.Automation.Language.CommandAst] -and $Node.Extent.Text -cmatch "Write-EndpointSamplerJson.*sampler.ready.json"},$true))[0]
if ($null -eq $HostSample -or $null -eq $Ready -or $HostSample.Extent.StartOffset -ge $Ready.Extent.StartOffset -or
	$Child.Extent.Text -cnotmatch 'SampleEndTicks - \$Hosts\[0\].SampleStartTicks\).* -gt 5000' -or
	$Child.Extent.Text -cnotmatch 'Elapsed - \$LastElapsed -lt 2000') { throw 'real first baseline/query bound/two-second cadence must remain' }
$Stop = @($Endpoint.FindAll({param($Node)
	$Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -ceq 'Stop-EndpointResourceSampler'},$true))[0]
if ($Stop.Extent.Text -cnotmatch 'WaitForExit\(5000\)' -or $Stop.Extent.Text -cnotmatch 'FileMode\]::CreateNew' -or
	$Stop.Extent.Text -cnotmatch 'Output.Length -gt 16777216' -or $Stop.Extent.Text -cnotmatch 'Sampler.EvidenceDirectory' -or
	$Start.Extent.Text -cnotmatch 'resource sampler registry must remain outside flat evidence') { throw 'bounded owned cleanup/flat archive retention must remain' }
$Workflow = [IO.File]::ReadAllText((Join-Path $PSScriptRoot '..\.github\workflows\native-ci.yml'))
if ($Fixture.Extent.Text -cnotmatch 'GARGANTUAN_SAMPLER_TEST_ARTIFACT_ROOT' -or
	$Fixture.Extent.Text -cnotmatch 'farm-sampler-test-' -or $Workflow -cnotmatch 'GARGANTUAN_SAMPLER_TEST_ARTIFACT_ROOT:' -or
	$Workflow -cnotmatch '(?s)Upload native CI diagnostics.*if: always\(\).*build-ci/sampler-test-diagnostics/') {
	throw 'fixed CI artifact path must retain actual owned sampler failure partials'
}
Write-Output '[Qualification:ResourceSamplerSource] PASS cases=9 scope=AST_ONLY actualInitialization=NOT_MEASURED'
