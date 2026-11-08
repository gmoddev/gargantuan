#!/usr/bin/env python3
"""Run bounded, contact-free retained-lambda replay controls on one exported graph.

The runner preserves all per-frame JSONL, stderr, and process receipts. A physical
energy or residual failure is data, while a process failure is recorded separately.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import time


def Float32(Value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", Value))[0]


def PrepareCompliantGraph(Graph: dict, Output: Path) -> dict:
    Graph = json.loads(json.dumps(Graph))
    HardEdges = 0
    BendEdges = 0
    StretchCompliance = Float32(1.0e-5)
    for Edge in Graph["Edges"]:
        Compliance = float(Edge["Compliance"])
        if Compliance == 0.0:
            Edge["Compliance"] = StretchCompliance
            HardEdges += 1
        else:
            # Preserve the captured float32 bend compliance byte value.
            BendEdges += 1
    if HardEdges == 0 or BendEdges == 0:
        raise ValueError("[Physics:ConvergedReplay] Expected mixed hard/stretch and compliant bend edges")
    Output.write_text(json.dumps(Graph, separators=(",", ":")), encoding="utf-8")
    return {"ConvertedStretchEdges": HardEdges, "PreservedBendEdges": BendEdges,
            "StretchComplianceFloat32": StretchCompliance}


def RunCase(Executable: Path, Graph: Path, Output: Path, Name: str,
            Precision: str, Rest: str, Steps: int, Iterations: int,
            Frames: int, Timeout: int) -> dict:
    Arguments = [str(Executable), "--graph", str(Graph), "--precision", Precision,
                 "--rest", Rest, "--steps", str(Steps), "--frames", str(Frames),
                 "--seed-index", "84", "--seed-z", "1", "--particles", "0",
                 "--stage-limit", "0", "--projection-iterations", str(Iterations),
                 "--tolerance", "0"]
    Started = time.monotonic()
    TimedOut = False
    try:
        Result = subprocess.run(Arguments, capture_output=True, timeout=Timeout,
                                check=False)
        Stdout = Result.stdout
        Stderr = Result.stderr
        ExitCode = Result.returncode
    except subprocess.TimeoutExpired as Error:
        TimedOut = True
        Stdout = Error.stdout or b""
        Stderr = Error.stderr or b""
        ExitCode = None
    Elapsed = time.monotonic() - Started
    (Output / f"{Name}.jsonl").write_bytes(Stdout)
    (Output / f"{Name}.stderr.txt").write_bytes(Stderr)
    Rows = []
    ParseError = None
    for Line in Stdout.splitlines():
        try:
            Rows.append(json.loads(Line))
        except (UnicodeDecodeError, json.JSONDecodeError) as Error:
            ParseError = str(Error)
            break
    FramesRows = [Row for Row in Rows if Row.get("Type") == "Frame"]
    Summary = next((Row for Row in Rows if Row.get("Type") == "Summary"), None)
    Initial = next((Row for Row in Rows if Row.get("Type") == "Config"), None)
    Finite = all(Row.get("Finite") is True for Row in FramesRows)
    MaximumKinetic = max((Row["KineticEnergy"] for Row in FramesRows), default=None)
    MaximumMechanicalEstimate = max((Row["KineticPlusElasticEstimate"] for Row in FramesRows), default=None)
    MaximumStationarityResidual = max((Row["MaxNormalizedEquilibriumResidual"] for Row in FramesRows), default=None)
    Receipt = {"Name": Name, "Arguments": Arguments, "ExitCode": ExitCode,
               "TimedOut": TimedOut, "TimeoutSeconds": Timeout,
               "ElapsedSeconds": Elapsed, "StdoutSha256": hashlib.sha256(Stdout).hexdigest(),
               "StderrSha256": hashlib.sha256(Stderr).hexdigest(),
               "ParseError": ParseError, "FrameCount": len(FramesRows),
               "Finite": Finite, "Initial": Initial, "Summary": Summary,
               "MaximumKineticEnergy": MaximumKinetic,
               "MaximumKineticPlusElasticEstimate": MaximumMechanicalEstimate,
               "MaximumNormalizedEquilibriumResidual": MaximumStationarityResidual,
               "LastFrame": FramesRows[-1] if FramesRows else None,
               "ProcessComplete": ExitCode == 0 and not TimedOut and ParseError is None
                                  and len(FramesRows) == Frames and Summary is not None}
    (Output / f"{Name}.receipt.json").write_text(
        json.dumps(Receipt, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return Receipt


def Main() -> int:
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--exe", dest="Exe", required=True, type=Path)
    Parser.add_argument("--graph", dest="Graph", required=True, type=Path)
    Parser.add_argument("--output", dest="Output", required=True, type=Path)
    Parser.add_argument("--frames", dest="Frames", type=int, default=150)
    Parser.add_argument("--timeout", dest="Timeout", type=int, default=600)
    Args = Parser.parse_args()
    if not 1 <= Args.Frames <= 150 or not 1 <= Args.Timeout <= 600:
        Parser.error("[Physics:ConvergedReplay] Frames and timeout outside bounded range")
    if not Args.Exe.is_file() or not Args.Graph.is_file():
        Parser.error("[Physics:ConvergedReplay] Executable or graph is missing")
    Args.Output.mkdir(parents=True, exist_ok=True)
    Graph = json.loads(Args.Graph.read_text(encoding="utf-8"))
    if len(Graph.get("Positions", [])) != 169 or len(Graph.get("Edges", [])) != 886:
        Parser.error("[Physics:ConvergedReplay] Expected exported 169-vertex, 886-edge cloth graph")
    HardGraph = Args.Output / "HardGraph.json"
    HardGraph.write_text(json.dumps(Graph, separators=(",", ":")), encoding="utf-8")
    CompliantGraph = Args.Output / "CompliantGraph.json"
    Conversion = PrepareCompliantGraph(Graph, CompliantGraph)
    Cases = [("HardFloatCapturedS4I1", HardGraph, "float", "captured", 4, 1)]
    Cases += [(f"HardDoubleRecomputedS4I{Iterations}", HardGraph, "double", "recomputed", 4, Iterations)
              for Iterations in (1, 2, 4, 8, 16, 32, 64, 128)]
    Cases += [(f"CompliantFloatCapturedS1I{Iterations}", CompliantGraph, "float", "captured", 1, Iterations)
              for Iterations in (1, 2, 4, 8, 16, 32)]
    Receipts = []
    for Name, CaseGraph, Precision, Rest, Steps, Iterations in Cases:
        Receipt = RunCase(Args.Exe, CaseGraph, Args.Output, Name, Precision,
                          Rest, Steps, Iterations, Args.Frames, Args.Timeout)
        Receipts.append(Receipt)
        print(json.dumps({"Name": Name, "ProcessComplete": Receipt["ProcessComplete"],
                          "FrameCount": Receipt["FrameCount"],
                          "MaximumKineticEnergy": Receipt["MaximumKineticEnergy"],
                          "MaximumKineticPlusElasticEstimate": Receipt["MaximumKineticPlusElasticEstimate"],
                          "MaximumNormalizedEquilibriumResidual": Receipt["MaximumNormalizedEquilibriumResidual"]}), flush=True)
    Manifest = {"GraphSha256": hashlib.sha256(Args.Graph.read_bytes()).hexdigest(),
                "CompliantGraphConversion": Conversion, "FramesRequested": Args.Frames,
                "TimeoutSecondsPerCase": Args.Timeout, "Receipts": Receipts}
    (Args.Output / "Receipts.json").write_text(
        json.dumps(Manifest, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return 0 if all(Item["ProcessComplete"] for Item in Receipts) else 2


if __name__ == "__main__":
    raise SystemExit(Main())
