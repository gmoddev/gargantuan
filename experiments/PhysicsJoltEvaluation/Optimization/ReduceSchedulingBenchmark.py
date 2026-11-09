"""Validate and reduce same-binary single-world scheduling receipts."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics


def ReadRun(Directory: Path, Mode: str, Workers: int, Repeat: int, Frames: int):
    Name = f'Schedule-{Mode}-W{Workers}-R{Repeat}'
    Stdout = (Directory / f'{Name}.jsonl').read_bytes()
    Stderr = (Directory / f'{Name}.stderr').read_bytes()
    Receipt = json.loads((Directory / f'{Name}.receipt.json').read_text(encoding='utf-8'))
    if Receipt.get('ExitCode') != 0 or Receipt.get('StdoutSha256') != hashlib.sha256(Stdout).hexdigest() or \
            Receipt.get('StderrSha256') != hashlib.sha256(Stderr).hexdigest() or Stderr:
        raise ValueError(f'{Name}: process, byte digest or stderr failure')
    Lines = Stdout.decode('utf-8').splitlines()
    if len(Lines) != 1:
        raise ValueError(f'{Name}: expected one JSON result line')
    Row = json.loads(Lines[0])
    if Row.get('Mode') != Mode or Row.get('Workers') != Workers or Row.get('Frames') != Frames:
        raise ValueError(f'{Name}: result identity mismatch')
    Command = Receipt.get('Command', [])
    if not Command or not all(Item in Command for Item in ('--mode', Mode, '--workers', str(Workers),
                                           '--frames', str(Frames))):
        raise ValueError(f'{Name}: command identity mismatch')
    Numeric = ('TotalUpdateMs', 'UpdateP95Ms', 'MaximumFloorPenetration',
               'MaximumPairPenetration')
    if any(not isinstance(Row.get(Key), (int, float)) or
           not math.isfinite(Row[Key]) for Key in Numeric):
        raise ValueError(f'{Name}: nonfinite or missing quality/cost value')
    if not Row.get('QualityPass') or not Row.get('FiniteEveryFrame') or \
            Row['TotalUpdateMs'] <= 0 or Row['UpdateP95Ms'] <= 0 or \
            Row.get('NativeContactPairFrames', 0) < max(1, Frames // 4) or \
            Row.get('NativeContactPairsAcrossFrames', 0) <= 0 or \
            Row['MaximumFloorPenetration'] > 0.25 or Row['MaximumPairPenetration'] > 0.25:
        raise ValueError(f'{Name}: physical quality gate failed')
    for Key in ('UpdateP50Ms', 'UpdateP99Ms', 'UpdateMaxMs'):
        if Key in Row and (not isinstance(Row[Key], (int, float)) or
                           not math.isfinite(Row[Key]) or Row[Key] <= 0):
            raise ValueError(f'{Name}: invalid optional frame latency {Key}')
    if all(Key in Row for Key in ('UpdateP50Ms', 'UpdateP99Ms', 'UpdateMaxMs')):
        if not Row['UpdateP50Ms'] <= Row['UpdateP95Ms'] <= Row['UpdateP99Ms'] <= Row['UpdateMaxMs']:
            raise ValueError(f'{Name}: frame latency percentiles out of order')
    Row['_BinaryPath'] = Command[0]
    return Row


def Reduce(Directory: Path, Frames: int):
    SourceBytes = (Directory / 'Source.json').read_bytes()
    CacheBytes = (Directory / 'CMakeCache.txt').read_bytes()
    Source = json.loads(SourceBytes)
    GroupBatch = Source.get('LargeClothGroupBatch')
    if Source.get('Profile') is not False or GroupBatch not in (256, 512, 1024) or \
            not Source.get('Engine') or not Source.get('Jolt'):
        raise ValueError('Source is not an unprofiled bounded-group candidate')
    Cache = dict(Line.split('=', 1) for Line in CacheBytes.decode('utf-8').splitlines()
                 if '=' in Line and not Line.startswith('//'))
    ExpectedCache = {'CMAKE_BUILD_TYPE:STRING': 'Release',
        'PHYSICS_EVALUATION_GANTRIA_NUMERICS:BOOL': 'ON',
        'PHYSICS_EVALUATION_GANTRIA_ATTACHMENTS:BOOL': 'ON',
        'PHYSICS_EVALUATION_GANTRIA_CONVERGENCE:BOOL': 'ON',
        'PHYSICS_EVALUATION_GANTRIA_SLEEP_FIX:BOOL': 'ON',
        'PHYSICS_EVALUATION_GANTRIA_GROUPING:BOOL': 'ON',
        'PHYSICS_EVALUATION_LARGE_CLOTH_GROUP_BATCH:STRING': str(GroupBatch),
        'PHYSICS_EVALUATION_JOLT_COMMIT:STRING': Source['Jolt'],
        'PHYSICS_EVALUATION_NATIVE_PROFILE:BOOL': 'OFF',
        'JPH_USE_EXTERNAL_PROFILE:BOOL': 'OFF',
        'PROFILER_IN_DEBUG_AND_RELEASE:BOOL': 'OFF',
        'PROFILER_IN_DISTRIBUTION:BOOL': 'OFF'}
    if any(Cache.get(Key) != Value for Key, Value in ExpectedCache.items()):
        raise ValueError('CMakeCache disagrees with the unprofiled candidate source')
    Results = {'Source': Source, 'SourceSha256': hashlib.sha256(SourceBytes).hexdigest(),
        'CMakeCacheSha256': hashlib.sha256(CacheBytes).hexdigest(),
        'Frames': Frames, 'RawReceiptValidated': 18, 'WorkerRows': []}
    BinaryPaths = set()
    for Workers in (1, 2, 8):
        Rows = {(Mode, Repeat): ReadRun(Directory, Mode, Workers, Repeat, Frames)
                for Mode in ('fast', 'full') for Repeat in (1, 2, 3)}
        BinaryPaths.update(Row['_BinaryPath'] for Row in Rows.values())
        Fast = [Rows['fast', Repeat] for Repeat in (1, 2, 3)]
        Full = [Rows['full', Repeat] for Repeat in (1, 2, 3)]
        FrameTailsAvailable = all(all(Key in Row for Key in
            ('UpdateP50Ms', 'UpdateP99Ms', 'UpdateMaxMs')) for Row in Fast + Full)
        WorkerRow = {
            'Workers': Workers,
            'FastMedianTotalUpdateMs': statistics.median(Row['TotalUpdateMs'] for Row in Fast),
            'FullMedianTotalUpdateMs': statistics.median(Row['TotalUpdateMs'] for Row in Full),
            'MedianPairedSpeedup': statistics.median(
                Rows['full', Repeat]['TotalUpdateMs'] /
                Rows['fast', Repeat]['TotalUpdateMs'] for Repeat in (1, 2, 3)),
            'FastMedianP95Ms': statistics.median(Row['UpdateP95Ms'] for Row in Fast),
            'FullMedianP95Ms': statistics.median(Row['UpdateP95Ms'] for Row in Full),
            'FastMinimumContactFrames': min(Row['NativeContactPairFrames'] for Row in Fast),
            'FullMinimumContactFrames': min(Row['NativeContactPairFrames'] for Row in Full),
            'FastMaximumFloorPenetrationM': max(Row['MaximumFloorPenetration'] for Row in Fast),
            'FullMaximumFloorPenetrationM': max(Row['MaximumFloorPenetration'] for Row in Full),
            'FastMaximumPairPenetrationM': max(Row['MaximumPairPenetration'] for Row in Fast),
            'FullMaximumPairPenetrationM': max(Row['MaximumPairPenetration'] for Row in Full),
            'FrameP50P99MaxAvailable': FrameTailsAvailable,
        }
        if FrameTailsAvailable:
            for Mode, ModeRows in (('Fast', Fast), ('Full', Full)):
                for Label, Key in (('P50', 'UpdateP50Ms'), ('P99', 'UpdateP99Ms'),
                                   ('Max', 'UpdateMaxMs')):
                    WorkerRow[f'{Mode}Median{Label}Ms'] = statistics.median(
                        Row[Key] for Row in ModeRows)
        Results['WorkerRows'].append(WorkerRow)
    if len(BinaryPaths) != 1:
        raise ValueError('Fast and full receipts used different binary paths')
    Results['BinaryPath'] = BinaryPaths.pop()
    return Results


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument('directory', type=Path)
    Parser.add_argument('--frames', type=int, default=240)
    Parser.add_argument('--output', type=Path)
    Options = Parser.parse_args()
    if Options.frames < 1 or Options.frames > 3600:
        raise ValueError('Frames out of bounds')
    Result = Reduce(Options.directory, Options.frames)
    Text = json.dumps(Result, indent=2, sort_keys=True) + '\n'
    if Options.output:
        Options.output.write_text(Text, encoding='utf-8')
    print(Text, end='')


if __name__ == '__main__':
    Main()
