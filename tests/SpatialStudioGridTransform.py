"""Verify the real Studio-edited grid pose and both chart-local ray results on cold Play."""
import argparse
from pathlib import Path
import time

from SpatialConstructionOrdinaryFrames import EditorHost


Proof = """
local Source = workspace.StampSource
local Domain = workspace.Workshop
assert(Source.SpatialRegion == Domain, "Reopened grid owner is not the authored Workshop")
local Frame = Source.CFrame
assert((Frame.Position - Vector3.new(4,3,-5)).Magnitude < 0.001, "Reopened position "..tostring(Frame.Position))
assert((Frame.RightVector - Vector3.new(0,0,-1)).Magnitude < 0.001, "Reopened right axis "..tostring(Frame.RightVector))
local Cell = Vector3.new(1,0,0)
assert(Source:GetBlock(Cell).Definition == "Stone", "The original Stone cell changed")
local Origin = Source:CellToWorld(Cell) + Frame.UpVector * 4
local Direction = Frame.UpVector * -8
local Params = RaycastParams.new()
Params.IncludeBlockGrids = true
Params.FilterType = Enum.RaycastFilterType.Include
Params.FilterDescendantsInstances = {Source}
local Hit = Domain:Raycast(Origin, Direction, Params)
assert(Hit and Hit.Instance == Source and Hit.GridHit.Cell == Cell, "The named chart failed to resolve the transformed cell")
local Allowed, Reason = pcall(function() return workspace:Raycast(Origin, Direction, Params) end)
assert(not Allowed and string.find(tostring(Reason), "filter root belongs to another world", 1, true),
    "Workspace must reject the named chart's filter root")
local Open = RaycastParams.new()
Open.IncludeBlockGrids = true
local Outside = workspace:Raycast(Origin, Direction, Open)
assert(not Outside or Outside.Instance ~= Source, "Workspace returned the named chart's transformed cell")
print("[Spatial:StudioTransform] Reopened translated and rotated grid; only Workshop hit Stone")
"""


def Verify(Engine, Project):
    with EditorHost(Engine) as Host:
        Host.Call("Handshake")
        Host.Call("OpenProject", Root=Project.as_posix())
        Objects = Host.Call("GetSnapshot")["Snapshot"]["Objects"]
        Space = next(O["Id"] for O in Objects if O["ClassName"] == "Workspace")
        Script = next(O for O in Host.Call("GetSchema")["Definitions"]
                      if O["Kind"] == "Class" and O["Namespace"] == "Engine" and O["Name"] == "Script")
        Host.Call("CreateInstance", ClassSchemaId=Script["SchemaId"], DefinitionVersion=Script["DefinitionVersion"],
                  Parent=Space, Name="TransformProof", InitialSource=Proof)
        Packet = Host.Call("StartPlaySession", PersistenceMode="Disabled")
        Session = Packet["PlaySessionId"]
        try:
            Deadline = time.monotonic() + 10
            while time.monotonic() < Deadline:
                for Entry in Packet.get("Diagnostics", []):
                    if Entry["Severity"] == "Error":
                        raise RuntimeError(Entry["Message"])
                    if Entry["Message"].startswith("[Spatial:StudioTransform] "):
                        print(Entry["Message"])
                        return
                time.sleep(.03)
                Packet = Host.Call("PollPlayDiagnostics", PlaySessionId=Session)
            raise TimeoutError("Missing transformed cell witness")
        finally:
            Host.Call("StopPlaySession", PlaySessionId=Session)


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--engine", type=Path, required=True)
    Parser.add_argument("--project", type=Path, required=True)
    Args = Parser.parse_args()
    Verify(Args.engine.resolve(strict=True), Args.project.resolve(strict=True))
