"""Non-capture tests. Native helper self-test is opt-in after compile-only validation."""
import os
import importlib.util
import copy
import csv
import json
import tempfile
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("SchedulerTraceValidate", ROOT / "tools/ci/SchedulerTraceValidate.py")
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)


class SchedulerTraceTests(unittest.TestCase):
    def AggregateFixture(self, Root, Exit=0):
        Metadata, _ = self.Fixture()
        Metadata.update(WorkloadCase='Aggregate32Structural', ChildExitCode=Exit, DecodedRows=4,
                        WorkloadArguments=['--reliable-workload-32-structural'])
        Lines = []
        for Index, (Name, Begin, End) in enumerate((('aggregate-baseline', 10, 200), ('aggregate-overload', 300, 600),
                                 ('aggregate-recovery', 700, 900))):
            for Boundary, Tick in (('BEGIN', Begin), ('END', End)):
                Lines.append(f'[Qualification:ClockAnchor] case={Name} profile=FULL_RESERVATION boundary={Boundary} phase_index={Index} pid=42 native_tid=43 native_valid=1 steady_ns={Tick * 100} qpc_before={Tick} qpc_after={Tick + 1} qpc_frequency=10000000')
        File = Root / 'scheduler.csv'
        with File.open('w', encoding='ascii', newline='') as Stream:
            Writer = csv.DictWriter(Stream, fieldnames=VALIDATOR.CSV_FIELDS)
            Writer.writeheader()
            for Row in ({'Qpc': 4, 'Opcode': 1, 'Version': 3, 'TargetTid': 43, 'TargetPid': 42},
                        {'Qpc': 5, 'Opcode': 36, 'Version': 5, 'NewTid': 43, 'OldTid': 44},
                        {'Qpc': 998, 'Opcode': 50, 'Version': 2, 'TargetTid': 43},
                        {'Qpc': 999, 'Opcode': 2, 'Version': 3, 'TargetTid': 43, 'TargetPid': 42}):
                Writer.writerow(Row)
        return Metadata, '\n'.join(Lines), File

    def test_aggregate_fixed_case_raw_coverage_and_original_failure(self):
        with tempfile.TemporaryDirectory() as Directory:
            Metadata, Log, File = self.AggregateFixture(Path(Directory))
            Result = VALIDATOR.Validate(Metadata, Log, 'Aggregate32Structural', File)
            self.assertEqual(Result['State'], 'LOSS_FREE_ANCHOR_WINDOW_RETAINED', Result)
            self.assertEqual(Result['CausalVerdict'], 'NOT_CLAIMED')
            self.assertEqual(Result['SchedulerCsvRows'], 4)
            # A first-phase assertion failure preserves its complete prefix;
            # loss-free tracing cannot turn the child's nonzero exit into PASS.
            Metadata['ChildExitCode'] = 17
            Result = VALIDATOR.Validate(Metadata, '\n'.join(Log.splitlines()[:2]), 'Aggregate32Structural', File)
            self.assertEqual(Result['State'], 'LOSS_FREE_ANCHOR_WINDOW_RETAINED', Result)
            Metadata['ChildExitCode'] = 0
            self.assertEqual(VALIDATOR.Validate(Metadata, '\n'.join(Log.splitlines()[:2]),
                'Aggregate32Structural', File)['State'], 'INCOMPLETE')

    def test_aggregate_identity_loss_bounds_case_and_raw_csv_denials(self):
        with tempfile.TemporaryDirectory() as Directory:
            Metadata, Log, File = self.AggregateFixture(Path(Directory))
            for Key, Value in (('WorkloadArguments', ['--reliable-workload']), ('WorkloadCase', 'Full'),
                               ('ChildLaunchAttempts', 2), ('ChildTreeReaped', False), ('EventsLost', 1),
                               ('DecodedRows', 3), ('MainFirstQpc', 3), ('CsvCapped', True)):
                with self.subTest(Key=Key):
                    self.assertEqual(VALIDATOR.Validate(dict(Metadata, **{Key: Value}), Log,
                        'Aggregate32Structural', File)['State'], 'INCOMPLETE')
            for Bad in (Log.replace('aggregate-baseline', 'recovery'), Log.replace('FULL_RESERVATION', 'POOLED_SERVICE'),
                        '\n'.join(Log.splitlines()[2:] + Log.splitlines()[:2]), Log + '\n' + Log,
                        Log.replace('phase_index=1', 'phase_index=0'), Log.replace('phase_index=0 ', ''),
                        '\n'.join(Log.splitlines()[Index] for Index in (0, 2, 1, 3, 4, 5)),
                        Log.replace('qpc_before=300 qpc_after=301', 'qpc_before=100 qpc_after=101')):
                self.assertEqual(VALIDATOR.Validate(Metadata, Bad, 'Aggregate32Structural', File)['State'], 'INCOMPLETE')
            Original = File.read_text()
            for Bad in (Original.replace('999,', '1,'), Original.replace('43,42', '43,99'),
                        Original.replace(',36,5,', ',36,99,'), Original.replace(',50,2,', ',1,3,')):
                File.write_text(Bad)
                self.assertEqual(VALIDATOR.Validate(Metadata, Log, 'Aggregate32Structural', File)['State'], 'INCOMPLETE')
            self.assertEqual(VALIDATOR.Validate(Metadata, Log, 'Aggregate32Structural', None)['State'], 'INCOMPLETE')

    def test_aggregate_validator_cli_accepts_only_complete_fixed_case(self):
        for Valid in (True, False):
            with tempfile.TemporaryDirectory() as Directory:
                Root = Path(Directory)
                Metadata, Log, _ = self.AggregateFixture(Root)
                if not Valid:
                    Metadata['WorkloadArguments'] = ['--reliable-workload-32-structural', '--pooled']
                (Root / 'metadata.json').write_text(json.dumps(Metadata), encoding='utf-8')
                (Root / 'workload.stdout.txt').write_text(Log, encoding='utf-8')
                Result = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/ci/SchedulerTraceValidate.py'), '--root', str(Root),
                    '--case', 'Aggregate32Structural'], text=True, capture_output=True, timeout=30,
                    creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                self.assertEqual(Result.returncode, 0 if Valid else 125, Result.stdout + Result.stderr)
                Coverage = json.loads((Root / 'coverage.json').read_text())
                self.assertEqual(Coverage['State'], 'LOSS_FREE_ANCHOR_WINDOW_RETAINED' if Valid else 'INCOMPLETE')
                self.assertEqual(Coverage['CausalVerdict'], 'NOT_CLAIMED')

    def test_normal_ci_preserves_five_workloads_with_fixed_fifth_traced_once(self):
        Workflow = (ROOT / '.github/workflows/native-ci.yml').read_text()
        Start = Workflow.index('      - name: Run qualified reliable workload timing in Release')
        End = Workflow.index('      - name:', Start + 20)
        Commands = Workflow[Start:End]
        for Flags in ('--pooled --reliable-workload\n', '--pooled --reliable-workload-32-structural\n',
                      'tests.exe --reliable-workload\n', 'tests.exe --reliable-workload-32\n'):
            self.assertEqual(Commands.count(Flags), 1)
        self.assertEqual(Commands.count('SchedulerTrace.ps1 -Case Aggregate32Structural'), 1)
        self.assertNotIn('tests.exe --reliable-workload-32-structural', Commands)
        Build = Workflow[Workflow.index('      - name: Build bounded scheduler diagnostic'):Workflow.index('      - name: Trace one FULL')]
        self.assertNotIn('        if:', Build)
        self.assertIn('test_scheduler_trace.py', Build)
        self.assertIn('SCHEDULER_TRACE_TEST_HELPER=', Build)
        Upload = Workflow[Workflow.index('      - name: Upload native CI diagnostics'):]
        self.assertIn('        if: always()', Upload)
        self.assertIn('            build-ci/scheduler-trace-aggregate32-structural/', Upload)
    def StatsFixture(self, Root):
        Metadata, Log = self.Fixture()
        Metadata.update(WorkloadCase='AckStats', ChildExitCode=1, UnsupportedLifecycleEvents=0, DecodedRows=2)
        Log = Log.replace('case=recovery', 'case=ackstats-control').replace('profile=FULL_RESERVATION', 'profile=ACK_STATS')
        Log += '\n[Qualification:AckStatsServiceThread] case=ackstats-control sequence=0 pid=42 native_tid=44 native_valid=1 qpc_before=30 qpc_after=31 qpc_frequency=10000000'
        Log += '\n[Qualification:AckStatsNativeSnapshot] case=ackstats-control stage=grant-failure side=sender token=7 available=1 native_us=3000000050000 pid=42 native_tid=43 native_valid=1 qpc_before=400 qpc_after=402 qpc_frequency=10000000'
        Log += '\n[Qualification:AckStatsDiagnostic] thread_count=1 valid=1'
        File = Root / 'scheduler.csv'
        with File.open('w', encoding='ascii', newline='') as Stream:
            Writer = csv.DictWriter(Stream, fieldnames=VALIDATOR.CSV_FIELDS)
            Writer.writeheader()
            for Tick, Opcode in ((20, 1), (800, 2)):
                Writer.writerow({'Qpc': Tick, 'Opcode': Opcode, 'Version': 3, 'TargetTid': 44, 'TargetPid': 42})
        return Metadata, Log, File

    def test_ack_stats_snapshot_and_service_lifecycle_without_workload(self):
        with tempfile.TemporaryDirectory() as Directory:
            Metadata, Log, File = self.StatsFixture(Path(Directory))
            Result = VALIDATOR.Validate(Metadata, Log, 'AckStats', File)
            self.assertEqual(Result['State'], 'LOSS_FREE_ANCHOR_WINDOW_RETAINED', Result)
            self.assertEqual(Result['ServiceThreadCount'], 1)
            self.assertEqual(Result['NativeClockMapping'], 'SNAPSHOT_BRACKETS_ONLY_NO_GLOBAL_OFFSET')
            self.assertEqual(Result['FiniteGrantTimestampResolutionNs'], 1000)

    def test_ack_stats_bad_snapshots_lifecycle_and_bounds_fail(self):
        with tempfile.TemporaryDirectory() as Directory:
            Metadata, Log, File = self.StatsFixture(Path(Directory))
            for Old, New in (('thread_count=1', 'thread_count=17'), ('pid=42 native_tid=44', 'pid=9 native_tid=44'),
                    ('native_us=3000000050000', 'native_us=0'), ('available=1', 'available=0'),
                    ('qpc_before=400', 'qpc_before=950'), ('sequence=0', 'sequence=1'),
                    ('side=sender', 'side=unknown'), ('thread_count=1 valid=1', 'thread_count=1 valid=0')):
                with self.subTest(Old=Old):
                    self.assertIn(Old, Log)
                    self.assertEqual(VALIDATOR.Validate(Metadata, Log.replace(Old, New), 'AckStats', File)['State'], 'INCOMPLETE')
            Changed = dict(Metadata, ChildExitCode=0)
            self.assertEqual(VALIDATOR.Validate(Changed, Log, 'AckStats', File)['State'], 'INCOMPLETE')
            OriginalCsv = File.read_text()
            for Old, New in (('44,42', '44,99'), ('800,', '19,'), (',2,3,', ',1,3,')):
                self.assertIn(Old, OriginalCsv)
                File.write_text(OriginalCsv.replace(Old, New))
                self.assertEqual(VALIDATOR.Validate(Metadata, Log, 'AckStats', File)['State'], 'INCOMPLETE')
            for Data in ('Qpc,Opcode,TargetTid,TargetPid\n20,1,44,99\n800,2,44,99\n',
                         'Qpc,Opcode,TargetTid,TargetPid\n20,1,44,42\n',
                         'Qpc,Opcode,TargetTid,TargetPid\n800,2,44,42\n20,1,44,42\n'):
                File.write_text(Data)
                self.assertEqual(VALIDATOR.Validate(Metadata, Log, 'AckStats', File)['State'], 'INCOMPLETE')

    def test_successful_ack_stats_requires_both_retained_service_lifetimes(self):
        with tempfile.TemporaryDirectory() as Directory:
            Metadata, Log, File = self.StatsFixture(Path(Directory))
            Metadata.update(ChildExitCode=0, DecodedRows=4)
            Lines = Log.splitlines()
            Control = [Line.replace('steady_ns=90000 qpc_before=900 qpc_after=901',
                                    'steady_ns=45000 qpc_before=450 qpc_after=451') for Line in Lines[:2]]
            Prompt = [Line.replace('ackstats-control', 'ackstats-prompt').replace(
                'steady_ns=1000 qpc_before=10 qpc_after=11', 'steady_ns=50000 qpc_before=500 qpc_after=501') for Line in Lines[:2]]
            Log = '\n'.join(Control + Prompt + Lines[2:4] + [
                Lines[2].replace('ackstats-control', 'ackstats-prompt').replace('sequence=0', 'sequence=1').replace(
                    'qpc_before=30 qpc_after=31', 'qpc_before=520 qpc_after=521'),
                Lines[3].replace('ackstats-control', 'ackstats-prompt').replace(
                    'qpc_before=400 qpc_after=402', 'qpc_before=600 qpc_after=602'),
                Lines[4].replace('thread_count=1', 'thread_count=2')])
            with File.open('w', encoding='ascii', newline='') as Stream:
                Writer = csv.DictWriter(Stream, fieldnames=VALIDATOR.CSV_FIELDS)
                Writer.writeheader()
                for Tick, Opcode in ((20, 1), (440, 2), (510, 1), (800, 2)):
                    Writer.writerow({'Qpc': Tick, 'Opcode': Opcode, 'Version': 3, 'TargetTid': 44, 'TargetPid': 42})
            Result = VALIDATOR.Validate(Metadata, Log, 'AckStats', File)
            self.assertEqual(Result['State'], 'LOSS_FREE_ANCHOR_WINDOW_RETAINED', Result)
            self.assertEqual(Result['ServiceThreadCount'], 2)

    def test_wrapper_rejects_arbitrary_case_and_pair_case_combination(self):
        for Arguments in (['-Case', 'arbitrary'], ['-Pair', '-Case', 'Full'],
                          ['-Pair', '-Case', 'Aggregate32Structural']):
            Result = subprocess.run(['pwsh', '-NoProfile', '-File', str(ROOT / 'tools/ci/SchedulerTrace.ps1'), *Arguments],
                text=True, capture_output=True, timeout=30, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            self.assertNotEqual(Result.returncode, 0)
    def Fixture(self):
        Metadata = {"Format": "GargantuanSchedulerTrace", "Version": 1}
        for Name in ("StartStatus", "QueryStatus", "StopStatus", "EventsLost", "LogBuffersLost", "RealTimeBuffersLost",
                     "HeaderEventsLost", "HeaderBuffersLost", "DecodeOpenStatus", "DecodeProcessStatus", "UnsupportedEvents"):
            Metadata[Name] = 0
        for Name in ("DiagnosticComplete", "ChildResumed", "ChildTreeReaped"):
            Metadata[Name] = True
        for Name in ("TimedOut", "ChildLogCapped", "ChildLogFailed", "CsvCapped", "DecodeTimedOut"):
            Metadata[Name] = False
        Metadata.update(ChildLaunchAttempts=1, ClockType=1, QpcFrequency=10000000, ControllerQpcFrequency=10000000,
                        ControllerStartQpc=1, AfterTraceStartQpc=2, BeforeChildResumeQpc=3, AfterChildExitQpc=1000,
                        ControllerEndQpc=1001, HeaderStartFileTime=1, HeaderEndFileTime=2,
                        ChildPid=42, ChildMainTid=43, MainFirstQpc=4, MainLastQpc=999)
        Lines = []
        for Boundary, Tick in (("BEGIN", 10), ("END", 900)):
            Lines.append(f"[Qualification:ClockAnchor] case=recovery profile=FULL_RESERVATION boundary={Boundary} pid=42 native_tid=43 native_valid=1 steady_ns={Tick * 100} qpc_before={Tick} qpc_after={Tick + 1} qpc_frequency=10000000")
        return Metadata, "\n".join(Lines)

    def test_loss_free_fixture_window_retained_without_causal_claim(self):
        Result = VALIDATOR.Validate(*self.Fixture())
        self.assertEqual(Result["State"], "LOSS_FREE_ANCHOR_WINDOW_RETAINED", Result)
        self.assertEqual(Result["CausalVerdict"], "NOT_CLAIMED")

    def test_loss_clock_coverage_and_cleanup_mutations_fail(self):
        Metadata, Log = self.Fixture()
        for Name, Value in (("EventsLost", 1), ("HeaderBuffersLost", 1), ("MainFirstQpc", 11),
                            ("MainLastQpc", 899), ("ChildPid", 9), ("ChildMainTid", 9),
                            ("ControllerQpcFrequency", 1), ("ClockType", 2), ("CsvCapped", True),
                            ("ChildTreeReaped", False), ("TimedOut", True), ("ChildLogCapped", True),
                            ("UnsupportedEvents", 1), ("StopStatus", 1), ("BeforeChildResumeQpc", 11),
                            ("WorkloadCase", "AckStats"), ("WorkloadCase", None)):
            Changed = copy.deepcopy(Metadata)
            Changed[Name] = Value
            with self.subTest(Name=Name):
                self.assertEqual(VALIDATOR.Validate(Changed, Log)["State"], "INCOMPLETE")

    def test_missing_malformed_or_duplicate_anchors_fail(self):
        Metadata, Log = self.Fixture()
        for Bad in ("", Log.splitlines()[0], Log + "\n" + Log, Log.replace("native_valid=1", "native_valid=0"),
                    Log.replace("qpc_before=10", "qpc_before=INVALID")):
            self.assertEqual(VALIDATOR.Validate(Metadata, Bad)["State"], "INCOMPLETE")
    def test_wrapper_exit_precedence_without_operational_calls(self):
        Result = subprocess.run(
            ["pwsh", "-NoProfile", "-File", str(ROOT / "tools/ci/SchedulerTrace.ps1"), "-SelfTest"],
            text=True, capture_output=True, timeout=30,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        self.assertEqual(Result.returncode, 0, Result.stdout + Result.stderr)
        self.assertIn("wrapper-self-test=PASS no-session-or-child-created", Result.stdout)

    @unittest.skipUnless(os.environ.get("SCHEDULER_TRACE_TEST_HELPER"), "native compile-only helper not supplied")
    def test_native_payload_bounds_and_ownership_without_capture(self):
        Result = subprocess.run(
            [os.environ["SCHEDULER_TRACE_TEST_HELPER"], "--self-test"],
            text=True, capture_output=True, timeout=30,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        self.assertEqual(Result.returncode, 0, Result.stdout + Result.stderr)
        self.assertIn("self-test=PASS v5-layout=PASS no-session-or-child-created", Result.stdout)

    @unittest.skipUnless(os.environ.get("SCHEDULER_TRACE_TEST_HELPER"), "native compile-only helper not supplied")
    def test_native_rejects_reserved_guid_before_any_trace(self):
        for Command in ('--run', '--run-ack-stats', '--run-aggregate32-structural'):
            Result = subprocess.run(
                [os.environ["SCHEDULER_TRACE_TEST_HELPER"], Command, str(ROOT), "00000000-0000-0000-0000-000000000000"],
                text=True, capture_output=True, timeout=30,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
            self.assertEqual(Result.returncode, 125)
            self.assertIn("reserved session GUID", Result.stderr)

    @unittest.skipUnless(os.environ.get("SCHEDULER_TRACE_TEST_HELPER"), "native compile-only helper not supplied")
    def test_native_fixed_case_rejects_arbitrary_flags_before_trace(self):
        for Arguments in (['--run-arbitrary', str(ROOT), '11111111-2222-4333-8444-555555555555'],
                          ['--run-aggregate32-structural', str(ROOT), '11111111-2222-4333-8444-555555555555', '--pooled']):
            Result = subprocess.run([os.environ['SCHEDULER_TRACE_TEST_HELPER'], *Arguments], text=True,
                capture_output=True, timeout=30, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            self.assertEqual(Result.returncode, 125)
            self.assertIn('invalid arguments', Result.stderr)


if __name__ == "__main__":
    unittest.main()
