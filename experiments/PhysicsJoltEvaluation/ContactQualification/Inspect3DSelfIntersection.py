"""Bounded nonlocal triangle-intersection check for small V1 cloth traces."""

import argparse
import gzip
import json
import math
from pathlib import Path

from ValidateContactTrace import Cross, Dot, Norm, Sub


MaximumVertices = 1024
MaximumFaces = 1922
MaximumFrames = 240
MaximumCandidatePairs = 2_000_000
Tolerance = 1e-9


def Turn2(A, B, C):
    return (B[0] - A[0]) * (C[1] - A[1]) - \
           (B[1] - A[1]) * (C[0] - A[0])


def CoplanarOverlap(A, B):
    Normal = Cross(Sub(A[1], A[0]), Sub(A[2], A[0]))
    Drop = max(range(3), key=lambda Index: abs(Normal[Index]))
    Axes = [Index for Index in range(3) if Index != Drop]
    A2 = [(Point[Axes[0]], Point[Axes[1]]) for Point in A]
    B2 = [(Point[Axes[0]], Point[Axes[1]]) for Point in B]
    for Triangle in (A2, B2):
        for Index in range(3):
            Start, End = Triangle[Index], Triangle[(Index + 1) % 3]
            Axis = (End[1] - Start[1], Start[0] - End[0])
            SpanA = [Dot(Point, Axis) for Point in A2]
            SpanB = [Dot(Point, Axis) for Point in B2]
            if min(max(SpanA), max(SpanB)) - max(min(SpanA), min(SpanB)) <= Tolerance:
                return False
    return True


def SegmentHitsTriangle(Start, End, Triangle, Normal):
    PlaneStart = Dot(Sub(Start, Triangle[0]), Normal)
    PlaneEnd = Dot(Sub(End, Triangle[0]), Normal)
    if (PlaneStart > Tolerance and PlaneEnd > Tolerance) or \
            (PlaneStart < -Tolerance and PlaneEnd < -Tolerance) or \
            abs(PlaneStart - PlaneEnd) <= Tolerance:
        return False
    Fraction = PlaneStart / (PlaneStart - PlaneEnd)
    if not -Tolerance <= Fraction <= 1.0 + Tolerance:
        return False
    Point = tuple(Start[Axis] + Fraction * (End[Axis] - Start[Axis])
                  for Axis in range(3))
    for Index in range(3):
        A, B = Triangle[Index], Triangle[(Index + 1) % 3]
        if Dot(Cross(Sub(B, A), Sub(Point, A)), Normal) < -Tolerance:
            return False
    return True


def TrianglesIntersect(A, B):
    NormalA = Cross(Sub(A[1], A[0]), Sub(A[2], A[0]))
    NormalB = Cross(Sub(B[1], B[0]), Sub(B[2], B[0]))
    LengthA, LengthB = Norm(NormalA), Norm(NormalB)
    if LengthA <= 1e-12 or LengthB <= 1e-12:
        raise ValueError('Degenerate triangle in 3D intersection check')
    NormalA = tuple(Value / LengthA for Value in NormalA)
    NormalB = tuple(Value / LengthB for Value in NormalB)
    DistanceB = [Dot(Sub(Point, A[0]), NormalA) for Point in B]
    DistanceA = [Dot(Sub(Point, B[0]), NormalB) for Point in A]
    if min(DistanceA) > Tolerance or max(DistanceA) < -Tolerance or \
            min(DistanceB) > Tolerance or max(DistanceB) < -Tolerance:
        return False
    if max(abs(Value) for Value in DistanceA + DistanceB) <= Tolerance:
        return CoplanarOverlap(A, B)
    return any(SegmentHitsTriangle(Triangle[Index], Triangle[(Index + 1) % 3],
                                   Other, Normal)
               for Triangle, Other, Normal in ((A, B, NormalB), (B, A, NormalA))
               for Index in range(3))


def FrameIntersections(Positions, Faces, RemainingPairs):
    Bounds = []
    for Index, Face in enumerate(Faces):
        Points = [Positions[Vertex] for Vertex in Face]
        Bounds.append((Index, min(Point[0] for Point in Points),
                       max(Point[0] for Point in Points),
                       min(Point[1] for Point in Points),
                       max(Point[1] for Point in Points),
                       min(Point[2] for Point in Points),
                       max(Point[2] for Point in Points)))
    Bounds.sort(key=lambda Value: Value[1])
    Active = []
    Candidates = 0
    for Current in Bounds:
        Active = [Other for Other in Active if Other[2] >= Current[1]]
        for Other in Active:
            if Other[4] < Current[3] or Current[4] < Other[3] or \
                    Other[6] < Current[5] or Current[6] < Other[5] or \
                    not set(Faces[Current[0]]).isdisjoint(Faces[Other[0]]):
                continue
            Candidates += 1
            if Candidates > RemainingPairs:
                raise ValueError('3D triangle candidate budget exceeded')
            A = [Positions[Vertex] for Vertex in Faces[Current[0]]]
            B = [Positions[Vertex] for Vertex in Faces[Other[0]]]
            if TrianglesIntersect(A, B):
                return Candidates, [Current[0], Other[0]]
        Active.append(Current)
    return Candidates, None


def Inspect(CaseDirectory):
    Fixture = json.loads((CaseDirectory / 'ContactFixtureV1.json').read_text(encoding='utf-8'))
    Faces = Fixture['Triangles']
    Vertices, Frames = len(Fixture['RestPositions']), Fixture['Frames']
    if Vertices > MaximumVertices or len(Faces) > MaximumFaces or Frames > MaximumFrames:
        return {'Status': 'INCOMPLETE_BUDGET', 'Vertices': Vertices,
                'Triangles': len(Faces), 'Frames': Frames}
    Candidates = 0
    Witnesses = []
    Seen = 0
    with gzip.open(CaseDirectory / 'ContactTraceV1.jsonl.gz', 'rt', encoding='utf-8') as Stream:
        for Frame, Line in enumerate(Stream):
            Seen += 1
            Row = json.loads(Line)
            if Row['Frame'] != Frame:
                raise ValueError('Frame order mismatch')
            Flat = Row['Positions']
            if len(Flat) != Vertices * 3 or not all(math.isfinite(Value) for Value in Flat):
                raise ValueError('Nonfinite or missing positions')
            Positions = [tuple(Flat[Index:Index + 3]) for Index in range(0, len(Flat), 3)]
            Checked, Pair = FrameIntersections(Positions, Faces,
                                                MaximumCandidatePairs - Candidates)
            Candidates += Checked
            if Pair is not None:
                Witnesses.append({'Frame': Frame, 'Faces': Pair,
                                  'Vertices': [Faces[Index] for Index in Pair]})
        if Seen != Frames:
            raise ValueError('Trace frame count mismatch')
    return {'Status': 'NONLOCAL_3D_INTERSECTION' if Witnesses else
            'NO_DISJOINT_FACE_INTERSECTION', 'Vertices': Vertices,
            'Triangles': len(Faces), 'Frames': Frames,
            'CandidatePairsTested': Candidates, 'Witnesses': Witnesses,
            'Scope': 'Disjoint-vertex face pairs only; local neighboring-face folds, shared-vertex contacts and work/impulse closure remain unqualified.'}


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument('CaseDirectory', type=Path)
    Parser.add_argument('--output', type=Path)
    Args = Parser.parse_args()
    Result = Inspect(Args.CaseDirectory)
    Text = json.dumps(Result, indent=2, sort_keys=True) + '\n'
    if Args.output:
        Args.output.write_text(Text, encoding='utf-8')
    else:
        print(Text, end='')


if __name__ == '__main__':
    Main()
