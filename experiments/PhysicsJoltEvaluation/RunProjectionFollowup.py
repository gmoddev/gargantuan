"""Predict modes from the native ordered graph, then excite them through native APIs."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

Parser = argparse.ArgumentParser()
Parser.add_argument('--instrumented', type=Path, required=True)
Parser.add_argument('--runs', type=Path, required=True)
Args = Parser.parse_args()
Root = Path(__file__).resolve().parent
Graph = Args.runs / 'OptimizedClothGraph.json'
Output = Args.runs / 'Projection'
Output.mkdir(exist_ok=False)
def Run(Name, Command):
    with (Output / f'{Name}.stdout.txt').open('w', encoding='utf-8') as Stdout, (Output / f'{Name}.stderr.txt').open('w', encoding='utf-8') as Stderr:
        Result = subprocess.run(Command, stdout=Stdout, stderr=Stderr, timeout=300)
    assert Result.returncode == 0, (Name, Result.returncode)

Spectrum = Output / 'RestSpectrum.json'
Run('Spectrum', [sys.executable, str(Root / 'AnalyzeProjectionModes.py'), str(Graph), '--output', str(Spectrum),
                 '--passes', '1,2,4', '--compliances', 'edge,1e-5', '--top', '2', '--finite-difference', '2'])
Data = json.loads(Spectrum.read_text(encoding='utf-8'))
for Steps in [1, 4]:
    Interval = 1.0 / (60 * Steps * 24)
    Scenario = next(S for S in Data['Scenarios'] if abs(S['IntervalSeconds']-Interval)<1e-12 and S['ProjectionPasses']==1 and S['ComplianceOverride']=='edge')
    Mode = Scenario['TopModes'][0]
    for Scale in [1, 0.1]:
        Prefix = f'Mode-S{Steps}-A{Scale}'
        Displacement = Output / f'{Prefix}.displacement.json'
        Velocity = Output / f'{Prefix}.velocity.json'
        Displacement.write_text(json.dumps([[Scale*X for X in V] for V in Mode['PositionDeltaMeters']]), encoding='utf-8')
        Velocity.write_text(json.dumps([[Scale*X for X in V] for V in Mode['VelocityDeltaMetersPerSecond']]), encoding='utf-8')
        Run(Prefix, [str(Args.instrumented.resolve()), '--frames', '12', '--steps', str(Steps), '--contact', '0', '--speed', '0',
                     '--displacement-file', str(Displacement), '--velocity-file', str(Velocity), '--trace', str(Output / f'{Prefix}.phases.jsonl'),
                     '--trace-start', '0', '--trace-end', '2'])
    Run(f'Unseeded-S{Steps}', [str(Args.instrumented.resolve()), '--frames', '150', '--steps', str(Steps), '--contact', '0', '--speed', '0', '--particles', '0'])
# A separate high-precision algorithm model is supporting evidence, not native parity.
for Steps in [1, 4]:
    Run(f'Replay-S{Steps}', [sys.executable, str(Root / 'ReplayProjectionSweeps.py'), str(Graph),
        '--output', str(Output / f'Replay-S{Steps}.json'), '--interval', f'1/{60*Steps*24}', '--steps', '192', '--sweeps', '1',
        '--center-index', '84', '--center-amplitude', '0.0001', '--seeds', 'center-x,center-z'])
(Output / 'Sha256.json').write_text(json.dumps({str(P.relative_to(Output)): hashlib.sha256(P.read_bytes()).hexdigest()
    for P in sorted(Output.rglob('*')) if P.is_file() and P.name!='Sha256.json'}, indent=2), encoding='utf-8')
