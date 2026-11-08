"""Narrow read-only C4/I48 versus C8/I24 contact-phase mechanism trace.

Runs the previously built S1 observational Jolt diagnostic and its clean twin.
Only frames 17–18 are traced. The ledger separates prediction, ordered edges,
position-only contact projection, reconstruction, and dynamic impulse pairs.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import time


Parser = argparse.ArgumentParser()
Parser.add_argument('--instrumented', type=Path, required=True)
Parser.add_argument('--clean', type=Path, required=True)
Parser.add_argument('--output', type=Path, required=True)
Parser.add_argument('--reference', type=Path,
                    help='Optional prior schedule-energy/Runs directory for scalar cross-checks')
Args = Parser.parse_args()
Args.output.mkdir(parents=True, exist_ok=False)
CreationFlags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
Profiles = [('C4I48', 4, 48), ('C8I24', 8, 24)]
Cases = [('C1', 1), ('C2', 2)]


def HashFile(PathName):
    Digest = hashlib.sha256()
    with PathName.open('rb') as Stream:
        for Chunk in iter(lambda: Stream.read(1024 * 1024), b''):
            Digest.update(Chunk)
    return Digest.hexdigest()


def ReadJsonLines(PathName, Kind=None):
    with PathName.open(encoding='utf-8') as Stream:
        for Line in Stream:
            Row = json.loads(Line)
            if Kind is None or Row.get('Type') == Kind:
                yield Row


def PhysicalFrames(PathName):
    return [{Key: Value for Key, Value in Row.items() if Key != 'StepMs'}
            for Row in ReadJsonLines(PathName, 'Frame')]


def VectorSquare(Value):
    return sum(float(X) * float(X) for X in Value)


def ImpulseEnergy(Row, SphereInertia):
    ParticleMass = 1.0 / Row['ParticleInverseMass']
    RigidMass = 1.0 / Row['RigidInverseMass']
    Particle = 0.5 * ParticleMass * (
        VectorSquare(Row['ParticleVelocityAfterImpulse']) -
        VectorSquare(Row['ParticleVelocityBeforeImpulse']))
    RigidLinear = 0.5 * RigidMass * (
        VectorSquare(Row['RigidVelocityAfter']) -
        VectorSquare(Row['RigidVelocityBefore']))
    RigidAngular = 0.5 * SphereInertia * (
        VectorSquare(Row['RigidAngularAfter']) -
        VectorSquare(Row['RigidAngularBefore']))
    return Particle, RigidLinear + RigidAngular


def AnalyzeTrace(PathName, Iterations, SphereInertia):
    Ledgers = []
    Current = None
    PendingProjection = None
    FrameOrdinal = {}
    for Row in ReadJsonLines(PathName):
        Kind = Row['Type']
        if Kind == 'Stage' and Row['Phase'] == 'BeforeIntegration':
            if Current is not None:
                raise ValueError('Incomplete previous integration trace')
            Frame = Row['Frame']
            Ordinal = FrameOrdinal.get(Frame, 0)
            FrameOrdinal[Frame] = Ordinal + 1
            Current = {'Frame': Frame, 'SolverOrdinal': Ordinal,
                       'CollisionStep': Ordinal // Iterations,
                       'SoftIteration': Ordinal % Iterations,
                       'Stages': {'BeforeIntegration': Row},
                       'StartSoftKPlusU': Row['SoftKE'] +
                           Row['CompliantEdgeElasticEstimate'],
                       'Events': [],
                       'ReconstructionK': 0.0, 'ReconstructionCount': 0,
                       'ProjectionU': 0.0, 'ProjectionCount': 0,
                       'LargestProjectionU': None,
                       'DynamicParticleK': 0.0, 'DynamicRigidK': 0.0,
                       'DynamicContactCount': 0,
                       'LargestPairImpulseK': None}
            continue
        if Current is None:
            raise ValueError(f'Event without integration stage: {Kind}')
        if Kind == 'Stage':
            Phase = Row['Phase']
            Current['Stages'][Phase] = Row
            if Phase == 'AfterIntegration':
                Before = Current['Stages']['BeforeIntegration']
                Current['Events'].append({
                    'Operation': 'Integration',
                    'DeltaK': Row['SoftKE'] - Before['SoftKE'],
                    'DeltaU': Row['CompliantEdgeElasticEstimate'] -
                        Before['CompliantEdgeElasticEstimate']})
            elif Phase == 'BeforeCollision':
                Integrated = Current['Stages']['AfterIntegration']
                Current['Events'].append({
                    'Operation': 'OrderedEdgeSweep', 'DeltaK': 0.0,
                    'DeltaU': Row['CompliantEdgeElasticEstimate'] -
                        Integrated['CompliantEdgeElasticEstimate'],
                    'CandidateKDelta': Row['ReconstructedCandidateKE'] -
                        Integrated['ReconstructedCandidateKE']})
            if Phase != 'AfterCollision':
                continue
            Stages = Current.pop('Stages')
            if set(Stages) != {'BeforeIntegration', 'AfterIntegration',
                               'BeforeCollision', 'AfterCollision'}:
                raise ValueError('Incomplete stage quartet')
            Before = Stages['BeforeIntegration']
            Integrated = Stages['AfterIntegration']
            Edged = Stages['BeforeCollision']
            After = Stages['AfterCollision']
            for Name, State in Stages.items():
                if (State['Frame'] != Current['Frame'] or
                    State['InternalIteration'] != Current['SoftIteration'] + 1):
                    raise ValueError(f'Wrong stage order at {Name}')
            Current['IntegrationCandidateKPlusUDelta'] = (
                Integrated['ReconstructedCandidateKE'] +
                Integrated['CompliantEdgeElasticEstimate'] -
                Before['SoftKE'] - Before['CompliantEdgeElasticEstimate'])
            Current['EdgeCandidateKDelta'] = (
                Edged['ReconstructedCandidateKE'] -
                Integrated['ReconstructedCandidateKE'])
            Current['EdgeUDelta'] = (
                Edged['CompliantEdgeElasticEstimate'] -
                Integrated['CompliantEdgeElasticEstimate'])
            Current['EdgeCandidateKPlusUDelta'] = (
                Current['EdgeCandidateKDelta'] + Current['EdgeUDelta'])
            Current['CollisionKPlusUDeltaIncludingCachedRigid'] = (
                After['SoftKE'] + After['CompliantEdgeElasticEstimate'] +
                Current['DynamicRigidK'] -
                Edged['ReconstructedCandidateKE'] -
                Edged['CompliantEdgeElasticEstimate'])
            Current['ReconstructionClosure'] = (
                Edged['ReconstructedCandidateKE'] - Edged['SoftKE'] -
                Current['ReconstructionK'])
            Current['ContactPositionUClosure'] = (
                After['CompliantEdgeElasticEstimate'] -
                Edged['CompliantEdgeElasticEstimate'] - Current['ProjectionU'])
            Current['ContactVelocityClosure'] = (
                After['SoftKE'] - Edged['ReconstructedCandidateKE'] -
                Current['DynamicParticleK'])
            Current['AfterCollisionSoftKE'] = After['SoftKE']
            Current['AfterCollisionU'] = After['CompliantEdgeElasticEstimate']
            Current['AfterCollisionCandidateKE'] = After['ReconstructedCandidateKE']
            Current['AfterCollisionMaxStrain'] = After['AllEdgeStrain']
            Current['AfterCollisionPinDrift'] = After['MaxPinDrift']
            Current['TotalLocalCandidateKPlusUDelta'] = (
                Current['IntegrationCandidateKPlusUDelta'] +
                Current['EdgeCandidateKPlusUDelta'] +
                Current['CollisionKPlusUDeltaIncludingCachedRigid'])
            Current['EndSoftKPlusU'] = (After['SoftKE'] +
                                        After['CompliantEdgeElasticEstimate'])
            Current['OperationClosure'] = (
                Current['EndSoftKPlusU'] + Current['DynamicRigidK'] -
                Current['StartSoftKPlusU'] -
                sum(Event['DeltaK'] + Event['DeltaU']
                    for Event in Current['Events']))
            Ledgers.append(Current)
            Current = None
            continue
        if Kind == 'Reconstruction':
            Current['ReconstructionK'] += Row['KineticChange']
            Current['ReconstructionCount'] += 1
            Current['Events'].append({
                'Operation': 'VelocityReconstruction',
                'Vertex': Row['Vertex'],
                'DeltaK': Row['KineticChange'], 'DeltaU': 0.0})
        elif Kind == 'ContactProjection':
            if Row['Phase'] == 'Before':
                if PendingProjection is not None:
                    raise ValueError('Overlapping contact projection records')
                PendingProjection = Row
            elif Row['Phase'] == 'After':
                Before = PendingProjection
                if (Before is None or Before['Vertex'] != Row['Vertex'] or
                    Before['Frame'] != Row['Frame']):
                    raise ValueError('Unpaired contact projection')
                DeltaU = (Row['CompliantEdgeElasticEstimate'] -
                          Before['CompliantEdgeElasticEstimate'])
                Current['ProjectionU'] += DeltaU
                Current['ProjectionCount'] += 1
                Current['Events'].append({
                    'Operation': 'ContactPositionProjection',
                    'Vertex': Row['Vertex'], 'DeltaK': 0.0,
                    'DeltaU': DeltaU, 'PlaneDepth': Row['PlaneDepth'],
                    'TrueGapBefore': Before['TrueFrozenSphereGap'],
                    'TrueGapAfter': Row['TrueFrozenSphereGap'],
                    'RigidFrozenCenter': Row['RigidFrozenCenter']})
                Largest = Current['LargestProjectionU']
                if Largest is None or DeltaU > Largest['DeltaU']:
                    Current['LargestProjectionU'] = {
                        'Vertex': Row['Vertex'], 'DeltaU': DeltaU,
                        'PlaneDepth': Row['PlaneDepth'],
                        'TrueGapBefore': Before['TrueFrozenSphereGap'],
                        'TrueGapAfter': Row['TrueFrozenSphereGap'],
                        'RigidFrozenCenter': Row['RigidFrozenCenter']}
                PendingProjection = None
            else:
                raise ValueError('Unknown contact projection phase')
        elif Kind == 'DynamicContact':
            Particle, Rigid = ImpulseEnergy(Row, SphereInertia)
            Current['DynamicParticleK'] += Particle
            Current['DynamicRigidK'] += Rigid
            Current['DynamicContactCount'] += 1
            Current['Events'].append({
                'Operation': 'DynamicContactImpulse',
                'DeltaK': Particle + Rigid, 'DeltaU': 0.0,
                'ParticleDeltaK': Particle, 'CachedRigidDeltaK': Rigid,
                'ProjectionDepth': Row['ProjectionDepth']})
            Pair = Particle + Rigid
            Largest = Current['LargestPairImpulseK']
            if Largest is None or Pair > Largest['PairDeltaK']:
                Current['LargestPairImpulseK'] = {
                    'PairDeltaK': Pair, 'ParticleDeltaK': Particle,
                    'RigidDeltaK': Rigid, 'ProjectionDepth': Row['ProjectionDepth']}
        else:
            raise ValueError(f'Unexpected trace type {Kind}')
    if Current is not None or PendingProjection is not None:
        raise ValueError('Incomplete final trace')
    return Ledgers


def Run(Name, Binary, Options):
    Output = Args.output / f'{Name}.jsonl'
    Error = Args.output / f'{Name}.stderr.txt'
    Started = time.monotonic()
    with Output.open('w', encoding='utf-8') as Stdout, Error.open('w', encoding='utf-8') as Stderr:
        Process = subprocess.run([str(Binary.resolve()), *Options], stdout=Stdout,
                                 stderr=Stderr, timeout=600,
                                 creationflags=CreationFlags)
    if Process.returncode != 0:
        raise RuntimeError(f'{Name}: exit {Process.returncode}; see {Error}')
    return {'ExitCode': Process.returncode, 'Seconds': time.monotonic() - Started,
            'OutputSha256': HashFile(Output), 'StderrSha256': HashFile(Error)}, Output


Receipts = []
for Profile, Steps, Iterations in Profiles:
    for Case, Count in Cases:
        Name = f'{Case}-{Profile}'
        Trace = Args.output / f'{Name}.trace.jsonl'
        Options = ['--frames', '20', '--steps', str(Steps),
                   '--iterations', str(Iterations), '--count', str(Count),
                   '--workers', '1', '--compliance', '1e-6',
                   '--bend-compliance', '1e-4', '--damping', '2.4',
                   '--friction', '0.2', '--restitution', '0',
                   '--contact', '1', '--dynamic', '1', '--pinned', '1',
                   '--seed', '0', '--speed', '5', '--particles', '0']
        CleanRun, CleanOut = Run(f'{Name}-Clean', Args.clean, Options)
        ObservedRun, ObservedOut = Run(f'{Name}-Observed', Args.instrumented,
                                       [*Options, '--trace', str(Trace),
                                        '--trace-start', '17', '--trace-end', '18'])
        CleanFrames = PhysicalFrames(CleanOut)
        ObservedFrames = PhysicalFrames(ObservedOut)
        ExactParity = CleanFrames == ObservedFrames and len(CleanFrames) == 20
        Config = next(ReadJsonLines(ObservedOut, 'Config'))
        Ledger = AnalyzeTrace(Trace, Iterations, Config['SphereInertia'])
        ExpectedStages = 2 * Steps * Iterations
        if len(Ledger) != ExpectedStages:
            raise ValueError(f'{Name}: {len(Ledger)} stage quartets, expected {ExpectedStages}')
        LedgerFile = Args.output / f'{Name}.ledger.json'
        LedgerFile.write_text(json.dumps(Ledger, indent=2), encoding='utf-8')
        FirstContact = next(({'Frame': Row['Frame'], 'SolverOrdinal': Row['SolverOrdinal'],
                              'CollisionStep': Row['CollisionStep'],
                              'SoftIteration': Row['SoftIteration']}
                             for Row in Ledger if Row['ProjectionCount'] or
                             Row['DynamicContactCount']), None)
        Receipt = {'Name': Name, 'Options': Options,
                   'CleanRun': CleanRun, 'ObservedRun': ObservedRun,
                   'TraceSha256': HashFile(Trace),
                   'LedgerSha256': HashFile(LedgerFile),
                   'FrameParity': ExactParity,
                   'StageQuartets': len(Ledger),
                   'ExpectedStageQuartets': ExpectedStages,
                   'FirstTracedContact': FirstContact,
                   'MaxAbsReconstructionClosure': max(abs(Row['ReconstructionClosure'])
                       for Row in Ledger),
                   'MaxAbsPositionUClosure': max(abs(Row['ContactPositionUClosure'])
                       for Row in Ledger),
                   'MaxAbsVelocityClosure': max(abs(Row['ContactVelocityClosure'])
                       for Row in Ledger),
                   'MaxAbsOperationClosure': max(abs(Row['OperationClosure'])
                       for Row in Ledger),
                   'MaxEdgeCandidateKPlusU': max(Row['EdgeCandidateKPlusUDelta']
                       for Row in Ledger),
                   'MaxProjectionU': max(Row['ProjectionU'] for Row in Ledger),
                   'MaxPairImpulseK': max((Row['LargestPairImpulseK'] or
                       {'PairDeltaK': -math.inf})['PairDeltaK'] for Row in Ledger)}
        if Receipt['MaxPairImpulseK'] == -math.inf:
            Receipt['MaxPairImpulseK'] = None
        if Args.reference:
            Prefix = f'M1e-6-S{Steps}I{Iterations}W1-{Case}'
            ReferenceRows = [Row for Row in ReadJsonLines(
                Args.reference / f'{Prefix}.solver.jsonl', 'SolverMicrostep')
                if Row['Frame'] in (17, 18)]
            ReferenceConfig = next(ReadJsonLines(
                Args.reference / f'{Prefix}.observed.jsonl', 'Config'))
            Limit = 1.05 * (ReferenceConfig['InitialKineticEnergy'] +
                            ReferenceConfig['InitialCompliantEdgeElasticEstimate']) + 1e-5
            FirstFailure = next(({'Frame': Row['Frame'],
                                  'SolverOrdinal': Row['SolverOrdinal'],
                                  'KineticEnergy': Row['KineticEnergy'],
                                  'CompliantEdgeElasticEstimate':
                                      Row['CompliantEdgeElasticEstimate'],
                                  'KPlusEdgeModelEnergy': Row['KPlusEdgeModelEnergy']}
                                 for Row in ReferenceRows
                                 if Row['KPlusEdgeModelEnergy'] > Limit), None)
            SameOrdinal = len(ReferenceRows) == len(Ledger) and all(
                A['Frame'] == B['Frame'] and A['SolverOrdinal'] == B['SolverOrdinal']
                for A, B in zip(Ledger, ReferenceRows))
            MaxSoftKDifference = max((abs(A['AfterCollisionSoftKE'] - B['SoftKE'])
                                      for A, B in zip(Ledger, ReferenceRows)), default=math.inf)
            MaxUDifference = max((abs(A['AfterCollisionU'] -
                                      B['CompliantEdgeElasticEstimate'])
                                  for A, B in zip(Ledger, ReferenceRows)), default=math.inf)
            Receipt['PriorScheduleReference'] = {
                'Rows': len(ReferenceRows), 'OrdinalsMatch': SameOrdinal,
                'MaxSoftKDifferenceJ': MaxSoftKDifference,
                'MaxUDifferenceJ': MaxUDifference,
                'ScalarParity': SameOrdinal and MaxSoftKDifference == 0 and
                    MaxUDifference == 0,
                'ReferenceModelLimitJ': Limit,
                'FirstReferenceModelFailure': FirstFailure}
        Receipts.append(Receipt)
        (Args.output / 'Receipts.json').write_text(json.dumps(Receipts, indent=2),
                                                   encoding='utf-8')
        print(json.dumps({'Name': Name, 'Parity': ExactParity,
                          'FirstTracedContact': FirstContact,
                          'MaxProjectionU': Receipt['MaxProjectionU']}), flush=True)

Verdict = {'Cases': len(Receipts), 'AllFrameParity': all(R['FrameParity'] for R in Receipts),
           'AllStageCountsExact': all(R['StageQuartets'] == R['ExpectedStageQuartets']
                                     for R in Receipts),
           'CleanBinarySha256': HashFile(Args.clean),
           'InstrumentedBinarySha256': HashFile(Args.instrumented),
           'ScriptSha256': HashFile(Path(__file__)),
           'Scope': 'Operation-level diagnostic ledger; no timing claim or production change'}
(Args.output / 'Verdict.json').write_text(json.dumps(Verdict, indent=2), encoding='utf-8')
if not Verdict['AllFrameParity'] or not Verdict['AllStageCountsExact']:
    raise SystemExit(3)
