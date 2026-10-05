#requires -Version 7.0
# Only harmless owned Python sleepers and an owned PowerShell sampler are run.
# Adapter queries are supplied fixtures; these tests do not qualify a farm/NIC.
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'PhysicalGameSessionFarmSamplerSourceTests.ps1')
$Endpoint = Join-Path $PSScriptRoot 'PhysicalGameSessionFarmEndpoint.ps1'
$Tokens = $null; $Errors = $null
$Ast = [Management.Automation.Language.Parser]::ParseFile($Endpoint, [ref]$Tokens, [ref]$Errors)
if ($Errors.Count) { throw 'endpoint sampler source syntax failed' }
foreach ($Definition in $Ast.FindAll({ param($Node) $Node -is [Management.Automation.Language.FunctionDefinitionAst] }, $true)) {
	. ([scriptblock]::Create($Definition.Extent.Text))
}
function Expect-SamplerRejection {
	param([scriptblock]$Case, [string]$Text)
	$Rejected = $false
	try { & $Case } catch { if ($_.Exception.Message -notlike "*$Text*") { throw }; $Rejected = $true }
	if (-not $Rejected) { throw "sampler test expected denial: $Text" }
}
function Wait-SamplerFile {
	param([string]$Path, [int]$Limit = 4500)
	$Clock = [Diagnostics.Stopwatch]::StartNew()
	while (-not [IO.File]::Exists($Path)) {
		if ($Clock.ElapsedMilliseconds -ge $Limit) { throw "sampler fixture deadline: $Path" }
		Start-Sleep -Milliseconds 20
	}
}
$ArtifactRoot = [Environment]::GetEnvironmentVariable('GARGANTUAN_SAMPLER_TEST_ARTIFACT_ROOT')
$ParentRoot = if ($ArtifactRoot) { [IO.Path]::GetFullPath($ArtifactRoot) } else { [IO.Path]::GetFullPath([IO.Path]::GetTempPath()) }
$Ancestor = $ParentRoot
while ($Ancestor) {
	if ([IO.File]::Exists($Ancestor) -or ([IO.Directory]::Exists($Ancestor) -and
		([IO.File]::GetAttributes($Ancestor) -band [IO.FileAttributes]::ReparsePoint))) { throw 'sampler fixture artifact parent redirected' }
	$Ancestor = [IO.Path]::GetDirectoryName($Ancestor)
}
$Root = Join-Path $ParentRoot ('farm-sampler-test-' + [Guid]::NewGuid().ToString('N'))
if (Test-Path -LiteralPath $Root) { throw 'sampler fixture root already exists' }
[void][IO.Directory]::CreateDirectory($Root)
$Children = [Collections.Generic.List[object]]::new()
$Samplers = [Collections.Generic.List[object]]::new()
$Finished = $false
$CounterPreparationMilliseconds = 'NOT_MEASURED'
try {
	# Match the endpoint's production precondition: Main prepares the counter
	# assembly before RunStartedTicks and the timed sampler initialization. The
	# child starts cold, loads verified bytes and reaches a real baseline in 5 s.
	$CounterPreparationClock = [Diagnostics.Stopwatch]::StartNew()
	$CounterEvidence = Join-Path $Root 'CounterEvidence'; [void][IO.Directory]::CreateDirectory($CounterEvidence)
	$CounterPreparation = Initialize-HostResourceCounters -PreparationRoot (Join-Path $Root 'CounterRegistry') -EvidenceDirectory $CounterEvidence
	$CounterPreparationMilliseconds = $CounterPreparationClock.ElapsedMilliseconds
	if (-not ('FarmHostNativeCounters' -as [type]) -or -not ('FarmSamplerPipeCopy' -as [type])) {
		throw 'production sampler counter preparation did not establish both types'
	}
	foreach ($Change in @(@('Sha256',('0'*64),'held hash'),@('SourceSha256',('0'*64),'source/runtime/path/size'),
		@('RuntimeSha256',('0'*64),'source/runtime/path/size'),@('Path',(Join-Path $Root 'wrong.dll'),'source/runtime/path/size'),
		@('Bytes',1048577L,'source/runtime/path/size'),@('LoadedAssembly',$null,'loaded type origin'),
		@('FullName','foreign-assembly','type/identity'),@('ModuleVersionId','00000000-0000-0000-0000-000000000000','type/identity'))) {
		$Bad = [ordered]@{}; foreach ($Name in $CounterPreparation.PSObject.Properties.Name) { $Bad[$Name] = $CounterPreparation.$Name }
		$Bad[$Change[0]] = $Change[1]
		Expect-SamplerRejection { Initialize-HostResourceCounters -Preparation ([pscustomobject]$Bad) -ExpectedRoot (Split-Path $CounterPreparation.Path -Parent) } $Change[2]
	}
	$FixtureHost = @'
function Add-HostResourceSample {
 param([object[]]$Owners,[string]$LocalRunId,[string]$LocalRole,[string]$LocalProvider,$Interface,
  [Collections.Generic.List[object]]$Samples,[long]$SupervisorElapsedMilliseconds)
 $Begin = [Diagnostics.Stopwatch]::GetTimestamp()
 if ($null -ne [Environment]::GetEnvironmentVariable('GARGANTUAN_TEST_NODE_SECRET')) { throw 'node secret inherited by sampler' }
 if ($Interface.Name -ceq 'Blocked' -and $Owners.Count) { Start-Sleep -Milliseconds 6000 }
 if ($Interface.Name -ceq 'Throw' -and $Owners.Count) { throw 'supplied adapter failure' }
 $Idle = [ulong]0; $Kernel = [ulong]0; $User = [ulong]0
 if (-not [FarmHostNativeCounters]::GetSystemTimes([ref]$Idle,[ref]$Kernel,[ref]$User)) { throw 'actual harmless host CPU read failed' }
 $Memory = [FarmHostNativeCounters+MemoryStatus]::new()
 $Memory.Length = [Runtime.InteropServices.Marshal]::SizeOf([type][FarmHostNativeCounters+MemoryStatus])
 if (-not [FarmHostNativeCounters]::GlobalMemoryStatusEx([ref]$Memory)) { throw 'actual harmless host memory read failed' }
 $Working = 0L; $Private = 0L
 foreach ($Owner in $Owners) { $Owner.Process.Refresh(); $Working += $Owner.Process.WorkingSet64; $Private += $Owner.Process.PrivateMemorySize64 }
 $Samples.Add([pscustomobject]@{
  RunId=$LocalRunId;Role=$LocalRole;Provider=$LocalProvider;HostName=$Interface.HostName;
  InterfaceIndex=$Interface.Index;InterfaceMacAddress=$Interface.MacAddress;InterfaceAddress=$Interface.Address;
  InterfaceLinkSpeed='10 Gbps';Utc=[DateTimeOffset]::UtcNow.ToString('O');SupervisorElapsedMilliseconds=$SupervisorElapsedMilliseconds;
  SampleStartTicks=$Begin;SampleEndTicks=[Diagnostics.Stopwatch]::GetTimestamp();MonotonicFrequency=[Diagnostics.Stopwatch]::Frequency;
  LiveOwnedProcessCount=$Owners.Count;OwnedWorkingSetBytes=$Working;OwnedPrivateBytes=$Private;
  HostTotalPhysicalBytes=$Memory.TotalPhysical;HostAvailablePhysicalBytes=$Memory.AvailablePhysical;HostCpuIdle100ns=$Idle;
  HostCpuKernel100ns=$Kernel;HostCpuUser100ns=$User;NicSentBytes=0L;NicReceivedBytes=0L;
  NicOutboundDiscardedPackets=0L;NicOutboundPacketErrors=0L;NicReceivedDiscardedPackets=0L;NicReceivedPacketErrors=0L
 })
}
'@
	$OriginalHost = @($Ast.FindAll({ param($Node) $Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -ceq 'Add-HostResourceSample' }, $true))[0].Extent.Text
	$FixtureSource = Join-Path $Root 'EndpointFixture.ps1'
	[IO.File]::WriteAllText($FixtureSource, ([IO.File]::ReadAllText($Endpoint).Replace($OriginalHost,$FixtureHost)), [Text.UTF8Encoding]::new($false))
	$PythonCommand = Get-Command python -CommandType Application -ErrorAction Stop | Select-Object -First 1
	$Python = [IO.Path]::GetFullPath($PythonCommand.Path)
	if (-not [IO.File]::Exists($Python)) { throw 'harmless owned-child runtime absent' }
	$ChildOutput = Join-Path $Root 'children'; [void][IO.Directory]::CreateDirectory($ChildOutput)
	function Start-TestSampler {
		param([string]$Name)
		$Directory = Join-Path $Root $Name; [void][IO.Directory]::CreateDirectory($Directory)
		foreach ($File in @('resource-counter-assembly.dll','resource-counter-preparation.json')) {
			[IO.File]::Copy((Join-Path $CounterEvidence $File),(Join-Path $Directory $File),$false)
		}
		if (-not ('FarmHostNativeCounters' -as [type]) -or -not ('FarmSamplerPipeCopy' -as [type])) {
			throw 'timed sampler fixture lacks production counter precondition'
		}
		$Before = [Diagnostics.Stopwatch]::GetTimestamp()
		$Sampler = $null
		try {
			$Sampler = Start-EndpointResourceSampler -EndpointSource $FixtureSource -OutputDirectory $Directory `
				-LocalRunId '12345678-1234-4234-8234-123456789abc' -LocalRole Clients -LocalProvider Local `
				-Interface ([pscustomobject]@{Index=1;MacAddress='00-11-22-33-44-55';Address='10.253.3.1';Name=$Name;HostName=[Environment]::MachineName}) `
				-StartedTicks ([Diagnostics.Stopwatch]::GetTimestamp()) -TimeoutMilliseconds 30000 -CounterPreparation $CounterPreparation `
				-Registry (Join-Path $Root ('Registry-'+$Name)) -SecretVariables @('GARGANTUAN_TEST_NODE_SECRET')
		} finally {
			# Failure-call elapsed time can include the helper's bounded cleanup;
			# do not mislabel it as an exclusive startup phase measurement.
			$StartCallMilliseconds = ([Diagnostics.Stopwatch]::GetTimestamp()-$Before)*1000/[Diagnostics.Stopwatch]::Frequency
			$Diagnostic = [ordered]@{ Scope='TEST_FIXTURE_ONLY'; ParentCountersPreparedBeforeTimedStart=$true;
				ParentCounterPreparationMilliseconds=$CounterPreparationMilliseconds;
				StartCallMillisecondsIncludingFailureCleanup=$StartCallMilliseconds;
				SamplerInitializationMilliseconds=if ($null -ne $Sampler) { $Sampler.InitializationMilliseconds } else { 'NOT_MEASURED' };
				ChildCounterCompilation='NOT_EXECUTED_VERIFIED_ASSEMBLY_LOAD'; ChildPowerShellStartupMilliseconds='SEE_MONOTONIC_PHASE_BREADCRUMBS';
				SourceParsingAndHashingMilliseconds='NOT_MEASURED'; FirstHostBaselineMilliseconds='NOT_MEASURED' }
			Write-EndpointSamplerJson -Path (Join-Path $Directory 'fixture-initialization.json') -Value $Diagnostic
		}
		if ($StartCallMilliseconds -ge 5000 -or
			$Sampler.InitializationMilliseconds -ge 5000) { throw 'sampler initialization outside five-second bound' }
		$Samplers.Add($Sampler); return $Sampler
	}
	$PreviousSecret = [Environment]::GetEnvironmentVariable('GARGANTUAN_TEST_NODE_SECRET')
	try {
		[Environment]::SetEnvironmentVariable('GARGANTUAN_TEST_NODE_SECRET','SUPPLIED_TEST_VALUE')
		$Fast = Start-TestSampler Fast
	} finally { [Environment]::SetEnvironmentVariable('GARGANTUAN_TEST_NODE_SECRET',$PreviousSecret) }
	# Match production ownership order: sampler initialization and its actual
	# host baseline precede the first native-owner launch/publication.
	for ($Slot = 0; $Slot -lt 32; $Slot++) {
		$Owner = Start-EndpointProcess -Executable $Python -WorkingDirectory $Root -Label ('client-{0:D2}' -f $Slot) `
			-OutputDirectory $ChildOutput -Arguments @('-I','-B','-c','import time; time.sleep(60)')
		$Children.Add($Owner)
		Publish-EndpointSamplerOwner -Sampler $Fast -Owner $Owner
	}
	# Faster startup can make sweep 0001 precede the last owner publication.
	# Wait for actual full-role coverage, within the same fixture wait bound.
	$FullSweepClock = [Diagnostics.Stopwatch]::StartNew()
	while ($true) {
		Assert-EndpointSampler -Sampler $Fast
		$HostPath = Join-Path $Fast.Root 'host-resources.csv'
		if ((Get-Item -LiteralPath $HostPath).Length -gt 16777216) { throw 'sampler host fixture exceeds raw bound' }
		$ObservedHosts = @(Import-Csv -LiteralPath $HostPath)
		$CompleteFullSweep = $false
		for ($Index=0; $Index -lt $ObservedHosts.Count; $Index++) {
			if ($ObservedHosts[$Index].LiveOwnedProcessCount -ceq '32' -and
				[IO.File]::Exists((Join-Path $Fast.Root ('{0:D4}.completed.json' -f $Index)))) { $CompleteFullSweep=$true; break }
		}
		if ($CompleteFullSweep) { break }
		if ($FullSweepClock.ElapsedMilliseconds -ge 4500) { throw 'sampler fixture full32 sweep deadline' }
		Start-Sleep -Milliseconds 20
	}
	$Rows = [Collections.Generic.List[object]]::new(); $Hosts = [Collections.Generic.List[object]]::new()
	Stop-EndpointResourceSampler -Sampler $Fast -Processes $Rows -Hosts $Hosts
	if ($Rows.Count -lt 64 -or $Hosts.Count -lt 2 -or @($Hosts | Where-Object LiveOwnedProcessCount -eq 32).Count -lt 1) { throw 'actual owned-child sampler lacks startup/full32 coverage' }
	foreach ($Label in @($Children.Label)) {
		$OwnRows = @($Rows | Where-Object Label -ceq $Label)
		if ($OwnRows.Count -lt 2 -or [long]$OwnRows[0].MonotonicTicks -ge [long]$OwnRows[1].MonotonicTicks -or
			[long]$OwnRows[0].SupervisorElapsedMilliseconds -ge [long]$OwnRows[1].SupervisorElapsedMilliseconds) { throw 'startup baseline/sweep clock ordering lost' }
	}
	$AcceptanceTokens = $null; $AcceptanceErrors = $null
	$AcceptanceAst = [Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'PhysicalGameSessionFarmAcceptance.ps1'), [ref]$AcceptanceTokens, [ref]$AcceptanceErrors)
	$ReadResources = @($AcceptanceAst.FindAll({ param($Node) $Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -ceq 'Read-ResourceObservation' }, $true))[0]
	. ([scriptblock]::Create($ReadResources.Extent.Text))
	$ProcessObservation = Read-ResourceObservation -Path (Join-Path $Fast.Root 'process-resources.csv') -RunId $Fast.RunId `
		-ExpectedLabels @($Children.Label) -ExpectedCount $Rows.Count -AggregateWorkingSetLimitBytes 2147483648L
	if ($ProcessObservation.ProcessCount -ne 32 -or $ProcessObservation.CompleteSweepCount -lt 1) { throw 'unchanged resource reader rejected actual startup/full32 chronology' }
	$ReadHost = @($AcceptanceAst.FindAll({ param($Node) $Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -ceq 'Read-HostResourceObservation' }, $true))[0]
	. ([scriptblock]::Create($ReadHost.Extent.Text))
	$HostObservation = Read-HostResourceObservation -Path (Join-Path $Fast.Root 'host-resources.csv') -RunId $Fast.RunId `
		-Role Clients -Provider Local -ExpectedLiveProcesses 32 -ExpectedCount $Hosts.Count
	if ($HostObservation.FullRoleSampleCount -lt 1) { throw 'unchanged host resource reader rejected actual native CPU/memory brackets with supplied NIC counters' }
	foreach ($Name in @('Read-BoundedJson','Assert-IndexedFile','Read-RoleEvidence')) {
		$Definition = @($AcceptanceAst.FindAll({ param($Node) $Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -ceq $Name },$true))[0]
		. ([scriptblock]::Create($Definition.Extent.Text))
	}
	# Only this supplied role envelope is synthetic; resource/assembly/archive
	# bytes above are actual harmless observations. No farm/provider PASS claim.
	$Rows | Export-Csv -LiteralPath (Join-Path $Fast.EvidenceDirectory 'process-resources.csv') -NoTypeInformation
	$Hosts | Export-Csv -LiteralPath (Join-Path $Fast.EvidenceDirectory 'host-resources.csv') -NoTypeInformation
	Write-EndpointSamplerJson -Path (Join-Path $Fast.EvidenceDirectory 'run-manifest.json') -Value @{RunId=$Fast.RunId;Scope='SUPPLIED_HARMLESS_FIXTURE'}
	Write-EndpointSamplerJson -Path (Join-Path $Fast.EvidenceDirectory 'result.json') -Value @{RunId=$Fast.RunId;Role='Clients';Status='PASS';Scope='SUPPLIED_HARMLESS_FIXTURE_NOT_PROVIDER_PASS'}
	Write-EndpointEvidenceHash -Directory $Fast.EvidenceDirectory -LocalRunId $Fast.RunId -LocalRole Clients
	$Report = @{ RunId=$Fast.RunId;Provider='Local';ManifestSha256=(Get-FileHash (Join-Path $Fast.EvidenceDirectory 'run-manifest.json')).Hash;
		ClientEvidenceSha256=(Get-FileHash (Join-Path $Fast.EvidenceDirectory 'evidence-sha256.json')).Hash }
	$FlatRole = Read-RoleEvidence -Root $Fast.EvidenceDirectory -Role Clients -Report $Report
	if ($FlatRole.IndexedFiles -gt 128 -or @(Get-ChildItem $Fast.EvidenceDirectory -Directory).Count) { throw 'sampler role evidence is not fully flat/bounded' }
	$Unexpected = Join-Path $Fast.EvidenceDirectory 'unindexed'; [void][IO.Directory]::CreateDirectory($Unexpected)
	try { Expect-SamplerRejection { Read-RoleEvidence -Root $Fast.EvidenceDirectory -Role Clients -Report $Report } 'complete role-local root' }
	finally { [IO.Directory]::Delete($Unexpected) }
	$RawEvidence = Get-Content (Join-Path $Fast.EvidenceDirectory 'resource-sampler-evidence.json') -Raw | ConvertFrom-Json -AsHashtable
	$ArchivePath = Join-Path $Fast.EvidenceDirectory 'resource-sampler-raw.zip'
	Assert-EndpointSamplerArchive -Path $ArchivePath -Files $RawEvidence.Files
	if ($RawEvidence.Archive.Sha256 -ine (Get-FileHash $ArchivePath).Hash -or $RawEvidence.ArchiveFailure) { throw 'sampler archive is not bound to all retained raws' }
	foreach ($Mode in @('Extra','Missing','Hash')) {
		$BadArchive = Join-Path $Root ($Mode+'.zip'); [IO.File]::Copy($ArchivePath,$BadArchive,$false)
		$Zip = [IO.Compression.ZipFile]::Open($BadArchive,[IO.Compression.ZipArchiveMode]::Update)
		try {
			if ($Mode -eq 'Extra') { [void]$Zip.CreateEntry('unknown.json') }
			elseif ($Mode -eq 'Missing') { $Zip.Entries[0].Delete() }
			else {
				$Entry = $Zip.GetEntry('parent-ready.json'); $Original = $Entry.Open(); $Memory = [IO.MemoryStream]::new()
				try { $Original.CopyTo($Memory); $Data=$Memory.ToArray() } finally { $Original.Dispose(); $Memory.Dispose() }
				$Entry.Delete(); $Changed = $Zip.CreateEntry('parent-ready.json'); $Output = $Changed.Open(); $Data[0]=$Data[0] -bxor 1
				try { $Output.Write($Data,0,$Data.Length) } finally { $Output.Dispose() }
			}
		} finally { $Zip.Dispose() }
		Expect-SamplerRejection { Assert-EndpointSamplerArchive -Path $BadArchive -Files $RawEvidence.Files } $(if($Mode -eq 'Hash'){'entry hash'}else{'complete membership'})
	}
	$Oversized = Join-Path $Root 'Oversized.zip'; $Large = [IO.File]::Open($Oversized,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
	try { $Large.SetLength(16777217) } finally { $Large.Dispose() }
	Expect-SamplerRejection { Assert-EndpointSamplerArchive -Path $Oversized -Files $RawEvidence.Files } 'exceeds 16 MiB'
	Expect-SamplerRejection { Assert-EndpointSamplerArchive -Files @([pscustomobject]@{Name='unknown.bin';Bytes=1;Sha256=('0'*64)}) -InventoryOnly } 'raw-file pin'
	$SourceProof = Get-Content -LiteralPath (Join-Path $Fast.EvidenceDirectory 'resource-sampler-source.json') -Raw | ConvertFrom-Json -AsHashtable
	if ($SourceProof.FunctionPins.Count -ne 7 -or $SourceProof.EndpointSha256 -ine (Get-FileHash $FixtureSource).Hash -or
		$SourceProof.ScriptSha256 -ine (Get-FileHash (Join-Path $Fast.Root 'sampler.ps1')).Hash -or $SourceProof.Version -ne 2 -or
		$SourceProof.CounterAssembly.Sha256 -cne $CounterPreparation.Sha256 -or
		(Get-FileHash (Join-Path $Fast.EvidenceDirectory 'resource-counter-assembly.dll')).Hash -ine $CounterPreparation.Sha256) { throw 'generated sampler source closure not pinned' }
	$LoadPhase = Get-Content (Join-Path $Fast.Root 'child-counter-load.json') -Raw | ConvertFrom-Json -AsHashtable
	$BaselinePhase = Get-Content (Join-Path $Fast.Root 'child-first-baseline.json') -Raw | ConvertFrom-Json -AsHashtable
	if ($LoadPhase.AssemblySha256 -cne $CounterPreparation.Sha256 -or $LoadPhase.FullName -cne $CounterPreparation.FullName -or
		$LoadPhase.ModuleVersionId -cne $CounterPreparation.ModuleVersionId -or $LoadPhase.EndedTicks -lt $LoadPhase.StartedTicks -or
		$BaselinePhase.StartedTicks -lt $LoadPhase.EndedTicks -or $BaselinePhase.EndedTicks -lt $BaselinePhase.StartedTicks -or
		$BaselinePhase.LiveOwnedProcessCount -ne 0) { throw 'cold verified load/first actual baseline chronology differs' }
	foreach ($Name in $SourceProof.FunctionPins.Keys) {
		$Body = @($Ast.FindAll({ param($Node) $Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -ceq $Name }, $true))[0].Extent.Text
		if ($Name -ceq 'Add-HostResourceSample') { $Body = $FixtureHost }
		$Actual = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.UTF8Encoding]::new($false).GetBytes($Body))).ToLowerInvariant()
		if ($SourceProof.FunctionPins[$Name] -cne $Actual) { throw 'sampler extracted function body hash differs' }
	}
	$OriginalOwnerBytes = [IO.File]::ReadAllBytes((Join-Path $Fast.Root 'client-00.owner.json'))
	Write-EndpointEvidenceHash -Directory $Fast.EvidenceDirectory -LocalRunId $Fast.RunId -LocalRole Clients
	$RoleIndex = Get-Content -LiteralPath (Join-Path $Fast.EvidenceDirectory 'evidence-sha256.json') -Raw | ConvertFrom-Json
	if (@($RoleIndex.Files.Name | Where-Object { $_ -in @('resource-sampler-source.json','resource-sampler-evidence.json') }).Count -ne 2) { throw 'role seal omitted sampler closure' }
	Expect-SamplerRejection { Publish-EndpointSamplerOwner -Sampler $Fast -Owner $Children[0] } 'already exists'
	if ([Convert]::ToBase64String($OriginalOwnerBytes) -cne [Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $Fast.Root 'client-00.owner.json')))) { throw 'owner duplicate publication changed original bytes' }
	$OwnerFile = Join-Path $Fast.Root 'client-00.owner.json'
	$OwnerRecord = Get-Content $OwnerFile -Raw | ConvertFrom-Json -AsHashtable
	$OwnerRecord.ProcessStartedUtcTicks++
	[IO.File]::WriteAllText($OwnerFile, ($OwnerRecord | ConvertTo-Json -Depth 8 -Compress))
	Expect-SamplerRejection { Get-EndpointSamplerOwners -Root $Fast.Root -LocalRunId $Fast.RunId -LocalRole Clients } 'PID generation changed'
	$OwnerRecord.ProcessStartedUtcTicks--; $OwnerRecord.Pid = $true
	[IO.File]::WriteAllText($OwnerFile, ($OwnerRecord | ConvertTo-Json -Depth 8 -Compress))
	Expect-SamplerRejection { Get-EndpointSamplerOwners -Root $Fast.Root -LocalRunId $Fast.RunId -LocalRole Clients } 'identity record'
	$PipeInput = [IO.MemoryStream]::new([byte[]]::new(4194305)); $PipeOutput = [IO.MemoryStream]::new()
	try {
		$Copy = [FarmSamplerPipeCopy]::Start($PipeInput,$PipeOutput)
		Expect-SamplerRejection { [void]$Copy.Wait(5000) } 'four MiB'
		if (-not $Copy.IsFaulted -or $PipeOutput.Length -ne 4194304) { throw 'bounded dedicated pipe reader concealed overflow or truncated its allowed prefix' }
	} finally { $PipeInput.Dispose(); $PipeOutput.Dispose() }
	$Probe = [pscustomobject]@{Stopped=$false;OutputCopy=[Threading.Tasks.Task]::CompletedTask;ErrorCopy=[Threading.Tasks.Task]::CompletedTask;
		OutputPath=$Fast.OutputPath;ErrorPath=$Fast.ErrorPath;Process=$Children[0].Process;Root=$Fast.Root}
	$PhasePath = Join-Path $Fast.Root 'child-counter-load.started.json'; $PhaseBytes = [IO.File]::ReadAllBytes($PhasePath)
	try {
		[IO.File]::WriteAllText($PhasePath, (@{RunId=$Fast.RunId;StartedTicks=([Diagnostics.Stopwatch]::GetTimestamp()-6*[Diagnostics.Stopwatch]::Frequency)} | ConvertTo-Json -Compress))
		Assert-EndpointSampler -Sampler $Probe # A diagnostic phase is never a host-query clock.
	} finally { [IO.File]::WriteAllBytes($PhasePath,$PhaseBytes) }
	Write-EndpointSamplerJson -Path (Join-Path $Fast.Root '0002.started.json') -Value @{
		RunId=$Fast.RunId;Sequence=2;StartedTicks=([Diagnostics.Stopwatch]::GetTimestamp()-6*[Diagnostics.Stopwatch]::Frequency) }
	Expect-SamplerRejection { Assert-EndpointSampler -Sampler $Probe } 'five-second bound'
	$Slow = Start-TestSampler Blocked
	Publish-EndpointSamplerOwner -Sampler $Slow -Owner $Children[0]
	Wait-SamplerFile (Join-Path $Slow.Root '0001.started.json')
	$PublishClock = [Diagnostics.Stopwatch]::StartNew()
	foreach ($Owner in @($Children | Select-Object -Skip 1)) { Publish-EndpointSamplerOwner -Sampler $Slow -Owner $Owner }
	$PublishMilliseconds = $PublishClock.ElapsedMilliseconds
	if ($PublishMilliseconds -ge 3000 -or @(Get-ChildItem $Slow.Root -Filter '*.owner.json').Count -ne 32) { throw 'blocked query serialized owner publications' }
	# Stop during the supplied six-second query: the cooperative five-second
	# join must fail and the owned helper must be terminated/drained, with raw
	# process rows retained. A failed sampler is never reported as sampler PASS.
	$PartialRows = [Collections.Generic.List[object]]::new(); $PartialHosts = [Collections.Generic.List[object]]::new()
	Expect-SamplerRejection { Stop-EndpointResourceSampler -Sampler $Slow -Processes $PartialRows -Hosts $PartialHosts } 'owned stop bound'
	if (-not $Slow.Stopped -or $PartialRows.Count -lt 2 -or $PartialHosts.Count -ne 1) { throw 'blocked sampler lost raw partials' }
	$Remaining = Get-Process -Id $Slow.Pid -ErrorAction SilentlyContinue
	if ($null -ne $Remaining) { $Remaining.Dispose(); throw 'owned blocked sampler remains alive' }
	$Evidence = Get-Content (Join-Path $Slow.EvidenceDirectory 'resource-sampler-evidence.json') -Raw | ConvertFrom-Json -AsHashtable
	if (-not $Evidence.StreamsJoined -or $null -eq $Evidence.StopFailure -or $Evidence.NativeTreeReaped -cne 'NOT_MEASURED_BY_SAMPLER' -or
		@($Evidence.Files | Where-Object Name -ceq 'process-resources.csv').Count -ne 1) { throw 'blocked sampler receipt concealed its failure/partials' }
	$Throwing = Start-TestSampler Throw
	Publish-EndpointSamplerOwner -Sampler $Throwing -Owner $Children[0]
	Wait-SamplerFile (Join-Path $Throwing.Root 'sampler.result.json')
	$FailureRows = [Collections.Generic.List[object]]::new(); $FailureHosts = [Collections.Generic.List[object]]::new()
	Expect-SamplerRejection { Stop-EndpointResourceSampler -Sampler $Throwing -Processes $FailureRows -Hosts $FailureHosts } 'sampler failed'
	if ($FailureRows.Count -lt 2 -or $FailureHosts.Count -ne 1) { throw 'supplied adapter failure lost process-first raw chronology' }
	$IntegerConfig = Get-Content -LiteralPath (Join-Path $Fast.Root 'configuration.json') -Raw | ConvertFrom-Json -AsHashtable
	if ($IntegerConfig.Interface.Index -isnot [long] -or $IntegerConfig.TimeoutMilliseconds -isnot [long]) { throw 'actual JSON integer representation differs' }
	$IntegerConfig.TimeoutMilliseconds = $true
	$BadConfigPath = Join-Path $Root 'bad-configuration.json'
	[IO.File]::WriteAllText($BadConfigPath, ($IntegerConfig | ConvertTo-Json -Depth 8 -Compress))
	Expect-SamplerRejection { Invoke-EndpointResourceSampler -ConfigurationPath $BadConfigPath } 'configuration/source differs'
	$Bounded = [Collections.Generic.List[object]]::new()
	for ($Index = 0; $Index -lt 20000; $Index++) { $Bounded.Add($null) }
	Expect-SamplerRejection { Add-EndpointResourceSamples -Owners @($Children[0]) -LocalRunId $Fast.RunId -Samples $Bounded -SupervisorElapsedMilliseconds 1 } 'resource evidence limit'
	$HostBounded = [Collections.Generic.List[object]]::new()
	for ($Index = 0; $Index -lt 1000; $Index++) { $HostBounded.Add($null) }
	Expect-SamplerRejection { Add-HostResourceSample -Owners @() -LocalRunId $Fast.RunId -LocalRole Clients -LocalProvider Local `
		-Interface @{} -Samples $HostBounded -SupervisorElapsedMilliseconds 1 } '1000 records'
	$Invalid = Join-Path $Root 'InvalidSource'; [void][IO.Directory]::CreateDirectory($Invalid)
	$InvalidSource = Join-Path $Invalid 'invalid.ps1'
	[IO.File]::WriteAllText($InvalidSource, [IO.File]::ReadAllText($FixtureSource).Replace('function Write-EndpointSamplerCsv {','function Missing-EndpointSamplerCsv {'))
	Expect-SamplerRejection { Start-EndpointResourceSampler -EndpointSource $InvalidSource -OutputDirectory $Invalid -LocalRunId $Fast.RunId `
		-LocalRole Clients -LocalProvider Local -Interface @{} -StartedTicks 1 -TimeoutMilliseconds 30000 -CounterPreparation $CounterPreparation `
		-Registry (Join-Path $Root 'InvalidRegistry') } 'source closure differs'
	Write-Output "[Qualification:ResourceSampler] OWNED_CHILD_TEST_OK Children=32 ParentCounterPreparationMilliseconds=$CounterPreparationMilliseconds InitializationMilliseconds=$($Fast.InitializationMilliseconds) PublishWhileBlockedMilliseconds=$PublishMilliseconds ActualProcessRows=$($Rows.Count) HostRows=$($Hosts.Count) FailedPartialRows=$($PartialRows.Count) Adapter=SUPPLIED_FIXTURE Farm=NOT_MEASURED"
	$Finished = $true
} finally {
	foreach ($Sampler in $Samplers) {
		if (-not $Sampler.Stopped) { try { Stop-EndpointResourceSampler -Sampler $Sampler -Processes ([Collections.Generic.List[object]]::new()) -Hosts ([Collections.Generic.List[object]]::new()) } catch {} }
	}
	foreach ($Child in $Children) { Stop-EndpointProcess -Owner $Child }
	$Resolved = [IO.Path]::GetFullPath($Root)
	$Temp = $ParentRoot.TrimEnd('\','/')
	if (-not $Resolved.StartsWith($Temp + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
		[IO.Path]::GetFileName($Resolved) -cnotmatch '^farm-sampler-test-[a-f0-9]{32}$') { throw 'sampler test cleanup outside owned temp root' }
	if ($Finished -and -not $ArtifactRoot) { Remove-Item -LiteralPath $Resolved -Recurse -Force }
	elseif ($Finished) { Write-Output "[Qualification:ResourceSampler] OWNED_RAW_RETAINED Root=$Resolved Scope=HARMLESS_FIXTURE_ONLY" }
	else { Write-Output "[Qualification:ResourceSampler] FAILED_RAW_RETAINED Root=$Resolved ParentCounterPreparationMilliseconds=$CounterPreparationMilliseconds ExclusiveInitializationPhases=NOT_MEASURED" }
}
