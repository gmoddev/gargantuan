"""Hosted long-horizon, contact-free ordered-edge replay at explicit cadences.

Every physical failure remains data. A process/parse failure is a harness error.
The replay is a scalar model of the exported graph, not a native solver run.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import time


def Float32(Value):
    return struct.unpack("<f", struct.pack("<f", Value))[0]


def HashFile(PathName):
    Digest = hashlib.sha256()
    with PathName.open("rb") as Source:
        for Chunk in iter(lambda: Source.read(1024 * 1024), b""):
            Digest.update(Chunk)
    return Digest.hexdigest()


def Measured(Value):
    return float(Value) if Value is not None else float("nan")


def MaximumFinite(States, Key):
    return max((Measured(Row.get(Key)) for Row in States
                if math.isfinite(Measured(Row.get(Key)))), default=None)


def PrepareGraph(Graph, Compliance, PathName):
    Converted = json.loads(json.dumps(Graph))
    Stretch = Bend = 0
    for Edge in Converted["Edges"]:
        if float(Edge["Compliance"]) == 0.0:
            Edge["Compliance"] = Float32(Compliance)
            Stretch += 1
        else:
            Bend += 1
    if Stretch == 0 or Bend == 0:
        raise ValueError("[Physics:LongFreeReplay] Expected mixed captured hard/stretch and compliant bend edges")
    PathName.write_text(json.dumps(Converted, separators=(",", ":")), encoding="utf-8")
    return {"StretchEdgesConverted": Stretch, "BendEdgesPreserved": Bend,
            "StretchComplianceFloat32": Float32(Compliance), "GraphSha256": HashFile(PathName)}


def RunCase(Executable, Graph, Output, Name, Precision, Integrations, Iterations, Frames, Timeout):
    Options = [str(Executable), "--graph", str(Graph), "--precision", Precision,
               "--rest", "captured", "--steps", "1", "--frames", str(Frames),
               "--integrations-per-frame", str(Integrations), "--seed-index", "84",
               "--seed-z", "1", "--particles", "0", "--stage-limit", "0",
               "--projection-iterations", str(Iterations), "--tolerance", "0"]
    Started = time.monotonic()
    TimedOut = False
    try:
        Process = subprocess.run(Options, capture_output=True, timeout=Timeout, check=False)
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
    Initial = float(Config.get("InitialVelocityKineticEnergy", float("nan"))) if Config else float("nan")
    Ceiling = 1.05 * 0.05 + 1.0e-5
    FirstEnergyFailure = next(({"Frame": Row["Frame"], "KPlusUJ": Row.get("KineticPlusElasticEstimate")}
                               for Row in States if not math.isfinite(Measured(Row.get("KineticPlusElasticEstimate")))
                               or Measured(Row.get("KineticPlusElasticEstimate")) > Ceiling), None)
    FirstGeometryFailure = next(({"Frame": Row["Frame"], "Strain": Row["MaxEdgeStrain"],
                                  "PinDrift": Row["MaxPinDrift"]}
                                 for Row in States if not Row.get("Finite") or
                                 not math.isfinite(Measured(Row.get("MaxEdgeStrain"))) or
                                 not math.isfinite(Measured(Row.get("MaxPinDrift"))) or
                                 Measured(Row.get("MaxEdgeStrain")) > 0.1 or
                                 Measured(Row.get("MaxPinDrift")) > 1.0e-4), None)
    ProcessExecuted = (ExitCode == 0 and not TimedOut and ParseError is None and
                       Config is not None and Summary is not None and Chronology and
                       Config.get("IntegrationsPerFrame") == Integrations and
                       Config.get("ProjectionIterations") == Iterations and
                       Config.get("Frames") == Frames and math.isclose(Initial, 0.05, rel_tol=1.0e-6))
    FullFramesObserved = len(States) == Frames
    Receipt = {"Name": Name, "Arguments": Options, "ExitCode": ExitCode,
               "TimedOut": TimedOut, "TimeoutSeconds": Timeout,
               "ElapsedSeconds": time.monotonic() - Started,
               "Raw": Raw.name, "RawSha256": hashlib.sha256(Stdout).hexdigest(),
               "Stderr": ErrorPath.name, "StderrSha256": hashlib.sha256(Stderr).hexdigest(),
               "ParseError": ParseError, "ProcessExecuted": ProcessExecuted,
               "FramesRequested": Frames, "FramesObserved": len(States),
               "FullFramesObserved": FullFramesObserved,
               "InitialVelocityKineticEnergyJ": Initial, "ReferenceModelCeilingJ": Ceiling,
               "MaximumKineticEnergyJ": MaximumFinite(States, "KineticEnergy"),
               "MaximumKPlusUJ": MaximumFinite(States, "KineticPlusElasticEstimate"),
               "MaximumStrain": MaximumFinite(States, "MaxEdgeStrain"),
               "MaximumPinDriftM": MaximumFinite(States, "MaxPinDrift"),
               "FirstReferenceModelFailure": FirstEnergyFailure,
               "FirstGeometryFailure": FirstGeometryFailure,
               "FirstNonfiniteFrame": Summary.get("FirstNonfiniteFrame") if Summary else None,
               "BoundedModelPass": ProcessExecuted and FullFramesObserved and
                   FirstEnergyFailure is None and FirstGeometryFailure is None,
               "ModelLimit": "Contact-free scalar replay; sampled once per frame, not every internal integration. No native Jolt parity or full energy-conservation claim."}
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
    Args = Parser.parse_args()
    if not 1 <= Args.Frames <= 3600 or not 1 <= Args.Timeout <= 3600:
        Parser.error("[Physics:LongFreeReplay] Frames or timeout outside bounded range")
    Exe = Args.Exe.resolve(strict=True)
    SourceGraph = Args.Graph.resolve(strict=True)
    Output = Args.Output
    Output.mkdir(parents=True, exist_ok=False)
    Graph = json.loads(SourceGraph.read_text(encoding="utf-8"))
    if len(Graph.get("Positions", [])) != 169 or len(Graph.get("Edges", [])) != 886:
        Parser.error("[Physics:LongFreeReplay] Expected the exported 169-vertex, 886-edge cloth graph")
    Graphs = {}
    for Material in (1.0e-6, 1.0e-5):
        Label = "M1e-6" if Material == 1.0e-6 else "M1e-5"
        PathName = Output / f"{Label}.graph.json"
        Meta = PrepareGraph(Graph, Material, PathName)
        Graphs[Label] = {"Path": PathName, **Meta}
    Cases = []
    for Precision in ("float", "double", "longdouble"):
        for Integrations in (192, 384):
            Cases.append((f"M1e-6-{Precision}-N{Integrations}-I1", "M1e-6", Precision, Integrations, 1))
        for Iterations in (2, 4):
            Cases.append((f"M1e-6-{Precision}-N192-I{Iterations}", "M1e-6", Precision, 192, Iterations))
    for Integrations in (96, 192):
        Cases.append((f"M1e-5-float-N{Integrations}-I1", "M1e-5", "float", Integrations, 1))
    Receipts = []
    for Name, Material, Precision, Integrations, Iterations in Cases:
        Receipt = RunCase(Exe, Graphs[Material]["Path"], Output, Name, Precision,
                          Integrations, Iterations, Args.Frames, Args.Timeout)
        Receipts.append(Receipt)
        (Output / "Receipts.json").write_text(
            json.dumps({"SourceGraphSha256": HashFile(SourceGraph),
                        "ReplayBinarySha256": HashFile(Exe),
                        "PreparedGraphs": {Key: {Field: Value for Field, Value in Meta.items()
                                                 if Field != "Path"} for Key, Meta in Graphs.items()},
                        "Receipts": Receipts}, indent=2, allow_nan=False), encoding="utf-8")
        print(json.dumps({"Name": Name, "ProcessExecuted": Receipt["ProcessExecuted"],
                          "BoundedModelPass": Receipt["BoundedModelPass"],
                          "FirstReferenceModelFailure": Receipt["FirstReferenceModelFailure"],
                          "MaximumKPlusUJ": Receipt["MaximumKPlusUJ"]}), flush=True)
    return 0 if all(Item["ProcessExecuted"] for Item in Receipts) else 2


if __name__ == "__main__":
    raise SystemExit(Main())
