# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Sample individual inclusive Engine steps through the existing rolling EditorHost diagnostics."""
import argparse
import csv
import hashlib
import math
from pathlib import Path
import statistics
import tempfile
import time

from SpatialConstructionOrdinaryFrames import EditorHost, Pattern, Probe


def Quantile(Values, Fraction):
    return sorted(Values)[math.ceil(len(Values) * Fraction) - 1]


def Start(Host, Project, Unused, Body):
    Host.Call("Handshake")
    Root = Host.Call("CreateProject", Destination=Project.as_posix(), Name="Ordinary Scene")["Root"]
    Host.Call("GetSnapshot")
    Script = next(Row for Row in Host.Call("GetSchema")["Definitions"]
                  if Row["Kind"] == "Class" and Row["Namespace"] == "Engine" and Row["Name"] == "Script")
    Source = "local WithUnusedObjects=" + ("true" if Unused else "false") + "\n" + Body
    Host.Call("CreateInstance", ClassSchemaId=Script["SchemaId"], DefinitionVersion=Script["DefinitionVersion"],
              Parent=Root, Name="FrameObserver", InitialSource=Source)
    return Host.Call("StartPlaySession", PersistenceMode="Disabled")["PlaySessionId"]


def Measure(Engine, Project, Unused, Target):
    with EditorHost(Engine) as Host:
        Session = Start(Host, Project, Unused, Probe)

        def Reset():
            Host.Call("ConfigureStreamingDiagnostics", PlaySessionId=Session, Enabled=False)
            Snapshot = Host.Call("ConfigureStreamingDiagnostics", PlaySessionId=Session, Enabled=True)["Diagnostics"]["Execution"]
            if Snapshot["Accounting"] != "EditorPlayInclusiveWallTimeV1" or Snapshot["Samples"] != 0:
                raise ValueError("The native step counters did not reset")

        def Snapshot():
            Result = Host.Call("GetStreamingDiagnostics", PlaySessionId=Session, MaximumUnits=0,
                               MaximumTimingSamples=0)["Diagnostics"]["Execution"]
            Step = Result["Phases"]["EngineStep"]
            return Result["Samples"], Step["TotalNanoseconds"], Step["MaximumNanoseconds"]

        try:
            for _ in range(2):
                Reset()
                Deadline = time.monotonic() + 4
                while Snapshot()[0] < 32:
                    if time.monotonic() > Deadline:
                        raise TimeoutError("Play did not warm up")
                    time.sleep(.012)
            Timing, Queries = [], []
            Gaps = MissedTicks = Windows = Saturated = 0
            Deadline = time.monotonic() + 12
            while len(Timing) < Target:
                Reset()
                Windows += 1
                LastCount = LastTotal = LastMax = 0
                while LastCount < 48 and len(Timing) < Target:
                    if time.monotonic() > Deadline:
                        raise TimeoutError("Not enough individually observed Engine steps")
                    time.sleep(.003)
                    Before = time.perf_counter_ns()
                    Count, Total, Maximum = Snapshot()
                    Queries.append(time.perf_counter_ns() - Before)
                    if Count == 60:
                        Saturated += 1
                        break
                    if Count < LastCount or Total < LastTotal:
                        raise ValueError("Native capture regressed before its rolling window filled")
                    DeltaCount, DeltaTime = Count - LastCount, Total - LastTotal
                    if DeltaCount == 1:
                        if DeltaTime <= 0 or max(LastMax, DeltaTime) != Maximum:
                            raise ValueError("Single-step delta disagrees with native window maximum")
                        Timing.append(DeltaTime)
                    elif DeltaCount > 1:
                        Gaps += 1
                        MissedTicks += DeltaCount
                    elif DeltaTime != 0 or Maximum != LastMax:
                        raise ValueError("Stationary sample count changed its total")
                    LastCount, LastTotal, LastMax = Count, Total, Maximum
            return dict(Frames=len(Timing), Windows=Windows, MissedIntervals=Gaps, ExcludedSteps=MissedTicks,
                        SaturatedWindows=Saturated, Snapshots=len(Queries), PollP50Us=round(statistics.median(Queries)/1000,3),
                        PollP95Us=round(Quantile(Queries,.95)/1000,3), P50Us=Quantile(Timing,.5)/1000,
                        P95Us=Quantile(Timing,.95)/1000, P99Us=Quantile(Timing,.99)/1000, WorstUs=max(Timing)/1000,
                        StepNanoseconds=" ".join(map(str,Timing)))
        finally:
            Host.Call("StopPlaySession", PlaySessionId=Session)


def MeasureObserverCost(Engine, Project, Enabled, Target):
    Body = Probe.replace("1088",str(64+Target))
    for Original, Rank in ((512,Target//2),(973,math.ceil(.95*Target)),(1014,math.ceil(.99*Target)),(1024,Target)):
        Body = Body.replace(f"Durations[{Original}]", f"Durations[{Rank}]")
    with EditorHost(Engine) as Host:
        Session = Start(Host, Project, False, Body)
        try:
            Host.Call("ConfigureStreamingDiagnostics", PlaySessionId=Session, Enabled=Enabled)
            Deadline = time.monotonic()+15
            while time.monotonic() < Deadline:
                Diagnostics = Host.Call("PollPlayDiagnostics", PlaySessionId=Session)["Diagnostics"]
                for Row in Diagnostics:
                    Match = Pattern.search(Row.get("Message",""))
                    if Row.get("Severity") == "Error":
                        raise RuntimeError(Row.get("Message"))
                    if Match:
                        if int(Match[1]) != Target:
                            raise ValueError("Incomplete observer cost capture")
                        return dict(Frames=Target, P50Us=Match[2], P95Us=Match[3], P99Us=Match[4], WorstUs=Match[5])
                time.sleep(.15)
            raise TimeoutError("Missing observer cost report")
        finally:
            Host.Call("StopPlaySession", PlaySessionId=Session)


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    for Name in ("engine", "baseline", "output"):
        Parser.add_argument("--" + Name, type=Path, required=True)
    Parser.add_argument("--trials", type=int, default=10)
    Parser.add_argument("--frames", type=int, default=192)
    Parser.add_argument("--observer-cost", action="store_true",
                        help="Pair the recent ordinary scene with native counters enabled and disabled; use its identical script subphase timer")
    Args = Parser.parse_args()
    if not 1 <= Args.trials <= 15 or not 32 <= Args.frames <= 512:
        Parser.error("Use 1–15 trials and 32–512 individually observed steps per process")
    Profiles = [(Args.engine.resolve(strict=True), "ordinary", False),
                (Args.engine.resolve(strict=True), "empty-domain-grid", True),
                (Args.baseline.resolve(strict=True), "historical-ordinary", False)]
    if Args.observer_cost:
        Profiles = [(Args.engine.resolve(strict=True), "observer-off", False), (Args.engine.resolve(strict=True), "observer-on", True)]
    Digests = {Engine: hashlib.sha256(Engine.read_bytes()).hexdigest() for Engine, _, _ in Profiles}
    with Args.output.open("x", encoding="utf-8", newline="") as Output, tempfile.TemporaryDirectory(prefix="gwt-") as Folder:
        Table = None
        for Trial in range(Args.trials):
            Order = Profiles[Trial % len(Profiles):] + Profiles[:Trial % len(Profiles)]
            for Engine, Label, Unused in Order:
                Method = MeasureObserverCost if Args.observer_cost else Measure
                Result = Method(Engine, Path(Folder) / f"{Label}-{Trial}", Unused, Args.frames)
                Row = dict(EngineSHA256=Digests[Engine], Profile=Label, Trial=Trial+1, **Result)
                if Table is None:
                    Table = csv.DictWriter(Output, fieldnames=Row.keys())
                    Table.writeheader()
                Table.writerow(Row)
                Output.flush()
                print(f"{Label} {Trial+1} p95={Row['P95Us']} us", flush=True)


if __name__ == "__main__":
    Main()
