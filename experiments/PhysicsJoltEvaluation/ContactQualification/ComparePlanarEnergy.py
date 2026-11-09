"""Compare source distance-compliance energy of failed frame and feasible shear."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

from ProveTransverseMode import Edges, Header
from ValidateContactTrace import TriangleDistanceSquared


OriginalTraceSha256 = 'b9cc9db786d0843550c1e6fab1b2bcea457784d3f58b780caadb76b861ce759f'
EffectiveSphereRadiusM = 1.05


def Energy(Positions, Resolution):
    Components = {'axial': 0.0, 'shear': 0.0, 'bend': 0.0}
    MaximumStrain = {'axial': 0.0, 'shear': 0.0, 'bend': 0.0}
    Count = {'axial': 0, 'shear': 0, 'bend': 0}
    for A, B, RestLength, Compliance in Edges(Resolution):
        Offset = B - A
        Kind = ('axial' if Offset in (1, Resolution) else
                'bend' if Offset in (2, 2 * Resolution) else 'shear')
        Length = math.dist(Positions[A], Positions[B])
        Components[Kind] += (Length - RestLength) ** 2 / (2 * Compliance)
        MaximumStrain[Kind] = max(MaximumStrain[Kind], abs(Length / RestLength - 1))
        Count[Kind] += 1
    return {'EnergyByKindJ': Components, 'TotalEnergyJ': sum(Components.values()),
            'MaximumAbsoluteStrainByKind': MaximumStrain, 'EdgeCountByKind': Count}


def SurfaceClearance(Positions, Resolution):
    Center = (4.0, 3.0, 0.0)
    MinimumDistanceSquared = math.inf
    PenetratingFaces = 0
    for Y in range(Resolution - 1):
        for X in range(Resolution - 1):
            A = Y * Resolution + X
            for Face in ((A, A + Resolution, A + Resolution + 1),
                         (A, A + Resolution + 1, A + 1)):
                DistanceSquared = TriangleDistanceSquared(
                    Center, *(Positions[Index] for Index in Face))
                MinimumDistanceSquared = min(MinimumDistanceSquared, DistanceSquared)
                PenetratingFaces += DistanceSquared < EffectiveSphereRadiusM ** 2 - 1e-12
    MinimumDistance = math.sqrt(MinimumDistanceSquared)
    return {'MinimumSphereToTriangleDistanceM': MinimumDistance,
            'MaximumSphereShellPenetrationM':
                max(0.0, EffectiveSphereRadiusM - MinimumDistance),
            'PenetratingFaceCount': PenetratingFaces}


def Compare(PathValue, Frame=39):
    Raw = PathValue.read_bytes()
    Digest = hashlib.sha256(Raw).hexdigest()
    if Digest != OriginalTraceSha256:
        raise ValueError('Original trace SHA-256 does not match sealed source')
    Magic, Version, Bodies, Vertices, Frames, Warmup, DeltaTime = Header.unpack_from(Raw)
    Resolution = math.isqrt(Vertices)
    if (Magic, Version, Bodies, Warmup, Resolution, Frames) != \
            (b'PJTRACE1', 1, 1, 0, 128, 40) or not 0 <= Frame < Frames or \
            len(Raw) != Header.size + Frames * Vertices * 12:
        raise ValueError('Original trace shape or requested frame differs')
    Start = Header.size + Frame * Vertices * 12
    Values = struct.unpack_from('<' + str(3 * Vertices) + 'f', Raw, Start)
    Original = [Values[Index:Index + 3] for Index in range(0, len(Values), 3)]
    K = EffectiveSphereRadiusM / math.sqrt(16 - EffectiveSphereRadiusM ** 2)
    Shear = [(-4 + 8 * (Index % Resolution) / (Resolution - 1) -
              K * 8 * (Index // Resolution) / (Resolution - 1),
              7 - 8 * (Index // Resolution) / (Resolution - 1), 0.0)
             for Index in range(Vertices)]
    OriginalEnergy = Energy(Original, Resolution)
    ShearEnergy = Energy(Shear, Resolution)
    return {'TraceSha256': Digest, 'Frame': Frame,
            'Graph': 'Source cloth axial/shear/two-hop-bend distance constraints; stretch compliance 1e-6, bend compliance 1e-4',
            'SphereCenterM': [4.0, 3.0, 0.0],
            'EffectiveSphereRadiusM': EffectiveSphereRadiusM,
            'AffineShearK': K,
            'Original': {**OriginalEnergy, **SurfaceClearance(Original, Resolution)},
            'AffineShear': {**ShearEnergy, **SurfaceClearance(Shear, Resolution)},
            'ShearToOriginalEnergyRatio': ShearEnergy['TotalEnergyJ'] /
                OriginalEnergy['TotalEnergyJ'],
            'Scope': 'Endpoint distance-compliance spring surrogate. It does not include kinetic/gravity/work, solver multipliers, contact potential, or optimize over all admissible deformations.'}


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument('OriginalTrace', type=Path)
    Parser.add_argument('--output', type=Path)
    Args = Parser.parse_args()
    Result = Compare(Args.OriginalTrace)
    Text = json.dumps(Result, indent=2, sort_keys=True) + '\n'
    if Args.output:
        Args.output.write_text(Text, encoding='utf-8')
    else:
        print(Text, end='')


if __name__ == '__main__':
    Main()
