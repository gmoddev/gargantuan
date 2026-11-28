# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Verify raw co-located native records and retain lossless benchmark evidence."""
import argparse
from collections import defaultdict
import csv
import gzip
import io
from pathlib import Path
import shutil
import statistics
import struct
import zipfile


def Basename(Row):
    return f"B{Row['Block']}R{Row['Run']}-{Row['Profile']}.csv"


def NativeRows(Folder, Name):
    if (Folder/Name).exists():
        with (Folder/Name).open(encoding="utf-8", newline="") as Stream:
            Rows = list(csv.DictReader(Stream))
    elif (Folder/(Name+".gz")).exists():
        with gzip.open(Folder/(Name+".gz"), "rt", encoding="utf-8", newline="") as Stream:
            Rows = list(csv.DictReader(Stream))
    else:
        with zipfile.ZipFile(Folder/"NativeTicks.zip") as Items, Items.open(Name) as Raw, io.TextIOWrapper(
                Raw, encoding="utf-8", newline="") as Stream:
            Rows = list(csv.DictReader(Stream))
    if not Rows or any(int(R["Dropped"]) for R in Rows) or len({R["Tick"] for R in Rows}) != len(Rows):
        raise ValueError("Missing, dropped or repeated native tick")
    return {int(R["Tick"]): R for R in Rows}


def CheckNative(Polls, Records):
    Covered = []
    for R in Polls:
        Before, After = int(R["BeforeTick"]), int(R["AfterTick"])
        if After == Before:
            continue
        Steps = [Records[I] for I in range(Before+1, After+1)]
        for Phase in ("EngineStepNs", "PreSimulationNs", "PhysicsNs", "AnimationNs", "RenderPublicationNs"):
            if Phase in Steps[0] and sum(int(Step[Phase]) for Step in Steps) != int(R[Phase]):
                raise ValueError(f"Per-tick native {Phase} sum differs from its released grouped diagnostic")
        Covered.extend(Steps)
    if len({R["Tick"] for R in Covered}) != len(Covered):
        raise ValueError("Overlapping native tick coverage")
    return Covered


def Float(Bits):
    return struct.unpack("<f", struct.pack("<I", int(Bits)))[0]


def Regime(Tagged, Rows):
    Single = [R for R in Rows if int(R["MotionCalls"]) == 1]
    Negative = [R for R in Single if int(R["NegativeMotionVelocity"]) == 1]
    Positive = [R for R in Single if int(R["NegativeMotionVelocity"]) == 0]
    if len(Negative) + len(Positive) != len(Single):
        raise ValueError("Invalid signed motion statistic")
    Result = dict(**Tagged, RingTicks=len(Rows), SingleMoveTicks=len(Single), Negative=len(Negative),
                  Nonnegative=len(Positive), GroundedSingleMove=sum(int(R["MotionHasFloor"]) for R in Single),
                  SwitchedTicks=sum(int(R["ContextSwitches"]) > 0 for R in Rows),
                  HardwareConfiguredTicks=sum(int(R["HardwareCount"]) != 0 for R in Rows))
    Ordered = sorted(int(R["EngineStepNs"]) for R in Rows)
    Result["AllRingP95Ns"] = Ordered[(95*len(Ordered)+99)//100-1]
    Result["NegativePreservedInputVelocity"] = sum(R["LastInputVelocityYBits"] == R["LastOutputVelocityYBits"] for R in Negative)
    for Label, Group in (("Negative", Negative), ("Nonnegative", Positive)):
        for Key in ("RequestedY", "InputVelocityY", "OutputVelocityY", "AppliedY", "PositionY"):
            Vals = [Float(R["Last"+Key+"Bits"]) for R in Group]
            if Key == "OutputVelocityY" and any((V < 0) != (Label == "Negative") for V in Vals):
                raise ValueError("Native motion classification differs from the IEEE-754 return")
            Result[Label+Key+"Min"] = min(Vals, default="")
            Result[Label+Key+"Max"] = max(Vals, default="")
        for Key in ("EngineStepNs", "PreSimulationNs", "CharacterAdmitMotionNs", "SignalCallbackNs",
                    "EngineStepAllocationCalls", "LuauAllocEvents"):
            Result[Label+Key+"Mean"] = statistics.mean(int(R[Key]) for R in Group) if Group else ""
    return Result


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--input", type=Path, required=True)
    Parser.add_argument("--output", type=Path, required=True)
    Parser.add_argument("--kind", choices=("timing", "operations"), required=True)
    Args = Parser.parse_args()
    with (Args.input/"Processes.csv").open(encoding="utf-8", newline="") as File:
        Processes = list(csv.DictReader(File))
    Polls = defaultdict(list)
    with gzip.open(Args.input/"Polls.csv.gz", "rt", encoding="utf-8", newline="") as File:
        for R in csv.DictReader(File):
            Polls[Basename(R)].append(R)
    if len(Polls) != len(Processes) or len(Processes) % 4:
        raise ValueError("Native diagnostic block incomplete")
    Args.output.mkdir(parents=True, exist_ok=False)
    Notes = []
    with zipfile.ZipFile(Args.output/"NativeTicks.zip", "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as Items:
        for P in Processes:
            Name = Basename(P)
            Records = NativeRows(Args.input, Name)
            Timed = CheckNative(Polls[Name], Records)
            if len(Timed) != int(P["RingTicks"]):
                raise ValueError("Set of independently confirmed native ticks is incomplete")
            Tagged = {K: P[K] for K in ("Scene", "Block", "Run", "Profile", "EngineSHA256")}
            if Args.kind == "timing":
                Notes.append(Regime(Tagged, Timed))
            else:
                Notes.append(dict(**Tagged, RingTicks=len(Timed), **{
                    K: sum(int(R[K]) for R in Timed) for K in Timed[0] if K not in ("Tick", "Dropped", "EngineStepNs")}))
            Items.write(Args.input/Name, arcname=Name)
    for Name in ("Processes.csv", "Polls.csv.gz"):
        shutil.copyfile(Args.input/Name, Args.output/Name)
    with (Args.output/("Regimes.csv" if Args.kind == "timing" else "OperationTotals.csv")).open(
            "x", encoding="utf-8", newline="") as F:
        Table = csv.DictWriter(F, fieldnames=list(Notes[0]))
        Table.writeheader()
        Table.writerows(Notes)
    if Args.kind == "operations":
        with (Args.output/"OperationSummary.csv").open("x", encoding="utf-8", newline="") as F:
            Table = csv.writer(F)
            Table.writerow(("Operation", "Profile", "Total", "Ticks", "PerTick", "MinPerProcess", "MaxPerProcess"))
            for Key in Notes[0]:
                if Key in ("Scene", "Block", "Run", "Profile", "EngineSHA256", "RingTicks"):
                    continue
                for Profile in ("baseline", "current"):
                    Selected = [R for R in Notes if R["Profile"] == Profile]
                    Total, Ticks = sum(R[Key] for R in Selected), sum(R["RingTicks"] for R in Selected)
                    Rates = [R[Key]/R["RingTicks"] for R in Selected]
                    Table.writerow((Key, Profile, Total, Ticks, Total/Ticks, min(Rates), max(Rates)))


if __name__ == "__main__":
    Main()
