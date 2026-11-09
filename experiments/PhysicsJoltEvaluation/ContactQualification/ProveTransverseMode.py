"""Find a negative transverse elastic-surrogate mode in a planar contact trace."""

import argparse
from array import array
import hashlib
import json
import math
from pathlib import Path
import struct
import sys


Header = struct.Struct('<8sIIIIIf')
StretchCompliance = 0.000001
BendCompliance = 0.0001


def Edges(Resolution):
    Spacing = 8.0 / (Resolution - 1)
    for Y in range(Resolution):
        for X in range(Resolution):
            A = Y * Resolution + X
            if X + 1 < Resolution:
                yield A, A + 1, Spacing, StretchCompliance
            if Y + 1 < Resolution:
                yield A, A + Resolution, Spacing, StretchCompliance
            if X + 1 < Resolution and Y + 1 < Resolution:
                yield A, A + Resolution + 1, Spacing * math.sqrt(2), StretchCompliance
                yield A + 1, A + Resolution, Spacing * math.sqrt(2), StretchCompliance
            if X + 2 < Resolution:
                yield A, A + 2, 2 * Spacing, BendCompliance
            if Y + 2 < Resolution:
                yield A, A + 2 * Resolution, 2 * Spacing, BendCompliance


def Certify(PathValue, Frame=34, ExpectedSha256=None):
    Raw = PathValue.read_bytes()
    Digest = hashlib.sha256(Raw).hexdigest()
    if ExpectedSha256 is not None and Digest != ExpectedSha256:
        raise ValueError('Original trace digest differs from sealed receipt')
    if len(Raw) < Header.size:
        raise ValueError('Trace header is truncated')
    Magic, Version, Bodies, Vertices, Frames, Warmup, Dt = Header.unpack_from(Raw)
    Resolution = math.isqrt(Vertices)
    if (Magic, Version, Bodies, Warmup) != (b'PJTRACE1', 1, 1, 0) or \
            Resolution * Resolution != Vertices or Resolution > 128 or \
            not 0 <= Frame < Frames or not math.isfinite(Dt) or Dt <= 0 or \
            len(Raw) != Header.size + Frames * Vertices * 12:
        raise ValueError('Trace shape, cadence or requested frame is invalid')
    Values = array('f')
    Start = Header.size + Frame * Vertices * 12
    Values.frombytes(Raw[Start:Start + Vertices * 12])
    if Values.itemsize != 4:
        raise ValueError('Float array is not binary32')
    if sys.byteorder != 'little':
        Values.byteswap()
    if any(not math.isfinite(Value) for Value in Values) or \
            any(Values[3 * Index + 2] != 0.0 for Index in range(Vertices)):
        raise ValueError('Requested snapshot is not finite and exactly planar')
    Curvature = [0.0] * Vertices
    Incident = [[] for _ in range(Vertices)]
    EdgeCount = 0
    for A, B, RestLength, Compliance in Edges(Resolution):
        CurrentLength = math.dist(Values[3 * A:3 * A + 3],
                                  Values[3 * B:3 * B + 3])
        if CurrentLength <= 0:
            raise ValueError('Degenerate current edge prevents Hessian analysis')
        Weight = (1.0 - RestLength / CurrentLength) / Compliance
        Curvature[A] += Weight
        Curvature[B] += Weight
        Incident[A].append((CurrentLength, RestLength, Compliance))
        Incident[B].append((CurrentLength, RestLength, Compliance))
        EdgeCount += 1
    Node = min(range(Resolution, Vertices), key=lambda Index: Curvature[Index])
    if Curvature[Node] >= 0:
        raise ValueError('No negative free-node transverse mode found')
    FiniteDifferences = []
    for Epsilon in (1e-5, 1e-4, 1e-3):
        DeltaEnergy = sum(
            ((math.hypot(Length, Epsilon) - RestLength) ** 2 -
             (Length - RestLength) ** 2) / (2 * Compliance)
            for Length, RestLength, Compliance in Incident[Node])
        FiniteDifferences.append({'EpsilonM': Epsilon,
            'DeltaSurrogateEnergyJPlusOrMinus': DeltaEnergy,
            'QuadraticPredictionJ': 0.5 * Curvature[Node] * Epsilon * Epsilon})
        if DeltaEnergy >= 0:
            raise AssertionError('Finite transverse perturbation failed to lower surrogate energy')
    Position = tuple(Values[3 * Node:3 * Node + 3])
    Progress = min((Frame + 1) * Dt / 0.66, 1.0)
    SphereCenter = (5.2 - 1.2 * Progress, 3.0, 0.0)
    return {'Status': 'NEGATIVE_TRANSVERSE_SURROGATE_MODE',
        'TraceSha256': Digest, 'Frame': Frame, 'Resolution': Resolution,
        'EdgeCount': EdgeCount, 'Node': Node,
        'NodeGridX': Node % Resolution, 'NodeGridY': Node // Resolution,
        'NodePositionM': Position, 'IncidentEdgeCount': len(Incident[Node]),
        'SingleNodeTransverseHessianJPerM2': Curvature[Node],
        'FiniteDifferences': FiniteDifferences,
        'SphereCenterM': SphereCenter,
        'NodeDistanceBeyondEffectiveSphereM':
            math.dist(Position, SphereCenter) - 1.05,
        'TopPinsUnchanged': Node >= Resolution,
        'FaceAndEdgePenetrationCannotIncrease':
            'The XY projection of every incident triangle and edge is unchanged; each barycentric point gains a nonnegative Z-squared distance to the Z=0 sphere center.',
        'EnergyScope': 'Spring potential surrogate U=sum((L-L0)^2/(2*Compliance)); excludes contact, damping, external work and native retained-lambda state'}


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument('trace', type=Path)
    Parser.add_argument('--frame', type=int, default=34)
    Parser.add_argument('--expected-sha256')
    Parser.add_argument('--output', type=Path)
    Args = Parser.parse_args()
    Result = Certify(Args.trace, Args.frame, Args.expected_sha256)
    Text = json.dumps(Result, indent=2, sort_keys=True) + '\n'
    if Args.output:
        Args.output.write_text(Text, encoding='utf-8')
    print(Text, end='')


if __name__ == '__main__':
    Main()
