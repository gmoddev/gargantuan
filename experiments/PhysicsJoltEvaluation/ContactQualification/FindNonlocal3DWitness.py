"""Bounded one-frame spatial-bucket search for a strict disjoint-face intersection."""

import argparse
import gzip
import hashlib
import json
import math
from pathlib import Path
import struct

import numpy as np


MaximumFaces = 32512
MaximumVertices = 16384
MaximumCellPostings = 600000
MaximumCandidatePairs = 500000
MaximumCellsPerFace = 512
SegmentMargin = 1e-3
BarycentricMargin = 1e-3
PlaneMarginM = 1e-6


def ReadJson(PathValue):
    return json.loads(PathValue.read_text(encoding="utf-8"))


def Sha(PathValue):
    return hashlib.sha256(PathValue.read_bytes()).hexdigest()


def StrictEdgeFaceHit(EdgeTriangle, FaceTriangle):
    B0, B1, B2 = FaceTriangle
    N = np.cross(B1 - B0, B2 - B0)
    NLength = float(np.linalg.norm(N))
    if NLength <= 1e-10:
        return None
    N /= NLength
    E0, E1 = B1 - B0, B2 - B0
    D00, D01, D11 = float(E0 @ E0), float(E0 @ E1), float(E1 @ E1)
    Denominator = D00 * D11 - D01 * D01
    if Denominator <= 1e-20:
        return None
    for Edge in range(3):
        Start, End = EdgeTriangle[Edge], EdgeTriangle[(Edge + 1) % 3]
        DistanceStart = float((Start - B0) @ N)
        DistanceEnd = float((End - B0) @ N)
        if DistanceStart * DistanceEnd >= 0 or \
                min(abs(DistanceStart), abs(DistanceEnd)) <= PlaneMarginM:
            continue
        Fraction = DistanceStart / (DistanceStart - DistanceEnd)
        if not SegmentMargin < Fraction < 1 - SegmentMargin:
            continue
        Point = Start + Fraction * (End - Start)
        V2 = Point - B0
        D20, D21 = float(V2 @ E0), float(V2 @ E1)
        Beta = (D11 * D20 - D01 * D21) / Denominator
        Gamma = (D00 * D21 - D01 * D20) / Denominator
        Barycentric = [1 - Beta - Gamma, Beta, Gamma]
        if min(Barycentric) <= BarycentricMargin:
            continue
        return {"PiercingEdge": Edge, "SegmentFraction": Fraction,
                "PointM": Point.tolist(), "FaceBarycentric": Barycentric,
                "SignedEndpointPlaneDistancesM": [DistanceStart, DistanceEnd],
                "FaceAreaM2": 0.5 * NLength,
                "MinimumFaceBarycentric": min(Barycentric)}
    return None


def Search(Positions, Faces, CellSize):
    FacePoints = Positions[Faces]
    Minimum = FacePoints.min(axis=1)
    Maximum = FacePoints.max(axis=1)
    Bins = {}
    CellPostings = 0
    Candidates = 0
    for Index, Face in enumerate(Faces):
        Low = np.floor(Minimum[Index] / CellSize).astype(int)
        High = np.floor(Maximum[Index] / CellSize).astype(int)
        Cells = (High - Low + 1)
        FaceCells = int(Cells[0] * Cells[1] * Cells[2])
        if FaceCells > MaximumCellsPerFace:
            raise ValueError(f"Face {Index} spans {FaceCells} cells; bounded search incomplete")
        CellPostings += FaceCells
        if CellPostings > MaximumCellPostings:
            raise ValueError("Cell posting budget exceeded; bounded search incomplete")
        Seen = set()
        for X in range(Low[0], High[0] + 1):
            for Y in range(Low[1], High[1] + 1):
                for Z in range(Low[2], High[2] + 1):
                    Key = (X, Y, Z)
                    for Other in Bins.get(Key, ()):
                        if Other in Seen:
                            continue
                        Seen.add(Other)
                        if np.any(Maximum[Other] < Minimum[Index]) or \
                                np.any(Maximum[Index] < Minimum[Other]) or \
                                any(Vertex in Face for Vertex in Faces[Other]):
                            continue
                        Candidates += 1
                        if Candidates > MaximumCandidatePairs:
                            raise ValueError("Candidate pair budget exceeded; bounded search incomplete")
                        Hit = StrictEdgeFaceHit(FacePoints[Index], FacePoints[Other])
                        Direction = "FirstEdgePiercesSecond"
                        if Hit is None:
                            Hit = StrictEdgeFaceHit(FacePoints[Other], FacePoints[Index])
                            Direction = "SecondEdgePiercesFirst"
                        if Hit is not None:
                            return {"Status": "STRICT_NONLOCAL_INTERSECTION",
                                    "FaceIndices": [Index, Other],
                                    "FaceVertices": [Face.tolist(), Faces[Other].tolist()],
                                    "FacePositionsM": [FacePoints[Index].tolist(),
                                                       FacePoints[Other].tolist()],
                                    "WitnessDirection": Direction, "Witness": Hit,
                                    "CandidatePairsTested": Candidates,
                                    "CellPostings": CellPostings,
                                    "CellSizeM": CellSize}
                    Bins.setdefault(Key, []).append(Index)
    return {"Status": "NO_STRICT_INTERIOR_EDGE_FACE_WITNESS_WITHIN_BOUNDS",
            "CandidatePairsTested": Candidates, "CellPostings": CellPostings,
            "CellSizeM": CellSize}


def Inspect(Case, Frame, CellSize):
    Source = ReadJson(Case / "Source.json")
    Fixture = ReadJson(Case / "ContactFixtureV1.json")
    if Sha(Case / "ContactFixtureV1.json") != Source["FixtureSha256"] or \
            Sha(Case / "ContactTraceV1.jsonl.gz") != Source["TraceSha256"]:
        raise ValueError("Source fixture or trace hash mismatch")
    Faces = np.asarray(Fixture["Triangles"], dtype=np.int32)
    if len(Faces) > MaximumFaces or len(Fixture["RestPositions"]) > MaximumVertices or \
            not 0 <= Frame < Fixture["Frames"]:
        raise ValueError("Out of bounded mesh/frame scope")
    Flat = None
    Row = None
    with gzip.open(Case / "ContactTraceV1.jsonl.gz", "rt", encoding="utf-8") as Stream:
        for Index, Line in enumerate(Stream):
            if Index == Frame:
                Row = json.loads(Line)
                if Row["Frame"] != Frame:
                    raise ValueError("Frame identity mismatch")
                Flat = Row["Positions"]
                break
    if Flat is None or len(Flat) != 3 * len(Fixture["RestPositions"]):
        raise ValueError("Missing selected full position frame")
    Reference = ReadJson(Case / "FrameHashes.json")[Frame]
    if Reference["Frame"] != Frame:
        raise ValueError("Selected frame hash index differs")
    FrameHashes = {}
    for Name in ("Positions", "Velocities"):
        Values = Row[Name]
        Raw = struct.pack("<" + str(len(Values)) + "f", *Values)
        FrameHashes[Name] = hashlib.sha256(Raw).hexdigest()
        if FrameHashes[Name] != Reference[Name + "Float32Sha256"]:
            raise ValueError(f"Selected {Name} float32 payload hash mismatch")
    Positions = np.asarray(Flat, dtype=np.float64).reshape((-1, 3))
    if not np.isfinite(Positions).all():
        raise ValueError("Nonfinite position")
    Rest = np.asarray(Fixture["RestPositions"], dtype=np.float64)
    RestTriangles, CurrentTriangles = Rest[Faces], Positions[Faces]
    RestCross = ((RestTriangles[:, 1, 0] - RestTriangles[:, 0, 0]) *
                 (RestTriangles[:, 2, 1] - RestTriangles[:, 0, 1]) -
                 (RestTriangles[:, 1, 1] - RestTriangles[:, 0, 1]) *
                 (RestTriangles[:, 2, 0] - RestTriangles[:, 0, 0]))
    CurrentCross = ((CurrentTriangles[:, 1, 0] - CurrentTriangles[:, 0, 0]) *
                    (CurrentTriangles[:, 2, 1] - CurrentTriangles[:, 0, 1]) -
                    (CurrentTriangles[:, 1, 1] - CurrentTriangles[:, 0, 1]) *
                    (CurrentTriangles[:, 2, 0] - CurrentTriangles[:, 0, 0]))
    if np.any(RestCross == 0):
        raise ValueError("Rest mesh has zero XY projected triangle area")
    ProjectedFlips = int(np.count_nonzero(RestCross * CurrentCross < 0))
    Result = Search(Positions, Faces, CellSize)
    return {"Case": Case.name, "Frame": Frame,
            "Vertices": len(Positions), "Faces": len(Faces),
            "Engine": Source["Engine"], "Native": Source["Native"],
            "FixtureSha256": Source["FixtureSha256"],
            "CompressedTraceSha256": Source["TraceSha256"], **Result,
            "SelectedFrameFloat32Sha256": FrameHashes,
            "ProjectedOrientationFlipFaces": ProjectedFlips,
            "Scope": "One selected frame, disjoint-vertex faces, strict edge-through-face interior with positive plane/barycentric margins; absence is not a global nonintersection certificate"}


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("Case", type=Path)
    Parser.add_argument("--frame", type=int, default=39)
    Parser.add_argument("--cell-size", type=float, default=0.12)
    Parser.add_argument("--output", type=Path)
    Args = Parser.parse_args()
    if not math.isfinite(Args.cell_size) or not 0.02 <= Args.cell_size <= 1.0:
        raise ValueError("Cell size outside bounded scope")
    Result = Inspect(Args.Case, Args.frame, Args.cell_size)
    Text = json.dumps(Result, indent=2) + "\n"
    if Args.output:
        Args.output.write_text(Text, encoding="utf-8")
    else:
        print(Text, end="")


if __name__ == "__main__":
    Main()
