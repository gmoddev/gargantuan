# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Bound missing durations from the externally interrupted anchored process."""
import argparse
import csv
import gzip
import json
import math
from pathlib import Path

from SpatialConstructionPrecisionAnalysis import Interval, MissingInterval, Percent


def MissingRanks(Ordered, ExistingMissing, Additional, Replace):
    # Place every unavailable value below or above the known sample. A replacement
    # may remove the shortest or longest known observations, whichever worsens the bound.
    OriginalCount = len(Ordered) + ExistingMissing
    Rank = (95 * (OriginalCount if Replace else OriginalCount + Additional) + 99) // 100
    LowIndex = Rank - ExistingMissing - Additional
    HighIndex = Rank + Additional if Replace else Rank
    Low = Ordered[LowIndex - 1] if LowIndex > 0 else 0
    High = Ordered[HighIndex - 1] if HighIndex <= len(Ordered) else math.inf
    return Low, High


def Limits(Row, Blocks, Lower, Upper):
    # The affected current process has one-half weight in its four-process block.
    Lo = [float(Block["LowerP95LogRatio"]) for Block in Blocks]
    Hi = [float(Block["UpperP95LogRatio"]) for Block in Blocks]
    Index = next(I for I, Block in enumerate(Blocks) if Block["Block"] == Row["Block"])
    Lo[Index] += math.log(Lower / float(Row["LowerP95Ns"])) / 2 if Lower else -math.inf
    Hi[Index] += math.log(Upper / float(Row["UpperP95Ns"])) / 2
    Means, OneSidedUpper = MissingInterval(Lo, Hi, len(Blocks) - 1)
    return dict(PositionP95Nanoseconds=[Lower, Upper if math.isfinite(Upper) else None],
                AdverseMeanPercentRange=Means, AdverseOneSided95UpperPercent=OneSidedUpper)


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--input", type=Path, required=True)
    Args = Parser.parse_args()
    with (Args.input / "Processes.csv").open(encoding="utf-8", newline="") as Stream:
        Row = next(R for R in csv.DictReader(Stream) if
                   (R["Scene"], R["Block"], R["Run"]) == ("ordinary", "35", "4"))
    if Row["Profile"] != "current" or Row["EngineSHA256"] != (
        "1378d39488e60a5c2120d94ab9e2b753889d5aba4b62f68f26763e7d34028adc"
    ):
        raise ValueError("The affected position must be the repaired executable")
    with gzip.open(Args.input / "Ticks.csv.gz", "rt", encoding="utf-8", newline="") as Stream:
        Raw = next(R for R in csv.DictReader(Stream) if
                   all(R[K] == Row[K] for K in ("Scene", "Block", "Run", "Profile")))
    with (Args.input / "Blocks.csv").open(encoding="utf-8", newline="") as Stream:
        Blocks = [R for R in csv.DictReader(Stream) if R["Scene"] == "ordinary"]
    if len(Blocks) != 64:
        raise ValueError("The recovered schedule must have all 64 anchored blocks")
    Ordered = sorted(map(int, Raw["StepNanoseconds"].split()))
    Missing = int(Row["Missing"])
    if len(Ordered) + Missing != int(Row["Attempted"]) or MissingRanks(Ordered, Missing, 0, False) != (
        int(Row["LowerP95Ns"]), int(Row["UpperP95Ns"])
    ):
        raise ValueError("The recovered position failed its original adverse-rank check")
    Result = dict(Block=35, Run=4, ObservedRetryTicks=len(Ordered), ExistingMissing=Missing)
    for Cap in (0, 21, 48):
        for Replace, Label in ((False, "Additional"), (True, "Substituted")):
            Values = [MissingRanks(Ordered, Missing, K, Replace) for K in range(Cap + 1)]
            Lower = min(Lo for Lo, _ in Values)
            Upper = max(Hi for _, Hi in Values)
            Result[f"{Label}ZeroThrough{Cap}"] = Limits(Row, Blocks, Lower, Upper)
    Others = [B for B in Blocks if B["Block"] != "35"]
    Exact = Interval([float(B["P95LogRatio"]) for B in Others])
    Means, Upper = MissingInterval([float(B["LowerP95LogRatio"]) for B in Others],
                                   [float(B["UpperP95LogRatio"]) for B in Others], len(Others) - 1)
    Result["OmitAffectedBlockDiagnostic"] = dict(
        Blocks=len(Others), ExactChangePercent=Percent(Exact["Mean"]), ExactUpperPercent=Percent(Exact["UpperOne"]),
        AdverseMeanPercentRange=Means, AdverseOneSided95UpperPercent=Upper)
    print(json.dumps(Result, indent=2, allow_nan=False))


if __name__ == "__main__":
    Main()
