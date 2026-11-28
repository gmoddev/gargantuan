#!/usr/bin/env python3
"""Verify six native moving Vulkan/GNS workloads and summarize their distinct measurement scopes."""
import argparse
import csv
import json
import math
from pathlib import Path


def Rows(File):
    return [json.loads(Line) for Line in File.read_text(encoding="utf-8-sig").splitlines() if Line.startswith("{")]


def Summarize(Root):
    Results = []
    for Count in (128, 256, 512, 1024, 2048, 4096):
        Frames = Rows(Root / f"WindowsVulkan{Count}.jsonl")
        if ([(R["Stage"], len(R["Frames"])) for R in Frames] !=
                [("Empty", 1), ("Cold", 1), ("Warm", 12), ("Moving", 30), ("Edit", 1), ("Retired", 1)] or
                any(R["MovingCells"] != Count for R in Frames) or
                Frames[0]["CoveredPixels"] or Frames[-1]["CoveredPixels"] or
                any(not R["CoveredPixels"] for R in Frames[1:-1]) or
                Frames[1]["CoverageHash"] != Frames[2]["CoverageHash"] or
                Frames[-1]["CoverageHash"] != Frames[0]["CoverageHash"]):
            raise ValueError(f"Inconsistent Vulkan lifetime at {Count}")
        Motion = Frames[3]
        Row = dict(Cells=Count, GpuMovingFrames=len(Motion["Frames"]), MovingScreenPixels=Motion["CoveredPixels"],
                   Draws30=Motion["DrawCalls"], MeshVertexBufferCreates30=Motion["VertexBufferCreates"],
                   UploadedBytes30=Motion["UploadedBytes"])
        for Phase in ("PublishUs", "SubmissionUs", "FenceReadbackUs", "ExtractUs", "CaptureUs"):
            Values = sorted(F[Phase] for F in Motion["Frames"])
            for Rank in (50, 95, 99, 100):
                Row[f"{Phase[:-2]}P{Rank}Us"] = Values[math.ceil(Rank * len(Values) / 100) - 1]
        Network = Rows(Root / f"WindowsGns{Count}.jsonl")
        if ([R["Phase"] for R in Network] != ["Join", "Motion", "Delete", "Retire"] or
                any(R["Representation"] != "Moving" or R["Occupied"] != Count for R in Network) or
                Network[1]["DistinctPosesIn40Ticks"] < 3 or Network[1]["PumpCalls"] != 48):
            raise ValueError(f"Inconsistent GNS lifetime at {Count}")
        Row["ObservedPoses40"] = Network[1]["DistinctPosesIn40Ticks"]
        for R in Network:
            for Measure in ("PumpCalls", "ServerPayloadBytes", "ClientPayloadBytes", "ServerMessages", "ClientMessages",
                            "ServerNativePackets", "ClientNativePackets", "ServerNativePacketBytes", "ClientNativePacketBytes"):
                Row[R["Phase"] + Measure] = R[Measure]
        Results.append(Row)
    return Results


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("directory", type=Path)
    Parser.add_argument("output", type=Path)
    Args = Parser.parse_args()
    Data = Summarize(Args.directory)
    with Args.output.open("x", newline="", encoding="utf-8") as File:
        Writer = csv.DictWriter(File, fieldnames=Data[0])
        Writer.writeheader()
        Writer.writerows(Data)
    print("[Spatial:CompoundPresentation] Six Vulkan and six GNS lifetimes verified")
