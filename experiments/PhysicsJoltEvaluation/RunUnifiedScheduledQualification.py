"""Qualify one observed Jolt schedule and pair its timing with pristine Jolt."""

import argparse
import gzip
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess
import time


Parser = argparse.ArgumentParser()
Parser.add_argument('--observed', type=Path, required=True)
Parser.add_argument('--pristine', type=Path, required=True)
Parser.add_argument('--world-updates', type=int, required=True)
Parser.add_argument('--collision-steps', type=int, required=True)
Parser.add_argument('--soft-iterations', type=int, required=True)
Parser.add_argument('--compliance', type=float, required=True)
Parser.add_argument('--output', type=Path, required=True)
Parser.add_argument('--compress-raw', action='store_true',
                    help='Losslessly gzip completed raw physics JSON after all checks')
Parser.add_argument('--velocity-reconstruction', choices=('difference', 'delta', 'applieddelta'),
                    default='difference')
Args = Parser.parse_args()
if not 1 <= Args.world_updates <= 384 or not 1 <= Args.collision_steps <= 8 or not 1 <= Args.soft_iterations <= 64:
    Parser.error('schedule outside unified runner bounds')
if not math.isfinite(Args.compliance) or Args.compliance <= 0.0:
    Parser.error('qualified compliance must be finite and positive')
Args.output.mkdir(parents=True, exist_ok=False)

Cases = ['RigidIntoCloth', 'Multiple', 'RubberPush', 'SphereOnCloth', 'Repeated']
ExtendedCases = ['SphereOnCloth', 'Repeated', 'RubberPush']
IgnoredFrameFields = {'StepMs', 'FrameWallMs', 'AcceptedUpdates', 'ObservedInternalCallbacks'}
CreationFlags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
Receipts = []
ReferenceHashes = {}


def HashFile(PathName):
    Digest = hashlib.sha256()
    with PathName.open('rb') as Stream:
        for Chunk in iter(lambda: Stream.read(1024 * 1024), b''):
            Digest.update(Chunk)
    return Digest.hexdigest()


def ArchiveRaw(PathName, ExpectedSha256):
    Original = PathName.resolve(strict=True)
    if Original.parent != Args.output.resolve(strict=True):
        raise ValueError(f'Raw physics file is outside output directory: {Original}')
    Archive = Original.with_name(Original.name + '.gz')
    Temporary = Archive.with_name(Archive.name + '.tmp')
    if Archive.exists() or Temporary.exists():
        raise FileExistsError(Archive)
    Digest = hashlib.sha256()
    OriginalBytes = 0
    with Original.open('rb') as Source, Temporary.open('xb') as Destination:
        with gzip.GzipFile(filename='', mode='wb', fileobj=Destination,
                           mtime=0, compresslevel=1) as Compressor:
            for Chunk in iter(lambda: Source.read(1024 * 1024), b''):
                Digest.update(Chunk)
                OriginalBytes += len(Chunk)
                Compressor.write(Chunk)
    OriginalSha256 = Digest.hexdigest()
    if OriginalSha256 != ExpectedSha256:
        raise ValueError(f'Raw physics hash changed before compression: {Original.name}')
    Verify = hashlib.sha256()
    VerifiedBytes = 0
    with gzip.open(Temporary, 'rb') as Source:
        for Chunk in iter(lambda: Source.read(1024 * 1024), b''):
            Verify.update(Chunk)
            VerifiedBytes += len(Chunk)
    if Verify.hexdigest() != OriginalSha256 or VerifiedBytes != OriginalBytes:
        raise ValueError(f'Compressed raw physics failed lossless verification: {Original.name}')
    ArchiveSha256 = HashFile(Temporary)
    ArchiveBytes = Temporary.stat().st_size
    Temporary.rename(Archive)
    Original.unlink()
    return {'OriginalPath': Original.name, 'OriginalSha256': OriginalSha256,
            'OriginalBytes': OriginalBytes, 'ArchivePath': Archive.name,
            'ArchiveSha256': ArchiveSha256, 'ArchiveBytes': ArchiveBytes,
            'Codec': 'gzip', 'GzipMtime': 0, 'LosslessRoundTripVerified': True}


def PhysicalFrames(Result, Control=False, Limit=None):
    Source = Result['ContactDisabledControl'] if Control else Result
    Frames = Source['Frames'] if Limit is None else Source['Frames'][:Limit]
    return [{Key: Value for Key, Value in Frame.items() if Key not in IgnoredFrameFields}
            for Frame in Frames]


def StateHash(Result, Limit=None):
    State = [PhysicalFrames(Result, False, Limit), PhysicalFrames(Result, True, Limit)]
    Payload = json.dumps(State, sort_keys=True, separators=(',', ':'), allow_nan=False)
    return hashlib.sha256(Payload.encode('utf-8')).hexdigest()


def FirstDifference(Left, Right, Prefix='$'):
    if isinstance(Left, dict) and isinstance(Right, dict):
        for Key in sorted(set(Left) | set(Right)):
            if Key not in Left or Key not in Right:
                return f'{Prefix}.{Key}: field present on only one side'
            Difference = FirstDifference(Left[Key], Right[Key], f'{Prefix}.{Key}')
            if Difference:
                return Difference
        return None
    if isinstance(Left, list) and isinstance(Right, list):
        if len(Left) != len(Right):
            return f'{Prefix}: lengths {len(Left)} and {len(Right)}'
        for Index, (A, B) in enumerate(zip(Left, Right)):
            Difference = FirstDifference(A, B, f'{Prefix}[{Index}]')
            if Difference:
                return Difference
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
                                 stderr=Stderr, timeout=1200, creationflags=CreationFlags)
    if Process.returncode not in (0, 3):
        raise RuntimeError(f'{Name}: executable returned {Process.returncode}; see {Error}')
    Result = json.loads(Output.read_text(encoding='utf-8'))
    Receipt = {'ExitCode': Process.returncode, 'Seconds': time.monotonic() - Started,
               'Raw': Output.name,
               'OutputSha256': HashFile(Output), 'StderrSha256': HashFile(Error),
               'FrameStateSha256': StateHash(Result),
               'MeanStepMs': statistics.mean(Frame['StepMs'] for Frame in Result['Frames']),
               'MeanFrameWallMs': statistics.mean(Frame['FrameWallMs'] for Frame in Result['Frames']),
               'Resources': Result.get('Resources')}
    return Result, Receipt


def ProfileMatches(Result, WorldUpdates, CollisionSteps, SoftIterations, Compliance,
                   VelocityReconstruction):
    return (Result.get('WorldUpdatesPerFrame') == WorldUpdates and
            Result.get('CollisionSteps') == CollisionSteps and
            Result.get('SolverIterations') == SoftIterations and
            abs(Result.get('SelectedStretchCompliance', float('nan')) - Compliance) <= 1e-11 and
            Result.get('VelocityReconstruction', 'difference') == VelocityReconstruction)


def FullObservation(Result, Frames, WorldUpdates, CollisionSteps, SoftIterations):
    ExpectedPerFrame = WorldUpdates * CollisionSteps * SoftIterations
    ExpectedTotal = Frames * ExpectedPerFrame
    Sources = (Result, Result['ContactDisabledControl'])
    return all(Source.get('EnergyObserverEnabled') is True and
               Source.get('AcceptedObservationCoverage') is True and
               Source.get('ReferenceModelPreconditionsQualified') is True and
               Source.get('ObservedInternalIntegrations') == ExpectedTotal and
               Source.get('ExpectedInternalIntegrations') == ExpectedTotal and
               len(Source['Frames']) == Frames and
               all(Frame.get('ObservedInternalCallbacks') == ExpectedPerFrame and
                   len(Frame.get('AcceptedUpdates', [])) == ExpectedPerFrame
                   for Frame in Source['Frames']) for Source in Sources)


def CallbackOrderExact(Result, CollisionSteps, SoftIterations):
    PerUpdate = CollisionSteps * SoftIterations
    for FrameNumber, Frame in enumerate(Result['Frames']):
        for Index, State in enumerate(Frame.get('AcceptedUpdates', [])):
            Update = Index // PerUpdate
            Inner = Index % PerUpdate
            Iteration = Inner % SoftIterations
            if (State.get('Frame') != FrameNumber or
                State.get('WorldUpdate') != Update or
                State.get('InternalOrdinalPerWorldUpdate') != Inner or
                State.get('CollisionStepOrdinal') != Inner // SoftIterations or
                State.get('SoftIterationOrdinal') != Iteration or
                State.get('ContextNextIteration') != Iteration + 1):
                return False
    return True


def QualitySummary(Result):
    Keys = ('QualityQualified', 'AcceptedQualityQualified',
            'AcceptedPhysicalQualityQualified', 'ReferenceModelGateQualified',
            'PhysicalQualificationStatus', 'ContactEvidence', 'RigidResponseQualified',
            'MaxAcceptedEdgeStretch', 'MaxAcceptedPinDrift',
            'MaxAcceptedParticlePenetration', 'MaxAcceptedVolumeError',
            'InvertedTetrahedronAcceptedUpdates', 'InitialAngularInclusiveKineticEnergy',
            'MaxAngularInclusiveKineticEnergy', 'InitialAbsoluteReferenceMechanicalEnergy',
            'MaxAbsoluteReferenceMechanicalEnergy', 'MaximumReferenceModelExcess',
            'FirstReferenceModelFailureFrame', 'FirstReferenceModelFailureUpdate',
            'AcceptedNecessaryEnergyBudget', 'SignedExternalWork',
            'PositiveExternalWork', 'NegativeExternalWork')
    return {Key: Result.get(Key) for Key in Keys}


ComplianceText = format(Args.compliance, '.12g')
Base = ['--backend', 'Jolt', '--world-updates', str(Args.world_updates),
        '--collision-steps', str(Args.collision_steps), '--soft-iterations',
        str(Args.soft_iterations), '--stretch-compliance', ComplianceText]
ModeOptions = [] if Args.velocity_reconstruction == 'difference' else [
    '--velocity-reconstruction', Args.velocity_reconstruction]
Base += ModeOptions


def RunPair(Name, Case, Workers, Frames, Repeat=None, Extended=False):
    Options = [*Base, '--case', Case, '--workers', str(Workers), '--frames', str(Frames)]
    Observed, ObservedRun = Run(f'{Name}-Observed', Args.observed,
                                [*Options, '--observe-accepted', '1'])
    Pristine, PristineRun = Run(f'{Name}-PristineFrameOnly', Args.pristine,
                                [*Options, '--observe-accepted', '0'])
    ObservedState = [PhysicalFrames(Observed), PhysicalFrames(Observed, True)]
    PristineState = [PhysicalFrames(Pristine), PhysicalFrames(Pristine, True)]
    Difference = FirstDifference(ObservedState, PristineState)
    Hash = ObservedRun['FrameStateSha256']
    if Case not in ReferenceHashes and not Extended:
        ReferenceHashes[Case] = Hash
    Checks = {
        'ObservedExitMatchesQuality': (ObservedRun['ExitCode'] == 0) is
            (Observed.get('QualityQualified') is True),
        'ObservedQualityPass': Observed.get('QualityQualified') is True and
            Observed.get('ReferenceModelGateQualified') is True,
        'ContactDisabledReferenceModelPass':
            Observed['ContactDisabledControl'].get('ReferenceModelGateQualified') is True,
        'FullInternalObservation': FullObservation(Observed, Frames,
            Args.world_updates, Args.collision_steps, Args.soft_iterations),
        'InternalCallbackOrderExact': all(CallbackOrderExact(Source,
            Args.collision_steps, Args.soft_iterations) for Source in
            (Observed, Observed['ContactDisabledControl'])),
        'ProfileMetadataMatches': all(ProfileMatches(Source, Args.world_updates,
            Args.collision_steps, Args.soft_iterations, Args.compliance,
            Args.velocity_reconstruction) for Source in
            (Observed, Observed['ContactDisabledControl'], Pristine,
             Pristine['ContactDisabledControl'])),
        'PristineFrameOnly': PristineRun['ExitCode'] == 0 and
            Pristine.get('EnergyObserverEnabled') is False and
            Pristine.get('AcceptedObservationCoverage') is False and
            Pristine.get('AcceptedQualityQualified') is None and
            Pristine['ContactDisabledControl'].get('AcceptedObservationCoverage') is False,
        'ExactObservedPristineFrameParity': Difference is None and
            Hash == PristineRun['FrameStateSha256'],
        'SamePhysicalStateAcrossRepeatsAndWorkers':
            True if Extended else Hash == ReferenceHashes[Case],
    }
    if Extended:
        Checks['ExtendedPrefixMatchesQualified150'] = (
            StateHash(Observed, 150) == ReferenceHashes[Case])
    Receipt = {'Name': Name, 'Case': Case, 'Workers': Workers, 'Repeat': Repeat,
               'Frames': Frames, 'Options': Options,
               'ObservedRun': ObservedRun, 'PristineFrameOnlyRun': PristineRun,
               'Checks': Checks, 'FirstPhysicalParityDifference': Difference,
               'Quality': QualitySummary(Observed),
               'ContactDisabledQuality': QualitySummary(Observed['ContactDisabledControl'])}
    Receipt['QualificationPass'] = all(Checks.values())
    if Args.compress_raw:
        ObservedArchive = ArchiveRaw(Args.output / ObservedRun['Raw'], ObservedRun['OutputSha256'])
        PristineArchive = ArchiveRaw(Args.output / PristineRun['Raw'], PristineRun['OutputSha256'])
        ObservedRun['RawArchive'] = ObservedArchive
        PristineRun['RawArchive'] = PristineArchive
        ObservedRun['Raw'] = ObservedArchive['ArchivePath']
        PristineRun['Raw'] = PristineArchive['ArchivePath']
    Receipts.append(Receipt)
    (Args.output / 'Receipts.json').write_text(json.dumps(Receipts, indent=2),
                                               encoding='utf-8')
    print(json.dumps({'Name': Name, 'Pass': Receipt['QualificationPass'],
                      'Quality': Receipt['Quality']['PhysicalQualificationStatus'],
                      'ParityDifference': Difference}), flush=True)


for Case in Cases:
    for Workers in (1, 2, 8):
        for Repeat in (1, 2, 3):
            RunPair(f'{Case}-T{Workers}-R{Repeat}', Case, Workers, 150, Repeat)

for Case in ExtendedCases:
    RunPair(f'{Case}-Extended600', Case, 1, 600, Extended=True)

HardOptions = [*Base, '--case', 'RigidIntoCloth', '--workers', '1',
               '--frames', '1', '--stretch-compliance', '0', '--observe-accepted', '1']
Hard, HardRun = Run('HardConstraintReferenceReject', Args.observed, HardOptions)
HardChecks = {'ExpectedHardConstraintRejection': HardRun['ExitCode'] == 3 and
              Hard.get('QualityQualified') is False and
              Hard.get('ReferenceModelPreconditionsQualified') is False and
              Hard.get('NonpositiveActiveConstraints', 0) > 0 and
              Hard.get('ReferenceModelGateQualified') is False,
              'CompleteObserverCoverage': FullObservation(Hard, 1, Args.world_updates,
                  Args.collision_steps, Args.soft_iterations) is False and
                  Hard.get('AcceptedObservationCoverage') is True,
              'VelocityReconstructionMatches': all(Source.get('VelocityReconstruction', 'difference') ==
                  Args.velocity_reconstruction for Source in (Hard, Hard['ContactDisabledControl']))}
# FullObservation intentionally rejects the hard control because its material
# precondition is false; callback counts are checked separately here.
ExpectedHardCallbacks = Args.world_updates * Args.collision_steps * Args.soft_iterations
HardChecks['HardControlObserverCount'] = all(Source.get('ObservedInternalIntegrations') ==
    ExpectedHardCallbacks for Source in (Hard, Hard['ContactDisabledControl']))
Receipts.append({'Name': 'HardConstraintReferenceReject', 'Options': HardOptions,
                 'ObservedRun': HardRun, 'Checks': HardChecks,
                 'QualificationPass': all(HardChecks.values()),
                 'Quality': QualitySummary(Hard)})

CoverageOptions = ['--backend', 'Jolt', '--case', 'RigidIntoCloth', '--workers', '1',
                   '--frames', '1', '--world-updates', '1', '--collision-steps', '1',
                   '--soft-iterations', '2', '--stretch-compliance', ComplianceText,
                   '--observe-accepted', '1', *ModeOptions]
Coverage, CoverageRun = Run('OmittedInternalEndpointReject', Args.pristine,
                            CoverageOptions)
CoverageChecks = {'ExpectedCoverageRejection': CoverageRun['ExitCode'] == 3 and
                  Coverage.get('EnergyObserverEnabled') is False and
                  Coverage.get('AcceptedObservationCoverage') is False and
                  Coverage.get('WorldUpdateObservationCoverage') is True and
                  Coverage.get('ReferenceModelGateQualified') is None and
                  Coverage.get('AcceptedQualityQualified') is None and
                  Coverage.get('QualityQualified') is False,
                  'VelocityReconstructionMatches': all(Source.get('VelocityReconstruction', 'difference') ==
                      Args.velocity_reconstruction for Source in (Coverage, Coverage['ContactDisabledControl']))}
Receipts.append({'Name': 'OmittedInternalEndpointReject', 'Options': CoverageOptions,
                 'PristineRun': CoverageRun, 'Checks': CoverageChecks,
                 'QualificationPass': all(CoverageChecks.values()),
                 'Quality': QualitySummary(Coverage)})
(Args.output / 'Receipts.json').write_text(json.dumps(Receipts, indent=2),
                                           encoding='utf-8')

Verdict = {'Type': 'UnifiedScheduledQualification',
           'VelocityReconstruction': Args.velocity_reconstruction,
           'RawPhysicsCompression': 'gzip-mtime0-lossless-verified' if Args.compress_raw else 'none',
           'QualifiedProfile': {'WorldUpdates': Args.world_updates,
                                'CollisionSteps': Args.collision_steps,
                                'SoftIterations': Args.soft_iterations,
                                'StretchCompliance': Args.compliance},
           'PairedCases': len(Cases) * 3 * 3,
           'ExtendedCases': len(ExtendedCases),
           'NegativeControls': 2,
           'AllRequiredChecksPass': all(Receipt['QualificationPass'] for Receipt in Receipts),
           'ObservedBinarySha256': HashFile(Args.observed),
           'PristineBinarySha256': HashFile(Args.pristine),
           'ScriptSha256': HashFile(Path(__file__)),
           'Scope': 'Only the specified material and world schedule; timing uses physically paired pristine frame-only runs'}
(Args.output / 'Verdict.json').write_text(json.dumps(Verdict, indent=2), encoding='utf-8')
if not Verdict['AllRequiredChecksPass']:
    raise SystemExit(3)
