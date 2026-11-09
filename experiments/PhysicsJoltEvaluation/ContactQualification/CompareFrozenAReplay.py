"""Compare packaged frozen XPBD A against the sealed High32 plane position trace."""

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import struct

from InspectPlanarTriangles import Inspect as InspectPlanar
from ValidateContactTrace import Inspect as InspectV1


PriorSha256 = 'c4d6fa6994d11bd9480db9cbcc6e6c8ea2683b75372d40f296dfdd8469c1f9a2'
Header = struct.Struct('<8sIIIIIf')


def F32(Value):
    return struct.unpack('<f', struct.pack('<f', Value))[0]


def TrajectoryArithmetic():
    DeltaTime = F32(1 / 60)
    Duration = F32(0.66)
    Distance = F32(1.2)
    Start = F32(5.2)
    Velocity = F32(-1.2 / 0.66)
    Differences = []
    for Frame in range(40):
        for Substep in range(32):
            Time = F32(F32(Frame + F32(F32(Substep + 1) / F32(32))) * DeltaTime)
            PriorCenter = F32(Start - F32(Distance * min(F32(Time / Duration), F32(1))))
            NewCenter = F32(Start + F32(Velocity * min(Time, Duration)))
            if struct.pack('<f', PriorCenter) != struct.pack('<f', NewCenter):
                Differences.append({'Frame': Frame, 'Substep': Substep,
                                    'PriorCenterXM': PriorCenter,
                                    'NewCenterXM': NewCenter})
    return {'PriorFormula': 'float32(5.2 - float32(1.2 * min(float32(t / float32(.66)), 1)))',
            'NewFormula': 'float32(5.2 + float32(float32(-1.2/.66) * min(t, float32(.66))))',
            'DifferingSubsteps': len(Differences),
            'MaximumCenterDifferenceM': max((abs(Row['PriorCenterXM'] -
                Row['NewCenterXM']) for Row in Differences), default=0.0),
            'FirstDifference': Differences[0] if Differences else None}


def Compare(PriorPath, CaseDirectory):
    Raw = PriorPath.read_bytes()
    if hashlib.sha256(Raw).hexdigest() != PriorSha256:
        raise ValueError('Prior High32 trace differs from sealed evidence')
    Magic, Version, Bodies, Vertices, Frames, Warmup, DeltaTime = Header.unpack_from(Raw)
    Fixture = json.loads((CaseDirectory / 'ContactFixtureV1.json').read_text(encoding='utf-8'))
    if (Magic, Version, Bodies, Vertices, Frames, Warmup) != \
            (b'PJTRACE1', 1, 1, 128 * 128, 40, 0) or \
            Fixture['Frames'] != Frames or len(Fixture['RestPositions']) != Vertices or \
            len(Raw) != Header.size + Frames * Vertices * 12:
        raise ValueError('High32 replay fixture or trace shape differs')
    Case = json.loads((CaseDirectory / 'Case.json').read_text(encoding='utf-8'))
    if Case['Backend'] != 'xpbd' or Case['Resolution'] != 128 or \
            Case['XpbdSubsteps'] != 32 or Case['InitialSpeedXMPerS'] != 2 or \
            Case['Pins'] != 'top' or len(Case['Spheres']) != 1 or \
            Case['Spheres'][0]['Motion'] != 'static':
        raise ValueError('Packaged case is not the authored High32 plane control')
    FirstDifference = None
    MismatchedBytes = 0
    MaximumErrorM = 0.0
    FrameComparison = []
    with gzip.open(CaseDirectory / 'ContactTraceV1.jsonl.gz', 'rt', encoding='utf-8') as Stream:
        for Frame, Line in enumerate(Stream):
            Row = json.loads(Line)
            if Row['Frame'] != Frame or len(Row['Positions']) != Vertices * 3:
                raise ValueError('Packaged frame identity or vertex count differs')
            Replayed = struct.pack('<' + str(Vertices * 3) + 'f', *Row['Positions'])
            Start = Header.size + Frame * len(Replayed)
            Prior = Raw[Start:Start + len(Replayed)]
            ByteDifferences = sum(A != B for A, B in zip(Replayed, Prior))
            OldValues = struct.unpack('<' + str(Vertices * 3) + 'f', Prior)
            Error = max(abs(A - B) for A, B in zip(Row['Positions'], OldValues))
            MismatchedBytes += ByteDifferences
            MaximumErrorM = max(MaximumErrorM, Error)
            if FirstDifference is None and ByteDifferences:
                FirstDifference = Frame
            FrameComparison.append({'Frame': Frame,
                                    'PositionByteDifferences': ByteDifferences,
                                    'MaximumAbsolutePositionDifferenceM': Error})
    if len(FrameComparison) != Frames:
        raise ValueError('Packaged High32 trace frame count differs')
    OldGeometry = InspectPlanar(PriorPath, PriorSha256)
    NewGeometry = InspectV1(CaseDirectory)
    Source = json.loads((CaseDirectory / 'Source.json').read_text(encoding='utf-8'))
    return {'PriorTraceSha256': PriorSha256,
            'NewTraceSha256': NewGeometry['CompressedTraceSha256'],
            'NewEngine': Source['Engine'], 'NewNative': Source['Native'],
            'NewWorkers': Source['Workers'],
            'Frames': Frames, 'Vertices': Vertices,
            'FirstDifferentFrame': FirstDifference,
            'MismatchedPositionBytes': MismatchedBytes,
            'MaximumAbsolutePositionDifferenceM': MaximumErrorM,
            'PriorMaximumAxialStrain': max(Row['MaximumAxialStrain']
                                           for Row in OldGeometry['FrameSummary']),
            'NewMaximumAxialStrain': NewGeometry['MaximumAxialStrain'],
            'PriorMinimumTriangleAreaRatio': OldGeometry['WorstSignedTriangleArea']['Ratio'],
            'NewMinimumTriangleAreaRatio': NewGeometry['MinimumTriangleAreaRatio'],
            'PriorFirstPlanarInversionFrame': OldGeometry['FirstPlanarInversionFrame'],
            'NewPlanarInversionPass': NewGeometry['PlanarInversionPass'],
            'TrajectoryArithmetic': TrajectoryArithmetic(),
            'FrameComparison': FrameComparison,
            'Scope': 'Same authored 128-square High32 static-drive plane geometry. Prescribed collider float32 arithmetic first differs at frame 7 substep 31, exactly the first position-divergence frame; do not attribute the difference to worker scheduling. Tight numeric parity does not mean byte-exact replay.'}


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument('PriorTrace', type=Path)
    Parser.add_argument('PackagedCase', type=Path)
    Parser.add_argument('--output', type=Path)
    Args = Parser.parse_args()
    Result = Compare(Args.PriorTrace, Args.PackagedCase)
    Text = json.dumps(Result, indent=2, sort_keys=True) + '\n'
    if Args.output:
        Args.output.write_text(Text, encoding='utf-8')
    else:
        print(Text, end='')


if __name__ == '__main__':
    Main()
