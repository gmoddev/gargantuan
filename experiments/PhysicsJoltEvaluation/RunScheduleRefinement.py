"""Discriminate finer soft integration from faster rigid/contact updates."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from RunScheduledEnergyQualification import ReadPublished, PhysicalRow, ScoreSolver

Parser = argparse.ArgumentParser()
Parser.add_argument('--pristine', type=Path, required=True)
Parser.add_argument('--observed', type=Path, required=True)
Parser.add_argument('--output', type=Path, required=True)
Args = Parser.parse_args()
Args.output.mkdir(parents=True, exist_ok=False)
Receipts = []


def Run(Binary, Name, Options):
    Output = Args.output / (Name + '.jsonl')
    Error = Args.output / (Name + '.stderr.txt')
    with Output.open('w', encoding='utf-8') as Stdout, Error.open('w', encoding='utf-8') as Stderr:
        Result = subprocess.run([str(Binary.resolve()), *Options], stdout=Stdout,
                                stderr=Stderr, timeout=1200)
    assert Result.returncode == 0, (Name, Result.returncode)
    return Output


def HashFrames(Frames):
    return hashlib.sha256(json.dumps([PhysicalRow(F) for F in Frames],
                                    sort_keys=True, separators=(',', ':')).encode()).hexdigest()


# Both schedules have 384 soft integrations/frame; only the outer cadence differs.
for Label, Updates, Steps, Iterations in [('W2C8I24', 2, 8, 24), ('W1C8I48', 1, 8, 48)]:
    Cases = [('Free1800', 1800, ['--contact', '0', '--speed', '0', '--seed', '1']),
             ('C2Speed10', 600, ['--count', '2', '--speed', '10'])]
    if Updates == 2:
        Cases += [('C1', 150, []), ('C2', 150, ['--count', '2'])]
    for Case, Frames, Extra in Cases:
        Name = Label + '-' + Case
        Base = ['--frames', str(Frames), '--world-updates', str(Updates), '--steps', str(Steps),
                '--iterations', str(Iterations), '--compliance', '1e-6', '--bend-compliance', '1e-4',
                '--workers', '1', '--particles', '0', '--observe-updates', '1', *Extra]
        CleanPath = Run(Args.pristine, Name + '-Pristine', Base)
        SolverPath = Args.output / (Name + '.solver.jsonl')
        ObservedPath = Run(Args.observed, Name + '-Observed', [*Base, '--solver-observations', str(SolverPath)])
        CleanConfig, CleanFrames, CleanMicro, _ = ReadPublished(CleanPath)
        Config, FrameRows, Accepted, _ = ReadPublished(ObservedPath)
        assert HashFrames(CleanFrames) == HashFrames(FrameRows), Name + ': observer changed frames'
        assert all(PhysicalRow(V) == PhysicalRow(Accepted[K]) for K, V in CleanMicro.items()), Name + ': observer changed updates'
        Score = ScoreSolver(SolverPath, Config, Accepted)
        Receipt = {'Name': Name, 'Options': Base, 'Score': Score,
                   'PhysicalFrameSha256': HashFrames(FrameRows), 'PristineParity': True}
        Receipts.append(Receipt)
        (Args.output / 'Receipts.json').write_text(json.dumps(Receipts, indent=2), encoding='utf-8')
        print(json.dumps({'Name': Name, 'BoundedPass': Score['CompleteBoundedPass'],
                          'MaxKPlusU': Score['MaximumSolverKPlusEdgeModelJ']}), flush=True)

# Calibrate the energy meter at the new internal timestep using a single spring.
Spring = Run(Args.observed, 'Spring384', ['--fixture', 'Spring', '--frames', '8', '--steps', '1',
             '--iterations', '1', '--world-updates', '384', '--compliance', '1e-6', '--damping', '0',
             '--contact', '0', '--speed', '0', '--workers', '1', '--particles', '1', '--observe-updates', '1'])
subprocess.run([sys.executable, str(Path(__file__).with_name('EnergyQualification.py')),
                '--input', str(Spring), '--output', str(Args.output / 'Spring384.quality.json')], check=True)
Calibration = json.loads((Args.output / 'Spring384.quality.json').read_text(encoding='utf-8'))
assert Calibration['CompleteBoundedPass'] and Calibration['NativeSpringCalibration']['Pass']
(Args.output / 'Verdict.json').write_text(json.dumps({'HarnessQualified': True,
    'Cases': len(Receipts), 'CalibrationPass': True,
    'Scope': 'Physical rejections remain results; 384-integration profiles are not predeclared qualified.'}, indent=2), encoding='utf-8')
