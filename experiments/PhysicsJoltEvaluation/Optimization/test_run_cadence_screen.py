"""Check targeted schedule coverage and preservation of failed screens."""

from contextlib import redirect_stdout
import io
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

import RunCadenceScreen as Screen


class CadenceScreenTests(unittest.TestCase):
    def test_soft_budget_keeps_all_physical_failures(self):
        Calls = []

        def Calibrate(*Args, **Options):
            Count = Options['IntegrationsPerFrame']
            self.assertEqual(Options['VelocityReconstruction'], 'delta')
            return {'Calibrated': Count != 32,
                    'InternalIntegrationDt': Screen.NativeStepTime(Count, 1, 1)}

        def Observe(*Args):
            Calls.append(Args)
            Name = Args[3]
            return {'Name': Name, 'PhysicalPass': not Name.endswith('I4'),
                    'Score': {'CompleteBoundedPass': not Name.endswith('I4')}}

        with TemporaryDirectory() as Directory, \
             patch.object(Screen, 'CalibrateSpring', side_effect=Calibrate), \
             patch.object(Screen, 'RunObservedCase', side_effect=Observe), \
             redirect_stdout(io.StringIO()):
            Output = Path(Directory) / 'screen'
            Screen.Main(['--pure', 'pure', '--observed', 'observed',
                         '--output', str(Output), '--screen', 'soft-budget'])
            Summary = json.loads((Output / 'Summary.json').read_text())
            Receipts = json.loads((Output / 'Receipts.json').read_text())
            self.assertEqual(len(Receipts), 8)
            self.assertEqual(Summary['Calibrations'],
                             {'16': True, '32': False, '64': True, '96': True})
            self.assertEqual([(Args[3], Args[4], Args[6:10], Args[11], Args[14])
                              for Args in Calls], [
                (f'{Prefix}-W2-C8-I{Iterations}', Fixture,
                 (8, Iterations, 2, Frames), Extra, 'delta')
                for Iterations in (1, 2, 4, 6)
                for Prefix, Fixture, Frames, Extra in (
                    ('Free', 'Free', 150, ()),
                    ('Speed10', 'C2', 80, ('--speed', '10')),
                )])
            self.assertEqual(sum(Item['CandidatePass'] for Item in Receipts), 4)
            self.assertEqual(sum(Item['PhysicalPass'] for Item in Receipts), 6)

    def test_harness_error_is_recorded_after_other_cases(self):
        def Calibrate(*Args, **Options):
            Count = Options['IntegrationsPerFrame']
            return {'Calibrated': True,
                    'InternalIntegrationDt': Screen.NativeStepTime(Count, 1, 1)}

        def Observe(*Args):
            if Args[3] == 'Speed10-W1-C16-I12':
                raise RuntimeError('simulated native failure')
            return {'Name': Args[3], 'PhysicalPass': True, 'Score': {}}

        with TemporaryDirectory() as Directory, \
             patch.object(Screen, 'CalibrateSpring', side_effect=Calibrate), \
             patch.object(Screen, 'RunObservedCase', side_effect=Observe), \
             redirect_stdout(io.StringIO()):
            Output = Path(Directory) / 'screen'
            with self.assertRaises(SystemExit) as Exit:
                Screen.Main(['--pure', 'pure', '--observed', 'observed',
                             '--output', str(Output)])
            self.assertEqual(Exit.exception.code, 2)
            Summary = json.loads((Output / 'Summary.json').read_text())
            Receipts = json.loads((Output / 'Receipts.json').read_text())
            self.assertEqual(len(Receipts), 3)
            self.assertEqual([Item['Name'] for Item in Receipts],
                             [Item[0] for Item in Screen.GetCases('cadence')])
            self.assertIsNone(Receipts[1]['PhysicalPass'])
            self.assertIn('simulated native failure', Receipts[1]['Error'])
            self.assertEqual(Summary['HarnessErrors'][0]['Name'], Receipts[1]['Name'])


if __name__ == '__main__':
    unittest.main()
