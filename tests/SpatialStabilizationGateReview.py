#!/usr/bin/env python3
"""Independently reconstruct the prespecified whole-step gate from its immutable native receipts."""
import argparse
import bisect
import csv
import gzip
import hashlib
import json
import math
from pathlib import Path
import random
import statistics


Historical = "0f43758828929e453d054689e81524a662e75c9e26d8ec4d4a4089396795d4aa"
Scenes = {"ordinary": (64, 1024), "rigid": (16, 768)}


def Check(Condition, Description):
    if not Condition:
        raise ValueError(Description)


def ReadRows(Pathname):
    Open = gzip.open if Pathname.suffix == ".gz" else open
    with Open(Pathname, "rt", encoding="utf-8-sig", newline="") as File:
        return list(csv.DictReader(File))


def StudentQuantile(Degrees, Probability):
    # Substituting t=sqrt(df)*tan(angle) leaves the integral of cos(angle)^(df-1).
    # The two fixed cohorts have odd degrees of freedom, so the reduction ends at angle.
    Check(Degrees % 2 == 1, "The fixed study requires odd degrees of freedom")

    def Integral(Angle):
        Area = Angle
        for Power in range(2, Degrees, 2):
            Area = (math.sin(Angle) * math.cos(Angle) ** (Power - 1) + (Power - 1) * Area) / Power
        return Area

    Denominator = Integral(math.pi / 2)
    Low, High = 0.0, 8.0
    for _ in range(60):
        Mid = (Low + High) / 2
        Cumulative = .5 + .5 * Integral(math.atan(Mid / math.sqrt(Degrees))) / Denominator
        if Cumulative < Probability:
            Low = Mid
        else:
            High = Mid
    return (Low + High) / 2


def GroupedRank(Known, Groups, Unseen, Rank, Minimum):
    def Below(Threshold, Maximize):
        if Threshold < Minimum:
            return 0
        Total = bisect.bisect_right(Known, Threshold)
        if Maximize:
            # A single value can absorb the excess sum in one group. Unseen resets can all be short.
            return Total + Unseen + sum(Length - int(Amount > Length * Threshold) for _, Length, Amount in Groups)
        for _, Length, Amount in Groups:
            Long = min(Length, (Amount - Length * Minimum) // (Threshold + 1 - Minimum))
            Total += Length - Long
        return Total

    def Threshold(Maximize):
        Low = Minimum - 1
        High = max([Known[-1]] + [Amount for _, _, Amount in Groups])
        if Below(High, Maximize) < Rank:
            return math.inf
        while High - Low > 1:
            Mid = (Low + High) // 2
            if Below(Mid, Maximize) >= Rank:
                High = Mid
            else:
                Low = Mid
        return High

    return Threshold(True), Threshold(False)


def ProcessRanks(Process, Raw, MinimumSteps):
    def Groups(Key):
        return [tuple(map(int, Group.split(":"))) for Group in Raw[Key].split()]

    Known = sorted(map(int, Raw["StepNanoseconds"].split()))
    Poll = Groups("OmittedGroupsStartCountTotal")
    Reset = Groups("ResetGapsStartCount")
    Attempted = int(Process["Attempted"])
    Missing = int(Process["Missing"])
    Check(Known and Known[0] > 0, "Missing positive native observations")
    Check(Attempted == int(Process["EndSimulationTick"]) - int(Process["StartSimulationTick"]) ==
          len(Known) + Missing, "Native interval does not match exact and omitted counts")
    PollCount = sum(Count for _, Count, _ in Poll)
    ResetCount = sum(Count for _, Count in Reset)
    Check(all(Amount >= Count for _, Count, Amount in Poll), "A polling group lacks positive native durations")
    Check(PollCount == int(Process["PollMissing"]) and ResetCount == int(Process["ResetMissing"]) and
          PollCount + ResetCount == Missing and len(Poll) == int(Process["Gaps"]) and
          len(Reset) == int(Process["ResetGaps"]), "Missing window or polling observations")
    Check(len(Known) == int(Process["Exact"]) and
          len(Known) + PollCount == int(Process["RingTicks"]) >= MinimumSteps, "Incomplete native cohort")
    End = 0
    for Start, Count in sorted([(Start, Count) for Start, Count, *_ in Poll + Reset]):
        Check(End <= Start and Count > 0 and Start + Count <= Attempted, "Overlapping or invalid omitted group")
        End = Start + Count
    Check(sum(Known) + sum(Amount for _, _, Amount in Poll) == int(Process["EngineStepNs"]),
          "Whole-step times disagree with the native phase counter")
    for Percent in (50, 95, 99):
        Index = (len(Known) * Percent + 99) // 100 - 1
        Check(Known[Index] == int(Process[f"P{Percent}Ns"]), "Exact nearest-rank result disagrees")
    Rank = (Attempted * 95 + 99) // 100
    Loose = (Known[Rank - Missing - 1] if Rank > Missing else 0,
             Known[Rank - 1] if Rank <= len(Known) else math.inf)
    Summed = GroupedRank(Known, Poll, ResetCount, Rank, 1)
    Zero = GroupedRank(Known, Poll, ResetCount, Rank, 0)
    for Prefix, Values in (("", Loose), ("Summed", Summed), ("WithZero", Zero)):
        for Bound, Value in zip(("LowerP95Ns", "UpperP95Ns"), Values):
            if Prefix + Bound in Process:
                Check(Value == float(Process[Prefix + Bound]), "Saved missing-order bound disagrees")
    return {"Exact": (int(Process["P95Ns"]),) * 2, "AllShortOrLong": Loose,
            "ObservedGroupSums": Summed, "ZeroPermitted": Zero}


def Percent(LogRatio):
    return 100 * math.expm1(LogRatio)


def Bounds(Blocks, OneSided):
    Count = len(Blocks)
    Low = statistics.mean(Row[0] for Row in Blocks)
    High = statistics.mean(Row[1] for Row in Blocks)
    if not all(math.isfinite(Value) for Pair in Blocks for Value in Pair):
        return {"MeanLow": Percent(Low), "MeanHigh": None, "Upper95": None}
    Center = (Low + High) / 2
    # Variance around the actual mean can only be smaller than variance around this fixed center.
    Radius = math.sqrt(sum(max(abs(A-Center), abs(B-Center)) ** 2 for A, B in Blocks) / (Count-1))
    return {"MeanLow": Percent(Low), "MeanHigh": Percent(High),
            "Upper95": Percent(High + OneSided * Radius / math.sqrt(Count))}


def SceneReview(Name, Processes, Raw, Candidate):
    Count, Target = Scenes[Name]
    Starts = ["baseline", "current"] * (Count // 2)
    random.Random(20261127).shuffle(Starts)
    Check(len(Processes) == len(Raw) == 4 * Count, f"Incomplete {Name} process schedule")
    Totals = {Arm: dict(Exact=0, PollMissing=0, ResetMissing=0, EngineStepNs=0) for Arm in ("baseline", "current")}
    Intervals = {Key: [] for Key in ("Exact", "AllShortOrLong", "ObservedGroupSums", "ZeroPermitted")}
    for Block, First in enumerate(Starts):
        Other = "baseline" if First == "current" else "current"
        Ranks = {"baseline": [], "current": []}
        for Slot, Arm in enumerate((First, Other, Other, First)):
            Index = 4 * Block + Slot
            P, R = Processes[Index], Raw[Index]
            for Key, Expected in zip(("Scene", "Block", "Run", "Profile"), (Name, str(Block + 1), str(Slot + 1), Arm)):
                Check(P[Key] == R[Key] == Expected, "Scheduled order or native identity disagrees")
            Check(P["EngineSHA256"].lower() == (Candidate if Arm == "current" else Historical)
                  and int(P["PriorityClass"]) == 32 and int(P["Warmup"]) >= 192, "Product, priority or warmup mismatch")
            Ranks[Arm].append(ProcessRanks(P, R, Target))
            for Key in Totals[Arm]:
                Totals[Arm][Key] += int(P[Key])

        for Key, Values in Intervals.items():
            def MeanLog(Arm, Side):
                Durations = [Pair[Key][Side] for Pair in Ranks[Arm]]
                return statistics.mean(math.log(V) if V > 0 else -math.inf for V in Durations)
            Values.append((MeanLog("current", 0) - MeanLog("baseline", 1),
                           MeanLog("current", 1) - MeanLog("baseline", 0)))
    Quantile = StudentQuantile(Count-1, .95)
    Results = {Key: Bounds(Values, Quantile) for Key, Values in Intervals.items()}
    Exact = [A for A, _ in Intervals["Exact"]]
    Average = statistics.mean(Exact)
    Half = StudentQuantile(Count-1, .975) * statistics.stdev(Exact) / math.sqrt(Count)
    Gate = [Results[Key]["Upper95"] for Key in ("Exact", "AllShortOrLong")]
    return {"Blocks": Count, "Processes": 4 * Count, "Totals": Totals, "TwoSided95": [Percent(Average-Half), Percent(Average+Half)],
            "Bounds": Results, "Pass": all(Value is not None and Value <= 2 for Value in Gate)}


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("directory", type=Path)
    Parser.add_argument("candidate_sha256")
    Args = Parser.parse_args()
    Files = [Args.directory / Name for Name in ("Processes.csv", "Ticks.csv.gz")]
    AllProcesses, AllTicks = map(ReadRows, Files)
    Check(len(AllProcesses) == len(AllTicks) == 320, "Both fixed scenes must be complete")
    Order = [Name for Name, (Blocks, _) in Scenes.items() for _ in range(4 * Blocks)]
    Check([Row["Scene"] for Row in AllProcesses] == [Row["Scene"] for Row in AllTicks] == Order,
          "Formal capture must complete the primary scene before the secondary scene")
    Results = {Name: SceneReview(Name, [P for P in AllProcesses if P["Scene"] == Name],
                                [T for T in AllTicks if T["Scene"] == Name], Args.candidate_sha256.lower()) for Name in Scenes}
    Results["Inputs"] = {File.name: hashlib.sha256(File.read_bytes()).hexdigest() for File in Files}
    Results["HistoricalEngine"] = Historical
    Results["CandidateEngine"] = Args.candidate_sha256.lower()
    print(json.dumps(Results, indent=2, allow_nan=False))
