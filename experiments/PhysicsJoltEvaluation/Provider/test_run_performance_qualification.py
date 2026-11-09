"""Verify pair order and raw preservation when a native attempt fails."""

from contextlib import redirect_stdout
import hashlib
import io
import json
import struct
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

import RunPerformanceQualification as Runner


class PerformanceQualificationTests(unittest.TestCase):
    def test_native_profile_cache_cannot_enter_timing_ranking(self):
        with TemporaryDirectory() as Directory:
            Cache = Path(Directory) / 'CMakeCache.txt'
            Cache.write_text('PHYSICS_EVALUATION_NATIVE_PROFILE:BOOL=OFF\n')
            Runner.ValidateTimingCache(Cache)
            for Name in ('PHYSICS_EVALUATION_NATIVE_PROFILE', 'JPH_USE_EXTERNAL_PROFILE',
                         'PROFILER_IN_DEBUG_AND_RELEASE', 'PROFILER_IN_DISTRIBUTION'):
                Cache.write_text(Name + ':BOOL=ON\n')
                with self.assertRaisesRegex(ValueError, 'Native profiler'):
                    Runner.ValidateTimingCache(Cache)

    def test_driven_response_requires_matched_free_trajectory_difference(self):
        Base = {'Scene': 'cloth4k', 'Workers': 1, 'Tail': 'cold',
                'Repeat': 0, 'Backend': 'xpbd'}
        Receipts = [
            {**Base, 'Name': 'free', 'Coupling': 'none',
             'Parsed': {'SoftTrajectoryHashes': [10, 20, 30]}},
            {**Base, 'Name': 'driven', 'Coupling': 'driven-sphere',
             'Parsed': {'SoftTrajectoryHashes': [10, 20, 31]}},
        ]
        Result = Runner.CompareDrivenControls(Receipts)
        self.assertTrue(Result[0]['ResponseObserved'])
        self.assertEqual(Result[0]['FirstDifferentFrame'], 2)
        Receipts[1]['Parsed']['SoftTrajectoryHashes'] = [10, 20, 30]
        self.assertFalse(Runner.CompareDrivenControls(Receipts)[0]['ResponseObserved'])

    def test_expired_total_deadline_stops_before_native_launch(self):
        with TemporaryDirectory() as Directory, \
             patch.object(Runner, 'RemainingTimeout', return_value=-1), \
             patch.object(Runner, 'RunProcess') as Run:
            Base = Path(Directory)
            Binary = Base / 'benchmark'
            Binary.write_bytes(b'binary')
            Cache = Base / 'CMakeCache.txt'
            Cache.write_bytes(b'cache')
            Output = Base / 'evidence'
            with self.assertRaises(SystemExit) as Exit:
                Runner.Main(['--binary', str(Binary), '--cmake-cache', str(Cache),
                             '--engine-source', 'engine-sha', '--jolt-source',
                             'jolt-sha', '--output', str(Output),
                             '--scenes', 'cloth4k', '--couplings', 'none',
                             '--workers', '1', '--tails', 'cold',
                             '--frames', '1', '--repetitions', '1',
                             '--total-timeout-seconds', '60'])
            self.assertEqual(Exit.exception.code, 2)
            Run.assert_not_called()
            Summary = json.loads((Output / 'Summary.json').read_text())
            self.assertEqual(Summary['Status'], 'FAILED')
            self.assertFalse(Summary['QualificationPass'])
            self.assertEqual(Summary['CompletedProcesses'], 0)
            self.assertIn('Total deadline', Summary['HarnessErrors'][0]['Reason'])

    def test_compiled_jolt_schedule_must_match_header_and_product(self):
        Profile = dict(zip(Runner.JOLT_PROFILE_FIELDS, (2, 8, 6, 2, 96, 0)))
        Header = {'backend': 'jolt', 'coupling': 'none', 'run_pair': 'p0000',
                  'warmup_frames': 0, 'timing_instrumented': 0,
                  'quality_trace': 0,
                  'contact_capture_instrumented': 0,
                  'drive_duration_seconds': 0, 'drive_distance_m': 0,
                  'xpbd_substeps_requested': 1, 'xpbd_substeps_actual': 0,
                  'xpbd_reference': 0, 'xpbd_quality_mode': 'not-applicable',
                  'xpbd_damping_mode': 'not-applicable',
                  'xpbd_substep_damping': 0, **Profile}
        Summary = {**Header, 'frames': 1, 'mean_total_ms': 1,
                   'mean_y': 0, 'center_y': 0, 'max_pin_error': 0,
                   'mean_edge_strain': 0, 'max_edge_strain': 0,
                   'mean_volume_ratio': 0, 'mean_tetra_volume_ratio': 0,
                   'max_tetra_volume_error': 0, 'mean_diagonal_strain': 0,
                   'xpbd_measured_substeps': 0,
                   'jolt_soft_contact_vertices': 0,
                   'soft_trajectory_hash': 123}
        Lifecycle = {'MeasuredFrames': 1, 'WarmupFrames': 0,
                     'TimingInstrumented': False, 'StartupAdmissionMs': 1,
                     'TeardownMs': 1}

        def Line(Values):
            return ' '.join(f'{Key}={Value}' for Key, Value in Values.items())

        with TemporaryDirectory() as Directory:
            Output = Path(Directory) / 'raw.txt'
            def Write():
                Output.write_text('\n'.join((
                    '[Physics:ProviderBenchmark] ' + Line(Header),
                    '[Physics:ProviderBenchmark] frame=0 total_ms=1',
                    '[Physics:ProviderBenchmark] state_frame=0 soft_trajectory_hash=123',
                    '[Physics:ProviderBenchmark] summary ' + Line(Summary),
                    '[Physics:ProviderBenchmark] lifecycle ' + json.dumps(Lifecycle),
                )) + '\n')
            Write()
            Parsed = Runner.ParseRaw(Output, 'jolt', 1, 0, 'p0000')
            self.assertEqual(Parsed['CompiledJoltProfile'], Profile)
            for Row in (Header, Summary):
                Row['quality_trace'] = 1
                Row['timing_instrumented'] = 1
            Lifecycle['TimingInstrumented'] = True
            Write()
            Runner.ParseRaw(Output, 'jolt', 1, 0, 'p0000', QualityTrace=True)
            with self.assertRaisesRegex(ValueError, 'Quality trace scope'):
                Runner.ParseRaw(Output, 'jolt', 1, 0, 'p0000')
            for Row in (Header, Summary):
                Row['quality_trace'] = 0
                Row['timing_instrumented'] = 0
            Lifecycle['TimingInstrumented'] = False
            Summary['jolt_soft_integrations_per_frame'] = 192
            Write()
            with self.assertRaisesRegex(ValueError, 'header and summary'):
                Runner.ParseRaw(Output, 'jolt', 1, 0, 'p0000')
            Summary['jolt_soft_integrations_per_frame'] = 96
            for Row in (Header, Summary):
                Row['coupling'] = 'driven-sphere'
                Row['timing_instrumented'] = 1
                Row['contact_capture_instrumented'] = 1
                Row['drive_duration_seconds'] = 0.66
                Row['drive_distance_m'] = 1.2
            Summary['jolt_soft_contact_vertices'] = 3
            Lifecycle['TimingInstrumented'] = True
            Write()
            Runner.ParseRaw(Output, 'jolt', 1, 0, 'p0000')
            Summary['jolt_soft_contact_vertices'] = 0
            Write()
            with self.assertRaisesRegex(ValueError, 'no actual soft-rigid contact'):
                Runner.ParseRaw(Output, 'jolt', 1, 0, 'p0000')

    def test_quality_trace_compares_all_vertices_and_refuses_truncated_file(self):
        with TemporaryDirectory() as Directory:
            Jolt = Path(Directory) / 'jolt.f32le'
            Xpbd = Path(Directory) / 'xpbd.f32le'
            Header = Runner.TRACE_HEADER.pack(b'PJTRACE1', 1, 1, 4096, 2, 1, 1 / 60)
            Frame = struct.pack('<fff', 0, 0, 0) * 4096
            Changed = struct.pack('<fff', 3, 4, 0) + Frame[12:]
            Jolt.write_bytes(Header + Frame + Frame)
            Xpbd.write_bytes(Header + Frame + Changed)
            Compared = Runner.CompareQualityTraces(Jolt, Xpbd, 'cloth4k', 1, 1)
            self.assertEqual(Compared['Frames'][0]['MaxDisplacementM'], 0)
            self.assertEqual(Compared['Frames'][1]['MaxDisplacementM'], 5)
            self.assertAlmostEqual(Compared['Frames'][1]['RmsDisplacementM'], 5 / 64)
            Xpbd.write_bytes(Xpbd.read_bytes()[:-4])
            with self.assertRaisesRegex(ValueError, 'byte count'):
                Runner.CompareQualityTraces(Jolt, Xpbd, 'cloth4k', 1, 1)

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
            self.assertEqual(Command[-2:], ['0', '1'])
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

    def test_failed_traced_attempt_keeps_partial_trace_hash(self):
        Partial = b'PJTRACE1\x01\x00partial'

        def FailProcess(Command, TimeoutSeconds):
            self.assertEqual(len(Command), 10)
            Path(Command[-1]).write_bytes(Partial)
            return {'Stdout': b'', 'Stderr': b'failure', 'ExitCode': 1,
                    'TimedOut': False, 'StartedUtc': '2026-11-08T00:00:00Z',
                    'ElapsedSeconds': 0.1}

        with TemporaryDirectory() as Directory, \
             patch.object(Runner, 'RunProcess', side_effect=FailProcess):
            Base = Path(Directory)
            Binary = Base / 'benchmark'
            Binary.write_bytes(b'binary')
            Cache = Base / 'CMakeCache.txt'
            Cache.write_bytes(b'cache')
            Output = Base / 'evidence'
            with self.assertRaises(SystemExit):
                Runner.Main(['--binary', str(Binary), '--cmake-cache', str(Cache),
                             '--engine-source', 'engine-sha', '--jolt-source',
                             'jolt-sha', '--output', str(Output),
                             '--scenes', 'cloth4k', '--couplings', 'none',
                             '--workers', '1', '--tails', 'cold',
                             '--frames', '1', '--repetitions', '1',
                             '--quality-trace'])
            Receipt = json.loads((Output / 'Receipts.json').read_text())[0]
            self.assertEqual((Output / Receipt['QualityTrace']).read_bytes(), Partial)
            self.assertEqual(Receipt['QualityTraceSha256'],
                             hashlib.sha256(Partial).hexdigest())
            self.assertEqual(Receipt['QualityTraceBytes'], len(Partial))


if __name__ == '__main__':
    unittest.main()
