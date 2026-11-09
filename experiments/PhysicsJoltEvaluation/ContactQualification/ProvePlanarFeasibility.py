"""Construct and certify an in-plane escape from the original driven sphere."""

import argparse
import json
import math
from pathlib import Path


def Certify(Resolution=128, Frames=40):
    if Resolution < 2 or Resolution > 128 or Frames < 1 or Frames > 600:
        raise ValueError('Planar certificate resolution or frames out of bounds')
    RadiusWithShell = 1.05
    KFinal = RadiusWithShell / math.sqrt(16.0 - RadiusWithShell ** 2)
    Spacing = 8.0 / (Resolution - 1)
    RestArea2 = -(Spacing ** 2)
    MaxAxialStrain = 0.0
    MinTriangleAreaRatio = math.inf
    MaxHalfplaneViolation = 0.0
    MinSphereClearanceLowerBound = math.inf
    MaxPinDrift = 0.0
    TrianglesCertified = 0
    for Frame in range(Frames):
        TimeSeconds = (Frame + 1) / 60.0
        Progress = min(TimeSeconds / 0.66, 1.0)
        K = KFinal * Progress
        SphereX = 5.2 - 1.2 * Progress
        DistanceToBoundaryLine = (SphereX - 4.0 + 4.0 * K) / math.sqrt(1 + K * K)
        MinSphereClearanceLowerBound = min(
            MinSphereClearanceLowerBound, DistanceToBoundaryLine - RadiusWithShell)
        MaxAxialStrain = max(MaxAxialStrain, math.sqrt(1 + K * K) - 1)
        Positions = []
        for YIndex in range(Resolution):
            Y = 7.0 - YIndex * Spacing
            for XIndex in range(Resolution):
                X = -4.0 + XIndex * Spacing
                MappedX = X - K * (7.0 - Y)
                Positions.append((MappedX, Y, 0.0))
                SignedHalfplane = MappedX - K * Y - (4.0 - 7.0 * K)
                MaxHalfplaneViolation = max(MaxHalfplaneViolation, SignedHalfplane)
                if YIndex == 0:
                    MaxPinDrift = max(MaxPinDrift, abs(MappedX - X))
        for YIndex in range(Resolution - 1):
            for XIndex in range(Resolution - 1):
                A = YIndex * Resolution + XIndex
                B = A + 1
                C = A + Resolution
                D = C + 1
                for I, J, L in ((A, B, C), (B, D, C)):
                    X0, Y0, _ = Positions[I]
                    X1, Y1, _ = Positions[J]
                    X2, Y2, _ = Positions[L]
                    Area2 = (X1 - X0) * (Y2 - Y0) - (Y1 - Y0) * (X2 - X0)
                    MinTriangleAreaRatio = min(MinTriangleAreaRatio, Area2 / RestArea2)
                    TrianglesCertified += 1
    Result = {
        'Status': 'CONSTRUCTIVE_KINEMATIC_FEASIBILITY',
        'Resolution': Resolution, 'Frames': Frames,
        'EffectiveSphereRadiusM': RadiusWithShell,
        'FinalShearK': KFinal,
        'MaximumAxialStrain': MaxAxialStrain,
        'MaximumPinDriftM': MaxPinDrift,
        'MinimumTriangleSignedAreaRatio': MinTriangleAreaRatio,
        'MaximumHalfplaneViolationM': MaxHalfplaneViolation,
        'MinimumSphereClearanceLowerBoundM': MinSphereClearanceLowerBound,
        'TrianglesCertifiedAcrossFrames': TrianglesCertified,
        'OriginalAxialGate': 0.10,
        'ProofScope': 'Prescribed planar positions, not a trajectory of the authored dynamical initial-value problem',
    }
    if MaxAxialStrain > 0.10 or MaxPinDrift > 1e-12 or \
            MinTriangleAreaRatio <= 0 or MaxHalfplaneViolation > 1e-12 or \
            MinSphereClearanceLowerBound < -1e-12:
        raise AssertionError('Constructive planar certificate failed')
    return Result


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument('--resolution', type=int, default=128)
    Parser.add_argument('--frames', type=int, default=40)
    Parser.add_argument('--output', type=Path)
    Args = Parser.parse_args()
    Result = Certify(Args.resolution, Args.frames)
    Text = json.dumps(Result, indent=2, sort_keys=True) + '\n'
    if Args.output:
        Args.output.write_text(Text, encoding='utf-8')
    print(Text, end='')


if __name__ == '__main__':
    Main()
