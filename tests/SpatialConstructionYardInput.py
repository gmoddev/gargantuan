#!/usr/bin/env python3
"""Replay the Construction Yard controls against the normal Engine EditorHost."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

Samples = Path(__file__).resolve().parents[1] / "samples"
sys.path.insert(0, str(Samples / "TerrainQuarry"))
from BuildProject import EditorHost

PlatformWitness = (Samples.parent / "tests/blockgrid/gameplay/ObserveYardPlatform.luau").read_text(encoding="utf-8")
Proof = PlatformWitness + r'''
local World = game.Workspace
local Workshop = World.Workshop
local Wall = World.RetainingWall
local Floor = Workshop.WorkshopBlocks
local Players = game:GetService("Players")
local Run = game:GetService("RunService")
local Target = Vector3.new(-2, 2, -5)
local Initial = World.Terrain:GetGridInfo().Revision
local Stage = 0
Run.PostSimulation:Connect(function()
    local Player = Players.LocalPlayer or Players:GetPlayers()[1]
    local Avatar = if Player then Player.Character else nil
    if not Avatar or not Avatar.RootPart then return end
    local Inside = Avatar.RootPart.SpatialRegion == Workshop and World.CurrentSpatialRegion == Workshop
    if Stage == 0 and World:FindFirstChild("OutdoorMarker") and Workshop:FindFirstChild("WorkshopMarker") and ObserveMovingPlatform(World) then
        Stage = 1
        print("[Spatial:YardInput] ready")
    elseif Stage == 1 and Wall:GetBlock(Target) then
        assert(Wall:GetBlock(Target).Definition == "Brick" and not Floor:GetBlock(Target))
        Stage = 2
        print("[Spatial:YardInput] built outside")
    elseif Stage == 2 and Inside then
        Stage = 3
        print("[Spatial:YardInput] entered workshop")
    elseif Stage == 3 and Floor:GetBlock(Target) then
        assert(Floor:GetBlock(Target).Definition == "Brick" and Wall:GetBlock(Target).Definition == "Brick")
        Stage = 4
        print("[Spatial:YardInput] built inside")
    elseif Stage == 4 and Avatar.RootPart.SpatialRegion == nil and World.CurrentSpatialRegion == nil then
        Stage = 5
        print("[Spatial:YardInput] returned to yard")
    elseif Stage == 5 and World.Terrain:GetGridInfo().Revision ~= Initial then
        Stage = 6
        print("[Spatial:YardInput] excavation committed")
    end
end)
'''

ReplayProof = r'''
local World = game.Workspace
local Target = Vector3.new(-2, 2, -5)
local Connection
Connection = game:GetService("RunService").PostSimulation:Connect(function()
    local Outside = World.RetainingWall:GetBlock(Target)
    local Inside = World.Workshop.WorkshopBlocks:GetBlock(Target)
    if Outside and Inside and Outside.Definition == "Brick" and Inside.Definition == "Brick" then
        print("[Spatial:YardInput] both constructions restored")
        Connection:Disconnect()
    end
end)
'''

def PrepareProject(Engine, Root, SavedProject=None):
    Project = Root / "Yard"
    if SavedProject:
        shutil.copytree(SavedProject.resolve(strict=True), Project)
    else:
        subprocess.run([sys.executable, "-B", str(Samples / "ConstructionYard" / "BuildProject.py"),
                        "--engine", str(Engine), "--project", str(Project)], check=True, timeout=90)
    with (Project / ".gargantuan" / "project.instance.json").open("rb") as Document:
        Digest = hashlib.file_digest(Document, "sha256").hexdigest()
    print(f"[Spatial:YardInput] Authored project.instance.json SHA256={Digest}", flush=True)
    return Project


def Verify(Engine, Project, Mode="Disabled", Replay=False):
    with EditorHost(Engine) as Host:
        Host.Call("Handshake")
        Host.Call("OpenProject", Root=Project.as_posix())
        Snapshot = Host.Call("GetSnapshot")["Snapshot"]
        Space = next(Object["Id"] for Object in Snapshot["Objects"] if Object["ClassName"] == "Workspace")
        Wall = next(Object["Id"] for Object in Snapshot["Objects"] if Object["Name"] == "RetainingWall")
        Script = next(Row for Row in Host.Call("GetSchema")["Definitions"]
                      if Row["Kind"] == "Class" and Row["Namespace"] == "Engine" and Row["Name"] == "Script")
        Host.Call("CreateInstance", ClassSchemaId=Script["SchemaId"], DefinitionVersion=Script["DefinitionVersion"],
                  Parent=Space, Name="QualificationObserver", InitialSource=ReplayProof if Replay else Proof)
        Started = Host.Call("StartPlaySession", PersistenceMode=Mode)
        Session = Started["PlaySessionId"]
        Messages = []

        def Collect(Packet):
            for Row in Packet.get("Diagnostics", []):
                if Row.get("Severity") == "Error":
                    raise RuntimeError(f"Construction Yard Play failed: {Row}")
                Messages.append(Row.get("Message", ""))

        def Wait(Marker, Prefix="[Spatial:YardInput] "):
            Limit = time.monotonic() + 12
            while not any(Prefix + Marker in Message for Message in Messages):
                if time.monotonic() >= Limit:
                    raise RuntimeError(f"Missing {Marker!r} in runtime diagnostics: {Messages[-12:]}")
                time.sleep(0.04)
                Collect(Host.Call("PollPlayDiagnostics", PlaySessionId=Session))

        try:
            Collect(Started)
            if Replay:
                Wait("ready edits=3 persistence=documents", "[Spatial:ConstructionYard] ")
                Wait("both constructions restored")
            else:
                Wait("ready")
                Wait("ready edits=0", "[Spatial:ConstructionYard] ")
            Actions = (("b", 5, "built outside"), ("f", 9, "entered workshop"),
                       ("b", 5, "built inside"), ("f", 9, "returned to yard"), ("e", 8, "excavation committed"))
            for Key, Physical, Expected in (() if Replay else Actions):
                for State in ("Pressed", "Released"):
                    Host.Call("SendPlayInput", PlaySessionId=Session, Type="Key",
                              Logical=ord(Key), Physical=Physical, State=State)
                    if State == "Pressed":
                        time.sleep(0.06)
                Wait(Expected)
        finally:
            Collect(Host.Call("StopPlaySession", PlaySessionId=Session))
        Cell = Host.Call("ReadBlockGridCell", Scope=Snapshot["Cursor"]["Scope"], Object=Wall, Cell=[-2, 2, -5])
        if Cell["Definition"] != "Air":
            raise RuntimeError("Edit world retained the runtime wall cell after Stop")
        End=Host.Call("GetSnapshot")["Snapshot"]
        Deck=next(Obj for Obj in End["Objects"] if Obj["Name"]=="FreightPlatform")
        Properties=Deck["EditorProperties"]
        Marker=Host.Call("ReadBlockGridCell",Scope=End["Cursor"]["Scope"],Object=Deck["Id"],Cell=[0,1,0])
        if (Properties["MotionMode"]["Value"]!="Static" or Properties["CFrame"]["Value"]!=[19,4,0,1,0,0,0,1,0,0,0,1] or
                Marker["Definition"]!="Stone"):
            raise RuntimeError("Runtime platform motion or geometry leaked into the saved document")
        if not Replay:
            print(next(Message for Message in Messages if "[Spatial:FreightPlatform]" in Message))
        print("[Spatial:YardInput] Durable restart restored both grids and excavation" if Replay else
              "[Spatial:YardInput] Both builds, both doorway crossings, excavation and Stop verified")


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--engine", type=Path, required=True)
    Parser.add_argument("--durable", action="store_true", help="Use a fresh SQLite profile and verify a fresh process replays it")
    Parser.add_argument("--project", type=Path, help="Open a temporary copy of this saved Construction Yard")
    Args = Parser.parse_args()
    Executable = Args.engine.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="gsc-") as Directory:
        Root = Path(Directory)
        Destination = PrepareProject(Executable, Root, Args.project)
        if Args.durable:
            DataRoot = Root / "PlayData"
            DataRoot.mkdir()
            os.environ["GARGANTUAN_STUDIO_PLAY_DATA_ROOT"] = str(DataRoot)
            Verify(Executable, Destination, "Persistent")
            Verify(Executable, Destination, "Persistent", Replay=True)
        else:
            Verify(Executable, Destination)

