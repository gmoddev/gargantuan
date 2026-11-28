# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Compare the stock player's response to the same low obstacle in Workspace and a named SpatialRegion."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import tempfile
import time

from SpatialConstructionOrdinaryFrames import EditorHost


Scene = r'''
local Space = game.Workspace
local Avatar = game:GetService("Players").LocalPlayer.Character
local Domain = Space
if Mode ~= "Workspace" then
    Domain = Instance.new("SpatialRegion")
    Domain.Parent = Space
    if Mode == "Inherited" then
        local Carrier = Instance.new("Part")
        Carrier.Name = "NestedRig"
        Carrier.Anchored = true
        Carrier.CanCollide = false
        Carrier.Parent = Avatar
        Carrier.SpatialRegion = Domain
        Avatar.RootPart.Parent = Carrier
    elseif Mode == "Parented" then
        Avatar.Parent = Domain
    else
        Avatar.RootPart.SpatialRegion = Domain
    end
    if Mode == "SpatialRegion" then Space.CurrentSpatialRegion = Domain end
end
assert(Avatar:GetMotionWorld() == Domain, "Preview world must follow the avatar independently of the camera")
local function AddPart(Name, X, Y, Width, Height, Depth)
    local Part = Instance.new("Part")
    Part.Name = Name
    Part.Anchored = true
    Part.CanCollide = true
    Part.Size = Vector3.new(Width, Height, Depth)
    Part.Position = Vector3.new(X, Y, 100)
    Part.Parent = Domain
end
AddPart("Floor", 200, -1, 60, 2, 60)
AddPart("Step", 204, 0.5, 4, 1, 6)
Avatar.CFrame = CFrame.new(200, 6, 100)
local Ready = false
local Frame, MaximumY = 0, 0
local Connection
Connection = game:GetService("RunService").PostSimulation:Connect(function()
    if not Ready and Avatar.Grounded then
        Ready = true
        print("[Spatial:StairWorld] ready")
    end
    if Ready and (not WaitForMotion or Avatar.Position.X > 200.3) then
        Frame += 1
        MaximumY = math.max(MaximumY, Avatar.Position.Y)
        if Frame == 120 then
            print(string.format("[Spatial:StairWorld] x=%.3f y=%.3f z=%.3f maxY=%.3f",
                Avatar.Position.X, Avatar.Position.Y, Avatar.Position.Z, MaximumY))
            Connection:Disconnect()
        end
    end
end)
'''
Witness = re.compile(r"\[Spatial:StairWorld\] x=([-\d.]+) y=([-\d.]+) z=([-\d.]+) maxY=([-\d.]+)")


def CreateScene(Host, Project, Mode, WaitForMotion=False):
    Host.Call("Handshake")
    Root = Host.Call("CreateProject", Destination=Project.as_posix(), Name="Stair domain")["Root"]
    Host.Call("GetSnapshot")
    Script = next(Row for Row in Host.Call("GetSchema")["Definitions"]
                  if Row["Kind"] == "Class" and Row["Namespace"] == "Engine" and Row["Name"] == "Script")
    Source = f"local Mode = {json.dumps(Mode)}\nlocal WaitForMotion = {json.dumps(WaitForMotion)}\n" + Scene
    Host.Call("CreateInstance", ClassSchemaId=Script["SchemaId"], DefinitionVersion=Script["DefinitionVersion"],
              Parent=Root, Name="StairObserver", InitialSource=Source)


def Observe(Engine, Project, Mode):
    with EditorHost(Engine) as Host:
        CreateScene(Host, Project, Mode)
        Packet = Host.Call("StartPlaySession", PersistenceMode="Disabled")
        Session = Packet["PlaySessionId"]
        Pressed = False
        try:
            Deadline = time.monotonic() + 12
            while time.monotonic() < Deadline:
                for Entry in Packet.get("Diagnostics", []):
                    Message = Entry.get("Message", "")
                    if Entry.get("Severity") == "Error":
                        raise RuntimeError(Message)
                    if Message == "[Spatial:StairWorld] ready" and not Pressed:
                        Host.Call("SendPlayInput", PlaySessionId=Session, Type="Key", Logical=ord("d"),
                                  Physical=7, State="Pressed")
                        Pressed = True
                    Match = Witness.fullmatch(Message)
                    if Pressed and Match:
                        X, Y, Z, MaximumY = map(float, Match.groups())
                        return {"X": X, "Y": Y, "Z": Z, "MaximumY": MaximumY, "Climbed": X > 206 and MaximumY > 2.75}
                time.sleep(.025)
                Packet = Host.Call("PollPlayDiagnostics", PlaySessionId=Session)
            raise TimeoutError("The stock player did not complete its grounded observation window")
        finally:
            if Pressed:
                Host.Call("SendPlayInput", PlaySessionId=Session, Type="Key", Logical=ord("d"), Physical=7, State="Released")
            Host.Call("StopPlaySession", PlaySessionId=Session)


def Digest(File):
    with File.open("rb") as Stream:
        return hashlib.file_digest(Stream, "sha256").hexdigest()


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--engine", type=Path, required=True)
    Target = Parser.add_mutually_exclusive_group(required=True)
    Target.add_argument("--output", type=Path)
    Target.add_argument("--seed-foreground", type=Path, help="Create a named-domain scene for native Studio Play input")
    Args = Parser.parse_args()
    Engine = Args.engine.resolve(strict=True)
    if Args.seed_foreground:
        Project = Args.seed_foreground.resolve()
        if Project.exists():
            raise FileExistsError(Project)
        with EditorHost(Engine) as Host:
            CreateScene(Host, Project, "SpatialRegion", WaitForMotion=True)
            Host.Call("SaveProject")
        raise SystemExit(0)
    with tempfile.TemporaryDirectory(prefix="gstair-") as Work:
        Result = {"EditorHostSHA256": Digest(Engine),
                  "DefaultLocomotionSHA256": Digest(Engine.parent / "runtime" / "DefaultLocomotion.luau"),
                  "GroundedWindowTicks": 120, "Key": "D"}
        for Mode in ("Workspace", "SpatialRegion", "ForeignView", "Inherited", "Parented"):
            Result[Mode] = Observe(Engine, Path(Work) / Mode, Mode)
    with Args.output.open("x", encoding="utf-8") as Output:
        json.dump(Result, Output, indent=2)
        Output.write("\n")
    print(json.dumps(Result))
    if not all(Result[Mode]["Climbed"] for Mode in ("Workspace", "SpatialRegion", "ForeignView", "Inherited", "Parented")):
        raise ValueError("The controller did not climb the low step in every collision domain")
