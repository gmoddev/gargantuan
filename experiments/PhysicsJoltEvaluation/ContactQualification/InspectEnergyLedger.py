"""Compute measured kinetic, gravity and distance-compliance energy histories."""

import argparse
import gzip
import json
import math
from pathlib import Path

from ValidateContactTrace import Dot, Norm, Sub


DirectRawMomentumLimit = 1e-5


def Near(A, B):
    return abs(A - B) <= 1e-7 * max(1.0, abs(A), abs(B))


def NearPublished(A, B):
    return abs(A - B) <= 1e-5 * max(1.0, abs(A), abs(B))


def FiniteNumber(Value):
    return type(Value) in (int, float) and math.isfinite(Value)


def RawMomentumClosurePass(MaximumNormalizedRawResidual):
    return MaximumNormalizedRawResidual is not None and \
        MaximumNormalizedRawResidual <= DirectRawMomentumLimit


def VerifyAccounting(Accounting, Stages, Frame, Fixture):
    if not isinstance(Accounting, dict) or not isinstance(Stages, dict):
        raise ValueError('Missing XPBD accounting stage')
    Vectors = {}
    for Name in ('NormalImpulseOnClothNs', 'FrictionImpulseOnClothNs',
                 'PrescribedColliderReactionImpulseNs',
                 'MomentumStartKgMPerS', 'MomentumEndKgMPerS',
                 'GravityForceImpulseNs', 'UniformImpulseNs', 'PointImpulseNs',
                 'DampingImpulseNs', 'PinConstraintImpulseOnClothNs',
                 'MomentumResidualNs'):
        Value = Accounting.get(Name)
        if not isinstance(Value, list) or len(Value) != 3 or \
                not all(FiniteNumber(Component) for Component in Value):
            raise ValueError(f'Invalid XPBD accounting vector {Name}')
        Vectors[Name] = Value
    Unpinned = Accounting.get('UnpinnedStructuralNumericalImpulseNs', [0.0, 0.0, 0.0])
    if not isinstance(Unpinned, list) or len(Unpinned) != 3 or \
            not all(FiniteNumber(Component) for Component in Unpinned):
        raise ValueError('Invalid unpinned structural numerical impulse')
    Vectors['UnpinnedStructuralNumericalImpulseNs'] = Unpinned
    PinNumerical = Accounting.get('PinConstraintNumericalImpulseNs', [0.0, 0.0, 0.0])
    if not isinstance(PinNumerical, list) or len(PinNumerical) != 3 or \
            not all(FiniteNumber(Component) for Component in PinNumerical):
        raise ValueError('Invalid pin constraint numerical impulse')
    Vectors['PinConstraintNumericalImpulseNs'] = PinNumerical
    StageMomentum = Stages.get('ContactProjectionVelocityDerivedMomentumDeltaNs')
    if not isinstance(StageMomentum, list) or len(StageMomentum) != 3 or \
            not all(FiniteNumber(Component) for Component in StageMomentum):
        raise ValueError('Invalid XPBD contact stage momentum')
    for Axis in range(3):
        Total = Vectors['NormalImpulseOnClothNs'][Axis] + \
            Vectors['FrictionImpulseOnClothNs'][Axis]
        if not Near(Total, StageMomentum[Axis]) or \
                not Near(Total + Vectors['PrescribedColliderReactionImpulseNs'][Axis], 0.0):
            raise ValueError(f'XPBD impulse split identity fails at frame {Frame}')
    Scalars = {}
    for Name in ('ContactKineticDeltaJ', 'PrescribedSurfaceWorkJ',
                 'ContactEnergyResidualJ', 'KineticStartJ', 'KineticEndJ',
                 'AppliedDynamicsKineticDeltaJ', 'DampingKineticDeltaJ',
                 'GravityForceKineticDeltaJ', 'UniformImpulseKineticDeltaJ',
                 'PointImpulseKineticDeltaJ',
                 'StructuralProjectionKineticDeltaJ', 'PinProjectionKineticDeltaJ',
                 'LedgerResidualJ', 'SubstepSeconds'):
        Value = Accounting.get(Name)
        if not FiniteNumber(Value):
            raise ValueError(f'Invalid XPBD accounting scalar {Name}')
        Scalars[Name] = Value
    IdealFields = ('IdealVelocityReconstructionEnabled',
                   'IntegrationPositionRoundingKineticDeltaJ',
                   'IdealVelocityReconstructionKineticDeltaJ')
    HasIdealFields = any(Name in Accounting for Name in IdealFields)
    if HasIdealFields and (not all(Name in Accounting for Name in IdealFields) or
                           type(Accounting['IdealVelocityReconstructionEnabled']) is not bool or
                           not FiniteNumber(Accounting['IntegrationPositionRoundingKineticDeltaJ']) or
                           not FiniteNumber(Accounting['IdealVelocityReconstructionKineticDeltaJ'])):
        raise ValueError(f'Invalid ideal velocity reconstruction metadata at frame {Frame}')
    IdealEnabled = Accounting['IdealVelocityReconstructionEnabled'] if HasIdealFields else False
    Mode = Accounting.get('VelocityPrecisionMode', '1' if IdealEnabled else '0')
    ShadowEnabled = Accounting.get('DoubleShadowEnabled', False)
    if Mode not in ('0', '1', '2') or (Mode in ('1', '2') and not HasIdealFields) or \
            type(ShadowEnabled) is not bool or \
            IdealEnabled != (Mode == '1') or ShadowEnabled != (Mode == '2') or \
            ('VelocityPrecisionMode' in Accounting) != ('DoubleShadowEnabled' in Accounting):
        raise ValueError(f'Invalid XPBD velocity precision mode at frame {Frame}')
    Scalars['IntegrationPositionRoundingKineticDeltaJ'] = (
        Accounting['IntegrationPositionRoundingKineticDeltaJ'] if HasIdealFields else 0.0)
    Scalars['IdealVelocityReconstructionKineticDeltaJ'] = (
        Accounting['IdealVelocityReconstructionKineticDeltaJ'] if HasIdealFields else 0.0)
    if not Near(Scalars['SubstepSeconds'],
                Fixture['DeltaTimeSeconds'] / Fixture['Authored']['XpbdSubsteps']) or \
            Accounting.get('PrescribedSurfaceVelocitySource') != \
                'center(end)-center(start) per substep' or \
            Accounting.get('Method') != \
                'source-contact-projection-and-previous-position-deltas' or \
            type(Accounting.get('StaticPoseWorkComplete')) is not bool or \
            type(Accounting.get('Corrections')) is not int or \
            Accounting['Corrections'] < 0 or \
            Accounting['Corrections'] != Stages.get('ContactProjectionCorrections'):
        raise ValueError(f'XPBD accounting metadata fails at frame {Frame}')
    ContactResidual = Scalars['ContactKineticDeltaJ'] - Scalars['PrescribedSurfaceWorkJ']
    LedgerResidual = Scalars['KineticEndJ'] - Scalars['KineticStartJ'] - \
        Scalars['AppliedDynamicsKineticDeltaJ'] - Scalars['DampingKineticDeltaJ'] - \
        Scalars['StructuralProjectionKineticDeltaJ'] - Scalars['PinProjectionKineticDeltaJ'] - \
        Scalars['ContactKineticDeltaJ'] - \
        Scalars['IntegrationPositionRoundingKineticDeltaJ'] - \
        Scalars['IdealVelocityReconstructionKineticDeltaJ']
    SplitDynamics = Scalars['GravityForceKineticDeltaJ'] + \
        Scalars['UniformImpulseKineticDeltaJ'] + Scalars['PointImpulseKineticDeltaJ']
    if not Near(ContactResidual, Scalars['ContactEnergyResidualJ']) or \
            not Near(LedgerResidual, Scalars['LedgerResidualJ']) or \
            not Near(SplitDynamics, Scalars['AppliedDynamicsKineticDeltaJ']):
        raise ValueError(f'XPBD kinetic stage ledger identity fails at frame {Frame}')
    Components = ('GravityForceImpulseNs', 'UniformImpulseNs', 'PointImpulseNs',
                  'DampingImpulseNs', 'PinConstraintImpulseOnClothNs',
                  'NormalImpulseOnClothNs', 'FrictionImpulseOnClothNs')
    ImpulseScale = max(1.0, sum(Norm(Vectors[Name]) for Name in Components))
    RawResidual = [Vectors['MomentumEndKgMPerS'][Axis] -
                   Vectors['MomentumStartKgMPerS'][Axis] -
                   sum(Vectors[Name][Axis] for Name in Components)
                   for Axis in range(3)]
    for Axis in range(3):
        Recomputed = RawResidual[Axis] - (0.0 if Mode in ('1', '2') else Unpinned[Axis])
        if not Near(Recomputed, Vectors['MomentumResidualNs'][Axis]):
            raise ValueError(f'XPBD momentum stage ledger identity fails at frame {Frame}')
    return {**Scalars, **Vectors,
            'IdealVelocityReconstructionEnabled': IdealEnabled,
            'DoubleShadowEnabled': ShadowEnabled,
            'VelocityPrecisionMode': Mode,
            'NormalizedMomentumResidual': Norm(Vectors['MomentumResidualNs']) /
                ImpulseScale,
            'NormalizedRawMomentumResidual': Norm(RawResidual) / ImpulseScale,
            'NormalizedUnpinnedStructuralNumericalImpulse':
                Norm(Unpinned) / ImpulseScale,
            'NormalizedPinConstraintNumericalImpulse':
                Norm(PinNumerical) / ImpulseScale,
            'StaticPoseWorkComplete': Accounting['StaticPoseWorkComplete']}


def Inspect(CaseDirectory):
    Fixture = json.loads((CaseDirectory / 'ContactFixtureV1.json').read_text(encoding='utf-8'))
    Material = Fixture['Authored']['Material']
    Compliance = {'axial': Material['StretchCompliance'],
                  'shear': Material['StretchCompliance'],
                  'bend': Material['BendCompliance']}
    if any(not isinstance(Value, (int, float)) or not math.isfinite(Value) or Value <= 0
           for Value in Compliance.values()):
        return {'Status': 'INCOMPLETE_COMPLIANCE'}
    Edges = Fixture['StructuralEdges']
    Masses = Fixture['VertexMassKg']
    Gravity = Fixture['GravityMPerS2']
    Count = len(Masses)
    def Energy(Positions, Velocities):
        Kinetic = sum(0.5 * Mass * Dot(Velocity, Velocity)
                      for Mass, Velocity in zip(Masses, Velocities))
        GravityPotential = -sum(Mass * Dot(Gravity, Position)
                                for Mass, Position in zip(Masses, Positions))
        DistancePotential = sum(
            (Norm(Sub(Positions[Edge['A']], Positions[Edge['B']])) -
             Edge['RestLengthM']) ** 2 / (2 * Compliance[Edge['Kind']])
            for Edge in Edges)
        Momentum = [sum(Mass * Velocity[Axis]
                        for Mass, Velocity in zip(Masses, Velocities))
                    for Axis in range(3)]
        return {'KineticJ': Kinetic, 'GravityPotentialJ': GravityPotential,
                'DistanceCompliancePotentialJ': DistancePotential,
                'TotalJ': Kinetic + GravityPotential + DistancePotential,
                'MomentumKgMPerS': Momentum}
    Initial = Energy(Fixture['RestPositions'], Fixture['InitialVelocities'])
    Series = []
    AccountingRows = []
    AccountingSeen = None
    ModeSeen = None
    with gzip.open(CaseDirectory / 'ContactTraceV1.jsonl.gz', 'rt', encoding='utf-8') as Stream:
        for Frame, Line in enumerate(Stream):
            Row = json.loads(Line)
            if Row['Frame'] != Frame or len(Row['Positions']) != 3 * Count or \
                    len(Row['Velocities']) != 3 * Count:
                raise ValueError('Energy trace frame identity or array shape mismatch')
            Positions = [Row['Positions'][Index:Index + 3]
                         for Index in range(0, 3 * Count, 3)]
            Velocities = [Row['Velocities'][Index:Index + 3]
                          for Index in range(0, 3 * Count, 3)]
            if not all(math.isfinite(Value) for Vector in Positions + Velocities
                       for Value in Vector):
                raise ValueError('Energy trace contains nonfinite state')
            Series.append({'Frame': Frame, **Energy(Positions, Velocities)})
            Accounting = Row.get('External', {}).get('XpbdContactAccounting')
            if AccountingSeen is None:
                AccountingSeen = Accounting is not None
            if (Accounting is not None) != AccountingSeen:
                raise ValueError('XPBD accounting appears in only part of the trace')
            if AccountingSeen:
                Reduced = VerifyAccounting(Accounting, Row.get('Stages'), Frame, Fixture)
                if ModeSeen is None:
                    ModeSeen = Reduced['VelocityPrecisionMode']
                elif Reduced['VelocityPrecisionMode'] != ModeSeen:
                    raise ValueError('Velocity precision mode changes within one trace')
                AccountingRows.append(Reduced)
    if len(Series) != Fixture['Frames']:
        raise ValueError('Energy trace frame count mismatch')
    Previous = Initial['TotalJ']
    for Row in Series:
        Row['ChangeFromPreviousJ'] = Row['TotalJ'] - Previous
        Previous = Row['TotalJ']
    PhysicalResiduals = []
    StaticPoseIncompleteFrames = []
    if AccountingSeen:
        for Index, (Row, Accounting) in enumerate(zip(Series, AccountingRows)):
            if not Near(Row['KineticJ'], Accounting['KineticEndJ']):
                raise ValueError(f'Published kinetic differs from source ledger at frame {Index}')
            if Index > 0 and not Near(Series[Index - 1]['KineticJ'],
                                      Accounting['KineticStartJ']):
                raise ValueError(f'XPBD stage kinetic start does not follow published frame {Index - 1}')
            if not all(NearPublished(Row['MomentumKgMPerS'][Axis],
                                     Accounting['MomentumEndKgMPerS'][Axis])
                       for Axis in range(3)):
                raise ValueError(f'Published momentum differs from source ledger at frame {Index}')
            if Index > 0 and not all(
                    NearPublished(Series[Index - 1]['MomentumKgMPerS'][Axis],
                                  Accounting['MomentumStartKgMPerS'][Axis])
                    for Axis in range(3)):
                raise ValueError(f'XPBD stage momentum start does not follow published frame {Index - 1}')
            if not Accounting['StaticPoseWorkComplete']:
                StaticPoseIncompleteFrames.append(Index)
            if Index > 0 and Accounting['StaticPoseWorkComplete']:
                PhysicalResiduals.append({
                    'Frame': Index,
                    'DeltaTotalJ': Row['ChangeFromPreviousJ'],
                    'PrescribedSurfaceWorkJ': Accounting['PrescribedSurfaceWorkJ'],
                    'MeasuredDampingLossJ': -Accounting['DampingKineticDeltaJ'],
                    'UnclosedPhysicalResidualJ': Row['ChangeFromPreviousJ'] -
                        Accounting['PrescribedSurfaceWorkJ'] -
                        Accounting['DampingKineticDeltaJ']})
    MaximumRawResidual = max((Row['NormalizedRawMomentumResidual']
                              for Row in AccountingRows), default=None)
    return {'Status': 'MEASURED_COMPONENTS_WITH_STAGE_LEDGER' if AccountingSeen else
            'MEASURED_COMPONENTS_UNCLOSED',
            'Initial': Initial, 'Final': Series[-1],
            'PeakTotalAboveInitialJ': max(Row['TotalJ'] - Initial['TotalJ']
                                           for Row in Series),
            'MaximumSingleFrameIncreaseJ': max(Row['ChangeFromPreviousJ']
                                                for Row in Series),
            'AccountingIdentityStatus': 'VERIFIED' if AccountingSeen else 'INCOMPLETE',
            'MaximumNormalizedMomentumResidual':
                max((Row['NormalizedMomentumResidual'] for Row in AccountingRows),
                    default=None),
            'MaximumNormalizedRawMomentumResidual': MaximumRawResidual,
            'DirectRawMomentumClosureLimit': DirectRawMomentumLimit,
            'DirectRawMomentumClosurePass': RawMomentumClosurePass(MaximumRawResidual),
            'MaximumNormalizedUnpinnedStructuralNumericalImpulse':
                max((Row['NormalizedUnpinnedStructuralNumericalImpulse']
                     for Row in AccountingRows), default=None),
            'MaximumNormalizedPinConstraintNumericalImpulse':
                max((Row['NormalizedPinConstraintNumericalImpulse']
                     for Row in AccountingRows), default=None),
            'IdealVelocityReconstructionEnabled': ModeSeen == '1',
            'DoubleShadowEnabled': ModeSeen == '2',
            'VelocityPrecisionMode': ModeSeen,
            'MaximumAbsoluteIntegrationPositionRoundingKineticDeltaJ':
                max((abs(Row['IntegrationPositionRoundingKineticDeltaJ'])
                     for Row in AccountingRows), default=None),
            'MaximumAbsoluteIdealVelocityReconstructionKineticDeltaJ':
                max((abs(Row['IdealVelocityReconstructionKineticDeltaJ'])
                     for Row in AccountingRows), default=None),
            'StaticPoseWorkIncompleteFrames': StaticPoseIncompleteFrames,
            'PhysicalResidualStatus': 'DIAGNOSTIC_UNGATED' if AccountingSeen and
                not StaticPoseIncompleteFrames else 'INCOMPLETE',
            'MaximumAbsoluteUnclosedPhysicalResidualJ':
                max((abs(Row['UnclosedPhysicalResidualJ']) for Row in PhysicalResiduals),
                    default=None),
            'PhysicalResidualsAfterInitialImpulse': PhysicalResiduals,
            'Series': Series,
            'Scope': 'Kinetic from published velocities, gravity and authored distance C²/(2 compliance). Stage identities are bookkeeping, not physical conservation. Frame 0 excludes an unseparated initial impulse. Unclosed physical residual uses measured damping and prescribed kinematic center work for later frames; static pose changes and reciprocal rigid momentum remain incomplete.'}


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
