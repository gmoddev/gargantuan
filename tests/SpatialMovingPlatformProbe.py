# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Verify live character landing on two positions of a physical moving platform inside a named world."""
import argparse
import csv
import hashlib
from pathlib import Path

from SpatialCharacterRouteProbe import Collect


PlatformScript = """
local Workspace = game:GetService("Workspace")
local Run = game:GetService("RunService")
local Region = Instance.new("SpatialRegion")
Region.Name = "PlatformRoom"
Region.Parent = Workspace
Region.Gravity = 0
local Platform = Instance.new("Part")
Platform.Name = "MovingPlatform"
Platform.Size = Vector3.new(6, 2, 6)
Platform.Anchored = false
Platform.CanCollide = true
Platform.CFrame = CFrame.new(120, 14, 150)
Platform.Parent = Region
Platform:SetLinearVelocity(Vector3.new(16, 0, 0))
local function Make(World)
    local Actor = Instance.new("KinematicCharacter")
    Actor.Parent = World
    local Root = Instance.new("Part")
    Root.Anchored = true
    Root.CanCollide = false
    Root.Parent = Actor
    Actor.RootPart = Root
    return Actor
end
local Controller = Make(Region)
local Coincident = Make(Workspace)
local FirstX
local Finished = false
local function Observe(Name, X)
    local Before = Controller.Position.X
    local Pose = CFrame.new(X, 20, 150)
    Controller.CFrame = Pose
    Coincident.CFrame = Pose
    local Down = Vector3.new(0, -8, 0)
    local Local = Controller:Move(Down, Down)
    local Other = Coincident:Move(Down, Down)
    local Hit = Region:Raycast(Vector3.new(X + 2, 22, 150), Vector3.new(0, -12, 0))
    local PreviousClear = true
    if FirstX then
        local Old = Region:Raycast(Vector3.new(FirstX + 1, 22, 150), Vector3.new(0, -12, 0))
        PreviousClear = not Old or Old.Instance == nil
    end
    print(string.format("[Character:PlatformProbe] %s %.5f %.5f %.5f %.5f %s %.5f %s %s %s %.5f",
        Name, X, Platform:GetLinearVelocity().X, Local.Position.Y, Local.FloorNormal.Y, tostring(Local.HasFloor),
        Other.Position.Y, tostring(Other.HasFloor), tostring(Hit ~= nil and Hit.Instance == Platform),
        tostring(PreviousClear), Before))
end
Run.PostSimulation:Connect(function()
    if Finished then return end
    local X = Platform.Position.X
    if not FirstX and X > 120.1 then
        Observe("First", X)
        FirstX = X
    elseif FirstX and X >= FirstX + 7 then
        Finished = true
        Observe("Second", X)
    end
end)
"""


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--engine", required=True, type=Path)
    Parser.add_argument("--output", required=True, type=Path)
    Args = Parser.parse_args()
    Engine = Args.engine.resolve(strict=True)
    Observed = Collect(Engine, PlatformScript, "[Character:PlatformProbe] ", 2)
    Fields = ["Position", "PlatformX", "BodyVelocityX", "LocalY", "FloorNormalY", "HasFloor",
              "CoincidentY", "CoincidentHasFloor", "HitPlatform", "PreviousLocationClear", "PriorActorX"]
    with Args.output.open("x", newline="", encoding="utf-8") as Output:
        Rows = csv.writer(Output)
        Rows.writerow(["EngineSha256", *Fields])
        Digest = hashlib.sha256(Engine.read_bytes()).hexdigest()
        for Name, Values in Observed.items():
            Rows.writerow([Digest, Name, *Values])
    for Name, Values in Observed.items():
        X, Velocity, LocalY, Normal, Floor, OtherY, OtherFloor, Hit, OldClear, _ = Values
        if (float(Velocity) < 10 or not 16.8 < float(LocalY) < 17.2 or float(Normal) < .99 or Floor != "true" or
                abs(float(OtherY) - 12) > .001 or OtherFloor != "false" or Hit != "true" or OldClear != "true"):
            raise ValueError(f"Moving platform proof failed at {Name}: {Values}")
    if float(Observed["Second"][0]) - float(Observed["First"][0]) < 6.99:
        raise ValueError("The observations must be more than a platform width apart")


if __name__ == "__main__":
    Main()
