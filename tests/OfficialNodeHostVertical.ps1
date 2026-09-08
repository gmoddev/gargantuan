param(
	[Parameter(Mandatory = $true)][ValidateSet('Prepare', 'Node', 'NodeCycle', 'NodeResidentSurvival', 'NodeCorrupt', 'NodeBootstrapFailure', 'Local', 'Churn')][string]$Mode,
	[string]$Packager,
	[string]$PlayerRuntimeDistribution,
	[string]$ServerRuntimeDistribution,
	[string]$ProjectRoot,
	[string]$TestRoot,
	[string]$DescriptorFile,
	[string]$NodeEndpoint,
	[string]$RootCertificateFile,
	[int]$RemoteFunctionCallCount = 5,
	[ValidateRange(0, 512)][int]$ContentObjectCount = 0,
	[ValidateRange(0, 1536)][int]$ContentNamePadding = 0,
	[ValidateRange(1, 10000)][int]$ChurnCycles = 1000,
	[switch]$ServerOnlyChurn,
	[ValidateRange(0, 65535)][int]$LifecycleGamePort = 0,
	[string]$MemoryOutputPrefix,
	[string]$TokenEnvironmentName = 'GARGANTUAN_ENGINE_ADAPTER_TOKEN',
	[string]$WrongTokenEnvironmentName = 'GARGANTUAN_ENGINE_ADAPTER_WRONG_TOKEN',
	[string]$LimitedTokenEnvironmentName = 'GARGANTUAN_ENGINE_ADAPTER_LIMITED_TOKEN',
	[string]$WrongTenantTokenEnvironmentName = 'GARGANTUAN_ENGINE_ADAPTER_TENANT_B_TOKEN',
	[string]$MissingTokenEnvironmentName = 'GARGANTUAN_ENGINE_ADAPTER_MISSING_TOKEN'
)

$ErrorActionPreference = 'Stop'

function Require-Value {
	param([string]$Name, [string]$Value)
	if ([string]::IsNullOrWhiteSpace($Value)) {
		throw "Missing $Name"
	}
}

function Disable-ProjectScripts {
	param($Node)
	if ($Node.ClassName -eq 'Script' -and $Node.Properties.Enabled) {
		$Node.Properties.Enabled.Bool = $false
	}
	foreach ($Child in @($Node.Children)) {
		Disable-ProjectScripts -Node $Child
	}
}

function Get-DocumentObjectCount {
	param($Node)
	$Count = 1
	foreach ($Child in @($Node.Children)) { $Count += Get-DocumentObjectCount -Node $Child }
	return $Count
}

function New-RemoteNode {
	param([string]$Name, [string]$ClassName, [string]$ClassSchemaId)
	return [pscustomobject][ordered]@{
		Name = $Name
		ClassName = $ClassName
		ClassSchemaId = $ClassSchemaId
		ClassDefinitionVersion = 1
		Properties = [pscustomobject]@{}
		Attributes = [pscustomobject]@{}
		Extensions = @()
		CustomProperties = @()
		Tags = @()
		Children = @()
	}
}

function Copy-ScriptTemplate {
	param($Template, [string]$Name, [string]$RunContext, [string]$Source)
	$Result = $Template | ConvertTo-Json -Depth 100 -Compress | ConvertFrom-Json
	$Result.Name = $Name
	$Result.Properties.Enabled.Bool = $true
	$Result.Properties.RunContext.EnumItem[1] = $RunContext
	$Result.Properties.Source.String = $Source
	return $Result
}

function Start-RuntimeProcess {
	param(
		[Parameter(Mandatory = $true)][string]$Executable,
		[Parameter(Mandatory = $true)][string]$WorkingDirectory,
		[string[]]$Arguments,
		[string[]]$RemoveEnvironmentVariables = @()
	)
	$StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
	$StartInfo.FileName = $Executable
	$StartInfo.WorkingDirectory = $WorkingDirectory
	$StartInfo.UseShellExecute = $false
	$StartInfo.CreateNoWindow = $true
	$StartInfo.RedirectStandardOutput = $true
	$StartInfo.RedirectStandardError = $true
	$StartInfo.Arguments = ($Arguments | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }) -join ' '
	$StartInfo.Environment['PATH'] = "$WorkingDirectory;$env:SystemRoot\System32;$env:SystemRoot"
	$StartInfo.Environment['SDL_LOGGING'] = '*=info'
	foreach ($EnvironmentVariable in $RemoveEnvironmentVariables) {
		[void]$StartInfo.Environment.Remove($EnvironmentVariable)
	}
	return [System.Diagnostics.Process]::Start($StartInfo)
}

function Complete-RuntimeProcess {
	param(
		[Parameter(Mandatory = $true)]$Process,
		[Parameter(Mandatory = $true)][string]$Label,
		[int]$TimeoutMilliseconds = 30000
	)
	$OutputRead = $Process.StandardOutput.ReadToEndAsync()
	$ErrorRead = $Process.StandardError.ReadToEndAsync()
	if (-not $Process.WaitForExit($TimeoutMilliseconds)) {
		Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
		throw "$Label timed out"
	}
	$Output = $OutputRead.GetAwaiter().GetResult() + $ErrorRead.GetAwaiter().GetResult()
	return [pscustomobject]@{ ExitCode = $Process.ExitCode; Output = $Output }
}

function Invoke-Runtime {
	param(
		[Parameter(Mandatory = $true)][string]$Executable,
		[Parameter(Mandatory = $true)][string]$WorkingDirectory,
		[Parameter(Mandatory = $true)][string]$Label,
		[string[]]$Arguments,
		[string[]]$RemoveEnvironmentVariables = @(),
		[int]$ExpectedExitCode = 0,
		[int]$TimeoutMilliseconds = 30000
	)
	$Process = Start-RuntimeProcess -Executable $Executable -WorkingDirectory $WorkingDirectory -Arguments $Arguments `
		-RemoveEnvironmentVariables $RemoveEnvironmentVariables
	$Result = Complete-RuntimeProcess -Process $Process -Label $Label -TimeoutMilliseconds $TimeoutMilliseconds
	if ($Result.ExitCode -ne $ExpectedExitCode) {
		throw "$Label returned $($Result.ExitCode), expected $ExpectedExitCode`n$($Result.Output)"
	}
	return $Result.Output
}

function Require-Marker {
	param([string]$Label, [string]$Output, [string]$Marker)
	if (-not $Output.Contains($Marker)) {
		Write-Output "[Content:OfficialHost] Missing marker $Marker from $Label; captured output follows"
		Write-Output $Output
		throw "$Label did not emit $Marker`n$Output"
	}
}

if ($Mode -eq 'Prepare') {
	if ($RemoteFunctionCallCount -lt 1 -or $RemoteFunctionCallCount -gt 100) {
		throw 'RemoteFunctionCallCount must be between 1 and 100'
	}
	foreach ($Pair in @(
		@('Packager', $Packager),
		@('PlayerRuntimeDistribution', $PlayerRuntimeDistribution),
		@('ServerRuntimeDistribution', $ServerRuntimeDistribution),
		@('ProjectRoot', $ProjectRoot),
		@('TestRoot', $TestRoot),
		@('DescriptorFile', $DescriptorFile)
	)) {
		Require-Value -Name $Pair[0] -Value $Pair[1]
	}
	$FixtureRoot = Join-Path $TestRoot 'OfficialNodeHostProject'
	$PlayerPackageRoot = Join-Path $TestRoot 'PlayerPackage'
	$ServerPackageRoot = Join-Path $TestRoot 'ServerPackage'
	Copy-Item -LiteralPath $ProjectRoot -Destination $FixtureRoot -Recurse -Force
	$ProjectDocumentPath = Join-Path $FixtureRoot '.gargantuan\project.instance.json'
	$ProjectDocument = Get-Content -LiteralPath $ProjectDocumentPath -Raw | ConvertFrom-Json
	Disable-ProjectScripts -Node $ProjectDocument
	if ($ContentObjectCount -ne 0) {
		$WorkspaceNode = $ProjectDocument.Children | Where-Object Name -eq 'Workspace'
		$CourseNode = $WorkspaceNode.Children | Where-Object Name -eq 'CollectionCourse'
		$GroundNode = $CourseNode.Children | Where-Object Name -eq 'Ground'
		$CurrentCount = Get-DocumentObjectCount -Node $CourseNode
		if (-not $GroundNode -or $CurrentCount -gt $ContentObjectCount) { throw 'Invalid content memory profile' }
		while ($CurrentCount -lt $ContentObjectCount) {
			$Part = $GroundNode | ConvertTo-Json -Depth 100 -Compress | ConvertFrom-Json
			$Part.Name = "MemoryPart$CurrentCount" + ('x' * $ContentNamePadding)
			$Part.Properties.CanCollide.Bool = $false
			$Part.Properties.CanTouch.Bool = $false
			$Part.Properties.Size.Vector3 = @(2, 1, 3)
			$Part.Properties.CFrame.CFrame[0] = $CurrentCount % 32
			$Part.Properties.CFrame.CFrame[1] = 2
			$Part.Properties.CFrame.CFrame[2] = [math]::Floor($CurrentCount / 32)
			$CourseNode.Children = @($CourseNode.Children) + @($Part)
			$CurrentCount++
		}
	}
	$GameScripts = $ProjectDocument.Children | Where-Object {
		$_.Name -eq 'GameScripts' -and $_.ClassName -eq 'Folder'
	} | Select-Object -First 1
	$ScriptTemplate = $GameScripts.Children | Where-Object { $_.ClassName -eq 'Script' } | Select-Object -First 1
	if (-not $GameScripts -or -not $ScriptTemplate) {
		throw 'Official Node host fixture has no script template'
	}
	$Event = New-RemoteNode -Name 'ContentSessionEvent' -ClassName 'RemoteEvent' -ClassSchemaId '321dbe4047c7b7b9ee6d5f1f6d7ee785'
	$Function = New-RemoteNode -Name 'ContentSessionFunction' -ClassName 'RemoteFunction' -ClassSchemaId '5cce39c76c37c9ebbabf0ced0f4ac5a7'
	$ServerSource = @'
local CharacterControl = game:GetService("CharacterControlService")
local Players = game:GetService("Players")
local RunService = game:GetService("RunService")
local Event = game:FindFirstChild("ContentSessionEvent", true)
local Function = game:FindFirstChild("ContentSessionFunction", true)
local PendingAttributes = {}

assert(Event and Function)
assert(CharacterControl:RegisterAction(
	"PackageLunge",
	"asset://d9d9e9649adbad59588d137c2a642e1d",
	0.5,
	Vector3.new(0.9, 0, 0),
	0,
	true
))
CharacterControl:SetActionPolicy(function(Player, Character, ActionName)
	local Accepted = ActionName == "PackageLunge" and Player.Character == Character
	if Accepted then Character:SetAttribute("PackageActionAuthorized", true) end
	return Accepted
end)

local function PrepareCharacter(Character)
	if Character:FindFirstChild("PackageSessionRig") then return end
	local Rig = Instance.new("MeshPart")
	Rig.Name = "PackageSessionRig"
	Rig.Mesh = "asset://d549080bd1e64aaee8041f4ece3e9f75"
	Rig.Anchored = true
	Rig.CanCollide = false
	Rig.CanTouch = false
	Rig.Size = Vector3.new(2, 2, 2)
	Rig.CFrame = Character.CFrame
	Rig.Parent = Character
	local Animator = Instance.new("Animator")
	Animator.Name = "PackageSessionAnimator"
	Animator.Parent = Rig
end
local function PreparePlayer(Player)
	Player.CharacterAdded:Connect(PrepareCharacter)
	if Player.Character then PrepareCharacter(Player.Character) end
end
Players.PlayerAdded:Connect(PreparePlayer)
for _, Player in Players:GetPlayers() do PreparePlayer(Player) end

Event.OnServerEvent:Connect(function(Peer, Message)
	if type(Peer) == "table" and type(Message) == "string" then PendingAttributes[Message] = true end
end)
Function:SetServerHandler(function(Peer, Message)
	if type(Peer) == "table" and Message == "node-content-ping" then
		PendingAttributes.RemoteFunctionObserved = true
		return "node-content-pong"
	end
	return "rejected"
end)
RunService.PostSimulation:Connect(function()
	for Name in PendingAttributes do
		CharacterControl:SetAttribute(Name, true)
		PendingAttributes[Name] = nil
	end
	for _, Player in Players:GetPlayers() do
		local Character = Player.Character
		if Character and Character:FindFirstChild("PackageSessionRig") == nil then PrepareCharacter(Character) end
		if Character and math.abs(Character.Position.Z) > 0.25 then Character:SetAttribute("PackageServerMoved", true) end
		if Character and Character:GetAttribute("PackageServerMoved") == true then
			-- Keep 3E's observer near the explicit streamable unit after movement has
			-- already been proven; automatic residency is deliberately not involved.
			Character.Position = Vector3.new(0, 6, 0)
		end
	end
end)
'@
	$ClientSource = @'
local CharacterControl = game:GetService("CharacterControlService")
local Players = game:GetService("Players")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")
local Event = game:FindFirstChild("ContentSessionEvent", true)
local Function = game:FindFirstChild("ContentSessionFunction", true)

assert(Event and Function)
assert(CharacterControl:RegisterAction(
	"PackageLunge",
	"asset://d9d9e9649adbad59588d137c2a642e1d",
	0.5,
	Vector3.new(0.9, 0, 0),
	0,
	true
))
local Resolved = false
local Ended = false
local Requested = false
local FunctionComplete = false
local FunctionStarted = false
local Stage = 0
local CompletionTicks = 0
local ClientStartedAt = os.clock()
local LocalPlayerReadyUs = 0
local CharacterReadyUs = 0
local MovementObservedUs = 0
local RemoteFunctionCompleteUs = 0
local FirstVisibleUs = 0
local EvictedVisibleUs = 0
local ReloadVisibleUs = 0
local BeforeContentEventSent = false
local TimelinePublished = false
local function ElapsedUs()
	return (os.clock() - ClientStartedAt) * 1000000
end
CharacterControl.ActionResolved:Connect(function(_, ActionName, Accepted)
	if ActionName == "PackageLunge" and Accepted then Resolved = true end
end)
CharacterControl.ActionEnded:Connect(function(_, ActionName)
	if ActionName == "PackageLunge" then Ended = true end
end)
RunService.PostSimulation:Connect(function()
	local LocalPlayer = Players.LocalPlayer
	if LocalPlayer and LocalPlayerReadyUs == 0 then LocalPlayerReadyUs = ElapsedUs() end
	local Character = LocalPlayer and LocalPlayer.Character
	if Character == nil then return end
	if CharacterReadyUs == 0 then CharacterReadyUs = ElapsedUs() end
	if not BeforeContentEventSent then
		BeforeContentEventSent = true
		Event:FireServer("BeforeContentEventObserved")
	end
	if MovementObservedUs == 0 and Character:GetAttribute("PackageServerMoved") == true then
		MovementObservedUs = ElapsedUs()
	end
	if not Requested and Character:GetAttribute("PackageServerMoved") == true then
		Requested = CharacterControl:RequestAction("PackageLunge")
	end
	if not FunctionStarted then
		FunctionStarted = true
		task.spawn(function()
			local Samples = {}
			local Total = 0
			local Timeouts = 0
			local Errors = 0
			for _ = 1, 5 do
				local StartedAt = os.clock()
				local Invoked, Result = pcall(function()
					return Function:InvokeServerWithTimeout(5, "node-content-ping")
				end)
				if not Invoked then
					Errors += 1
					if type(Result) == "string" and string.find(Result, "timeout", 1, true) then Timeouts += 1 end
				elseif Result ~= "node-content-pong" then
					Errors += 1
				end
				local Elapsed = (os.clock() - StartedAt) * 1000000
				Total += Elapsed
				table.insert(Samples, Elapsed)
				task.wait()
			end
			table.sort(Samples)
			local function Percentile(Percent)
				return Samples[math.floor((#Samples - 1) * Percent / 100) + 1]
			end
			local MetricsText = string.format(
				"[Content:RemoteFunction] samples=%d mean_us=%.0f p50_us=%.0f p95_us=%.0f p99_us=%.0f max_us=%.0f timeouts=%d errors=%d",
				#Samples, Total / #Samples, Percentile(50), Percentile(95), Percentile(99), Samples[#Samples], Timeouts, Errors
			)
			print(MetricsText)
			CharacterControl:SetAttribute("RemoteFunctionMetrics", MetricsText)
			FunctionComplete = Errors == 0
			RemoteFunctionCompleteUs = ElapsedUs()
		end)
	end
	local Region = Workspace:FindFirstChild("CollectionCourse", false)
	if Stage == 0 and Region == nil then
		Stage = 1
	elseif Stage == 1 and Region then
		local Ground = Region:FindFirstChild("Ground", false)
		if Ground and Ground:IsA("Part") and Ground.Anchored and Ground.Size == Vector3.new(32, 1, 32) then
			FirstVisibleUs = ElapsedUs()
			Event:FireServer("ContentResidentObserved")
			Stage = 2
		end
	elseif Stage == 2 and Region == nil then
		EvictedVisibleUs = ElapsedUs()
		Event:FireServer("ContentEvictedObserved")
		Stage = 3
	elseif Stage == 3 and Region then
		local Ground = Region:FindFirstChild("Ground", false)
		if Ground and Ground:IsA("Part") and Ground.Anchored then
			ReloadVisibleUs = ElapsedUs()
			Event:FireServer("ContentReloadedObserved")
			Stage = 4
		end
	end
	local Step = 9
	if Character:GetAttribute("PackageServerMoved") == true then Step = 8 end
	if Requested then Step = 7 end
	if Resolved then Step = 6 end
	if Ended then Step = 5 end
	if Character:GetAttribute("PackageActionAuthorized") == true then Step = 4 end
	if FunctionComplete then Step = 3 end
	if Stage >= 3 then Step = 2 end
	if Stage == 4 and FunctionComplete and Requested and Resolved and Ended and
		Character:GetAttribute("PackageActionAuthorized") == true then Step = 1 end
	CharacterControl:SetAttribute("SessionSmokeStep", Step)
	if Step == 1 then
		if not TimelinePublished then
			TimelinePublished = true
			CharacterControl:SetAttribute("OfficialHostTimeline", string.format(
				"[Content:OfficialHost] ClientTimeline local_player_us=%.0f character_us=%.0f movement_us=%.0f remote_function_us=%.0f first_visible_us=%.0f evicted_us=%.0f reload_visible_us=%.0f",
				LocalPlayerReadyUs, CharacterReadyUs, MovementObservedUs, RemoteFunctionCompleteUs,
				FirstVisibleUs, EvictedVisibleUs, ReloadVisibleUs
			))
		end
		CompletionTicks += 1
		if CompletionTicks >= 20 then CharacterControl:SetAttribute("SessionSmokeComplete", true) end
	end
end)
'@
	$ClientSource = $ClientSource.Replace('for _ = 1, 5 do', "for _ = 1, $RemoteFunctionCallCount do")
	$ServerScript = Copy-ScriptTemplate -Template $ScriptTemplate -Name 'OfficialNodeHostServerProof' -RunContext 'Server' -Source $ServerSource
	$ClientScript = Copy-ScriptTemplate -Template $ScriptTemplate -Name 'OfficialNodeHostClientProof' -RunContext 'Client' -Source $ClientSource
	# Keep Remotes at the DataModel root, matching the retained healthy 3L.1
	# process fixture and ensuring publication precedes client script traffic.
	$ProjectDocument.Children = @($ProjectDocument.Children) + @($Event, $Function)
	$GameScripts.Children = @($GameScripts.Children) + @($ServerScript, $ClientScript)
	$ProjectJson = $ProjectDocument | ConvertTo-Json -Depth 100 -Compress
	[System.IO.File]::WriteAllText($ProjectDocumentPath, $ProjectJson, [System.Text.UTF8Encoding]::new($false))

	$SavedPath = $env:PATH
	try {
		# The packager is a build-time tool, not part of either shipped host. Give it
		# the already-staged Player native closure while creating the two packages;
		# host execution below still uses a deliberately scrubbed PATH.
		$env:PATH = "$PlayerRuntimeDistribution;$SavedPath"
		& $Packager build --project $FixtureRoot --output $PlayerPackageRoot --runtime $PlayerRuntimeDistribution --configuration Release
		if ($LASTEXITCODE -ne 0) { throw "Player packager exited with $LASTEXITCODE" }
		& $Packager build --project $FixtureRoot --output $ServerPackageRoot --runtime $ServerRuntimeDistribution --configuration Release
		if ($LASTEXITCODE -ne 0) { throw "Server packager exited with $LASTEXITCODE" }
	} finally {
		$env:PATH = $SavedPath
	}
	$Package = Get-Content -LiteralPath (Join-Path $ServerPackageRoot 'game.package.json') -Raw | ConvertFrom-Json
	$ContentManifest = Get-Content -LiteralPath (Join-Path $ServerPackageRoot 'content\content.manifest.json') -Raw | ConvertFrom-Json
	if (@($ContentManifest.Entries).Count -ne 1) {
		throw "Official Node host fixture produced $(@($ContentManifest.Entries).Count) streamable units instead of one"
	}
	$Descriptor = [ordered]@{
		ProjectId = $Package.ProjectId
		PackageVersion = [uint64]$Package.Revision
		ContentKey = $ContentManifest.Entries[0].Key
		ContentRootName = 'CollectionCourse'
		RemoteFunctionCallCount = $RemoteFunctionCallCount
		ContentObjectCount = $ContentManifest.Entries[0].ObjectCount
		ContentPayloadBytes = $ContentManifest.Entries[0].UncompressedBytes
		PlayerPackageRoot = $PlayerPackageRoot
		ServerPackageRoot = $ServerPackageRoot
	}
	[System.IO.File]::WriteAllText(
		$DescriptorFile,
		($Descriptor | ConvertTo-Json -Compress),
		[System.Text.UTF8Encoding]::new($false)
	)
	Write-Output '[Content:OfficialHost] PREPARE_OK'
	return
}

foreach ($Pair in @(
	@('DescriptorFile', $DescriptorFile),
	@('TestRoot', $TestRoot)
)) {
	Require-Value -Name $Pair[0] -Value $Pair[1]
}
$Descriptor = Get-Content -LiteralPath $DescriptorFile -Raw | ConvertFrom-Json
$Server = Join-Path $Descriptor.ServerPackageRoot 'GargantuanServer.exe'
$Player = Join-Path $Descriptor.PlayerPackageRoot 'GargantuanPlayer.exe'
$ContentKey = $Descriptor.ContentKey
$NodeSecretEnvironmentNames = @(
	$TokenEnvironmentName,
	$WrongTokenEnvironmentName,
	$LimitedTokenEnvironmentName,
	$WrongTenantTokenEnvironmentName,
	$MissingTokenEnvironmentName
)

if ($Mode -eq 'Churn') {
	Require-Value -Name 'MemoryOutputPrefix' -Value $MemoryOutputPrefix
	$GameEndpoint = "127.0.0.1:$(44000 + ($PID % 1000))"
	$ProviderArguments = @('--content-provider', 'local')
	if ($NodeEndpoint) {
		Require-Value -Name 'RootCertificateFile' -Value $RootCertificateFile
		$ProviderArguments = @('--content-provider', 'node', '--content-node-endpoint', $NodeEndpoint,
			'--content-node-root-ca', $RootCertificateFile, '--content-node-token-env', $TokenEnvironmentName)
	}
	$ServerProcess = $null
	$PlayerProcess = $null
	$Samples = [System.Collections.Generic.List[object]]::new()
	$Watch = [System.Diagnostics.Stopwatch]::StartNew()
	try {
		$RoleArguments = @('--bind', $GameEndpoint, '--session-smoke')
		if ($ServerOnlyChurn) { $RoleArguments = @('--startup-smoke') }
		$ServerProcess = Start-RuntimeProcess -Executable $Server -WorkingDirectory $Descriptor.ServerPackageRoot -Arguments (
			$RoleArguments + @('--max-ticks', '300000',
			'--content-residency', 'on-demand', '--content-lifecycle-smoke', $ContentKey,
			'--content-churn-cycles', [string]$ChurnCycles) + $ProviderArguments)
		# Drain both pipes from launch: sustained lifecycle diagnostics must never
		# turn a full redirected pipe into artificial Server backpressure.
		$ServerOutput = $ServerProcess.StandardOutput.ReadToEndAsync()
		$ServerError = $ServerProcess.StandardError.ReadToEndAsync()
		while (-not $ServerProcess.HasExited) {
			$ServerProcess.Refresh()
			$Samples.Add([pscustomobject]@{ ElapsedMs = $Watch.ElapsedMilliseconds;
				UnixMilliseconds = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds();
				WorkingSetBytes = $ServerProcess.WorkingSet64; PrivateBytes = $ServerProcess.PrivateMemorySize64;
				Threads = $ServerProcess.Threads.Count; Handles = $ServerProcess.HandleCount })
			if (-not $ServerOnlyChurn -and -not $PlayerProcess -and $Watch.ElapsedMilliseconds -ge 2300) {
				$PlayerProcess = Start-RuntimeProcess -Executable $Player -WorkingDirectory $Descriptor.PlayerPackageRoot -Arguments @(
					'--headless', '--connect', $GameEndpoint, '--session-smoke', '--max-frames', '1800'
				) -RemoveEnvironmentVariables $NodeSecretEnvironmentNames
				$PlayerOutput = $PlayerProcess.StandardOutput.ReadToEndAsync()
				$PlayerError = $PlayerProcess.StandardError.ReadToEndAsync()
			}
			if ($PlayerProcess -and $PlayerProcess.HasExited -and $PlayerProcess.ExitCode -ne 0) {
				throw "Churn Player failed: $($PlayerProcess.ExitCode)"
			}
			if ($Watch.Elapsed.TotalMinutes -gt 20) { throw 'Churn Server timed out' }
			if (-not $ServerOnlyChurn -and $Watch.Elapsed.TotalMinutes -gt 3 -and
				$PlayerProcess -and -not $PlayerProcess.HasExited) { throw 'Churn network proof timed out after three minutes' }
			Start-Sleep -Milliseconds 100
		}
		$ServerProcess.WaitForExit()
		$Output = $ServerOutput.GetAwaiter().GetResult() + $ServerError.GetAwaiter().GetResult()
		[System.IO.File]::WriteAllText("$MemoryOutputPrefix-server.log", $Output)
		if ($ServerProcess.ExitCode -ne 0) { throw "Churn Server failed: $($ServerProcess.ExitCode)`n$Output" }
		Require-Marker -Label 'Churn Server' -Output $Output -Marker "CONTENT_CHURN_OK cycles=$ChurnCycles"
		if ($ServerOnlyChurn) {
			Write-Output "[Content:Memory] SERVER_ONLY_CHURN_OK cycles=$ChurnCycles objects=$($Descriptor.ContentObjectCount) payload_bytes=$($Descriptor.ContentPayloadBytes) elapsed_ms=$($Watch.ElapsedMilliseconds) samples=$($Samples.Count)"
			return
		}
		if (-not $PlayerProcess -or -not $PlayerProcess.WaitForExit(30000)) { throw 'Churn Player did not stop' }
		if ($PlayerProcess.ExitCode -ne 0) { throw "Churn Player failed: $($PlayerProcess.ExitCode)" }
		$ClientOutput = $PlayerOutput.GetAwaiter().GetResult() + $PlayerError.GetAwaiter().GetResult()
		[System.IO.File]::WriteAllText("$MemoryOutputPrefix-player.log", $ClientOutput)
		Require-Marker -Label 'Churn Player' -Output $ClientOutput -Marker '[Content:OfficialHost] ClientTimeline'
		Write-Output "[Content:Memory] CHURN_OK cycles=$ChurnCycles objects=$($Descriptor.ContentObjectCount) payload_bytes=$($Descriptor.ContentPayloadBytes) elapsed_ms=$($Watch.ElapsedMilliseconds) samples=$($Samples.Count)"
		Write-Output $ClientOutput
	} finally {
		$Samples | Export-Csv -LiteralPath "$MemoryOutputPrefix-memory.csv" -NoTypeInformation
		if ($PlayerProcess -and -not $PlayerProcess.HasExited) { Stop-Process -Id $PlayerProcess.Id -Force }
		if ($ServerProcess -and -not $ServerProcess.HasExited) { Stop-Process -Id $ServerProcess.Id -Force }
		if ($ServerProcess) {
			$ServerProcess.WaitForExit()
			[System.IO.File]::WriteAllText("$MemoryOutputPrefix-server.log", $ServerOutput.GetAwaiter().GetResult() + $ServerError.GetAwaiter().GetResult())
		}
		if ($PlayerProcess) {
			$PlayerProcess.WaitForExit()
			[System.IO.File]::WriteAllText("$MemoryOutputPrefix-player.log", $PlayerOutput.GetAwaiter().GetResult() + $PlayerError.GetAwaiter().GetResult())
		}
		if ($ServerProcess) { $ServerProcess.Dispose() }
		if ($PlayerProcess) { $PlayerProcess.Dispose() }
	}
	return
}

if ($Mode -eq 'Node' -or $Mode -eq 'NodeCycle') {
	foreach ($Pair in @(
		@('NodeEndpoint', $NodeEndpoint),
		@('RootCertificateFile', $RootCertificateFile),
		@('TokenEnvironmentName', $TokenEnvironmentName)
	)) {
		Require-Value -Name $Pair[0] -Value $Pair[1]
	}
	$Port = 41000 + ($PID % 1000)
	if ($LifecycleGamePort -ne 0) { $Port = $LifecycleGamePort }
	$GameEndpoint = "127.0.0.1:$Port"
	$NodeArguments = @(
		'--content-provider', 'node',
		'--content-residency', 'on-demand',
		'--content-node-endpoint', $NodeEndpoint,
		'--content-node-root-ca', $RootCertificateFile,
		'--content-node-token-env', $TokenEnvironmentName
	)
	$ServerProcess = $null
	$PlayerProcess = $null
	$NodeSemanticDigest = $null
	try {
		$UseExtendedBudget = $Mode -eq 'Node' -or [int]$Descriptor.RemoteFunctionCallCount -gt 5
		$ServerMaximumTicks = if ($UseExtendedBudget) { '1200' } else { '720' }
		$PlayerMaximumFrames = if ($UseExtendedBudget) { '900' } else { '600' }
		$ServerArguments = @(
			'--bind', $GameEndpoint, '--session-smoke', '--max-ticks', $ServerMaximumTicks,
			'--content-lifecycle-smoke', $ContentKey
		) + $NodeArguments
		$ServerProcess = Start-RuntimeProcess -Executable $Server -WorkingDirectory $Descriptor.ServerPackageRoot -Arguments $ServerArguments
		Start-Sleep -Seconds 2
		Start-Sleep -Milliseconds 300
		if ($ServerProcess.HasExited) {
			$Failed = Complete-RuntimeProcess -Process $ServerProcess -Label 'Node OnDemand Server'
			throw "Node OnDemand Server stopped before Player startup ($($Failed.ExitCode))`n$($Failed.Output)"
		}
		$PlayerProcess = Start-RuntimeProcess -Executable $Player -WorkingDirectory $Descriptor.PlayerPackageRoot -Arguments @(
			'--headless', '--connect', $GameEndpoint, '--session-smoke', '--max-frames', $PlayerMaximumFrames
		) -RemoveEnvironmentVariables $NodeSecretEnvironmentNames
		$PlayerResult = Complete-RuntimeProcess -Process $PlayerProcess -Label 'Official GargantuanPlayer' -TimeoutMilliseconds 30000
		if ($PlayerResult.ExitCode -ne 0) {
			if (-not $ServerProcess.HasExited) {
				Stop-Process -Id $ServerProcess.Id -Force -ErrorAction SilentlyContinue
			}
			$ServerFailure = Complete-RuntimeProcess -Process $ServerProcess -Label 'Official GargantuanServer failure readback'
			throw "Official GargantuanPlayer returned $($PlayerResult.ExitCode)`n$($PlayerResult.Output)`nSERVER:`n$($ServerFailure.Output)"
		}
		$ServerResult = Complete-RuntimeProcess -Process $ServerProcess -Label 'Official GargantuanServer' -TimeoutMilliseconds 30000
		if ($ServerResult.ExitCode -ne 0) {
			throw "Official GargantuanServer returned $($ServerResult.ExitCode)`n$($ServerResult.Output)"
		}
		Require-Marker -Label 'Official GargantuanServer' -Output $ServerResult.Output -Marker 'CONTENT_LIFECYCLE_FIRST_RESIDENT'
		Require-Marker -Label 'Official GargantuanServer' -Output $ServerResult.Output -Marker 'CONTENT_LIFECYCLE_EVICTED'
		Require-Marker -Label 'Official GargantuanServer' -Output $ServerResult.Output -Marker 'CONTENT_LIFECYCLE_RELOAD_OK'
		Require-Marker -Label 'Official GargantuanPlayer' -Output $PlayerResult.Output -Marker "[Content:RemoteFunction] samples=$($Descriptor.RemoteFunctionCallCount)"
		Require-Marker -Label 'Official GargantuanPlayer' -Output $PlayerResult.Output -Marker '[Content:OfficialHost] ClientTimeline'
		$NodeDigestMatch = [regex]::Match($ServerResult.Output, 'CONTENT_LIFECYCLE_RELOAD_OK[^\r\n]*SemanticDigest=([0-9a-f]{64})')
		if (-not $NodeDigestMatch.Success) { throw 'Node OnDemand semantic digest was absent' }
		$NodeSemanticDigest = $NodeDigestMatch.Groups[1].Value
		Write-Output $ServerResult.Output
		Write-Output $PlayerResult.Output
	} finally {
		if ($PlayerProcess -and -not $PlayerProcess.HasExited) { Stop-Process -Id $PlayerProcess.Id -Force }
		if ($ServerProcess -and -not $ServerProcess.HasExited) { Stop-Process -Id $ServerProcess.Id -Force }
	}
	if ($Mode -eq 'NodeCycle') {
		Write-Output '[Content:OfficialHost] NODE_CYCLE_OK'
		return
	}

	$NodeFullyOutput = Invoke-Runtime -Executable $Server -WorkingDirectory $Descriptor.ServerPackageRoot -Label 'Node FullyResident Server' -Arguments @(
		'--startup-smoke', '--max-ticks', '400', '--content-lifecycle-smoke', $ContentKey,
		'--content-provider', 'node', '--content-residency', 'fully-resident',
		'--content-node-endpoint', $NodeEndpoint, '--content-node-root-ca', $RootCertificateFile,
		'--content-node-token-env', $TokenEnvironmentName
	)
	Require-Marker -Label 'Node FullyResident Server' -Output $NodeFullyOutput -Marker 'CONTENT_FULLY_RESIDENT_OK'
	$NodeFullyDigestMatch = [regex]::Match($NodeFullyOutput, 'CONTENT_FULLY_RESIDENT_OK[^\r\n]*SemanticDigest=([0-9a-f]{64})')
	if (-not $NodeFullyDigestMatch.Success -or $NodeFullyDigestMatch.Groups[1].Value -ne $NodeSemanticDigest) {
		throw 'Node provider residency policies produced different semantic worlds'
	}

	$InFlightOutput = Invoke-Runtime -Executable $Server -WorkingDirectory $Descriptor.ServerPackageRoot -Label 'Node in-flight shutdown' -Arguments @(
		'--content-stop-in-flight-smoke', $ContentKey,
		'--content-provider', 'node', '--content-residency', 'on-demand',
		'--content-node-endpoint', $NodeEndpoint, '--content-node-root-ca', $RootCertificateFile,
		'--content-node-token-env', $TokenEnvironmentName
	)
	Require-Marker -Label 'Node in-flight shutdown' -Output $InFlightOutput -Marker 'CONTENT_REQUEST_IN_FLIGHT'
	Require-Marker -Label 'Node in-flight shutdown' -Output $InFlightOutput -Marker 'admissions=0'
	Require-Marker -Label 'Node in-flight shutdown' -Output $InFlightOutput -Marker 'stale=0'
	$FreshOutput = Invoke-Runtime -Executable $Server -WorkingDirectory $Descriptor.ServerPackageRoot -Label 'fresh Node Server after in-flight shutdown' -Arguments @(
		'--startup-smoke', '--max-ticks', '500', '--content-lifecycle-smoke', $ContentKey,
		'--content-provider', 'node', '--content-residency', 'on-demand',
		'--content-node-endpoint', $NodeEndpoint, '--content-node-root-ca', $RootCertificateFile,
		'--content-node-token-env', $TokenEnvironmentName
	)
	Require-Marker -Label 'fresh Node Server after in-flight shutdown' -Output $FreshOutput -Marker 'CONTENT_LIFECYCLE_RELOAD_OK'
	Require-Marker -Label 'fresh Node Server after in-flight shutdown' -Output $FreshOutput -Marker 'stale=0'

	foreach ($Failure in @(
		@('Missing token environment value', $MissingTokenEnvironmentName),
		@('Wrong token', $WrongTokenEnvironmentName),
		@('Missing capability', $LimitedTokenEnvironmentName),
		@('Wrong tenant', $WrongTenantTokenEnvironmentName)
	)) {
		$FailureOutput = Invoke-Runtime -Executable $Server -WorkingDirectory $Descriptor.ServerPackageRoot -Label $Failure[0] -ExpectedExitCode 7 -Arguments @(
			'--startup-smoke', '--content-provider', 'node', '--content-residency', 'on-demand',
			'--content-node-endpoint', $NodeEndpoint, '--content-node-root-ca', $RootCertificateFile,
			'--content-node-token-env', $Failure[1]
		)
		if ($FailureOutput.Contains('engine-adapter-')) { throw "$($Failure[0]) output exposed a workload token" }
	}
	Write-Output $NodeFullyOutput
	Write-Output $InFlightOutput
	Write-Output $FreshOutput
	Write-Output "[Content:OfficialHost] NODE_SEMANTIC_DIGEST=$NodeSemanticDigest"
	Write-Output '[Content:OfficialHost] NODE_OK'
	return
}

if ($Mode -eq 'NodeCorrupt') {
	$Output = Invoke-Runtime -Executable $Server -WorkingDirectory $Descriptor.ServerPackageRoot -Label 'corrupt Node content' -ExpectedExitCode 9 -Arguments @(
		'--startup-smoke', '--max-ticks', '500', '--content-lifecycle-smoke', $ContentKey,
		'--content-provider', 'node', '--content-residency', 'on-demand',
		'--content-node-endpoint', $NodeEndpoint, '--content-node-root-ca', $RootCertificateFile,
		'--content-node-token-env', $TokenEnvironmentName
	)
	Require-Marker -Label 'corrupt Node content' -Output $Output -Marker 'admissions=0'
	Write-Output $Output
	Write-Output '[Content:OfficialHost] NODE_CORRUPT_REJECTED'
	return
}

if ($Mode -eq 'NodeBootstrapFailure') {
	$Output = Invoke-Runtime -Executable $Server -WorkingDirectory $Descriptor.ServerPackageRoot -Label 'Node package mismatch' -ExpectedExitCode 7 -Arguments @(
		'--startup-smoke', '--content-provider', 'node', '--content-residency', 'on-demand',
		'--content-node-endpoint', $NodeEndpoint, '--content-node-root-ca', $RootCertificateFile,
		'--content-node-token-env', $TokenEnvironmentName
	)
	Write-Output $Output
	Write-Output '[Content:OfficialHost] NODE_BOOTSTRAP_REJECTED'
	return
}

if ($Mode -eq 'NodeResidentSurvival') {
	foreach ($Pair in @(
		@('NodeEndpoint', $NodeEndpoint),
		@('RootCertificateFile', $RootCertificateFile),
		@('TokenEnvironmentName', $TokenEnvironmentName)
	)) {
		Require-Value -Name $Pair[0] -Value $Pair[1]
	}
	$Output = Invoke-Runtime -Executable $Server -WorkingDirectory $Descriptor.ServerPackageRoot -Label 'resident Node outage survival' -TimeoutMilliseconds 15000 -Arguments @(
		'--startup-smoke', '--max-ticks', '800',
		'--content-lifecycle-smoke', $ContentKey,
		'--content-provider', 'node', '--content-residency', 'fully-resident',
		'--content-node-endpoint', $NodeEndpoint, '--content-node-root-ca', $RootCertificateFile,
		'--content-node-token-env', $TokenEnvironmentName
	)
	Require-Marker -Label 'resident Node outage survival' -Output $Output -Marker 'CONTENT_FULLY_RESIDENT_OK'
	Require-Marker -Label 'resident Node outage survival' -Output $Output -Marker 'admissions=1'
	Require-Marker -Label 'resident Node outage survival' -Output $Output -Marker 'evictions=0'
	Write-Output $Output
	Write-Output '[Content:OfficialHost] NODE_RESIDENT_SURVIVED_OUTAGE'
	return
}

$LocalDigests = @()
foreach ($Residency in @('fully-resident', 'on-demand')) {
	$Output = Invoke-Runtime -Executable $Server -WorkingDirectory $Descriptor.ServerPackageRoot -Label "Local $Residency Server" -Arguments @(
		'--startup-smoke', '--max-ticks', '500', '--content-provider', 'local',
		'--content-residency', $Residency, '--content-lifecycle-smoke', $ContentKey
	)
	$ExpectedMarker = if ($Residency -eq 'fully-resident') { 'CONTENT_FULLY_RESIDENT_OK' } else { 'CONTENT_LIFECYCLE_RELOAD_OK' }
	Require-Marker -Label "Local $Residency Server" -Output $Output -Marker $ExpectedMarker
	$DigestMatch = [regex]::Match($Output, $ExpectedMarker + '[^\r\n]*SemanticDigest=([0-9a-f]{64})')
	if (-not $DigestMatch.Success) { throw "Local $Residency semantic digest was absent" }
	$LocalDigests += $DigestMatch.Groups[1].Value
	Write-Output $Output
}
if ($LocalDigests[0] -ne $LocalDigests[1]) { throw 'Local provider residency policies produced different semantic worlds' }
Write-Output "[Content:OfficialHost] LOCAL_SEMANTIC_DIGEST=$($LocalDigests[0])"

$Port = 43000 + ($PID % 1000)
$Endpoint = "127.0.0.1:$Port"
foreach ($Iteration in 1..2) {
	$RestartOutput = Invoke-Runtime -Executable $Server -WorkingDirectory $Descriptor.ServerPackageRoot -Label "same-port restart $Iteration" -Arguments @(
		'--bind', $Endpoint, '--startup-smoke', '--max-ticks', '12'
	)
}
$OfflineOutput = Invoke-Runtime -Executable $Player -WorkingDirectory $Descriptor.PlayerPackageRoot -Label 'relocated offline Player' -Arguments @(
	'--headless', '--startup-smoke', '--max-frames', '12'
) -RemoveEnvironmentVariables $NodeSecretEnvironmentNames
Write-Output '[Content:OfficialHost] LOCAL_OK'
