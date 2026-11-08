"""Screen Jolt cadence candidates with the existing full observed physical gates.

These short screens identify candidates only. A pass does not replace the long
free, impact, material, worker, and timing qualification.
"""

import argparse
import json
from pathlib import Path
import sys
import traceback
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from RunScheduledEnergyQualification import CalibrateSpring, NativeStepTime
from RunScheduledProfileQualification import RunObservedCase


def WriteJson(PathName, Value):
    PathName.write_text(json.dumps(Value, indent=2) + '\n', encoding='utf-8')


def GetCases(Screen):
    if Screen == 'cadence':
        return [
            ('Free-W1-C4-I48', 'Free', 1, 4, 48, 1800, ()),
            ('Speed10-W1-C16-I12', 'C2', 1, 16, 12, 600, ('--speed', '10')),
            ('Speed10-W4-C4-I12', 'C2', 4, 4, 12, 600, ('--speed', '10')),
        ]
    return [
        (f'{Prefix}-W2-C8-I{Iterations}', Fixture, 2, 8, Iterations,
         Frames, Extra)
        for Iterations in (1, 2, 4, 6)
        for Prefix, Fixture, Frames, Extra in (
            ('Free', 'Free', 150, ()),
            ('Speed10', 'C2', 80, ('--speed', '10')),
        )
    ]


def Failure(Phase, Name):
    return {'Phase': Phase, 'Name': Name, 'Error': traceback.format_exc()}


def Main(Arguments=None):
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument('--pure', dest='Pure', type=Path, required=True)
    Parser.add_argument('--observed', dest='Observed', type=Path, required=True)
    Parser.add_argument('--output', dest='Output', type=Path, required=True)
    Parser.add_argument('--screen', dest='Screen', choices=('cadence', 'soft-budget'),
                        default='cadence')
    Args = Parser.parse_args(Arguments)
    Args.Output.mkdir(parents=True, exist_ok=False)

    Cases = GetCases(Args.Screen)
    IntegrationCounts = sorted({Updates * Steps * Iterations
                                for _, _, Updates, Steps, Iterations, _, _ in Cases})
    Calibrations = {}
    Receipts = []
    Errors = []
    for Count in IntegrationCounts:
        try:
            Calibration = CalibrateSpring(SimpleNamespace(threads=1), Args.Pure,
                Args.Observed, Args.Output, '1e-6', IntegrationsPerFrame=Count,
                VelocityReconstruction='delta')
            Calibrations[str(Count)] = Calibration
        except Exception:
            Error = Failure('Calibration', f'N{Count}')
            Errors.append(Error)
            Calibrations[str(Count)] = {'Calibrated': False,
                'InternalIntegrationDt': NativeStepTime(Count, 1, 1),
                'VelocityReconstruction': 'delta', 'Error': Error['Error']}
        WriteJson(Args.Output / 'Calibrations.json', Calibrations)
    if Args.Screen == 'cadence':
        WriteJson(Args.Output / 'Calibration.json', Calibrations['192'])

    for Name, Fixture, Updates, Steps, Iterations, Frames, Extra in Cases:
        Count = Updates * Steps * Iterations
        Calibration = Calibrations[str(Count)]
        try:
            Receipt = RunObservedCase(Args.Pure, Args.Observed, Args.Output, Name,
                Fixture, '1e-6', Steps, Iterations, Updates, Frames, 1, Extra,
                True, Calibration['InternalIntegrationDt'], 'delta')
            Receipt['SpringCalibrationPass'] = bool(Calibration['Calibrated'])
            Receipt['CandidatePass'] = bool(Receipt['PhysicalPass'] and
                                            Receipt['SpringCalibrationPass'])
        except Exception:
            Error = Failure('ObservedCase', Name)
            Errors.append(Error)
            Receipt = {'Name': Name, 'Fixture': Fixture, 'WorldUpdates': Updates,
                'CollisionSteps': Steps, 'SoftIterations': Iterations,
                'Frames': Frames, 'VelocityReconstruction': 'delta',
                'SpringCalibrationPass': bool(Calibration['Calibrated']),
                'PhysicalPass': None, 'CandidatePass': False,
                'Error': Error['Error']}
        Receipts.append(Receipt)
        WriteJson(Args.Output / 'Receipts.json', Receipts)
        print(json.dumps({'Name': Name, 'PhysicalPass': Receipt['PhysicalPass'],
                          'CandidatePass': Receipt['CandidatePass'],
                          'Score': Receipt.get('Score'),
                          'Error': Receipt.get('Error')}), flush=True)

    Summary = {
        'Screen': Args.Screen,
        'Calibrations': {Count: Value['Calibrated']
                         for Count, Value in Calibrations.items()},
        'Cases': [{'Name': Item['Name'], 'PhysicalPass': Item['PhysicalPass'],
                   'SpringCalibrationPass': Item['SpringCalibrationPass'],
                   'CandidatePass': Item['CandidatePass']} for Item in Receipts],
        'Scope': ('Candidate screens only; long free, 600-frame impact, '
                  'material-response calibration, workers, and timing remain '
                  'required before a performance claim'),
        'PhysicalFailuresRetained': True,
        'HarnessErrors': Errors,
    }
    WriteJson(Args.Output / 'Summary.json', Summary)
    if Errors:
        raise SystemExit(2)


if __name__ == '__main__':
    Main()
