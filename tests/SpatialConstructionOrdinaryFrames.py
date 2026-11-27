# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Time the ordinary simulation phase in real EditorHost Play across fresh processes."""
import argparse
import csv
import hashlib
from pathlib import Path
import re
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "samples" / "TerrainQuarry"))
from BuildProject import EditorHost


Probe = r'''
local Space=game:GetService("Workspace")
local Run=game:GetService("RunService")
for Z=0,3 do
    for X=0,5 do
        local P=Instance.new("Part")
        P.Anchored=true
        P.Size=Vector3.new(2,1,2)
        P.Position=Vector3.new(X*3,-1,Z*3)
        P.Parent=Space
    end
end
if WithUnusedObjects then
    local Domain=Instance.new("SpatialRegion")
    Domain.Parent=Space
    local Grid=Instance.new("BlockGrid")
    Grid.Parent=Domain
end
local Count=0
local Start
local Durations={}
Run.PreSimulation:Connect(function() Start=os.clock() end)
Run.PostSimulation:Connect(function()
    if not Start or Count>=1088 then return end
    local Finish=os.clock()
    Count+=1
    if Count>64 then table.insert(Durations,(Finish-Start)*1000000) end
    if Count==1088 then
        table.sort(Durations)
        print(string.format("[Spatial:OrdinaryFrames] n=%d p50=%.3f p95=%.3f p99=%.3f max=%.3f",
            #Durations,Durations[512],Durations[973],Durations[1014],Durations[1024]))
    end
end)
'''
Pattern = re.compile(r"\[Spatial:OrdinaryFrames\] n=(\d+) p50=([.\d]+) p95=([.\d]+) p99=([.\d]+) max=([.\d]+)")


def Measure(Engine, Project, Unused):
    with EditorHost(Engine) as Host:
        Host.Call("Handshake")
        Root = Host.Call("CreateProject", Destination=Project.as_posix(), Name="Ordinary Scene")["Root"]
        Host.Call("GetSnapshot")
        Schema = next(Row for Row in Host.Call("GetSchema")["Definitions"]
                      if Row["Kind"] == "Class" and Row["Namespace"] == "Engine" and Row["Name"] == "Script")
        Source = "local WithUnusedObjects=" + ("true" if Unused else "false") + "\n" + Probe
        Host.Call("CreateInstance", ClassSchemaId=Schema["SchemaId"], DefinitionVersion=Schema["DefinitionVersion"],
                  Parent=Root, Name="FrameObserver", InitialSource=Source)
        Started = Host.Call("StartPlaySession", PersistenceMode="Disabled")
        Session = Started["PlaySessionId"]
        Deadline = time.monotonic() + 65
        Packet = Started
        try:
            while time.monotonic() < Deadline:
                for Row in Packet.get("Diagnostics", []):
                    if Row.get("Severity") == "Error":
                        raise RuntimeError(Row.get("Message"))
                    Match = Pattern.search(Row.get("Message", ""))
                    if Match:
                        if Match[1] != "1024":
                            raise RuntimeError("Incomplete frame distribution")
                        return list(Match.groups())
                time.sleep(.20)
                Packet = Host.Call("PollPlayDiagnostics", PlaySessionId=Session)
            raise RuntimeError("The ordinary scene did not complete 1,024 measured simulation frames")
        finally:
            Host.Call("StopPlaySession", PlaySessionId=Session)


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--engine", type=Path, required=True)
    Parser.add_argument("--baseline", type=Path)
    Parser.add_argument("--trials", type=int, default=3)
    Parser.add_argument("--output", type=Path, required=True)
    Args = Parser.parse_args()
    if not 1 <= Args.trials <= 15:
        Parser.error("--trials must be between 1 and 15")
    Profiles = [(Args.engine.resolve(strict=True), "ordinary", False),
                (Args.engine.resolve(strict=True), "empty-domain-grid", True)]
    if Args.baseline:
        Profiles.append((Args.baseline.resolve(strict=True), "historical-ordinary", False))
    Digests = {Engine: hashlib.sha256(Engine.read_bytes()).hexdigest() for Engine, _, _ in Profiles}
    with Args.output.open("x", newline="") as Output, tempfile.TemporaryDirectory(prefix="gof-") as Folder:
        Table = csv.writer(Output)
        Table.writerow(["EngineSHA256", "Profile", "Trial", "Frames", "P50Us", "P95Us", "P99Us", "WorstUs"])
        for Trial in range(Args.trials):
            Order = Profiles[Trial % len(Profiles):] + Profiles[:Trial % len(Profiles)]
            for Engine, Label, Unused in Order:
                Times = Measure(Engine, Path(Folder) / f"{Label}-{Trial}", Unused)
                Table.writerow([Digests[Engine], Label, Trial + 1, *Times])
                Output.flush()
                print(Label, Trial + 1, *Times, flush=True)
