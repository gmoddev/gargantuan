"""Independently rehash and reduce packaged ContactFixtureV1 corpus evidence."""

import argparse
import gzip
import hashlib
import json
import math
from pathlib import Path
import struct

from ValidateContactTrace import Inspect
from Inspect3DSelfIntersection import Inspect as InspectNonlocalIntersections
from InspectEnergyLedger import Inspect as InspectEnergy


def FloatBits(Value):
    return struct.unpack('<I', struct.pack('<f', Value))[0]


def ExpectedGridFaces(Resolution):
    Faces = []
    for Y in range(Resolution - 1):
        for X in range(Resolution - 1):
            A = Y * Resolution + X
            Faces.extend(([A, A + Resolution, A + Resolution + 1],
                          [A, A + Resolution + 1, A + 1]))
    return Faces


def CheckFixtureFaces(Directory):
    Fixture = json.loads((Directory / 'ContactFixtureV1.json').read_text(encoding='utf-8'))
    Resolution = math.isqrt(len(Fixture['RestPositions']))
    if Resolution * Resolution != len(Fixture['RestPositions']) or \
            Fixture['Triangles'] != ExpectedGridFaces(Resolution):
        raise ValueError(f'{Directory.name}: actual source mesh face topology differs')
    return Fixture


def TraceRows(Directory):
    with gzip.open(Directory / 'ContactTraceV1.jsonl.gz', 'rt', encoding='utf-8') as Stream:
        for Line in Stream:
            yield json.loads(Line)


def VerifyFrames(Directory):
    Expected = json.loads((Directory / 'FrameHashes.json').read_text(encoding='utf-8'))
    Count = 0
    for Count, (Row, Reference) in enumerate(zip(TraceRows(Directory), Expected), 1):
        if Row['Frame'] != Count - 1 or Reference['Frame'] != Count - 1:
            raise ValueError(f'{Directory.name}: frame sequence mismatch')
        for Name in ('Positions', 'Velocities'):
            Values = Row[Name]
            Raw = struct.pack('<' + str(len(Values)) + 'f', *Values)
            if hashlib.sha256(Raw).hexdigest() != Reference[Name + 'Float32Sha256']:
                raise ValueError(f'{Directory.name}: {Name} frame digest mismatch')
    if Count != len(Expected):
        raise ValueError(f'{Directory.name}: missing frame hash')
    return Count


def Mirror(Positive, Negative):
    Result = {'Frames': 0, 'XYBitMismatches': 0,
              'NonzeroZReflectionBitMismatches': 0,
              'MaximumReflectionErrorM': 0.0,
              'ColliderXYBitMismatches': 0,
              'ColliderZReflectionBitMismatches': 0}
    for First, Second in zip(TraceRows(Positive), TraceRows(Negative)):
        if First['Frame'] != Second['Frame'] or \
                len(First['Positions']) != len(Second['Positions']) or \
                len(First['Velocities']) != len(Second['Velocities']):
            raise ValueError('Mirror trace frame or shape mismatch')
        Result['Frames'] += 1
        for Name in ('Positions', 'Velocities'):
            for Index, (A, B) in enumerate(zip(First[Name], Second[Name])):
                Sign = -1 if Index % 3 == 2 else 1
                Result['MaximumReflectionErrorM'] = max(
                    Result['MaximumReflectionErrorM'], abs(Sign * A - B))
                BitsA, BitsB = FloatBits(A), FloatBits(B)
                if Sign == 1:
                    Result['XYBitMismatches'] += BitsA != BitsB
                elif (BitsA & 0x7fffffff) != 0 or (BitsB & 0x7fffffff) != 0:
                    Result['NonzeroZReflectionBitMismatches'] += \
                        (BitsA ^ 0x80000000) != BitsB
        for A, B in zip(First['ColliderStates'], Second['ColliderStates']):
            if A['Id'] != B['Id']:
                raise ValueError('Mirror collider identity mismatch')
            for Name in ('Center', 'LinearVelocity', 'AngularVelocity'):
                for Index in range(3):
                    BitsA, BitsB = FloatBits(A[Name][Index]), FloatBits(B[Name][Index])
                    if Index < 2:
                        Result['ColliderXYBitMismatches'] += BitsA != BitsB
                    elif (BitsA & 0x7fffffff) != 0 or (BitsB & 0x7fffffff) != 0:
                        Result['ColliderZReflectionBitMismatches'] += \
                            (BitsA ^ 0x80000000) != BitsB
    return Result


def InspectCorpus(Root):
    Cases = sorted(Directory for Directory in Root.iterdir()
                   if Directory.is_dir() and Directory.name.startswith('xpbd-'))
    Results = []
    SourcePins = set()
    Fixtures = {}
    for Directory in Cases:
        Fixtures[Directory.name] = CheckFixtureFaces(Directory)
        Verdict = Inspect(Directory)
        Stored = json.loads((Directory / 'Verdict.json').read_text(encoding='utf-8'))
        if Verdict != Stored:
            raise ValueError(f'{Directory.name}: independently reduced verdict differs')
        Frames = VerifyFrames(Directory)
        if Frames != Verdict['Frames']:
            raise ValueError(f'{Directory.name}: frame count differs')
        Source = json.loads((Directory / 'Source.json').read_text(encoding='utf-8'))
        SourcePins.add((Source['Engine'], Source['Native'], Source['Workers'],
                        Source['Compiler']))
        Energy = InspectEnergy(Directory)
        Results.append({'CaseId': Directory.name,
                        'MaximumAxialStrain': Verdict['MaximumAxialStrain'],
                        'MaximumSpherePenetrationM': Verdict['MaximumSpherePenetrationM'],
                        'MinimumTriangleAreaRatio': Verdict['MinimumTriangleAreaRatio'],
                        'GeometryGatePass': Verdict['GeometryGatePass'],
                        'SelfIntersectionStatus': Verdict['SelfIntersectionStatus'],
                        'FullPhysicalQualification': Verdict['FullPhysicalQualification'],
                        'FrameHashCount': Frames,
                        'EnergyStatus': Energy['Status'],
                        'InitialTotalEnergyJ': Energy['Initial']['TotalJ'],
                        'FinalTotalEnergyJ': Energy['Final']['TotalJ'],
                        'PeakTotalAboveInitialJ': Energy['PeakTotalAboveInitialJ'],
                        'MaximumSingleFrameIncreaseJ': Energy['MaximumSingleFrameIncreaseJ']})
    MirrorResults = {}
    for Mode in ('A', 'D0', 'E', 'G'):
        for Offset in ('10um', '1cm'):
            Positive = Root / f'xpbd-{Mode}-plane-plus{Offset}'
            Negative = Root / f'xpbd-{Mode}-plane-minus{Offset}'
            MirrorResults[f'{Mode}-{Offset}'] = Mirror(Positive, Negative)
    NoContact = [Root / f'xpbd-{Mode}-no-contact' for Mode in ('A', 'D0', 'E', 'G')]
    Baseline = json.loads((NoContact[0] / 'FrameHashes.json').read_text(encoding='utf-8'))
    NoContactBitExact = all(
        json.loads((Directory / 'FrameHashes.json').read_text(encoding='utf-8')) == Baseline
        for Directory in NoContact[1:])
    JoltFixtureCount = 0
    for Directory in Root.iterdir():
        if not Directory.is_dir() or not Directory.name.startswith('jolt-'):
            continue
        JoltFixture = CheckFixtureFaces(Directory)
        XpbdName = 'xpbd-A-' + Directory.name.split('-', 2)[2]
        XpbdFixture = Fixtures[XpbdName]
        if JoltFixture['Triangles'] != XpbdFixture['Triangles'] or \
                JoltFixture['RestPositions'] != XpbdFixture['RestPositions']:
            raise ValueError(f'{Directory.name}: Jolt and XPBD authored mesh differs')
        JoltFixtureCount += 1
    NonlocalIntersections = {}
    for Case in Results:
        if Case['CaseId'].startswith('xpbd-E-'):
            Directory = Root / Case['CaseId']
            NonlocalIntersections[Case['CaseId']] = InspectNonlocalIntersections(Directory)
    return {'Schema': 'Gargantuan.ContactIndependentReview.V1',
            'Corpus': Root.name, 'CompletedXpbdCases': len(Results),
            'VerdictsAndFrameHashesRecomputed': True,
            'SourcePins': [list(Pin) for Pin in sorted(SourcePins)],
            'NoContactA_D0_E_GBitExact': NoContactBitExact,
            'ActualGridFaceFixtureCount': len(Cases) + JoltFixtureCount,
            'JoltXpbdRestMeshParityPass': True,
            'MirrorPairs': MirrorResults,
            'Nonlocal3DTriangleChecks': NonlocalIntersections,
            'Cases': Results,
            'Scope': 'Geometry, velocity payload, per-frame digests, disjoint-vertex 3D face pairs and measured kinetic/gravity/distance-compliance energy components only. Shared-vertex or neighboring-face folds, work and reciprocal impulses are not certified.'}


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument('Corpus', type=Path)
    Parser.add_argument('--output', type=Path)
    Args = Parser.parse_args()
    Result = InspectCorpus(Args.Corpus)
    Text = json.dumps(Result, indent=2, sort_keys=True) + '\n'
    if Args.output:
        Args.output.write_text(Text, encoding='utf-8')
    else:
        print(Text, end='')


if __name__ == '__main__':
    Main()
