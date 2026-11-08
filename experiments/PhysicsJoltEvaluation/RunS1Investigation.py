"""Bounded causal probes only. Quality failures are retained, never reclassified as passes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

Parser = argparse.ArgumentParser()
Parser.add_argument('--clean', type=Path, required=True)
Parser.add_argument('--instrumented', type=Path, required=True)
Parser.add_argument('--output', type=Path, required=True)
Args = Parser.parse_args()
Args.output.mkdir(parents=True, exist_ok=False)
Paths = {'Clean': Args.clean.resolve(), 'Instrumented': Args.instrumented.resolve()}
def Hash(PathName): return hashlib.sha256(PathName.read_bytes()).hexdigest()
Binaries = {Kind: Hash(PathName) for Kind, PathName in Paths.items()}
Jobs = []
def Add(Name, Kind, Options): Jobs.append((Name, Kind, Options))
for Count in [1, 2]:
    for Steps in [1, 4]:
        Common = ['--frames', '150', '--count', str(Count), '--steps', str(Steps), '--compliance', '1e-5', '--particles', '0']
        Add(f'Material-C{Count}-S{Steps}-Clean', 'Clean', Common)
        Add(f'Material-C{Count}-S{Steps}-Observed', 'Instrumented', Common + ['--trace', str(Args.output / f'Material-C{Count}-S{Steps}.phases.jsonl'), '--trace-start', '17', '--trace-end', '19'])
        if Steps == 1:
            Add(f'Material-C{Count}-S1-Refresh', 'Instrumented', Common + ['--refresh-sphere-plane', '1', '--trace', str(Args.output / f'Refresh-C{Count}-S1.phases.jsonl'), '--trace-start', '17', '--trace-end', '19'])
            Add(f'Material-C{Count}-S1-Frictionless', 'Clean', Common + ['--friction', '0'])
            for Passes in [2, 4]:
                Add(f'Material-C{Count}-S1-P{Passes}', 'Instrumented', Common + ['--projection-passes', str(Passes)])
for Steps in [1, 4]:
    for Axis in ['x', 'z']:
        Common = ['--frames', '150', '--steps', str(Steps), '--contact', '0', '--speed', '0', '--particles', '0', '--seed-x' if Axis == 'x' else '--seed', '1']
        Add(f'Free-{Axis}-S{Steps}-Clean', 'Clean', Common + (['--export-graph', str(Args.output / 'OptimizedClothGraph.json')] if Axis == 'x' and Steps == 1 else []))
        for Passes in [1, 2, 4]:
            Trace = ['--trace', str(Args.output / f'Free-{Axis}-S{Steps}-P{Passes}.phases.jsonl'), '--trace-start', '0', '--trace-end', '7'] if Passes == 1 and Steps == 4 else []
            Add(f'Free-{Axis}-S{Steps}-P{Passes}', 'Instrumented', Common + ['--projection-passes', str(Passes)] + Trace)
Receipts = []
for Name, Kind, Options in Jobs:
    Out = Args.output / f'{Name}.jsonl'
    Err = Args.output / f'{Name}.stderr.txt'
    Start = time.monotonic()
    with Out.open('w', encoding='utf-8') as Stdout, Err.open('w', encoding='utf-8') as Stderr:
        Result = subprocess.run([str(Paths[Kind]), *Options], stdout=Stdout, stderr=Stderr, timeout=180)
    Rows = [json.loads(Line) for Line in Out.read_text(encoding='utf-8').splitlines()]
    Frames = [Row for Row in Rows if Row['Type'] == 'Frame']
    Receipt = {'Name': Name, 'Kind': Kind, 'Options': Options, 'ExitCode': Result.returncode, 'Seconds': time.monotonic()-Start,
               'OutputSha256': Hash(Out), 'Frames': len(Frames), 'MaximumKE': max((R['KineticEnergy'] for R in Frames), default=None),
               'MaximumKEPlusEstimate': max((R['KineticEnergy']+R['CompliantEdgeElasticEstimate'] for R in Frames), default=None)}
    Receipts.append(Receipt)
    (Args.output / 'Receipts.json').write_text(json.dumps({'Binaries': Binaries, 'Runs': Receipts}, indent=2), encoding='utf-8')
    print(json.dumps(Receipt), flush=True)
    assert Result.returncode == 0 and len(Frames) == 150, f'Harness failed: {Name}'
assert Binaries == {Kind: Hash(PathName) for Kind, PathName in Paths.items()}, 'Binary changed during measurement'
for Count in [1, 2]:
    for Steps in [1, 4]:
        def States(Suffix):
            Rows = [json.loads(Line) for Line in (Args.output / f'Material-C{Count}-S{Steps}-{Suffix}.jsonl').read_text(encoding='utf-8').splitlines()]
            return [{K: V for K, V in Row.items() if K != 'StepMs'} for Row in Rows if Row['Type'] == 'Frame']
        assert States('Clean') == States('Observed'), f'Observation changed physics C{Count} S{Steps}'
(Args.output / 'DefaultParity.json').write_text(json.dumps({'ExactFrameStateParity': True, 'Cases': 4}), encoding='utf-8')
