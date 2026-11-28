# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Observe scripted motion through a real headless authoring Play world."""
import argparse
import csv
import hashlib
from pathlib import Path
import tempfile
import time

from SpatialCreatorTrial import Create, EditorHost


ScriptSource = """
local World = game:GetService("Workspace")
local Region = Instance.new("SpatialRegion")
Region.Name = "ProbeRegion"
Region.Parent = World
local Preview = Instance.new("ViewportFrame")
Preview.Parent = Region
local function Make(Parent)
    local Actor = Instance.new("KinematicCharacter")
    Actor.CFrame = CFrame.new(7, 80, 5)
    Actor.Parent = Parent
    local Root = Instance.new("Part")
    Root.Anchored = true
    Root.CanCollide = false
    Root.Parent = Actor
    Actor.RootPart = Root
    return Actor, Root
end
local function Observe(Name, Actor)
    local Expected = if Name == "Default" or Name == "Unscoped" then World
        elseif Name == "Named" or Name == "ReplacedRoot" or Name == "ClearedRoot" then Region else nil
    local HasBeforeQuery, BeforeWorld = pcall(function() return Actor:GetMotionWorld() end)
    if HasBeforeQuery then assert(BeforeWorld == Expected, "Wrong physical world before motion: " .. Name) end
    local Before = Actor.Position
    local Ok, Result = pcall(function() return Actor:Move(Vector3.new(0.5, 0, 0), Vector3.zero) end)
    local Delta = Actor.Position - Before
    local HasQuery, MotionWorld = pcall(function() return Actor:GetMotionWorld() end)
    if HasQuery then assert(MotionWorld == Expected, "Wrong physical world after motion: " .. Name) end
    local Route = if not HasQuery then "Unavailable" elseif MotionWorld == nil then "Empty" else MotionWorld.Name
    print(string.format("[Character:RouteProbe] %s %s %.4f %s", Name, tostring(Ok), Delta.X, Route))
end
local Default = Make(World)
Observe("Default", Default)
local Named = Make(Region)
Observe("Named", Named)
local PreviewActor = Make(Preview)
Observe("Preview", PreviewActor)
local External, OldRoot = Make(Region)
OldRoot.Parent = World
Observe("ReparentedRoot", External)
local ReplacementRoot = Instance.new("Part")
ReplacementRoot.Anchored = true
ReplacementRoot.CanCollide = false
ReplacementRoot.Parent = External
External.RootPart = ReplacementRoot
Observe("ReplacedRoot", External)
local Removed, RemovedRoot = Make(Region)
RemovedRoot:Destroy()
Observe("DestroyedRoot", Removed)
Removed.RootPart = nil
Observe("ClearedRoot", Removed)
local Detached = Make(game:GetService("ReplicatedStorage"))
Observe("Unscoped", Detached)
"""


def Collect(Engine, Source, Marker, ExpectedCount):
    Observed = {}
    with tempfile.TemporaryDirectory(prefix="gchar-route-") as Directory, EditorHost(Engine) as Host:
        Scratch = Path(Directory)
        Script = Scratch / "Route.server.luau"
        Script.write_text(Source, encoding="utf-8")
        Create(Host, Scratch / "Project", [Script])
        Packet = Host.Call("StartPlaySession", PersistenceMode="Disabled")
        Session = Packet["PlaySessionId"]
        try:
            Deadline = time.monotonic() + 10
            while len(Observed) < ExpectedCount:
                for Event in Packet.get("Diagnostics", []):
                    Message = Event.get("Message", "")
                    if Event.get("Severity") == "Error":
                        raise RuntimeError(Message)
                    if Marker in Message:
                        Name, *Values = Message.split(Marker)[1].split()
                        Observed[Name] = Values
                if time.monotonic() >= Deadline:
                    raise TimeoutError(f"Missing motion probes from {Observed}")
                if len(Observed) < ExpectedCount:
                    time.sleep(.02)
                    Packet = Host.Call("PollPlayDiagnostics", PlaySessionId=Session)
        finally:
            Host.Call("StopPlaySession", PlaySessionId=Session)
    return Observed


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--engine", type=Path, required=True)
    Parser.add_argument("--output", type=Path, required=True)
    Parser.add_argument("--verify", action="store_true")
    Args = Parser.parse_args()
    Engine = Args.engine.resolve(strict=True)
    Digest = hashlib.sha256(Engine.read_bytes()).hexdigest()
    Expected = {"Default": (True, "Workspace"), "Named": (True, "ProbeRegion"), "Preview": (False, "Empty"),
                "ReparentedRoot": (False, "Empty"), "ReplacedRoot": (True, "ProbeRegion"),
                "DestroyedRoot": (False, "Empty"), "ClearedRoot": (True, "ProbeRegion"), "Unscoped": (True, "Workspace")}
    Captured = Collect(Engine, ScriptSource, "[Character:RouteProbe] ", len(Expected))
    Observed = {Name: (Accepted == "true", float(Delta), Route) for Name, (Accepted, Delta, Route) in Captured.items()}
    with Args.output.open("x", newline="", encoding="utf-8") as Output:
        Rows = csv.writer(Output)
        Rows.writerow(["EngineSha256", "Case", "Accepted", "AppliedX", "MotionWorld"])
        for Name, (Accepted, Delta, Route) in Observed.items():
            Rows.writerow([Digest, Name, Accepted, Delta, Route])
    if Args.verify:
        for Name, (Accepted, Route) in Expected.items():
            if Observed[Name] != (Accepted, .5 if Accepted else 0, Route):
                raise ValueError(f"{Name} expected acceptance {Accepted} and {Route}, recorded {Observed[Name]}")


if __name__ == "__main__":
    Main()
