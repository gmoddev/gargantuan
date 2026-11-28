# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Independently recompute the 64/16 fixed comparison from native integer observations (NumPy)."""
import argparse
import csv
import gzip
import json
import math
from pathlib import Path
import random

import numpy as np


# Table values for the fixed two-sided and one-sided Student intervals.
Fixed = {"ordinary": (64, 1024, 1.9983405425, 1.6694022225),
         "rigid": (16, 768, 2.1314495456, 1.7530503557)}
Historical = "0f43758828929e453d054689e81524a662e75c9e26d8ec4d4a4089396795d4aa"


def Groups(Field):
    return [tuple(int(V) for V in Item.split(":")) for Item in Field.split()]


def GroupRanks(Known, Merged, Unobserved):
    # Independent feasibility in counts at or below x. A sum exceeding k*x
    # needs at least one long step; every long step costs at least x extra
    # nanoseconds beyond the positive minimum of one per group member.
    GroupsOnly = [(Size, Total) for _, Size, Total in Merged]
    TotalCount = len(Known) + Unobserved + sum(Size for Size, _ in GroupsOnly)
    Rank = (95 * TotalCount + 99) // 100
    High = max([int(Known[-1]), *(Total for _, Total in GroupsOnly)])

    def CountAtMost(Threshold, Possible):
        Observed = int(np.searchsorted(Known, Threshold, side="right"))
        if Possible:
            return Observed + Unobserved + sum(Size - int(Total > Size * Threshold) for Size, Total in GroupsOnly)
        return Observed + sum(max(0, Size - (Total - Size) // Threshold) for Size, Total in GroupsOnly)

    def First(Possible):
        if CountAtMost(High, Possible) < Rank:
            return math.inf
        Left, Right = 0, High
        while Right - Left > 1:
            Mid = (Left + Right) // 2
            if CountAtMost(Mid, Possible) >= Rank:
                Right = Mid
            else:
                Left = Mid
        return Right

    return First(True), First(False)


def Values(Receipt, Raw, Target):
    Ticks = np.fromstring(Raw["StepNanoseconds"], dtype=np.int64, sep=" ")
    if len(Ticks) != len(Raw["StepNanoseconds"].split()):
        raise ValueError("Unparsed native tick value")
    Gaps = Groups(Raw["OmittedGroupsStartCountTotal"])
    Resets = Groups(Raw["ResetGapsStartCount"])
    N = int(Receipt["EndSimulationTick"]) - int(Receipt["StartSimulationTick"])
    Coverage = np.zeros(N, dtype=np.uint8)
    for Start, Size, *Rest in Gaps + Resets:
        if Start < 0 or Size < 1 or Start + Size > N or np.any(Coverage[Start:Start+Size]):
            raise ValueError("Invalid or overlapping omitted native ordinal range")
        Coverage[Start:Start+Size] = 1
    if any(Size < 2 or Total < Size for _, Size, Total in Gaps) or not np.all(Ticks > 0):
        raise ValueError("Nonpositive duration or malformed grouped poll")
    PollMissing = sum(Size for _, Size, _ in Gaps)
    ResetMissing = sum(Size for _, Size in Resets)
    Whole = int(Receipt["EngineStepNs"])
    Components = sum(int(Receipt[Field + "Ns"]) for Field in ("PreSimulation", "Physics", "Animation", "RenderPublication"))
    Checks = {
        "Attempted": N, "Missing": int(Coverage.sum()), "Exact": len(Ticks),
        "PollMissing": PollMissing, "ResetMissing": ResetMissing, "Gaps": len(Gaps), "ResetGaps": len(Resets),
        "RingTicks": len(Ticks) + PollMissing,
        "EngineStepNs": int(Ticks.sum()) + sum(Total for _, _, Total in Gaps),
    }
    if (any(int(Receipt[K]) != V for K, V in Checks.items()) or
            len(Ticks) != N - int(Coverage.sum()) or Checks["RingTicks"] < Target or Components > Whole):
        raise ValueError("Raw native totals, count, scope or ordinal coverage disagree")
    P50, P95, P99 = np.percentile(Ticks, [50, 95, 99], method="inverted_cdf").astype(int).tolist()
    if [int(Receipt["P" + str(P) + "Ns"]) for P in (50, 95, 99)] != [P50, P95, P99]:
        raise ValueError("A native process percentile differs")
    Missing = int(Coverage.sum())
    Lower = np.percentile(np.r_[Ticks, np.zeros(Missing)], 95, method="inverted_cdf")
    Upper = np.percentile(np.r_[Ticks, np.full(Missing, math.inf)], 95, method="inverted_cdf")
    Summed = GroupRanks(np.sort(Ticks), Gaps, ResetMissing)
    if any(float(Receipt[K]) != V for K, V in zip(
            ("LowerP95Ns", "UpperP95Ns", "SummedLowerP95Ns", "SummedUpperP95Ns"), [Lower, Upper, *Summed])):
        raise ValueError("Independent omitted-step percentile differs")
    return [P95, Lower, Upper, *Summed]


def Audit(Scene, Processes, Raw, Blocks, Candidate):
    BlockCount, Target, TTwo, TOne = Fixed[Scene]
    Starts = ["baseline", "current"] * (BlockCount // 2)
    random.Random(20261127).shuffle(Starts)
    if len(Processes) != 4 * BlockCount or len(Raw) != len(Processes) or len(Blocks) != BlockCount:
        raise ValueError("Missing scheduled native processes or block receipts")
    Ratios, Deltas, LooseRange, SumRange = [], [], [], []
    Totals = {Name: dict(Exact=0, PollMissing=0, ResetMissing=0, EngineStepNs=0) for Name in ("baseline", "current")}
    for Index, First in enumerate(Starts):
        Sign = np.array([1, -1, -1, 1] if First == "current" else [-1, 1, 1, -1]) / 2
        Numbers = []
        for Slot in range(4):
            I = Index * 4 + Slot
            P, R = Processes[I], Raw[I]
            Name = "current" if Sign[Slot] > 0 else "baseline"
            if (P["Block"], P["Run"], P["Profile"], P["EngineSHA256"]) != (
                    str(Index+1), str(Slot+1), Name, Candidate if Name == "current" else Historical):
                raise ValueError("Scheduled order or executable SHA-256 differs")
            if int(P["Warmup"]) < 192 or P["PriorityClass"] != "32" or any(P[K] != R[K] for K in ("Scene", "Block", "Run", "Profile")):
                raise ValueError("Collector identity, warmup or scheduling differs")
            Numbers.append(Values(P, R, Target))
            for K in Totals[Name]:
                Totals[Name][K] += int(P[K])
        Numbers = np.asarray(Numbers)
        D = Sign @ (Numbers[:, 0] / 1000)
        Logs = np.log(Numbers)
        Ratio = Sign @ Logs[:, 0]
        Ratios.append(Ratio)
        Deltas.append(D)
        Receipt = Blocks[Index]
        if not (math.isclose(float(Receipt["P95DiffUs"]), D, abs_tol=1e-8) and
                math.isclose(float(Receipt["P95LogRatio"]), Ratio, abs_tol=1e-10)):
            raise ValueError("Archived independent-block estimate differs")
        for Offset, Store in ((1, LooseRange), (3, SumRange)):
            Lo = np.where(Sign > 0, Logs[:, Offset], Logs[:, Offset+1])
            Hi = np.where(Sign > 0, Logs[:, Offset+1], Logs[:, Offset])
            Store.append([Sign @ Lo, Sign @ Hi])
    def Interval(Values, Relative):
        V = np.asarray(Values)
        Mu, SE = V.mean(), V.std(ddof=1) / np.sqrt(BlockCount)
        Four = np.array([Mu, Mu - TTwo*SE, Mu + TTwo*SE, Mu + TOne*SE])
        return (np.expm1(Four)*100 if Relative else Four).tolist()
    def Bounds(Pairs):
        Values = np.array(Pairs)
        Low, High = Values.mean(axis=0)
        if not np.all(np.isfinite(Values)):
            return [float(np.expm1(Low)*100), None if math.isinf(High) else float(np.expm1(High)*100), None]
        Center = (High+Low)/2
        MaxSD = np.linalg.norm(np.max(np.abs(Values-Center), axis=1)) / np.sqrt(BlockCount-1)
        return (np.expm1([Low, High, High+TOne*MaxSD/np.sqrt(BlockCount)])*100).tolist()
    return dict(Blocks=BlockCount, P95Percent=Interval(Ratios, True), P95Microseconds=Interval(Deltas, False),
                MissingBounds=Bounds(LooseRange), GroupSumBounds=Bounds(SumRange), NativeTotals=Totals)


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--input", type=Path, required=True)
    Parser.add_argument("--candidate-sha256", required=True)
    Args = Parser.parse_args()
    with (Args.input/"Processes.csv").open(encoding="utf-8", newline="") as F:
        Processes = list(csv.DictReader(F))
    with gzip.open(Args.input/"Ticks.csv.gz", "rt", encoding="utf-8", newline="") as F:
        Raw = list(csv.DictReader(F))
    with (Args.input/"Blocks.csv").open(encoding="utf-8", newline="") as F:
        Blocks = list(csv.DictReader(F))
    if ({R["Scene"] for R in Processes} != set(Fixed) or {R["Scene"] for R in Raw} != set(Fixed)
            or {R["Scene"] for R in Blocks} != set(Fixed)):
        raise ValueError("Fixed scenes are incomplete")
    Report = {Scene: Audit(Scene, *([R for R in Table if R["Scene"] == Scene] for Table in (Processes, Raw, Blocks)),
                           Args.candidate_sha256) for Scene in Fixed}
    print(json.dumps(Report, indent=2, allow_nan=False))


if __name__ == "__main__":
    Main()
