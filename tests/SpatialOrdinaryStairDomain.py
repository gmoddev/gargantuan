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
if Named then
    Domain = Instance.new("SpatialRegion")
    Domain.Parent = Space
    Avatar.RootPart.SpatialRegion = Domain
    Space.CurrentSpatialRegion = Domain
end
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
    if Ready then
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


def Observe(Engine, Project, Named):
    with EditorHost(Engine) as Host:
        Host.Call("Handshake")
        Root = Host.Call("CreateProject", Destination=Project.as_posix(), Name="Stair domain")["Root"]
        Host.Call("GetSnapshot")
        Script = next(Row for Row in Host.Call("GetSchema")["Definitions"]
                      if Row["Kind"] == "Class" and Row["Namespace"] == "Engine" and Row["Name"] == "Script")
        Source = "local Named = " + ("true" if Named else "false") + "\n" + Scene
        Host.Call("CreateInstance", ClassSchemaId=Script["SchemaId"], DefinitionVersion=Script["DefinitionVersion"],
                  Parent=Root, Name="StairObserver", InitialSource=Source)
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
    Parser.add_argument("--output", type=Path, required=True)
    Args = Parser.parse_args()
    Engine = Args.engine.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="gstair-") as Work:
        Result = {"EditorHostSHA256": Digest(Engine),
                  "DefaultLocomotionSHA256": Digest(Engine.parent / "runtime" / "DefaultLocomotion.luau"),
                  "GroundedWindowTicks": 120, "Key": "D",
                  "Workspace": Observe(Engine, Path(Work) / "Default", False),
                  "SpatialRegion": Observe(Engine, Path(Work) / "Named", True)}
    if not Result["Workspace"]["Climbed"]:
        raise ValueError("The ordinary Workspace control did not climb the low step")
    with Args.output.open("x", encoding="utf-8") as Output:
        json.dump(Result, Output, indent=2)
        Output.write("\n")
    print(json.dumps(Result))
