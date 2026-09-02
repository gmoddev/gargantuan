param(
	[Parameter(Mandatory = $true)][string]$Packager,
	[Parameter(Mandatory = $true)][string]$RuntimeDistribution,
	[Parameter(Mandatory = $true)][string]$ProjectRoot,
	[string]$GraphicalClient = 'OFF'
)

$ErrorActionPreference = 'Stop'
$TestRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("gps-{0}-{1}" -f $PID, [Guid]::NewGuid().ToString('N').Substring(0, 8))
$PackageRoot = Join-Path $TestRoot 'Package'
$FixtureRoot = Join-Path $TestRoot 'FirstCompleteGame'
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
	Copy-Item -LiteralPath $ProjectRoot -Destination $FixtureRoot -Recurse -Force
	$ProjectDocumentPath = Join-Path $FixtureRoot '.gargantuan\project.instance.json'
	$ProjectDocument = Get-Content -LiteralPath $ProjectDocumentPath -Raw | ConvertFrom-Json
	$GameScripts = $ProjectDocument.Children | Where-Object { $_.Name -eq 'GameScripts' -and $_.ClassName -eq 'Folder' } | Select-Object -First 1
	$ScriptTemplate = $GameScripts.Children | Where-Object { $_.Name -eq 'RoundManager' -and $_.ClassName -eq 'Script' } | Select-Object -First 1
	if (-not $GameScripts -or -not $ScriptTemplate) {
		throw 'FirstCompleteGame script fixture is unavailable'
	}
	$ServerScript = $ScriptTemplate | ConvertTo-Json -Depth 100 -Compress | ConvertFrom-Json
	$ServerScript.Name = 'NetworkSessionServerProof'
	$ServerScript.Properties.RunContext.EnumItem[1] = 'Server'
	$ServerScript.Properties.Source.String = @'
local CharacterControl = game:GetService("CharacterControlService")
local Players = game:GetService("Players")
local RunService = game:GetService("RunService")

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
	if Accepted then
		Character:SetAttribute("PackageActionAuthorized", true)
	end
	return Accepted
end)

local function PrepareCharacter(Character)
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
	if Player.Character then
		PrepareCharacter(Player.Character)
	end
end
Players.PlayerAdded:Connect(PreparePlayer)
Players.PlayerRemoving:Connect(function()
	print("[Package:SessionProof] server-player-removed")
end)
for _, Player in Players:GetPlayers() do
	PreparePlayer(Player)
end
RunService.PostSimulation:Connect(function()
	for _, Player in Players:GetPlayers() do
		local Character = Player.Character
		if Character and Character:FindFirstChild("PackageSessionRig") == nil then
			PrepareCharacter(Character)
		end
		if Character and math.abs(Character.Position.Z) > 0.25 then
			Character:SetAttribute("PackageServerMoved", true)
		end
	end
end)
'@
	$ClientScript = $ScriptTemplate | ConvertTo-Json -Depth 100 -Compress | ConvertFrom-Json
	$ClientScript.Name = 'NetworkSessionClientProof'
	$ClientScript.Properties.RunContext.EnumItem[1] = 'Client'
	$ClientScript.Properties.Source.String = @'
local CharacterControl = game:GetService("CharacterControlService")
local Players = game:GetService("Players")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

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
local Reported = false
local InitialPosition = nil
CharacterControl.ActionResolved:Connect(function(_, ActionName, Accepted)
	if ActionName == "PackageLunge" and Accepted then
		Resolved = true
	end
end)
CharacterControl.ActionEnded:Connect(function(_, ActionName)
	if ActionName == "PackageLunge" then
		Ended = true
	end
end)
RunService.PostSimulation:Connect(function()
	local LocalPlayer = Players.LocalPlayer
	local Character = LocalPlayer and LocalPlayer.Character
	if Character == nil then
		return
	end
	InitialPosition = InitialPosition or Character.Position
	if not Requested and Character:GetAttribute("PackageServerMoved") == true then
		Requested = CharacterControl:RequestAction("PackageLunge")
	end
	local RemoteCharacters = 0
	for _, Object in Workspace:GetDescendants() do
		if Object ~= Character and Object:IsA("KinematicCharacter") then
			RemoteCharacters += 1
		end
	end
	local Step = 9
	if Character:GetAttribute("PackageServerMoved") == true then
		Step = 11
	end
	if Requested then
		Step = 12
	end
	if Resolved then
		Step = 1
		if Ended then
			Step = 2
			if Character:GetAttribute("PackageActionAuthorized") == true then
				Step = 3
				if Character:GetAttribute("PackageServerMoved") == true then
					Step = 4
					if Character.Position.X > InitialPosition.X + 0.4 then
						Step = 5
						Step = 6
						if RemoteCharacters >= 2 then
							Step = 7
						end
					end
				end
			end
		end
	end
	CharacterControl:SetAttribute("SessionSmokeStep", Step)
	if not Reported
		and Step == 7
	then
		Reported = true
		CharacterControl:SetAttribute("SessionSmokeComplete", true)
	end
end)
'@
	$GameScripts.Children = @($GameScripts.Children) + @($ServerScript, $ClientScript)
	$ProjectJson = $ProjectDocument | ConvertTo-Json -Depth 100 -Compress
	[System.IO.File]::WriteAllText($ProjectDocumentPath, $ProjectJson, [System.Text.UTF8Encoding]::new($false))

	& $Packager build --project $FixtureRoot --output $PackageRoot --runtime $RuntimeDistribution --configuration Release
	if ($LASTEXITCODE -ne 0) {
		throw "Packager exited with $LASTEXITCODE"
	}

	$Player = Join-Path $PackageRoot 'GargantuanPlayer.exe'
	$Port = 40000 + ($PID % 1000)
	$Endpoint = "127.0.0.1:$Port"
	$ServerProcess = Start-PlayerProcess -Arguments @('--headless', '--server-bind', $Endpoint, '--session-smoke', '--max-frames', '360')
	Start-Sleep -Milliseconds 300
	$ClientArguments = @('--connect', $Endpoint, '--session-smoke', '--max-frames', '240')
	if ($GraphicalClient -ne 'ON') {
		$ClientArguments = @('--headless') + $ClientArguments
	}
	$ClientProcess = Start-PlayerProcess -Arguments $ClientArguments
	if (-not $ClientProcess.WaitForExit(20000)) {
		Stop-Process -Id $ClientProcess.Id -Force
		throw 'Packaged game-session client timed out'
	}
	$ClientOutput = $ClientProcess.StandardOutput.ReadToEnd() + $ClientProcess.StandardError.ReadToEnd()
	if ($ClientProcess.ExitCode -ne 0) {
		$ClientDiagnostic = if ($ClientProcess.HasExited) { $ClientProcess.StandardError.ReadToEnd() } else { '' }
		$ServerDiagnostic = if ($ServerProcess.HasExited) { $ServerProcess.StandardError.ReadToEnd() } else { '' }
		throw "Packaged client session proof failed: client=$($ClientProcess.ExitCode) output=$ClientOutput clientError=$ClientDiagnostic serverError=$ServerDiagnostic"
	}
	if (-not $ServerProcess.WaitForExit(20000)) {
		Stop-Process -Id $ServerProcess.Id -Force
		throw 'Packaged game-session server timed out'
	}
	$ServerOutput = $ServerProcess.StandardOutput.ReadToEnd() + $ServerProcess.StandardError.ReadToEnd()
	if ($ServerProcess.ExitCode -ne 0) {
		throw "Packaged server session proof failed: server=$($ServerProcess.ExitCode) output=$ServerOutput"
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
