"""Verify pair order and raw preservation when a native attempt fails."""

from contextlib import redirect_stdout
import hashlib
import io
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

import RunPerformanceQualification as Runner


class PerformanceQualificationTests(unittest.TestCase):
    def test_backend_order_is_counterbalanced_across_matrix(self):
        Plan = Runner.PlanMatrix(('cloth4k', 'cloth16k'), ('none',), (1,), 3,
                                 ('cold', 'warm'))
        self.assertEqual(len(Plan), 12)
        self.assertEqual(sum(Item['Order'] == ('jolt', 'xpbd') for Item in Plan), 6)
        self.assertEqual(sum(Item['Order'] == ('xpbd', 'jolt') for Item in Plan), 6)
        self.assertEqual(len({Item['Pair'] for Item in Plan}), 12)
        self.assertEqual({Item['Tail'] for Item in Plan}, {'cold', 'warm'})

    def test_failed_native_attempt_keeps_exact_raw_and_incremental_receipt(self):
        RawOut = b'partial native output\r\n\x00tail'
        RawErr = b'[Physics:ProviderBenchmark] native failure\n'

        def FailProcess(Command, TimeoutSeconds):
            self.assertEqual(Command[-1], '0')
            self.assertEqual(TimeoutSeconds, 30)
            return {'Stdout': RawOut, 'Stderr': RawErr, 'ExitCode': 7,
                    'TimedOut': False, 'StartedUtc': '2026-11-08T00:00:00Z',
                    'ElapsedSeconds': 0.5}

        with TemporaryDirectory() as Directory, \
             patch.object(Runner, 'RunProcess', side_effect=FailProcess), \
             redirect_stdout(io.StringIO()):
            Base = Path(Directory)
            Binary = Base / 'benchmark'
            Binary.write_bytes(b'binary bytes')
            Cache = Base / 'CMakeCache.txt'
            Cache.write_bytes(b'cache bytes')
            Output = Base / 'evidence'
            with self.assertRaises(SystemExit) as Exit:
                Runner.Main(['--binary', str(Binary), '--cmake-cache', str(Cache),
                             '--engine-source', 'engine-sha', '--jolt-source',
                             'jolt-sha', '--output', str(Output),
                             '--scenes', 'cloth4k', '--couplings', 'none',
                             '--workers', '1', '--tails', 'cold',
                             '--frames', '1', '--repetitions', '1',
                             '--timeout-seconds', '30'])
            self.assertEqual(Exit.exception.code, 2)
            Receipts = json.loads((Output / 'Receipts.json').read_text())
            Summary = json.loads((Output / 'Summary.json').read_text())
            self.assertEqual(len(Receipts), 1)
            Receipt = Receipts[0]
            self.assertEqual((Output / Receipt['Stdout']).read_bytes(), RawOut)
            self.assertEqual((Output / Receipt['Stderr']).read_bytes(), RawErr)
            self.assertEqual(Receipt['StdoutSha256'], hashlib.sha256(RawOut).hexdigest())
            self.assertEqual(Receipt['StderrSha256'], hashlib.sha256(RawErr).hexdigest())
            self.assertEqual(Receipt['ExitCode'], 7)
            self.assertEqual(Summary['CompletedProcesses'], 1)
            self.assertEqual(Summary['CompletedPairs'], 0)
            self.assertEqual(len(Summary['HarnessErrors']), 1)


if __name__ == '__main__':
    unittest.main()
