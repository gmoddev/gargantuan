# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Compare paired script call paths through a stock EditorHost Play process."""
import argparse
import csv
import hashlib
from pathlib import Path
import tempfile
import time

from SpatialCreatorTrial import Create, EditorHost


Lookup = """
local Character = Instance.new("KinematicCharacter")
local Part = Instance.new("Part")
local function Measure(Qualified)
    local Positive = if Qualified then "Engine.KinematicCharacter" else "KinematicCharacter"
    local Negative = if Qualified then "Engine.MeshPart" else "MeshPart"
    local Count = 0
    local Start = os.clock()
    for I = 1, 256 do
        if Character:IsA(Positive) then Count += 1 end
        if Part:IsA(Negative) then Count += 1 end
    end
    local Ns = (os.clock() - Start) * 1000000000 / 512
    assert(Count == 256)
    return Ns
end
for Round = 1, 20 do
    local FirstQualified = Round % 2 == 0
    local First = Measure(FirstQualified)
    local Second = Measure(not FirstQualified)
    print(string.format("[Workspace:ClassLookup] %d %.1f %.1f", Round,
        if FirstQualified then Second else First, if FirstQualified then First else Second))
    task.wait()
end
"""


Motion = """
local World = game:GetService("Workspace")
local Pose = Vector3.new(200, 300, 400)
local Zero = Vector3.zero
local Characters = {}
for _, Depth in {0, 4, 12} do
    local Parent = World
    for N = 1, Depth do
        local Folder = Instance.new("Folder")
        Folder.Parent = Parent
        Parent = Folder
    end
    local Character = Instance.new("KinematicCharacter")
    Character.CFrame = CFrame.new(Pose.X, Pose.Y, Pose.Z)
    Character.Parent = Parent
    local Root = Instance.new("Part")
    Root.Anchored = true
    Root.CanCollide = false
    Root.CFrame = Character.CFrame
    Root.Parent = Character
    Character.RootPart = Root
    Characters[Depth] = Character
end
local function Measure(Character, Direct)
    local Result
    local Start = os.clock()
    if Direct then
        for I = 1, 48 do Result = World:MoveKinematicCapsule(Pose, 1, 4, Zero, Zero) end
    else
        for I = 1, 48 do Result = Character:Move(Zero, Zero) end
    end
    local Ns = (os.clock() - Start) * 1e9 / 48
    assert(Result.Position == Pose and not Result.HasFloor and Result.AppliedTranslation == Zero)
    return Ns
end
for Round = 1, 36 do
    local Depth = ({0, 4, 12})[1 + (Round - 1) % 3]
    local Direct = Round % 2 == 0
    local First = Measure(Characters[Depth], Direct)
    local Second = Measure(Characters[Depth], not Direct)
    print(string.format("[Workspace:MotionPath] %d %d %.1f %.1f", Round, Depth,
        if Direct then Second else First, if Direct then First else Second))
    task.wait()
end
"""


def Measure(Engine, Scratch, Writer, Run, Profile):
    Script = Scratch / "Probe.server.luau"
    Text, Marker, Count = (Motion, "[Workspace:MotionPath] ", 36) if Profile == "motion" else (
        Lookup, "[Workspace:ClassLookup] ", 20)
    Script.write_text(Text, encoding="utf-8")
    Digest = hashlib.sha256(Engine.read_bytes()).hexdigest()
    with EditorHost(Engine) as Host:
        Create(Host, Scratch / "Project", [Script])
        Packet = Host.Call("StartPlaySession", PersistenceMode="Disabled")
        Session = Packet["PlaySessionId"]
        Rows = {}
        Deadline = time.monotonic() + 30
        try:
            while len(Rows) < Count:
                for Event in Packet.get("Diagnostics", []):
                    Message = Event.get("Message", "")
                    if Event.get("Severity") == "Error":
                        raise RuntimeError(Message)
                    if Marker in Message:
                        Round, *Values = Message.split(Marker)[1].split()
                        Rows[int(Round)] = Values
                if time.monotonic() >= Deadline:
                    raise TimeoutError(f"Received {len(Rows)} of {Count} paired measurements")
                time.sleep(.04)
                Packet = Host.Call("PollPlayDiagnostics", PlaySessionId=Session)
        finally:
            Host.Call("StopPlaySession", PlaySessionId=Session)
        for Round in range(1, Count + 1):
            Writer.writerow([Run, Digest, Round, *Rows[Round]])


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--engine", type=Path, action="append", required=True)
    Parser.add_argument("--output", type=Path, required=True)
    Parser.add_argument("--profile", choices=("lookup", "motion"), default="lookup")
    Args = Parser.parse_args()
    with Args.output.open("x", newline="") as Output:
        Writer = csv.writer(Output)
        Fields = ["Depth", "CharacterNsPerCall", "DirectNsPerCall"] if Args.profile == "motion" else [
            "ShortNsPerCall", "QualifiedNsPerCall"]
        Writer.writerow(["Run", "EngineSHA256", "Round", *Fields])
        for Run, Engine in enumerate(Args.engine, 1):
            with tempfile.TemporaryDirectory(prefix="glookup-") as Directory:
                Measure(Engine.resolve(strict=True), Path(Directory), Writer, Run, Args.profile)
            Output.flush()
