# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Check and summarize a complete three-trial native moving-construction ladder."""
import argparse
import csv
import json
import math
import statistics
from pathlib import Path

Counts = (128, 256, 512, 1024, 2048, 4096)
Layouts = ("Solid", "Fragmented", "Hollow", "Irregular", "Mixed")
SampleGroups = (
    ("ContactStep", "ContactStepSamplesUs", 120),
    ("WorldRay", "RaySamplesUs", None),
    ("FreeMotion", "FreeMotionSamplesUs", 30),
    ("Velocity", "VelocitySamplesUs", 16),
)


def ReadWorlds(Pathname):
    Lines = Pathname.read_text(encoding="utf-8-sig").splitlines()
    Raw = [json.loads(Line) for Line in Lines if Line.lstrip().startswith("{")]
    Rows = {(Row["Layout"], Row["Occupied"], Row["Trial"]): Row for Row in Raw}
    Expected = {(Layout, Count, Trial) for Count in Counts for Layout in Layouts for Trial in (1, 2, 3)}
    if len(Rows) != len(Raw) or Rows.keys() != Expected:
        raise ValueError(f"Missing, extra or duplicate native scale cases in {Pathname}")
    return Rows


def Summarize(Pathname, Original, Repeats=()):
    Paths = [Pathname, *Repeats]
    if len({P.resolve() for P in Paths}) != len(Paths):
        raise ValueError("Each native process receipt must be supplied once")
    Processes = [ReadWorlds(P) for P in Paths]
    Results = []
    for Count in Counts:
        for Layout in Layouts:
            Trials = [Rows[Layout, Count, Trial] for Rows in Processes for Trial in (1, 2, 3)]
            Admitted = Count == 128 or (not Original and Layout != "Fragmented")
            if any(T["Accepted"] != Admitted or T["Box3DBytesAfterWorldRetirement"] != 0 for T in Trials):
                raise ValueError(f"Unexpected admission or native retention at {Layout} {Count}")

            def Median(Key):
                return statistics.median(T[Key] for T in Trials)

            Summary = dict(Cells=Count, Layout=Layout, Admitted=Admitted, Trials=len(Trials),
                           CanonicalFieldBytes=Median("CanonicalFieldBytes"),
                           FieldSnapshotBudgetChargeBytes=Median("FieldSnapshotBudgetChargeBytes"),
                           MedianBox3DStaticBytes=Median("Box3DStaticBytesOverEmpty"),
                           MedianFieldEditUs=Median("EditFieldUs"), MedianStaticInstallUs=Median("StaticInstallUs"),
                           MedianActivationAttemptUs=Median("ActivateUs"), MaxBox3DBytesAfterWorldRetirement=0)
            Results.append(Summary)
            if not Admitted:
                Reasons = {T["Admission"] for T in Trials}
                if len(Reasons) != 1 or "" in Reasons:
                    raise ValueError(f"Missing consistent rejection at {Layout} {Count}")
                Summary["Admission"] = Reasons.pop()
                continue

            Children = {T["InstalledHulls"] for T in Trials}
            if len(Children) != 1 or not (1 <= min(Children) <= 128):
                raise ValueError(f"Unstable native convex-child count at {Layout} {Count}")
            if Layout == "Solid":
                Extents = (16, math.ceil(Count / 256), min(16, Count // 16))
                Leaves = math.prod(math.ceil(Axis / 8) for Axis in Extents)
                ExpectedChildren = Count if Original else Leaves
                if min(Children) != ExpectedChildren:
                    raise ValueError(f"Solid leaf cover differs at {Count} cells")
            if Layout == "Fragmented" and min(Children) != Count:
                raise ValueError("A separated unit cube disappeared from the physical construction")
            Summary.update(Hulls=min(Children), Mass=Median("Mass"),
                           MedianBox3DActiveBytes=Median("Box3DDynamicBytesOverEmpty"),
                           MedianNativeBodyAndShapeCreationUs=Median("NativeBodyAndShapeCreationUs"),
                           MedianRemoveUs=Median("MovingRemoveUs"), MedianInsertUs=Median("MovingInsertUs"),
                           MedianRetireUs=Median("RetireGridUs"), MedianPairs120Us=Median("PhysicsPairsProfile120Us"),
                           MedianCollide120Us=Median("PhysicsCollideProfile120Us"),
                           MedianSolve120Us=Median("PhysicsSolveProfile120Us"),
                           WorstCenterError=max(T[P+"COMError"] for T in Trials for P in ("", "Removed", "Restored")),
                           WorstRelativeTensorError=max(T[P+"InertiaRelativeError"] for T in Trials for P in ("", "Removed", "Restored")))
            for Name, Key, PerTrial in SampleGroups:
                Observations = [T[Key] for T in Trials]
                if any(not Samples or (PerTrial and len(Samples) != PerTrial) for Samples in Observations):
                    raise ValueError(f"Incomplete {Name} samples at {Layout} {Count}")
                Ordered = sorted(Value for Samples in Observations for Value in Samples)
                if any(not math.isfinite(Value) or Value < 0 for Value in Ordered):
                    raise ValueError(f"Invalid {Name} sample at {Layout} {Count}")
                Summary[Name+"N"] = len(Ordered)
                for Rank in (50, 95, 99):
                    Summary[f"{Name}P{Rank}Us"] = Ordered[math.ceil(len(Ordered) * Rank / 100) - 1]
                Summary[Name+"WorstUs"] = Ordered[-1]
    return Results


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("input", type=Path)
    Parser.add_argument("output", type=Path)
    Parser.add_argument("--original", action="store_true")
    Parser.add_argument("--repeat", type=Path, action="append", default=[],
                        help="Pool another complete 90-world run of the same product")
    Args = Parser.parse_args()
    Results = Summarize(Args.input, Args.original, Args.repeat)
    Fields = dict.fromkeys(Key for Row in Results for Key in Row)
    with Args.output.open("x", encoding="utf-8", newline="") as File:
        Writer = csv.DictWriter(File, fieldnames=Fields)
        Writer.writeheader()
        Writer.writerows(Results)
    print(f"[Spatial:CompoundScale] {90*(1+len(Args.repeat))} native worlds, "
          f"{sum(R['Admitted'] for R in Results)} / 30 admitted cases")
