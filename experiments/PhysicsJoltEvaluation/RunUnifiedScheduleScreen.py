"""Retain observed schedule quality and verify final-state parity with pristine Jolt."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time


Parser = argparse.ArgumentParser()
Parser.add_argument('--observed', '--instrumented', dest='Observed', type=Path, required=True)
Parser.add_argument('--pristine', type=Path, required=True)
Parser.add_argument('--output', type=Path, required=True)
Parser.add_argument('--frames', type=int, default=150)
Args = Parser.parse_args()
if not 1 <= Args.frames <= 600:
    Parser.error('frames must be between 1 and 600')
Args.output.mkdir(parents=True, exist_ok=False)

Cases = ['RigidIntoCloth', 'Multiple', 'RubberPush', 'SphereOnCloth', 'Repeated']
# The collision-step profiles use one PhysicsSystem::Update with C rigid advances.
# The world-update profiles use U separate PhysicsSystem::Update calls.
Profiles = [
    ('C8I24-c1e-6', 1, 8, 24, '1e-6'),
    ('U192I1-c1e-6', 192, 1, 1, '1e-6'),
    ('C4I48-c1e-6', 1, 4, 48, '1e-6'),
    ('C4I24-c1e-5', 1, 4, 24, '1e-5'),
    ('U96I1-c1e-5', 96, 1, 1, '1e-5'),
]
IgnoredFrameFields = {'StepMs', 'FrameWallMs', 'AcceptedUpdates',
                      'ObservedInternalCallbacks'}
Receipts = []
CreationFlags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def HashFile(PathName):
    Digest = hashlib.sha256()
    with PathName.open('rb') as Stream:
        for Chunk in iter(lambda: Stream.read(1024 * 1024), b''):
            Digest.update(Chunk)
    return Digest.hexdigest()


def PhysicalFrames(Result, Control=False):
    Source = Result['ContactDisabledControl'] if Control else Result
    return [{Key: Value for Key, Value in Frame.items()
             if Key not in IgnoredFrameFields} for Frame in Source['Frames']]


def FrameHash(Frames):
    Payload = json.dumps(Frames, sort_keys=True, separators=(',', ':'), allow_nan=False)
    return hashlib.sha256(Payload.encode('utf-8')).hexdigest()


def FirstDifference(Left, Right, Prefix='$'):
    if isinstance(Left, dict) and isinstance(Right, dict):
        for Key in sorted(set(Left) | set(Right)):
            if Key not in Left or Key not in Right:
                return f'{Prefix}.{Key}: field present on only one side'
            Found = FirstDifference(Left[Key], Right[Key], f'{Prefix}.{Key}')
            if Found:
                return Found
        return None
    if isinstance(Left, list) and isinstance(Right, list):
        if len(Left) != len(Right):
            return f'{Prefix}: lengths {len(Left)} versus {len(Right)}'
        for Index, (A, B) in enumerate(zip(Left, Right)):
            Found = FirstDifference(A, B, f'{Prefix}[{Index}]')
            if Found:
                return Found
        return None
    if Left != Right:
        return f'{Prefix}: {str(Left)[:100]} versus {str(Right)[:100]}'
    return None


def Run(Name, Binary, Options):
    Output = Args.output / f'{Name}.json'
    Error = Args.output / f'{Name}.stderr.txt'
    Started = time.monotonic()
    with Output.open('w', encoding='utf-8') as Stdout, Error.open('w', encoding='utf-8') as Stderr:
        Process = subprocess.run([str(Binary.resolve()), *Options], stdout=Stdout,
                                 stderr=Stderr, timeout=900, creationflags=CreationFlags)
    if Process.returncode not in (0, 3):
        raise RuntimeError(f'{Name}: executable returned {Process.returncode}; see {Error}')
    Result = json.loads(Output.read_text(encoding='utf-8'))
    return Result, {'ExitCode': Process.returncode,
                    'Seconds': time.monotonic() - Started,
                    'OutputSha256': HashFile(Output),
                    'StderrSha256': HashFile(Error)}


def QualitySummary(Result):
    Keys = ('QualityQualified', 'AcceptedQualityQualified',
            'AcceptedPhysicalQualityQualified', 'ReferenceModelGateQualified',
            'ReferenceModelPreconditionsQualified', 'PhysicalQualificationStatus',
            'ContactEvidence', 'RigidResponseQualified', 'MaxAcceptedEdgeStretch',
            'MaxAcceptedPinDrift', 'MaxAcceptedParticlePenetration',
            'MaxAcceptedVolumeError', 'InvertedTetrahedronAcceptedUpdates',
            'InitialAngularInclusiveKineticEnergy', 'MaxAngularInclusiveKineticEnergy',
            'InitialAbsoluteReferenceMechanicalEnergy',
            'MaxAbsoluteReferenceMechanicalEnergy', 'MaximumReferenceModelExcess',
            'FirstReferenceModelFailureFrame', 'FirstReferenceModelFailureUpdate',
            'AcceptedNecessaryEnergyBudget', 'SignedExternalWork',
            'PositiveExternalWork', 'NegativeExternalWork')
    return {Key: Result.get(Key) for Key in Keys}


def ProfileMatches(Result, WorldUpdates, CollisionSteps, SoftIterations, Compliance):
    return (Result.get('WorldUpdatesPerFrame') == WorldUpdates and
            Result.get('CollisionSteps') == CollisionSteps and
            Result.get('SolverIterations') == SoftIterations and
            abs(Result.get('SelectedStretchCompliance', float('nan')) -
                float(Compliance)) <= 1e-11)


for Profile, WorldUpdates, CollisionSteps, SoftIterations, Compliance in Profiles:
    for Case in Cases:
        Name = f'{Case}-{Profile}'
        Base = ['--backend', 'Jolt', '--case', Case, '--frames', str(Args.frames),
                '--workers', '1', '--world-updates', str(WorldUpdates),
                '--collision-steps', str(CollisionSteps), '--soft-iterations',
                str(SoftIterations), '--stretch-compliance', Compliance]
        Observed, ObservedRun = Run(f'{Name}-Observed', Args.Observed,
                                    [*Base, '--observe-accepted', '1'])
        Pristine, PristineRun = Run(f'{Name}-PristineFrameOnly', Args.Pristine,
                                    [*Base, '--observe-accepted', '0'])
        Expected = Args.frames * WorldUpdates * CollisionSteps * SoftIterations
        PerFrame = WorldUpdates * CollisionSteps * SoftIterations
        Checks = {
            'ObservedExitMatchesQuality': (ObservedRun['ExitCode'] == 0) is
                (Observed.get('QualityQualified') is True),
            'PristineTimingExitZero': PristineRun['ExitCode'] == 0,
            'BinariesDeclareObserverMode': Observed.get('EnergyObserverEnabled') is True and
                Observed['ContactDisabledControl'].get('EnergyObserverEnabled') is True and
                Pristine.get('EnergyObserverEnabled') is False and
                Pristine['ContactDisabledControl'].get('EnergyObserverEnabled') is False,
            'ProfileMetadataMatches': all(ProfileMatches(Source, WorldUpdates,
                CollisionSteps, SoftIterations, Compliance) for Source in
                (Observed, Observed['ContactDisabledControl'], Pristine,
                 Pristine['ContactDisabledControl'])),
            'ObservedCountExact': Observed.get('ObservedInternalIntegrations') == Expected and
                Observed['ContactDisabledControl'].get('ObservedInternalIntegrations') == Expected,
            'ObservedFrameCountsExact': all(
                Frame.get('ObservedInternalCallbacks') == PerFrame and
                len(Frame.get('AcceptedUpdates', [])) == PerFrame
                for Source in (Observed, Observed['ContactDisabledControl'])
                for Frame in Source['Frames']),
            'AcceptedObservationCoverage': Observed.get('AcceptedObservationCoverage') is True and
                Observed['ContactDisabledControl'].get('AcceptedObservationCoverage') is True,
            'PristineFrameOnlyCoverage': Pristine.get('AcceptedObservationCoverage') is False and
                Pristine.get('AcceptedQualityQualified') is None and
                Pristine['ContactDisabledControl'].get('AcceptedObservationCoverage') is False,
            'FrameCountExact': all(len(Source['Frames']) == Args.frames for Source in
                (Observed, Observed['ContactDisabledControl'], Pristine,
                 Pristine['ContactDisabledControl'])),
        }
        Parity = {}
        for Control in (False, True):
            Label = 'ContactDisabledControl' if Control else 'ContactEnabled'
            ObservedFrames = PhysicalFrames(Observed, Control)
            PristineFrames = PhysicalFrames(Pristine, Control)
            Difference = FirstDifference(ObservedFrames, PristineFrames)
            Parity[Label] = {
                'Exact': Difference is None,
                'FirstDifference': Difference,
                'ObservedFrameSha256': FrameHash(ObservedFrames),
                'PristineFrameSha256': FrameHash(PristineFrames),
            }
        Checks['PhysicalFrameParity'] = all(Value['Exact'] for Value in Parity.values())
        Receipt = {'Name': Name, 'Profile': {'WorldUpdates': WorldUpdates,
                   'CollisionSteps': CollisionSteps, 'SoftIterations': SoftIterations,
                   'StretchCompliance': Compliance, 'Frames': Args.frames,
                   'ExpectedInternalIntegrations': Expected},
                   'Options': Base, 'ObservedRun': ObservedRun,
                   'PristineFrameOnlyRun': PristineRun, 'Checks': Checks,
                   'Parity': Parity, 'Quality': QualitySummary(Observed),
                   'ControlQuality': QualitySummary(Observed['ContactDisabledControl'])}
        Receipt['HarnessPass'] = all(Checks.values())
        Receipts.append(Receipt)
        (Args.output / 'Receipts.json').write_text(json.dumps(Receipts, indent=2),
                                                   encoding='utf-8')
        print(json.dumps({'Name': Name, 'HarnessPass': Receipt['HarnessPass'],
                          'QualityQualified': Receipt['Quality']['QualityQualified'],
                          'Status': Receipt['Quality']['PhysicalQualificationStatus'],
                          'FirstParityDifference': next((Value['FirstDifference'] for Value in
                              Parity.values() if Value['FirstDifference']), None)}), flush=True)

Verdict = {'Type': 'UnifiedScheduleScreen', 'Cases': len(Receipts),
           'ExpectedCases': len(Profiles) * len(Cases),
           'AllHarnessChecksPass': all(Receipt['HarnessPass'] for Receipt in Receipts),
           'ObservedBinarySha256': HashFile(Args.Observed),
           'PristineBinarySha256': HashFile(Args.Pristine),
           'ScriptSha256': HashFile(Path(__file__)),
           'Scope': 'Harness/parity verdict only; physical quality outcomes are retained independently'}
(Args.output / 'Verdict.json').write_text(json.dumps(Verdict, indent=2), encoding='utf-8')
if not Verdict['AllHarnessChecksPass']:
    raise SystemExit(3)
