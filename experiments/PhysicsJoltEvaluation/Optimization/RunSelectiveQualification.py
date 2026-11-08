"""Fresh-process quality gate and paired cost reduction for the split cadence fixture.

This gate is supplemental to the existing internal soft energy qualification.
It does not rank stock and corrected solvers as physically equivalent.
"""

import argparse
import hashlib
import json
import math
import statistics
import subprocess
from pathlib import Path


def Digest(PathValue: Path) -> str:
    Hash = hashlib.sha256()
    with PathValue.open("rb") as Source:
        for Block in iter(lambda: Source.read(1024 * 1024), b""):
            Hash.update(Block)
    return Hash.hexdigest()


def Run(Executable: Path, Mode: str, Workers: int, Frames: int,
        IsolatedBodies: int, Reconstruction: str) -> dict:
    Command = [str(Executable), "--mode", Mode, "--workers", str(Workers),
               "--frames", str(Frames), "--isolated-bodies", str(IsolatedBodies),
               "--velocity-reconstruction", Reconstruction]
    Result = subprocess.run(Command, capture_output=True, text=True, timeout=120,
                            check=False)
    if Result.returncode != 0:
        raise RuntimeError(f"[Physics:Selective] {Executable.name} {Mode} T{Workers} "
                           f"exit {Result.returncode}: stdout={Result.stdout[-1200:]} "
                           f"stderr={Result.stderr[-1200:]}")
    Lines = Result.stdout.splitlines()
    if not Lines:
        raise RuntimeError("[Physics:Selective] Empty benchmark output")
    Row = json.loads(Lines[0])
    if not Row.get("BenchmarkQualityPass") or not Row.get("IsolatedFiniteEveryFrame") or \
       not Row.get("CoupledFiniteEveryFrame") or not Row.get("RequiresFullInternalCouplingQualification"):
        raise RuntimeError(f"[Physics:Selective] Physical gate failed: {Row}")
    for Key in ("TotalUpdateMs", "GuardMs", "QualityMs", "SceneSetupMs",
                "MaxIsolatedSpeedAcrossFrames", "MaxIsolatedPenetrationAcrossFrames",
                "MaxEdgeStretchAcrossFrames"):
        if not math.isfinite(Row[Key]):
            raise RuntimeError(f"[Physics:Selective] Nonfinite {Key}")
    return Row


def RunRefusal(Executable: Path, Mode: str, Reconstruction: str) -> dict:
    Result = subprocess.run([str(Executable), "--mode", Mode, "--workers", "1",
                             "--frames", "120", "--isolated-bodies", "8",
                             "--velocity-reconstruction", Reconstruction],
                            capture_output=True, text=True, timeout=30, check=False)
    if Result.returncode != 0 or not Result.stdout.splitlines():
        raise RuntimeError(f"[Physics:Selective] Refusal mode {Mode} failed: {Result.stderr[-1200:]}")
    Row = json.loads(Result.stdout.splitlines()[0])
    if Row.get("RefusedBeforeUpdate") is not True:
        raise RuntimeError(f"[Physics:Selective] Unsafe scene admitted: {Mode}")
    return Row


def Main() -> None:
    Parser = argparse.ArgumentParser()
    Parser.add_argument("--stock", required=True, type=Path)
    Parser.add_argument("--patched", required=True, type=Path)
    Parser.add_argument("--frames", type=int, default=120)
    Parser.add_argument("--isolated-bodies", type=int, default=128)
    Parser.add_argument("--repeats", type=int, default=4)
    Parser.add_argument("--output", type=Path)
    Options = Parser.parse_args()
    if not 1 <= Options.Frames <= 600 or not 1 <= Options.IsolatedBodies <= 512 or \
       not 2 <= Options.Repeats <= 6 or Options.Repeats % 2:
        raise ValueError("[Physics:Selective] Option outside bounded range")
    for Executable in (Options.Stock, Options.Patched):
        if not Executable.is_file():
            raise FileNotFoundError(Executable)

    Rows = []
    Summary = []
    Refusals = []
    Backends = (("Stock", Options.Stock, "difference"),
                ("Patched", Options.Patched, "delta"))
    BackendHashes = {Backend: set() for Backend, _, _ in Backends}
    PairCosts = {(Backend, Workers): [] for Backend, _, _ in Backends
                 for Workers in (1, 2, 8)}
    for Backend, Executable, Reconstruction in Backends:
        for Mode in ("refusal-near", "refusal-crossing", "refusal-joint"):
            Refusals.append({"Backend": Backend, "Mode": Mode,
                             "Result": RunRefusal(Executable, Mode, Reconstruction)})

    for WorkerIndex, Workers in enumerate((1, 2, 8)):
        for Repeat in range(Options.Repeats):
            BackendOrder = Backends if (WorkerIndex + Repeat) % 2 == 0 else Backends[::-1]
            ModeOrder = ("global", "selective") if Repeat % 2 == 0 else ("selective", "global")
            for Backend, Executable, Reconstruction in BackendOrder:
                Results = {Mode: Run(Executable, Mode, Workers, Options.Frames,
                                     Options.IsolatedBodies, Reconstruction) for Mode in ModeOrder}
                Global = Results["global"]
                Selective = Results["selective"]
                if Global["CoupledTrajectoryHash"] != Selective["CoupledTrajectoryHash"]:
                    raise RuntimeError(f"[Physics:Selective] Coupled trajectory changed: "
                                       f"{Backend} T{Workers} R{Repeat}")
                BackendHashes[Backend].add(Global["CoupledTrajectoryHash"])
                GlobalCost = Global["TotalUpdateMs"] + Global["GuardMs"]
                SelectiveCost = Selective["TotalUpdateMs"] + Selective["GuardMs"]
                PairCosts[(Backend, Workers)].append((GlobalCost, SelectiveCost))
                Rows.append({"Backend": Backend, "Workers": Workers, "Repeat": Repeat,
                             "ModeOrder": list(ModeOrder),
                             "Global": Global, "Selective": Selective})
    for Backend, _, _ in Backends:
        for Workers in (1, 2, 8):
            Costs = PairCosts[(Backend, Workers)]
            Summary.append({"Backend": Backend, "Workers": Workers,
                            "GlobalMedianMs": statistics.median(Pair[0] for Pair in Costs),
                            "SelectiveMedianMs": statistics.median(Pair[1] for Pair in Costs),
                            "MedianPairedSpeedup": statistics.median(
                                Pair[0] / Pair[1] for Pair in Costs)})
        if len(BackendHashes[Backend]) != 1:
            raise RuntimeError(f"[Physics:Selective] Repeated/cross-worker coupled hash drift: "
                               f"{Backend}: {sorted(BackendHashes[Backend])}")
    Output = {"BenchmarkQualityPass": True, "CoupledHashInvariant": True,
              "FullInternalCouplingQualificationRequired": True,
              "StockPatchedPhysicalEquivalenceClaimed": False,
              "StockBinarySha256": Digest(Options.Stock),
              "PatchedBinarySha256": Digest(Options.Patched),
              "Summary": Summary, "Refusals": Refusals, "Runs": Rows}
    Serialized = json.dumps(Output, indent=2, sort_keys=True)
    if Options.Output:
        Options.Output.write_text(Serialized + "\n", encoding="utf-8")
    print(Serialized)


if __name__ == "__main__":
    Main()
