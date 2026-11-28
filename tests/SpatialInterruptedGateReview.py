#!/usr/bin/env python3
"""Bound the administrative replacement of repaired anchored block 35, run 4."""
import argparse
import json
import math
from pathlib import Path
import statistics

from SpatialStabilizationGateReview import Bounds, Check, Percent, ReadRows, StudentQuantile


def UpperFor62():
    # The leave-one-block-out sensitivity has 63 blocks. For df=62 the
    # trigonometric Student integral starts with integral(cos(x)) = sin(x).
    def Integral(Angle):
        Area = math.sin(Angle)
        for Power in range(3, 62, 2):
            Area = (math.sin(Angle) * math.cos(Angle) ** (Power-1) + (Power-1) * Area) / Power
        return Area
    Half = Integral(math.pi / 2)
    Low, High = 0.0, 5.0
    for _ in range(60):
        Mid = (Low + High) / 2
        if Integral(math.atan(Mid / math.sqrt(62))) / Half < .9:
            Low = Mid
        else:
            High = Mid
    return (Low + High) / 2


def ProcessRange(Row, TickRow, Mode, Possible):
    Known = sorted(map(int, TickRow["StepNanoseconds"].split()))
    Missing, Total = int(Row["Missing"]), int(Row["Attempted"])
    Check(len(Known) + Missing == Total, "Mismatched native observation count")
    Rank = (95 * (Total + (Possible if Mode == "Add" else 0)) + 99) // 100
    LowRank = Rank - Missing - Possible
    HighRank = Rank + (Possible if Mode == "Replace" else 0)
    return (math.log(Known[LowRank-1]) if LowRank >= 1 else -math.inf,
            math.log(Known[HighRank-1]) if HighRank <= len(Known) else math.inf)


def Review(Directory):
    Processes = [R for R in ReadRows(Directory / "Processes.csv") if R["Scene"] == "ordinary"]
    Ticks = [R for R in ReadRows(Directory / "Ticks.csv.gz") if R["Scene"] == "ordinary"]
    Check(len(Processes) == len(Ticks) == 256, "Only a completed primary cohort may be assessed")
    Check((Processes[139]["Block"], Processes[139]["Run"], Processes[139]["Profile"]) == ("35", "4", "current"),
          "The interrupted candidate slot differs")
    Blocks = []
    for Index in range(0, 256, 4):
        Values = []
        for Offset in range(4):
            Position = Index + Offset
            Row, Raw = Processes[Position], Ticks[Position]
            Check(all(Row[K] == Raw[K] for K in ("Scene", "Block", "Run", "Profile")),
                  "Per-process and native rows name different observations")
            Values.append((Row["Profile"], *ProcessRange(Row, Raw, "Add", 0)))
        Blocks.append(Values)

    def BlockInterval(Values):
        Candidate = [R for R in Values if R[0] == "current"]
        Historical = [R for R in Values if R[0] == "baseline"]
        Check(len(Candidate) == len(Historical) == 2, "Unbalanced four-process block")
        return (statistics.mean(R[1] for R in Candidate) - statistics.mean(R[2] for R in Historical),
                statistics.mean(R[2] for R in Candidate) - statistics.mean(R[1] for R in Historical))

    Original = [BlockInterval(Block) for Block in Blocks]
    Quantile = StudentQuantile(63, .95)
    Results = {"NoExtraUnknown": Bounds(Original, Quantile)}
    # The original time brackets and the refined NTFS close / outer rounded
    # timestamps are retained as separate sensitivities.
    for MaxLost in (20, 21, 47, 48):
        for Mode in ("Add", "Replace"):
            Cases = []
            for Lost in range(MaxLost + 1):
                Values = list(Blocks[34])
                Values[3] = ("current", *ProcessRange(Processes[139], Ticks[139], Mode, Lost))
                Sensitivity = list(Original)
                Sensitivity[34] = BlockInterval(Values)
                Cases.append(Bounds(Sensitivity, Quantile))
            Uppers = [R["Upper95"] for R in Cases]
            Results[f"{Mode}0Through{MaxLost}"] = {
                "SmallestMeanPercent": min(R["MeanLow"] for R in Cases),
                "LargestMeanPercent": None if any(R["MeanHigh"] is None for R in Cases) else max(R["MeanHigh"] for R in Cases),
                "LargestUpper95Percent": None if None in Uppers else max(Uppers),
            }
    Remaining = Original[:34] + Original[35:]
    Exact = []
    for Block in range(64):
        if Block == 34:
            continue
        Values = [(R["Profile"], math.log(int(R["P95Ns"])), math.log(int(R["P95Ns"])))
                  for R in Processes[4*Block:4*Block+4]]
        Exact.append(BlockInterval(Values)[0])
    Results["OmitBlock35"] = {"Blocks": 63, "EstimatePercent": Percent(statistics.mean(Exact)),
                               "Bounds": Bounds(Remaining, UpperFor62())}
    return Results


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("directory", type=Path)
    Args = Parser.parse_args()
    print(json.dumps(Review(Args.directory), indent=2, allow_nan=False))
