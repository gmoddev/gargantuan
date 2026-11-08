"""Check cold Engine matrix coverage and failed-process raw preservation."""

from contextlib import redirect_stdout
import io
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

import RunEnginePerformanceQualification as Runner


class EnginePerformanceTests(unittest.TestCase):
    def test_compiled_solver_profile_is_consistent_and_recorded(self):
        with TemporaryDirectory() as Directory:
            Raw = Path(Directory) / 'raw.txt'
            Profile = dict(zip(Runner.JOLT_PROFILE_FIELDS, (2, 8, 6, 2, 96, 1)))
            Identity = {'backend': 'jolt', 'scene': 'cloth4k', 'coupling': 'none',
                        'frames': 1, 'workers_requested': 1}
            Summary = {**Identity, **Profile, 'vertices': 4096, 'constraints': 8000,
                       'observed_contact_band': 0}
            Summary.update({Field: 0 for Field in Runner.QUALITY_FIELDS + Runner.TIME_FIELDS})
            Summary['mean_physics_step_wall_ms'] = 1

            def WriteRaw(HeaderProfile):
                Header = {**Identity, **HeaderProfile}
                Format = lambda Row: ' '.join(f'{Key}={Value}' for Key, Value in Row.items())
                Raw.write_text('[Physics:EngineBenchmark] ' + Format(Header) + '\n'
                               '[Physics:EngineBenchmark] frame=0 physics_step_wall_ms=1\n'
                               '[Physics:EngineBenchmark] summary ' + Format(Summary) + '\n',
                               encoding='utf-8')

            WriteRaw(Profile)
            Parsed = Runner.ParseRaw(Raw, 'jolt', 'cloth4k', 'none', 1, 1)
            self.assertEqual(Parsed['JoltSolverProfile'], Profile)
            WriteRaw({**Profile, 'jolt_soft_integrations_per_frame': 192})
            with self.assertRaisesRegex(ValueError, 'changed between Engine header'):
                Runner.ParseRaw(Raw, 'jolt', 'cloth4k', 'none', 1, 1)

    def test_expired_total_deadline_keeps_failed_summary_without_launch(self):
        with TemporaryDirectory() as Directory, \
             patch.object(Runner, 'RemainingTimeout', return_value=-1), \
             patch.object(Runner, 'RunProcess') as Run:
            Base = Path(Directory)
            Binary = Base / 'engine-benchmark'
            Binary.write_bytes(b'binary')
            Cache = Base / 'CMakeCache.txt'
            Cache.write_bytes(b'cache')
            Output = Base / 'evidence'
            with self.assertRaises(SystemExit) as Exit:
                Runner.Main(['--binary', str(Binary), '--cmake-cache', str(Cache),
                             '--engine-source', 'engine-sha', '--jolt-source',
                             'jolt-sha', '--output', str(Output),
                             '--scenes', 'cloth4k', '--couplings', 'none',
                             '--workers', '1', '--frames', '1',
                             '--repetitions', '1',
                             '--total-timeout-seconds', '60'])
            self.assertEqual(Exit.exception.code, 2)
            Run.assert_not_called()
            Summary = json.loads((Output / 'Summary.json').read_text())
            self.assertEqual(Summary['Status'], 'FAILED')
            self.assertFalse(Summary['QualificationPass'])
            self.assertEqual(Summary['CompletedProcesses'], 0)
            self.assertIn('Total deadline', Summary['HarnessErrors'][0]['Reason'])

    def test_default_cold_matrix_balances_order(self):
        Plan = Runner.PlanMatrix(Runner.SCENES, Runner.COUPLINGS,
                                 (1, 2, 8), 3, ('cold',))
        self.assertEqual(len(Plan), 90)
        self.assertEqual(sum(Item['Order'] == ('jolt', 'xpbd') for Item in Plan), 45)
        self.assertEqual(sum(Item['Order'] == ('xpbd', 'jolt') for Item in Plan), 45)
        self.assertEqual({Item['Workers'] for Item in Plan}, {1, 2, 8})

    def test_failed_engine_run_retains_receipt_without_winner(self):
        Raw = b'[Physics:EngineBenchmark] admission failed\n'

        def FailProcess(Command, TimeoutSeconds):
            self.assertEqual(len(Command), 6)
            self.assertEqual(TimeoutSeconds, 30)
            return {'Stdout': Raw, 'Stderr': b'native error\n',
                    'ExitCode': 1, 'TimedOut': False,
                    'StartedUtc': '2026-11-08T00:00:00Z',
                    'ElapsedSeconds': 0.1}

        with TemporaryDirectory() as Directory, \
             patch.object(Runner, 'RunProcess', side_effect=FailProcess), \
             redirect_stdout(io.StringIO()):
            Base = Path(Directory)
            Binary = Base / 'engine-benchmark'
            Binary.write_bytes(b'binary')
            Cache = Base / 'CMakeCache.txt'
            Cache.write_bytes(b'cache')
            Output = Base / 'evidence'
            with self.assertRaises(SystemExit) as Exit:
                Runner.Main(['--binary', str(Binary), '--cmake-cache', str(Cache),
                             '--engine-source', 'engine-sha', '--jolt-source',
                             'jolt-sha', '--output', str(Output),
                             '--scenes', 'cloth4k', '--couplings', 'none',
                             '--workers', '1', '--frames', '1',
                             '--repetitions', '1', '--timeout-seconds', '30'])
            self.assertEqual(Exit.exception.code, 2)
            Receipt = json.loads((Output / 'Receipts.json').read_text())[0]
            Summary = json.loads((Output / 'Summary.json').read_text())
            self.assertEqual((Output / Receipt['Stdout']).read_bytes(), Raw)
            self.assertEqual(Receipt['ExitCode'], 1)
            self.assertEqual(Summary['CompletedProcesses'], 1)
            self.assertEqual(Summary['CompletedPairs'], 0)
            self.assertIsNone(Summary['TimingWinner'])


if __name__ == '__main__':
    unittest.main()
