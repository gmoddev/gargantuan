"""Stream PJTRACE1 physics traces and apply the frozen final-quality protocol.

Only one frame from each trace is decoded at a time. The tool never ranks
quality-trace timings; writing those traces instruments the provider frame.
"""

from __future__ import annotations

import argparse
from array import array
import json
import math
from pathlib import Path
import re
import struct
import sys


MAGIC = b"PJTRACE1"
HEADER = struct.Struct("<8sIIIIIf")
MASK = (1 << 64) - 1
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211
STATE_HASH = re.compile(r"state_frame=(\d+)\s+soft_trajectory_hash=(\d+)")
SCENES = {"cloth1k": (False, 32), "cloth4k": (False, 64),
          "cloth16k": (False, 128), "rubber4k": (True, 16)}


def Limit(Text: str, Pattern: str) -> float:
    Match = re.search(Pattern, Text)
    if Match is None:
        raise ValueError(f"QualityProtocol lacks {Pattern}")
    return float(Match.group(1))


def Thresholds(Protocol: dict, Scope: str) -> dict[str, float]:
    Trial = Protocol["GroupingTrial"]
    Text = Trial["FortyFrameGate"] if Scope == "forty" else Trial["LongHorizonGate"]
    Result = {"PinMeters": Limit(Text, r"pin<=([0-9.]+)m"),
              "RmsMeters": Limit(Text, r"RMS<=([0-9.]+)m"),
              "MaxMeters": Limit(Text, r"max(?: vertex displacement)?<=([0-9.]+)m")}
    if Scope == "forty":
        Result["PeakAxialStrain"] = Limit(Text, r"peak axial strain<=([0-9.]+)")
        Result["FinalAxialStrain"] = Limit(Text, r"final<=([0-9.]+)")
    return Result


class Trace:
    def __init__(self, PathValue: Path):
        self.Path = PathValue
        self.File = PathValue.open("rb")
        HeaderBytes = self.File.read(HEADER.size)
        if len(HeaderBytes) != HEADER.size:
            raise ValueError(f"Truncated trace header: {PathValue}")
        Magic, Version, Bodies, Vertices, Frames, Warmup, DeltaTime = HEADER.unpack(HeaderBytes)
        if Magic != MAGIC or Version != 1 or Bodies < 1 or Vertices < 1 or Frames < 1:
            raise ValueError(f"Invalid PJTRACE1 header: {PathValue}")
        if Warmup > Frames or not math.isfinite(DeltaTime) or DeltaTime <= 0:
            raise ValueError(f"Invalid trace cadence: {PathValue}")
        self.Bodies = Bodies
        self.Vertices = Vertices
        self.Frames = Frames
        self.Warmup = Warmup
        self.DeltaTime = DeltaTime
        self.FrameBytes = Bodies * Vertices * 3 * 4
        ExpectedBytes = HEADER.size + Frames * self.FrameBytes
        if PathValue.stat().st_size != ExpectedBytes:
            raise ValueError(f"Trace size mismatch: {PathValue} expected {ExpectedBytes}")

    def Frame(self) -> tuple[bytes, array]:
        Raw = self.File.read(self.FrameBytes)
        if len(Raw) != self.FrameBytes:
            raise ValueError(f"Truncated frame in {self.Path}")
        Values = array("f")
        Values.frombytes(Raw)
        if Values.itemsize != 4:
            raise ValueError("Python float array is not IEEE-754 binary32")
        if sys.byteorder != "little":
            Values.byteswap()
        return Raw, Values

    def Close(self) -> None:
        self.File.close()


def FrameHash(Hash: int, Raw: bytes) -> int:
    for (Bits,) in struct.iter_unpack("<I", Raw):
        Hash = ((Hash ^ Bits) * FNV_PRIME) & MASK
    return Hash


def LogHashes(PathValue: Path | None, Frames: int) -> list[int] | None:
    if PathValue is None:
        return None
    Matches = [(int(Frame), int(Hash)) for Frame, Hash in
               STATE_HASH.findall(PathValue.read_text(encoding="utf-8", errors="replace"))]
    if len(Matches) != Frames or [Frame for Frame, _ in Matches] != list(range(Frames)):
        raise ValueError(f"Missing or nonsequential state-frame hashes: {PathValue}")
    return [Hash for _, Hash in Matches]


def RestTetrahedra(Resolution: int) -> list[tuple[int, int, int, int, float]]:
    Spacing = 2.0 / (Resolution - 1)
    Unit = Spacing ** 3 / 6.0
    Result = []
    Index = lambda X, Y, Z: (Z * Resolution + Y) * Resolution + X
    for Z in range(Resolution - 1):
        for Y in range(Resolution - 1):
            for X in range(Resolution - 1):
                A, B = Index(X, Y, Z), Index(X + 1, Y, Z)
                C, D = Index(X, Y + 1, Z), Index(X + 1, Y + 1, Z)
                E, F = Index(X, Y, Z + 1), Index(X + 1, Y, Z + 1)
                G, H = Index(X, Y + 1, Z + 1), Index(X + 1, Y + 1, Z + 1)
                Result.extend(((A, B, C, E, Unit), (B, D, C, H, Unit),
                               (B, C, E, H, 2.0 * Unit), (B, E, F, H, Unit),
                               (C, G, E, H, Unit)))
    return Result


def TetraVolume(Values: array, Offset: int, A: int, B: int, C: int, D: int) -> float:
    A, B, C, D = (Offset + 3 * V for V in (A, B, C, D))
    X1, Y1, Z1 = Values[B] - Values[A], Values[B + 1] - Values[A + 1], Values[B + 2] - Values[A + 2]
    X2, Y2, Z2 = Values[C] - Values[A], Values[C + 1] - Values[A + 1], Values[C + 2] - Values[A + 2]
    X3, Y3, Z3 = Values[D] - Values[A], Values[D + 1] - Values[A + 1], Values[D + 2] - Values[A + 2]
    return abs(X1 * (Y2 * Z3 - Z2 * Y3) + Y1 * (Z2 * X3 - X2 * Z3) + Z1 * (X2 * Y3 - Y2 * X3)) / 6.0


def Geometry(Values: array, Bodies: int, Vertices: int, Scene: str,
             Tetrahedra: list[tuple[int, int, int, int, float]]) -> dict:
    Rubber, Resolution = SCENES[Scene]
    if Vertices != Resolution ** (3 if Rubber else 2):
        raise ValueError(f"Scene vertex count mismatch: {Scene}, {Vertices}")
    MaximumPin = 0.0
    MaximumStrain = 0.0
    MinimumBoundsRatio = math.inf
    MaximumBoundsRatio = 0.0
    MaximumTetraError = 0.0
    TetraRatioTotal = 0.0
    for Body in range(Bodies):
        Offset = Body * Vertices * 3
        if not Rubber:
            for X in range(Resolution):
                I = Offset + 3 * X
                ExpectedX = 20.0 * Body + 8.0 * (X / (Resolution - 1) - 0.5)
                MaximumPin = max(MaximumPin, math.dist(
                    (Values[I], Values[I + 1], Values[I + 2]), (ExpectedX, 7.0, 0.0)))
        RestLength = (2.0 if Rubber else 8.0) / (Resolution - 1)
        ZCount = Resolution if Rubber else 1
        for Z in range(ZCount):
            for Y in range(Resolution):
                for X in range(Resolution):
                    A = Offset + 3 * ((Z * Resolution + Y) * Resolution + X)
                    for B in ((A + 3) if X + 1 < Resolution else None,
                              (A + 3 * Resolution) if Y + 1 < Resolution else None,
                              (A + 3 * Resolution * Resolution) if Z + 1 < ZCount else None):
                        if B is not None:
                            D = math.sqrt(sum((Values[B + Axis] - Values[A + Axis]) ** 2
                                              for Axis in range(3)))
                            MaximumStrain = max(MaximumStrain, abs(D / RestLength - 1.0))
        if Rubber:
            Bounds = []
            for Axis in range(3):
                Components = Values[Offset + Axis:Offset + Vertices * 3:3]
                Bounds.append(max(Components) - min(Components))
            Ratio = math.prod(Bounds) / 8.0
            MinimumBoundsRatio = min(MinimumBoundsRatio, Ratio)
            MaximumBoundsRatio = max(MaximumBoundsRatio, Ratio)
            Current = Rest = 0.0
            for A, B, C, D, RestVolume in Tetrahedra:
                Volume = TetraVolume(Values, Offset, A, B, C, D)
                Current += Volume
                Rest += RestVolume
                MaximumTetraError = max(MaximumTetraError, abs(Volume / RestVolume - 1.0))
            TetraRatioTotal += Current / Rest
    Result = {"MaximumPinErrorMeters": MaximumPin,
              "MaximumAxialStrain": MaximumStrain}
    if Rubber:
        Result.update({"MinimumBoundsVolumeRatio": MinimumBoundsRatio,
                       "MaximumBoundsVolumeRatio": MaximumBoundsRatio,
                       "MeanTetraVolumeRatio": TetraRatioTotal / Bodies,
                       "MaximumTetraVolumeError": MaximumTetraError})
    return Result


def Compare(Args: argparse.Namespace) -> dict:
    Protocol = json.loads(Args.Quality.read_text(encoding="utf-8"))
    Limits = Thresholds(Protocol, Args.Scope)
    Baseline, Candidate = Trace(Args.Baseline), Trace(Args.Candidate)
    try:
        ShapeB = (Baseline.Bodies, Baseline.Vertices, Baseline.Frames, Baseline.Warmup, Baseline.DeltaTime)
        ShapeC = (Candidate.Bodies, Candidate.Vertices, Candidate.Frames, Candidate.Warmup, Candidate.DeltaTime)
        if ShapeB != ShapeC:
            raise ValueError(f"Trace header mismatch: {ShapeB} != {ShapeC}")
        if Args.Scope == "forty" and Baseline.Frames - Baseline.Warmup != 40:
            raise ValueError("Forty-frame scope requires 40 measured frames")
        if Args.Scope == "long" and Baseline.Frames - Baseline.Warmup != (300 if Args.Scene == "rubber4k" else 600):
            raise ValueError("Long scope frame count does not match protocol")
        Logs = (LogHashes(Args.BaselineLog, Baseline.Frames),
                LogHashes(Args.CandidateLog, Candidate.Frames))
        Tetrahedra = RestTetrahedra(SCENES[Args.Scene][1]) if SCENES[Args.Scene][0] else []
        Hashes = [FNV_OFFSET, FNV_OFFSET]
        Rows = []
        FirstHashMismatch = None
        FirstLogMismatch = None
        PeakRms = PeakMax = PeakPinB = PeakPinC = PeakStrainB = PeakStrainC = 0.0
        AllFinite = True
        for Frame in range(Baseline.Frames):
            RawB, ValuesB = Baseline.Frame()
            RawC, ValuesC = Candidate.Frame()
            Hashes[0] = FrameHash(Hashes[0], RawB)
            Hashes[1] = FrameHash(Hashes[1], RawC)
            if FirstHashMismatch is None and Hashes[0] != Hashes[1]:
                FirstHashMismatch = Frame
            if FirstLogMismatch is None and any(
                Log is not None and Hashes[Side] != Log[Frame]
                for Side, Log in enumerate(Logs)):
                FirstLogMismatch = Frame
            Finite = all(map(math.isfinite, ValuesB)) and all(map(math.isfinite, ValuesC))
            if not Finite:
                AllFinite = False
                raise ValueError(f"Nonfinite position in frame {Frame}")
            Squared = 0.0
            Largest = 0.0
            for I in range(0, len(ValuesB), 3):
                Delta = sum((ValuesB[I + Axis] - ValuesC[I + Axis]) ** 2 for Axis in range(3))
                Squared += Delta
                Largest = max(Largest, Delta)
            Rms = math.sqrt(Squared / (Baseline.Bodies * Baseline.Vertices))
            Maximum = math.sqrt(Largest)
            GeometryB = Geometry(ValuesB, Baseline.Bodies, Baseline.Vertices, Args.Scene, Tetrahedra)
            GeometryC = Geometry(ValuesC, Candidate.Bodies, Candidate.Vertices, Args.Scene, Tetrahedra)
            PeakRms, PeakMax = max(PeakRms, Rms), max(PeakMax, Maximum)
            PeakPinB = max(PeakPinB, GeometryB["MaximumPinErrorMeters"])
            PeakPinC = max(PeakPinC, GeometryC["MaximumPinErrorMeters"])
            PeakStrainB = max(PeakStrainB, GeometryB["MaximumAxialStrain"])
            PeakStrainC = max(PeakStrainC, GeometryC["MaximumAxialStrain"])
            Rows.append({"Frame": Frame, "BaselineHash": Hashes[0], "CandidateHash": Hashes[1],
                         "RmsMeters": Rms, "MaxMeters": Maximum,
                         "Baseline": GeometryB, "Candidate": GeometryC})
        Last = Rows[-1]
        Checks = {"Finite": AllFinite, "HashLogs": FirstLogMismatch is None,
                  "Pin": max(PeakPinB, PeakPinC) <= Limits["PinMeters"],
                  "Rms": PeakRms <= Limits["RmsMeters"],
                  "Maximum": PeakMax <= Limits["MaxMeters"]}
        if Args.Scope == "forty":
            Checks["PeakAxialStrain"] = max(PeakStrainB, PeakStrainC) <= Limits["PeakAxialStrain"]
            Checks["FinalAxialStrain"] = max(
                Last["Baseline"]["MaximumAxialStrain"],
                Last["Candidate"]["MaximumAxialStrain"]) <= Limits["FinalAxialStrain"]
        if Args.ExpectExact:
            Checks["ExactHashes"] = FirstHashMismatch is None
        return {"Schema": "PhysicsJoltTraceComparison1", "Baseline": str(Args.Baseline),
                "Candidate": str(Args.Candidate), "Scene": Args.Scene, "Scope": Args.Scope,
                "Header": {"Bodies": Baseline.Bodies, "VerticesPerBody": Baseline.Vertices,
                           "Frames": Baseline.Frames, "Warmup": Baseline.Warmup,
                           "DeltaTime": Baseline.DeltaTime},
                "Thresholds": Limits, "Checks": Checks, "Pass": all(Checks.values()),
                "FirstHashMismatchFrame": FirstHashMismatch,
                "FirstLogMismatchFrame": FirstLogMismatch,
                "PeakRmsMeters": PeakRms, "PeakMaxMeters": PeakMax,
                "PeakBaselinePinMeters": PeakPinB, "PeakCandidatePinMeters": PeakPinC,
                "PeakBaselineAxialStrain": PeakStrainB, "PeakCandidateAxialStrain": PeakStrainC,
                "Final": Last, "Frames": Rows}
    finally:
        Baseline.Close()
        Candidate.Close()


def Main() -> int:
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--baseline", dest="Baseline", type=Path, required=True)
    Parser.add_argument("--candidate", dest="Candidate", type=Path, required=True)
    Parser.add_argument("--baseline-log", dest="BaselineLog", type=Path, required=True)
    Parser.add_argument("--candidate-log", dest="CandidateLog", type=Path, required=True)
    Parser.add_argument("--scene", dest="Scene", choices=SCENES, required=True)
    Parser.add_argument("--scope", dest="Scope", choices=("forty", "long"), required=True)
    Parser.add_argument("--quality", dest="Quality", type=Path, required=True)
    Parser.add_argument("--expect-exact", dest="ExpectExact", action="store_true")
    Parser.add_argument("--output", dest="Output", type=Path, required=True)
    Args = Parser.parse_args()
    try:
        Result = Compare(Args)
        Args.Output.parent.mkdir(parents=True, exist_ok=True)
        Args.Output.write_text(json.dumps(Result, separators=(",", ":")), encoding="utf-8")
        print("[Physics:TraceCompare] " + json.dumps({
            "Pass": Result["Pass"], "Scene": Result["Scene"], "Scope": Result["Scope"],
            "Checks": Result["Checks"], "PeakRmsMeters": Result["PeakRmsMeters"],
            "PeakMaxMeters": Result["PeakMaxMeters"],
            "FirstHashMismatchFrame": Result["FirstHashMismatchFrame"],
            "Output": str(Args.Output)}))
        return 0 if Result["Pass"] else 1
    except (OSError, ValueError, KeyError) as Error:
        print(f"[Physics:TraceCompare] {Error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(Main())
