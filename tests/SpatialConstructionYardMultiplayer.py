# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Exercise the authored Yard actions with relocated Players and a restarted SQLite-backed Server."""
import argparse
from contextlib import ExitStack, contextmanager
import hashlib
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time

from SpatialConstructionPackageSession import Build, ConsoleCapture, ManagedEnvironment
from SpatialConstructionYardInput import PlatformWitness, PrepareProject

Samples = Path(__file__).resolve().parents[1] / "samples"
sys.path.insert(0, str(Samples / "TerrainQuarry"))
from BuildProject import EditorHost


TerrainProof = r'''
local Lo, Hi = Vector3.new(-8,-6,-8), Vector3.new(9,6,9)
local function Signature(Bytes)
	local Sum=0
	for I=1,#Bytes do Sum=(Sum*131+string.byte(Bytes,I))%4294967291 end
	return Sum
end
local function Surface()
	local Space=game.Workspace
	local Params=RaycastParams.new()
	Params.IncludeTerrain=true
	Params.FilterType=Enum.RaycastFilterType.Include
	Params.FilterDescendantsInstances={Space.Terrain}
	local Rows={}
	local HasHit=false
	for Z=-6,6,2 do
		for X=-6,6,2 do
			local Hit=Space:Raycast(Vector3.new(X,10,Z),Vector3.new(0,-20,0),Params)
			table.insert(Rows,if Hit then tostring(math.round(Hit.Position.Y*32)) else ".")
			HasHit=HasHit or Hit~=nil
		end
	end
	return if HasHit then Signature(table.concat(Rows,",")) else nil
end
'''

FreightVisit = (Samples.parent / "tests/blockgrid/gameplay/YardFreightVisit.luau").read_text(encoding="utf-8")
ServerProbe = TerrainProof + r'''
local Probe = game.ReplicatedStorage.YardProcessProbe
local Players = game:GetService("Players")
local Space = game.Workspace
local Cell = Vector3.new(-2,2,-5)
local Roles = {}
local First, Second, DigConfirmed, HasReturn
local Restored
''' + FreightVisit + r'''
task.spawn(function()
	local Store=game:GetService("DataStoreService"):GetDocumentStore("ConstructionYard")
	local LastStatus
	for Attempt=1,3 do
		local Read=Store:GetAsync("YardV1",2000)
		for _=1,400 do
			local Result=Read:Poll()
			LastStatus=Result.Status
			if LastStatus=="Missing" then Restored=false return end
			if LastStatus=="Ok" then
				local Events=Result.Record.Value.Events
				assert(#Events==3 and Events[1].Kind=="wall" and Events[2].Kind=="dig" and Events[3].Kind=="floor")
				Restored=true
				return
			end
			if LastStatus=="Busy" or LastStatus=="Deadline" then break end
			assert(LastStatus=="Pending",LastStatus)
			task.wait(.04)
		end
		if LastStatus=="Pending" then Read:Cancel() end
		if Attempt<3 then task.wait(.1*Attempt) end
	end
	error("Yard recovery observer could not read its baseline: "..tostring(LastStatus))
end)
Probe.OnPlayerEvent:Connect(function(Player, Kind)
	if Kind == "join" then
		if Restored==nil then return end
		if Restored and (not Space.RetainingWall:GetBlock(Cell) or not Space.Workshop.WorkshopBlocks:GetBlock(Cell)) then return end
		if not Roles[Player] then
			if Restored then Roles[Player] = "restart"
			elseif not First then First = Player; Roles[Player] = "builder"
			elseif not Second then Second = Player; Roles[Player] = "digger"
			else Roles[Player] = "rejoin" end
		end
		local Expected
		if Restored then
			Expected=Surface()
			if not Expected then return end
			warn("[Spatial:YardField] "..Signature(buffer.tostring(Space.Terrain:ReadRegion(Lo,Hi))))
		end
		Probe:TryFireClient(Player, Roles[Player], Expected)
	elseif Kind == "dig-visible" and Player == Second then
		warn("[Spatial:YardField] "..Signature(buffer.tostring(Space.Terrain:ReadRegion(Lo,Hi))))
		DigConfirmed = true
	elseif Kind == "return-visible" and Player == First then
		assert(Space.RetainingWall:GetBlock(Cell).Definition == "Brick")
		assert(Space.Workshop.WorkshopBlocks:GetBlock(Cell).Definition == "Brick")
		HasReturn = true
		warn("[Spatial:YardProcess] Builder returned with two persisted constructions")
	elseif Kind == "poll" then
		if Player == First and DigConfirmed then Probe:TryFireClient(Player, "dig-visible") end
		if Roles[Player] == "rejoin" and HasReturn then Probe:TryFireClient(Player, "rejoined") end
	end
end)
'''

ClientProbe = TerrainProof + PlatformWitness + r'''
local Space = game.Workspace
local Player = game:GetService("Players").LocalPlayer
local Probe = game.ReplicatedStorage.YardProcessProbe
local Actions = game.ReplicatedStorage.YardRequest
local Cell = Vector3.new(-2,2,-5)
local Role, DigVisible, Rejoined, RestoredSurface
local TourStage
local AppliedDoors=0
Actions.OnClientEvent:Connect(function(Value)
	if Value=="DoorApplied" then AppliedDoors+=1 end
end)
Probe.OnClientEvent:Connect(function(Value, Expected)
	if Value=="dig-visible" then DigVisible=true
	elseif Value=="rejoined" then Rejoined=true
	elseif Value=="near-freight" or Value=="original-yard" then TourStage=Value
	else Role=Value; RestoredSurface=Expected end
end)
local function Wait(Label, Predicate)
	local LastError
	for _=1,800 do
		local Ok, Value=pcall(Predicate)
		if Ok and Value then return Value end
		if not Ok then LastError=tostring(Value) end
		task.wait(.04)
	end
	error(string.format("[Spatial:YardProcess] Timeout at %s; role=%s appliedDoors=%d lastQuery=%s",Label,tostring(Role),AppliedDoors,tostring(LastError)))
end
local function Send(Remote,Value)
	Wait("send "..Value,function() return Remote:TryFireServer(Value) end)
end
local function Tile(Grid)
	local Block=Grid and Grid:GetBlock(Cell)
	return Block and Block.Definition=="Brick"
end

local function Outside()
	return Player and Player.Character and Player.Character.RootPart and
		Player.Character.RootPart.SpatialRegion==nil and Space:FindFirstChild("RetainingWall")
end
task.spawn(function()
	Wait("join role",function() if not Role then Probe:TryFireServer("join") end return Role end)
	Wait("outdoor character and construction",Outside)
	Wait("far freight read correctly denied", function()
		local Grid=Space:FindFirstChild("FreightPlatform")
		if not Grid or Grid.MotionMode~="Dynamic" then return false end
		local Ok,Problem=pcall(function() return Grid:GetBlock(Vector3.new(0,1,0)) end)
		if Ok or not string.find(tostring(Problem),"outside installed replica coverage",1,true) then return false end
		warn(string.format("[Spatial:FreightVisit] far actor=(%.2f,%.2f,%.2f) deckX=%.2f",
			Player.Character.Position.X,Player.Character.Position.Y,Player.Character.Position.Z,Grid:GetAssemblyState().CFrame.Position.X))
		return true
	end)
	local PreviousPossession=Player.PossessedEntity
	local CartName="FreightInspector"..tostring(Player.PlayerId)
	Wait("possess the authorized freight inspection cart",function()
		if TourStage~="near-freight" then Probe:TryFireServer("freight-visit") end
		local Cart=Player.PossessedEntity
		return TourStage=="near-freight" and Cart and Cart.Name==CartName and Cart.Parent==Space and
			(Cart.Position-Vector3.new(22,3,1)).Magnitude<.01
	end)
	Wait("replicated freight platform motion, mass and cell hit",function() return ObserveMovingPlatform(Space) end)
	local Focus=Player.PossessedEntity
	warn(string.format("[Spatial:FreightVisit] near actor=(%.2f,%.2f,%.2f)",
		Focus.Position.X,Focus.Position.Y,Focus.Position.Z))
	Wait("return to Yard controls",function()
		if TourStage~="original-yard" then Probe:TryFireServer("freight-return") end
		return TourStage=="original-yard" and Outside() and Player.PossessedEntity==PreviousPossession and
			not Space:FindFirstChild(CartName,false)
	end)
	warn("[Spatial:YardProcess] Verified 512-cell moving platform for "..Role)
	if Role=="builder" then
		Send(Actions,"YardBuild")
		Wait("outdoor brick",function() return Tile(Space.RetainingWall) end)
		warn("[Spatial:YardProcess] Builder placed exterior brick")
		Wait("second Player excavation confirmation",function() if not DigVisible then Probe:TryFireServer("poll") end return DigVisible end)
		Send(Actions,"YardDoor")
		Wait("confirmed workshop arrival",function() return AppliedDoors==1 and Player.Character.RootPart.SpatialRegion~=nil and Space:FindFirstChild("Workshop") end)
		Send(Actions,"YardBuild")
		Wait("interior brick",function() return Tile(Space.Workshop.WorkshopBlocks) end)
		Send(Actions,"YardDoor")
		Wait("confirmed return",function() return AppliedDoors==2 and Outside() and Tile(Space.RetainingWall) end)
		Send(Probe,"return-visible")
	elseif Role=="digger" then
		Wait("outdoor brick",function() return Tile(Space.RetainingWall) end)
		assert(not Space:FindFirstChild("Workshop"))
		local Before=Wait("initial terrain surface",Surface)
		Send(Actions,"YardDig")
		local After=Wait("changed terrain surface",function()
			local Value=Surface()
			return Value~=Before and Value
		end)
		warn("[Spatial:YardSurface] "..After)
		Send(Probe,"dig-visible")
		warn("[Spatial:YardProcess] Second Player observed the brick and excavated replicated terrain")
	elseif Role=="rejoin" then
		Wait("rejoined edited construction",function() Probe:TryFireServer("poll") return Rejoined and Tile(Space.RetainingWall) end)
		warn("[Spatial:YardProcess] Reconnected Player materialized the edited yard")
	elseif Role=="restart" then
		Wait("outdoor brick",function() return Tile(Space.RetainingWall) end)
		Wait("replayed terrain collision",function() return RestoredSurface and Surface()==RestoredSurface end)
		warn("[Spatial:YardSurface] "..RestoredSurface)
		Send(Actions,"YardDoor")
		Wait("confirmed restored workshop",function() return AppliedDoors==1 and Player.Character.RootPart.SpatialRegion~=nil and Tile(Space.Workshop.WorkshopBlocks) end)
		warn("[Spatial:YardProcess] Fresh Server replayed both constructions")
	end
end)
'''


def AddObserver(Engine, Project):
    with EditorHost(Engine) as Host:
        Host.Call("Handshake")
        Host.Call("OpenProject", Root=Project.as_posix())
        Snapshot = Host.Call("GetSnapshot")["Snapshot"]
        Schema = {Row["Name"]: Row for Row in Host.Call("GetSchema")["Definitions"]
                  if Row["Kind"] == "Class" and Row["Namespace"] == "Engine"}
        Storage = next(Row["Id"] for Row in Snapshot["Objects"] if Row["ClassName"] == "ReplicatedStorage")
        Root = next(Row["Id"] for Row in Snapshot["Objects"] if Row["ClassName"] == "DataModel")
        Event = Schema["RemoteEvent"]
        Host.Call("CreateInstance", ClassSchemaId=Event["SchemaId"], DefinitionVersion=Event["DefinitionVersion"],
                  Parent=Storage, Name="YardProcessProbe")
        Script = Schema["Script"]
        for Context, Source in (("Server", ServerProbe), ("Client", ClientProbe)):
            Host.Call("CreateInstance", ClassSchemaId=Script["SchemaId"], DefinitionVersion=Script["DefinitionVersion"],
                      Parent=Root, Name="YardProcess" + Context, InitialSource=Source,
                      InitialProperties=[dict(DeclaringClassSchemaId=Script["SchemaId"],
                                              DeclaringDefinitionVersion=Script["DefinitionVersion"], Property="RunContext",
                                              Value=dict(Type="EnumItem", Enum="RunContext", Value=Context))])
        Host.Call("SaveProject")


@contextmanager
def Launch(Args, Root, Env, Name):
    Log = Root / (Name + ".log")
    with Log.open("wb") as Output:
        Process = subprocess.Popen([str(Arg) for Arg in Args], cwd=Root, env=Env, stdout=Output,
                                   stderr=subprocess.STDOUT, **ConsoleCapture())
        try:
            yield Process, Log
        finally:
            if Process.poll() is None:
                Process.terminate()
                try:
                    Process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    Process.kill()
                    Process.wait(timeout=5)


def Await(Owner, Marker, Others=()):
    End = time.monotonic() + 45
    while True:
        Text = Owner[1].read_text(encoding="utf-8", errors="replace")
        if Marker in Text:
            return
        if time.monotonic() >= End or any(Process.poll() is not None for Process, _ in (Owner, *Others)):
            Logs = "\n".join(f"{File.name} (exit={Process.poll()})\n" +
                             File.read_text(encoding="utf-8", errors="replace")[-2600:]
                             for Process, File in (Owner, *Others))
            raise RuntimeError(f"Missing {Marker}\n{Logs}")
        time.sleep(.05)


def ReportPlatform(Role, Owner):
    Log=Owner[1].read_text(encoding="utf-8",errors="replace")
    Rows=re.findall(r"\[Spatial:FreightPlatform\][^\r\n]*",Log)
    if not Rows: raise RuntimeError(f"{Role} completed without a freight platform observation")
    for Stage in ("far","near"):
        Visits=re.findall(r"\[Spatial:FreightVisit\] "+Stage+r" actor=[^\r\n]*",Log)
        if not Visits: raise RuntimeError(f"{Role} omitted its {Stage} viewer position")
        print(f"{Role} {Visits[-1]}",flush=True)
    print(f"{Role} {Rows[-1]}",flush=True)


def RunYard(Options, Root):
    Project = PrepareProject(Options.engine.resolve(strict=True), Root, Options.project)
    AddObserver(Options.engine.resolve(), Project)
    Policies = Samples / "ConstructionYard"
    Server, ServerMetadata = Build(Options.packager, Project, Options.server_runtime, "DedicatedServer", Root, "Development",
                      Policies / "DedicatedServer.json")
    Player, PlayerMetadata = Build(Options.packager, Project, Options.player_runtime, "DistributedPlayer", Root, "Development",
                      Policies / "DistributedPlayer.json")
    if ServerMetadata["ClientCodeIdentity"]!=PlayerMetadata["ClientCodeIdentity"]:
        raise RuntimeError("The relocated Yard pair disagrees on client code identity")
    for Package in (Server,Player):
        Digest=hashlib.sha256((Package/"game.package.json").read_bytes()).hexdigest()
        print(f"[Spatial:YardPackage] {Package.name} ManifestSHA256={Digest}",flush=True)
    Env = ManagedEnvironment()
    Exe = ".exe" if os.name == "nt" else ""
    Database = Root / "yard.sqlite"
    DigSignature = None
    for Round in (1, 2):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as Port:
            Port.bind(("127.0.0.1", 0))
            Endpoint = f"127.0.0.1:{Port.getsockname()[1]}"
        ServerArgs = [Server / ("GargantuanServer" + Exe), "--bind", Endpoint, "--max-ticks", "9000",
                      "--persistence-provider", "sqlite", "--persistence-local-db", Database]
        ClientArgs = [Player / ("GargantuanPlayer" + Exe), "--headless", "--connect", Endpoint, "--max-frames", "9000"]
        with ExitStack() as Processes:
            Host = Processes.enter_context(Launch(ServerArgs,Root,Env,f"server-{Round}"))
            Await(Host,"[Runtime:Server] StartupWallMilliseconds=")
            Await(Host,"[Spatial:ConstructionYard] ready edits=")
            First = Processes.enter_context(Launch(ClientArgs,Root,Env,f"first-{Round}"))
            if Round == 2:
                Await(First,"[Spatial:YardProcess] Fresh Server replayed both constructions",(Host,))
                ReportPlatform("restart",First)
                Surfaces = re.findall(r"\[Spatial:YardSurface\] (\d+)",First[1].read_text(errors="replace"))
                Fields = re.findall(r"\[Spatial:YardField\] (\d+)",Host[1].read_text(errors="replace"))
                if not Surfaces or not Fields or (Surfaces[-1], Fields[-1]) != DigSignature:
                    raise RuntimeError("Restart diverged from the observed client surface or authoritative terrain samples")
                print(f"[Spatial:YardProcess] Stable surface={Surfaces[-1]} field={Fields[-1]}",flush=True)
                break
            Await(First,"[Spatial:YardProcess] Builder placed exterior brick",(Host,))
            ReportPlatform("builder",First)
            with Launch(ClientArgs,Root,Env,"second") as Second:
                Await(Second,"[Spatial:YardProcess] Second Player observed the brick and excavated replicated terrain",(Host,First))
                ReportPlatform("digger",Second)
                Await(Host,"[Spatial:YardField] ",(First,Second))
                DigSignature = (re.findall(r"\[Spatial:YardSurface\] (\d+)",Second[1].read_text(errors="replace"))[-1],
                                re.findall(r"\[Spatial:YardField\] (\d+)",Host[1].read_text(errors="replace"))[-1])
            Await(Host,"[Spatial:YardProcess] Builder returned with two persisted constructions",(First,))
            Rejoin = Processes.enter_context(Launch(ClientArgs,Root,Env,"rejoin"))
            Await(Rejoin,"[Spatial:YardProcess] Reconnected Player materialized the edited yard",(Host,First))
            ReportPlatform("rejoin",Rejoin)
    print("[Spatial:YardProcess] Two-client build, dig, door travel, reconnect and SQLite restart passed")


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    for Name in ("engine", "packager", "player-runtime", "server-runtime"):
        Parser.add_argument("--" + Name, type=Path, required=True)
    Parser.add_argument("--project", type=Path, help="Package a temporary copy of this saved Construction Yard")
    with tempfile.TemporaryDirectory(prefix="gym-") as Directory:
        RunYard(Parser.parse_args(),Path(Directory).resolve())
