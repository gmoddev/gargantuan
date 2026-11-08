"""Discriminate lower Update overhead without changing existing physical gates."""
import argparse
import json
from pathlib import Path
import sys
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from RunScheduledEnergyQualification import CalibrateSpring
from RunScheduledProfileQualification import RunObservedCase

Parser = argparse.ArgumentParser()
Parser.add_argument('--pure', dest='Pure', type=Path, required=True)
Parser.add_argument('--observed', dest='Observed', type=Path, required=True)
Parser.add_argument('--output', dest='Output', type=Path, required=True)
Args = Parser.parse_args()
Args.Output.mkdir(parents=True, exist_ok=False)
Calibration = CalibrateSpring(SimpleNamespace(threads=1), Args.Pure, Args.Observed,
                              Args.Output, '1e-6', IntegrationsPerFrame=192,
                              VelocityReconstruction='delta')
(Args.Output / 'Calibration.json').write_text(json.dumps(Calibration, indent=2), encoding='utf-8')
assert Calibration['Calibrated']
Cases = [
    ('Free-W1-C4-I48', 'Free', 1, 4, 48, 1800, ()),
    ('Speed10-W1-C16-I12', 'C2', 1, 16, 12, 600, ('--speed', '10')),
    ('Speed10-W4-C4-I12', 'C2', 4, 4, 12, 600, ('--speed', '10')),
]
Receipts = []
for Name, Fixture, Updates, Steps, Iterations, Frames, Extra in Cases:
    Receipt = RunObservedCase(Args.Pure, Args.Observed, Args.Output, Name, Fixture,
        '1e-6', Steps, Iterations, Updates, Frames, 1, Extra, True,
        Calibration['InternalIntegrationDt'], 'delta')
    Receipts.append(Receipt)
    (Args.Output / 'Receipts.json').write_text(json.dumps(Receipts, indent=2), encoding='utf-8')
    print(json.dumps({'Name': Name, 'PhysicalPass': Receipt['PhysicalPass'],
                      'Score': Receipt['Score']}), flush=True)
(Args.Output / 'Summary.json').write_text(json.dumps({
    'Cases': [{'Name': Item['Name'], 'PhysicalPass': Item['PhysicalPass']} for Item in Receipts],
    'Scope': 'Candidate screens only; not complete schedule qualification',
    'PhysicalFailuresRetained': True}, indent=2), encoding='utf-8')
