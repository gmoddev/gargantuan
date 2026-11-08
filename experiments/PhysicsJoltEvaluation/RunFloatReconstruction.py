"""Hosted float-only free-cloth velocity reconstruction ablation.

The scalar replay preserves raw trajectories and treats energy-bound failures
as observations, not process errors. No native Jolt behavior is changed.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import time

from RunLongFreeReplay import HashFile, MaximumFinite, Measured, PrepareGraph


Modes = ("difference", "delta", "applieddelta")
CreationFlags = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def RunCase(Executable, Graph, Output, Name, Mode, Integrations, Frames, Timeout):
    Options = [str(Executable), "--graph", str(Graph), "--precision", "float",
               "--rest", "captured", "--steps", "1", "--frames", str(Frames),
               "--integrations-per-frame", str(Integrations), "--seed-index", "84",
               "--seed-z", "1", "--particles", "0", "--stage-limit", "0",
               "--projection-iterations", "1", "--tolerance", "0",
               "--velocity-reconstruction", Mode]
    Started = time.monotonic()
    TimedOut = False
    try:
        Process = subprocess.run(Options, capture_output=True, timeout=Timeout,
                                 check=False, creationflags=CreationFlags)
        Stdout, Stderr, ExitCode = Process.stdout, Process.stderr, Process.returncode
    except subprocess.TimeoutExpired as Error:
        TimedOut = True
        Stdout, Stderr, ExitCode = Error.stdout or b"", Error.stderr or b"", None
    Raw = Output / f"{Name}.jsonl"
    ErrorPath = Output / f"{Name}.stderr.txt"
    Raw.write_bytes(Stdout)
    ErrorPath.write_bytes(Stderr)
    Rows = []
    ParseError = None
    try:
        Rows = [json.loads(Line) for Line in Stdout.splitlines() if Line.strip()]
    except (UnicodeDecodeError, json.JSONDecodeError) as Error:
        ParseError = str(Error)
    Config = next((Row for Row in Rows if Row.get("Type") == "Config"), None)
    Summary = next((Row for Row in Rows if Row.get("Type") == "Summary"), None)
    States = [Row for Row in Rows if Row.get("Type") == "Frame"]
    Chronology = [Row.get("Frame") for Row in States] == list(range(len(States)))
    InitialK = Measured(Config.get("InitialVelocityKineticEnergy")) if Config else float("nan")
    Ceiling = 1.05 * 0.05 + 1.0e-5
    FirstEnergyFailure = next(({"Frame": Row["Frame"],
                                "KPlusUJ": Row.get("KineticPlusElasticEstimate")}
                               for Row in States if not math.isfinite(Measured(Row.get("KineticPlusElasticEstimate")))
                               or Measured(Row.get("KineticPlusElasticEstimate")) > Ceiling), None)
    FirstGeometryFailure = next(({"Frame": Row["Frame"],
                                  "Strain": Row.get("MaxEdgeStrain"),
                                  "PinDriftM": Row.get("MaxPinDrift")}
                                 for Row in States if Row.get("Finite") is not True or
                                 not math.isfinite(Measured(Row.get("MaxEdgeStrain"))) or
                                 not math.isfinite(Measured(Row.get("MaxPinDrift"))) or
                                 Measured(Row.get("MaxEdgeStrain")) > 0.1 or
                                 Measured(Row.get("MaxPinDrift")) > 1.0e-4), None)
    Executed = (ExitCode == 0 and not TimedOut and ParseError is None and
                Config is not None and Summary is not None and Chronology and
                Config.get("VelocityReconstruction") == Mode and
                Config.get("IntegrationsPerFrame") == Integrations and
                Config.get("ProjectionIterations") == 1 and
                Config.get("Frames") == Frames and
                math.isclose(InitialK, 0.05, rel_tol=1.0e-6))
    Receipt = {"Name": Name, "Mode": Mode, "IntegrationsPerFrame": Integrations,
               "Arguments": Options, "ExitCode": ExitCode, "TimedOut": TimedOut,
               "ElapsedSeconds": time.monotonic() - Started,
               "Raw": Raw.name, "RawSha256": hashlib.sha256(Stdout).hexdigest(),
               "Stderr": ErrorPath.name, "StderrSha256": hashlib.sha256(Stderr).hexdigest(),
               "ParseError": ParseError, "ProcessExecuted": Executed,
               "FramesRequested": Frames, "FramesObserved": len(States),
               "InitialKineticEnergyJ": InitialK, "ReferenceModelCeilingJ": Ceiling,
               "MaximumKineticEnergyJ": MaximumFinite(States, "KineticEnergy"),
               "MaximumKPlusUJ": MaximumFinite(States, "KineticPlusElasticEstimate"),
               "MaximumStrain": MaximumFinite(States, "MaxEdgeStrain"),
               "MaximumPinDriftM": MaximumFinite(States, "MaxPinDrift"),
               "MaximumVelocityReconstructionDifferenceRmsMps":
                   MaximumFinite(States, "VelocityReconstructionDifferenceRmsMps"),
               "MaximumVelocityReconstructionDifferenceMps":
                   MaximumFinite(States, "VelocityReconstructionDifferenceMaxMps"),
               "FirstReferenceModelFailure": FirstEnergyFailure,
               "FirstGeometryFailure": FirstGeometryFailure,
               "FirstNonfiniteFrame": Summary.get("FirstNonfiniteFrame") if Summary else None,
               "BoundedModelPass": Executed and len(States) == Frames and
                   FirstEnergyFailure is None and FirstGeometryFailure is None,
               "Scope": "Contact-free scalar float replay; frame-sampled K+U and geometry. This ablates velocity reconstruction, not native Jolt physics."}
    (Output / f"{Name}.receipt.json").write_text(
        json.dumps(Receipt, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return Receipt


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--exe", dest="Exe", type=Path, required=True)
    Parser.add_argument("--graph", dest="Graph", type=Path, required=True)
    Parser.add_argument("--output", dest="Output", type=Path, required=True)
    Parser.add_argument("--frames", dest="Frames", type=int, default=1800)
    Parser.add_argument("--timeout", dest="Timeout", type=int, default=1800)
    Parser.add_argument("--quantized-controls-only", dest="QuantizedControlsOnly",
                        action="store_true", help="Run only c1e-6 N192/N384 quantized-base controls")
    Args = Parser.parse_args()
    if not 1 <= Args.Frames <= 3600 or not 1 <= Args.Timeout <= 3600:
        Parser.error("[Physics:FloatReconstruction] Frames or timeout outside bounded range")
    Executable = Args.Exe.resolve(strict=True)
    GraphPath = Args.Graph.resolve(strict=True)
    Output = Args.Output
    Output.mkdir(parents=True, exist_ok=False)
    Graph = json.loads(GraphPath.read_text(encoding="utf-8"))
    if len(Graph.get("Positions", [])) != 169 or len(Graph.get("Edges", [])) != 886:
        Parser.error("[Physics:FloatReconstruction] Expected exported 169-vertex, 886-edge cloth graph")
    Prepared = {}
    Materials = ((1.0e-6, "M1e-6"),) if Args.QuantizedControlsOnly else (
        (1.0e-6, "M1e-6"), (1.0e-5, "M1e-5"))
    for Material, Label in Materials:
        PathName = Output / f"{Label}.graph.json"
        Prepared[Label] = {"Path": PathName, **PrepareGraph(Graph, Material, PathName)}
    if Args.QuantizedControlsOnly:
        Cases = [(f"M1e-6-N{Integrations}-quantizedbase", "M1e-6", Integrations,
                  "quantizedbase") for Integrations in (192, 384)]
    else:
        Cases = [(f"M1e-6-N{Integrations}-{Mode}", "M1e-6", Integrations, Mode)
                 for Integrations in (192, 384) for Mode in Modes]
        Cases += [(f"M1e-5-N96-{Mode}", "M1e-5", 96, Mode) for Mode in Modes]
    Receipts = []
    for Name, Material, Integrations, Mode in Cases:
        Receipt = RunCase(Executable, Prepared[Material]["Path"], Output, Name,
                          Mode, Integrations, Args.Frames, Args.Timeout)
        Receipts.append(Receipt)
        (Output / "Receipts.json").write_text(json.dumps({
            "SourceGraphSha256": HashFile(GraphPath),
            "ReplayBinarySha256": HashFile(Executable),
            "PreparedGraphs": {Key: {Field: Value for Field, Value in Meta.items()
                                     if Field != "Path"} for Key, Meta in Prepared.items()},
            "Receipts": Receipts}, indent=2, allow_nan=False), encoding="utf-8")
        print(json.dumps({"Name": Name, "ProcessExecuted": Receipt["ProcessExecuted"],
                          "BoundedModelPass": Receipt["BoundedModelPass"],
                          "MaximumKPlusUJ": Receipt["MaximumKPlusUJ"],
                          "FirstReferenceModelFailure": Receipt["FirstReferenceModelFailure"]}), flush=True)
    return 0 if all(Receipt["ProcessExecuted"] for Receipt in Receipts) else 2


if __name__ == "__main__":
    raise SystemExit(Main())
