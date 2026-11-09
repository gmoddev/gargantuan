"""Inspect exact grid-face orientation in an existing PJTRACE1 cloth trace."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

import numpy as np


Header = struct.Struct('<8sIIIIIf')


def Faces(Resolution):
    Result = []
    for Y in range(Resolution - 1):
        for X in range(Resolution - 1):
            A = Y * Resolution + X
            Result.append((A, A + Resolution, A + Resolution + 1))
            Result.append((A, A + Resolution + 1, A + 1))
    return np.asarray(Result, dtype=np.int32)


def Boundary(Resolution):
    Top = [X for X in range(Resolution)]
    Right = [Y * Resolution + Resolution - 1 for Y in range(1, Resolution)]
    Bottom = [(Resolution - 1) * Resolution + X
              for X in range(Resolution - 2, -1, -1)]
    Left = [Y * Resolution for Y in range(Resolution - 2, 0, -1)]
    return np.asarray(Top + Right + Bottom + Left, dtype=np.int32)


def BoundaryCrossings(Positions, Indices, PairA, PairB):
    Start = Positions[Indices, :2]
    End = Positions[np.roll(Indices, -1), :2]
    A, B = Start[PairA], End[PairA]
    C, D = Start[PairB], End[PairB]
    AB, CD = B - A, D - C
    Cross = lambda U, V: U[:, 0] * V[:, 1] - U[:, 1] * V[:, 0]
    AC, AD = C - A, D - A
    CA, CB = A - C, B - C
    S1, S2 = Cross(AB, AC), Cross(AB, AD)
    S3, S4 = Cross(CD, CA), Cross(CD, CB)
    BoundsMeet = np.all(np.maximum(np.minimum(A, B), np.minimum(C, D)) <=
                        np.minimum(np.maximum(A, B), np.maximum(C, D)), axis=1)
    Crosses = BoundsMeet & (S1 * S2 <= 0) & (S3 * S4 <= 0)
    Hits = np.flatnonzero(Crosses)
    Witness = [int(PairA[Hits[0]]), int(PairB[Hits[0]])] if len(Hits) else None
    Length = np.linalg.norm(End - Start, axis=1)
    return int(len(Hits)), Witness, float(Length.min())


def AdjacentFaces(FaceIndices):
    EdgeOwner = {}
    Neighbors = [[] for _ in FaceIndices]
    for Face, Indices in enumerate(FaceIndices):
        for A, B in ((Indices[0], Indices[1]), (Indices[1], Indices[2]),
                     (Indices[2], Indices[0])):
            Key = tuple(sorted((int(A), int(B))))
            Prior = EdgeOwner.get(Key)
            if Prior is None:
                EdgeOwner[Key] = Face
            else:
                Neighbors[Face].append((Prior, Key))
                Neighbors[Prior].append((Face, Key))
    return Neighbors


def Side(A, B, C):
    return float((B[0] - A[0]) * (C[1] - A[1]) -
                 (B[1] - A[1]) * (C[0] - A[0]))


def LocalOverlapWitness(Positions, FaceIndices, Neighbors, Candidates):
    for Face in Candidates:
        First = set(int(Value) for Value in FaceIndices[Face])
        for Other, (A, B) in Neighbors[Face]:
            Second = set(int(Value) for Value in FaceIndices[Other])
            FirstTip = next(iter(First - {A, B}))
            SecondTip = next(iter(Second - {A, B}))
            SideOne = Side(Positions[A], Positions[B], Positions[FirstTip])
            SideTwo = Side(Positions[A], Positions[B], Positions[SecondTip])
            if SideOne * SideTwo > 1e-20:
                return {'FaceA': int(Face), 'FaceB': int(Other),
                        'SharedEdge': [A, B], 'OppositeVertices':
                            [FirstTip, SecondTip],
                        'SignedSideA': SideOne, 'SignedSideB': SideTwo}
    return None


def Inspect(PathValue, ExpectedSha256=None):
    Raw = PathValue.read_bytes()
    Digest = hashlib.sha256(Raw).hexdigest()
    if ExpectedSha256 is not None and Digest != ExpectedSha256:
        raise ValueError('Trace differs from expected sealed digest')
    if len(Raw) < Header.size:
        raise ValueError('Truncated PJTRACE1 header')
    Magic, Version, Bodies, Vertices, Frames, Warmup, DeltaTime = Header.unpack_from(Raw)
    Resolution = math.isqrt(Vertices)
    if (Magic, Version, Bodies, Warmup) != (b'PJTRACE1', 1, 1, 0) or \
            Resolution ** 2 != Vertices or not 2 <= Resolution <= 128 or \
            not 1 <= Frames <= 600 or not math.isfinite(DeltaTime) or DeltaTime <= 0 or \
            len(Raw) != Header.size + Frames * Vertices * 12:
        raise ValueError('Invalid or oversized PJTRACE1 grid')
    FaceIndices = Faces(Resolution)
    Neighbors = AdjacentFaces(FaceIndices)
    BoundaryIndices = Boundary(Resolution)
    PairA, PairB = np.triu_indices(len(BoundaryIndices), k=1)
    NonAdjacent = (PairB - PairA > 1) & \
        ~((PairA == 0) & (PairB == len(BoundaryIndices) - 1))
    PairA, PairB = PairA[NonAdjacent], PairB[NonAdjacent]
    X = np.tile(np.linspace(-4.0, 4.0, Resolution), Resolution)
    Y = np.repeat(np.linspace(7.0, -1.0, Resolution), Resolution)
    Rest = np.column_stack((X, Y))
    RestArea2 = (8.0 / (Resolution - 1)) ** 2
    AxialA = np.concatenate((
        np.arange(Vertices).reshape(Resolution, Resolution)[:, :-1].ravel(),
        np.arange(Vertices).reshape(Resolution, Resolution)[:-1, :].ravel()))
    AxialB = np.concatenate((AxialA[:Resolution * (Resolution - 1)] + 1,
                             AxialA[Resolution * (Resolution - 1):] + Resolution))
    RestLengths = np.linalg.norm(Rest[AxialA] - Rest[AxialB], axis=1)
    Summary = []
    FirstAxialFailure = FirstPlanarInversion = FirstLocalOverlap = None
    FirstBoundaryCrossing = None
    WorstArea = {'Ratio': math.inf}
    for Frame in range(Frames):
        Start = Header.size + Frame * Vertices * 12
        Positions = np.frombuffer(Raw, dtype='<f4', count=Vertices * 3,
                                  offset=Start).reshape(Vertices, 3).astype(np.float64)
        if not np.isfinite(Positions).all():
            raise ValueError(f'Nonfinite position at frame {Frame}')
        if np.any(Positions[:, 2] != 0):
            raise ValueError(f'Trace is not exactly planar at frame {Frame}')
        AB = Positions[FaceIndices[:, 1], :2] - Positions[FaceIndices[:, 0], :2]
        AC = Positions[FaceIndices[:, 2], :2] - Positions[FaceIndices[:, 0], :2]
        AreaRatio = (AB[:, 0] * AC[:, 1] - AB[:, 1] * AC[:, 0]) / RestArea2
        Worst = int(np.argmin(AreaRatio))
        if float(AreaRatio[Worst]) < WorstArea['Ratio']:
            WorstArea = {'Frame': Frame, 'Face': Worst,
                         'Triangle': FaceIndices[Worst].tolist(),
                         'Ratio': float(AreaRatio[Worst])}
        Lengths = np.linalg.norm(Positions[AxialA] - Positions[AxialB], axis=1)
        Strain = np.abs(Lengths / RestLengths - 1.0)
        AxialMaximum = float(Strain.max())
        Inverted = np.flatnonzero(AreaRatio < 0)
        CollapsedOrInverted = np.flatnonzero(AreaRatio <= 1e-6)
        BoundaryCount, BoundaryWitness, MinimumBoundaryEdgeLength = \
            BoundaryCrossings(Positions, BoundaryIndices, PairA, PairB)
        Witness = LocalOverlapWitness(Positions[:, :2], FaceIndices, Neighbors,
                                      Inverted) if len(Inverted) else None
        if FirstAxialFailure is None and AxialMaximum > 0.1:
            FirstAxialFailure = Frame
        if FirstPlanarInversion is None and len(Inverted):
            FirstPlanarInversion = Frame
        if FirstLocalOverlap is None and Witness is not None:
            FirstLocalOverlap = {'Frame': Frame, **Witness}
        if FirstBoundaryCrossing is None and BoundaryCount:
            FirstBoundaryCrossing = {'Frame': Frame,
                                     'BoundaryEdges': BoundaryWitness}
        Summary.append({'Frame': Frame, 'MaximumAxialStrain': AxialMaximum,
                        'MinimumSignedTriangleAreaRatio': float(AreaRatio[Worst]),
                        'MinimumAreaFace': Worst,
                        'InvertedTriangleCount': int(len(Inverted)),
                        'CollapsedOrInvertedTriangleCount':
                            int(len(CollapsedOrInverted)),
                        'BoundaryCrossingPairCount': BoundaryCount,
                        'MinimumBoundaryEdgeLengthM': MinimumBoundaryEdgeLength,
                        'PlanarInjectivityCertified':
                            len(CollapsedOrInverted) == 0 and BoundaryCount == 0 and
                            MinimumBoundaryEdgeLength > 0})
    return {'TraceSha256': Digest, 'Resolution': Resolution, 'Frames': Frames,
            'Triangles': int(len(FaceIndices)), 'FirstAxialFailureFrame':
                FirstAxialFailure, 'FirstPlanarInversionFrame': FirstPlanarInversion,
            'FirstAdjacentInteriorOverlap': FirstLocalOverlap,
            'FirstBoundaryCrossing': FirstBoundaryCrossing,
            'WorstSignedTriangleArea': WorstArea, 'FrameSummary': Summary,
            'Scope': 'Exact XY planar winding and simple outer boundary. Strictly positive oriented faces plus a simple Jordan boundary certify injectivity of the triangulated disk; post-inversion nonadjacent overlaps are not enumerated.'}


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument('Trace', type=Path)
    Parser.add_argument('--expected-sha256')
    Parser.add_argument('--output', type=Path)
    Args = Parser.parse_args()
    Result = Inspect(Args.Trace, Args.expected_sha256)
    Text = json.dumps(Result, indent=2, sort_keys=True) + '\n'
    if Args.output:
        Args.output.write_text(Text, encoding='utf-8')
    else:
        print(Text, end='')


if __name__ == '__main__':
    Main()
