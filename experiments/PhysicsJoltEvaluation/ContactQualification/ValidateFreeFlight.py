"""Apply prespecified analytic free-uniform-translation gates to a full V1 trace."""

import argparse
import gzip
import json
import math
from pathlib import Path


VelocityLimitMPerS = 1e-5
PositionLimitM = 2e-6
RelativeMomentumLimit = 1e-5
RelativeKineticLimit = 1e-5


def Norm(Values):
    return math.sqrt(sum(Value * Value for Value in Values))


def FrameErrors(Rest, InitialVelocity, Masses, PositionFlat, VelocityFlat, TimeSeconds):
    Count = len(Rest)
    if len(PositionFlat) != 3 * Count or len(VelocityFlat) != 3 * Count:
        raise ValueError('Free-flight frame array shape mismatch')
    MaximumPosition = 0.0
    MaximumVelocity = 0.0
    Momentum = [0.0, 0.0, 0.0]
    Kinetic = 0.0
    for Index, (RestPosition, Mass) in enumerate(zip(Rest, Masses)):
        Position = PositionFlat[3 * Index:3 * Index + 3]
        Velocity = VelocityFlat[3 * Index:3 * Index + 3]
        if not all(math.isfinite(Value) for Value in Position + Velocity):
            raise ValueError('Nonfinite free-flight state')
        MaximumPosition = max(MaximumPosition, Norm([
            Position[Axis] - (RestPosition[Axis] + InitialVelocity[Axis] * TimeSeconds)
            for Axis in range(3)]))
        MaximumVelocity = max(MaximumVelocity, Norm([
            Velocity[Axis] - InitialVelocity[Axis] for Axis in range(3)]))
        for Axis in range(3):
            Momentum[Axis] += Mass * Velocity[Axis]
        Kinetic += 0.5 * Mass * sum(Value * Value for Value in Velocity)
    InitialMomentum = [sum(Masses) * Value for Value in InitialVelocity]
    InitialKinetic = 0.5 * sum(Masses) * sum(Value * Value for Value in InitialVelocity)
    return {
        'MaximumVertexPositionErrorM': MaximumPosition,
        'MaximumVertexVelocityErrorMPerS': MaximumVelocity,
        'RelativeTotalMomentumDrift': Norm([
            Momentum[Axis] - InitialMomentum[Axis] for Axis in range(3)]) /
            Norm(InitialMomentum),
        'RelativeTotalKineticDrift': abs(Kinetic - InitialKinetic) / InitialKinetic,
    }


def FreeFlightGatePass(Maximum):
    return Maximum['MaximumVertexVelocityErrorMPerS'] <= VelocityLimitMPerS and \
        Maximum['MaximumVertexPositionErrorM'] <= PositionLimitM and \
        Maximum['RelativeTotalMomentumDrift'] <= RelativeMomentumLimit and \
        Maximum['RelativeTotalKineticDrift'] <= RelativeKineticLimit


def Inspect(CaseDirectory):
    Fixture = json.loads((CaseDirectory / 'ContactFixtureV1.json').read_text(encoding='utf-8'))
    Rest = Fixture['RestPositions']
    Masses = Fixture['VertexMassKg']
    Initial = Fixture['InitialVelocities']
    Target = [2.0, 0.0, 0.0]
    Material = Fixture['Authored']['Material']
    if len(Rest) != 81 or len(Masses) != 81 or len(Initial) != 81 or \
            Fixture['Pins'] or Fixture['Colliders'] or \
            Fixture['GravityMPerS2'] != [0.0, 0.0, 0.0] or \
            Material['AppliedStepDamping'] != 0.0 or \
            Material['AuthoredFrameDamping'] != 0.0 or \
            any(Velocity != Target for Velocity in Initial) or \
            not all(math.isfinite(Mass) and Mass > 0 for Mass in Masses):
        raise ValueError('Fixture is not the prespecified 9x9 free uniform launch')
    Maximum = {Name: 0.0 for Name in (
        'MaximumVertexPositionErrorM', 'MaximumVertexVelocityErrorMPerS',
        'RelativeTotalMomentumDrift', 'RelativeTotalKineticDrift')}
    Frames = 0
    with gzip.open(CaseDirectory / 'ContactTraceV1.jsonl.gz', 'rt', encoding='utf-8') as Stream:
        for Frames, Line in enumerate(Stream, 1):
            Row = json.loads(Line)
            TimeSeconds = (Frames * Fixture['DeltaTimeSeconds'])
            if Row['Frame'] != Frames - 1 or \
                    abs(Row['TimeSeconds'] - TimeSeconds) > 1e-7:
                raise ValueError('Free-flight frame or time sequence mismatch')
            Errors = FrameErrors(Rest, Target, Masses, Row['Positions'],
                                 Row['Velocities'], TimeSeconds)
            for Name, Value in Errors.items():
                Maximum[Name] = max(Maximum[Name], Value)
    if Frames != Fixture['Frames']:
        raise ValueError('Free-flight frame count mismatch')
    Pass = FreeFlightGatePass(Maximum)
    return {'CaseId': Fixture['CaseId'], 'Frames': Frames, 'Vertices': len(Rest),
            **Maximum, 'GatePass': Pass,
            'PrespecifiedLimits': {
                'MaximumVertexVelocityErrorMPerS': VelocityLimitMPerS,
                'MaximumVertexPositionErrorM': PositionLimitM,
                'RelativeTotalMomentumDrift': RelativeMomentumLimit,
                'RelativeTotalKineticDrift': RelativeKineticLimit},
            'Scope': 'Analytic free uniform translation only; other physical energy remains ungated'}


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
