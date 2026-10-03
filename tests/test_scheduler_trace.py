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
            def Emit(Label, **Fields):
                Lines.append('[Qualification:' + Label + '] case=' + Name +
                    ' profile=FULL_RESERVATION phase_index=' + str(Index) + ' ' +
                    ' '.join(Key + '=' + str(Value) for Key, Value in Fields.items()))
            Emit('AggregateOperationCoverage', observed_steps=480, snapshots=0, capacity=64, invalid=0,
                 overflow=0, selection='existing_p95_gap_ge_150ms', coverage='SAMPLED_NOT_COMPLETE_CAUSAL_PROOF')
            for Peer in range(33):
                for Kind in ('poll', 'engine', 'session'):
                    Emit('AggregateOperationSpan', peer=Peer, side='server' if Peer == 32 else 'client',
                         slot=0 if Peer == 32 else Peer + 1, generation=0 if Peer == 32 else 1,
                         coverage='phase_maximum', tick=1, subphase=Kind, start_ns=Begin * 100 + 50,
                         end_ns=Begin * 100 + 60, native_tid=43, timestamps_valid=1, thread_cpu_ms=0,
                         process_cpu_ms=0, before_thread_100ns=1, after_thread_100ns=1,
                         before_process_100ns=1, after_process_100ns=1, before_thread_valid=1,
                         after_thread_valid=1, before_process_valid=1, after_process_valid=1)
            Active = 32 if Index == 1 else 8
            Count = 16 if Index == 1 else 4
            for Peer in range(32):
                Context = dict(peer=Peer, slot=Peer + 1, generation=1)
                Emit('AggregatePeerEvidence', **Context, configured=1, invalid=0)
                Emit('RemoteChronology', **Context, records=Count if Peer < Active else 0,
                     capacity=208, invalid=0, overflow=0)
                for Id in range(1, (Count if Peer < Active else 0) + 1):
                    Start = Begin * 100 + 100 + Id * 20
                    Finished = Start + 10
                    Emit('RemoteSpan', **Context, kind='RPC', id=Id, recovery_probe=0, outcome='TERMINAL',
                         start_step=1, start_ns=Start, start_tid=43, submitted_step=1, submitted_ns=Start + 5,
                         submitted_tid=43, accepted=1, terminal=1, end_step=2, end_ns=Finished, end_tid=43,
                         terminal_status=0, payload_matched=1, observed_step=480, observed_ns=End * 100)
                    Counters = {Boundary + '_' + Kind + '_valid': 1
                        for Boundary in ('start_before', 'start_after', 'end_before', 'end_after')
                        for Kind in ('thread', 'process')}
                    Counters.update({Boundary + '_' + Kind + '_100ns': Value
                        for Boundary, Value in zip(('start_before', 'start_after', 'end_before', 'end_after'), (1, 2, 3, 4))
                        for Kind in ('thread', 'process')})
                    Emit('RemoteResourceSpan', **Context, **Counters, kind='RPC', id=Id, start_ns=Start,
                         end_ns=Finished, start_tid=43, end_tid=43, terminal=1, endpoint_order_valid=1,
                         start_sample_before_ns=Start - 1, start_sample_after_ns=Start + 1,
                         end_sample_before_ns=Finished - 1, end_sample_after_ns=Finished + 1,
                         thread_cpu_lower_100ns=1, thread_cpu_upper_100ns=3,
                         process_cpu_lower_100ns=1, process_cpu_upper_100ns=3,
                         start_sleep_count=0, start_sleep_requested_ns=0, start_sleep_actual_ns=0,
                         start_sleep_last_completed_ns=0, start_sleep_valid=1, end_sleep_count=0,
                         end_sleep_requested_ns=0, end_sleep_actual_ns=0, end_sleep_last_completed_ns=0,
                         end_sleep_valid=1, sleep_delta_valid=1, measured_sleep_count=0,
                         measured_sleep_requested_ns=0, measured_sleep_actual_ns=0)
            for Peer in range(Active):
                Lines.append(f'[Qualification:Aggregate] overload={int(Index == 1)} connected=32 active={Active} peer={Peer} accepted={Count} completed={Count} errors=0')
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
            self.assertEqual(Result['CompleteAggregatePhases'], 3)
            self.assertEqual(Result['CompleteRpcRecords'], 576)
            # A first-phase assertion failure preserves its complete prefix;
            # loss-free tracing cannot turn the child's nonzero exit into PASS.
            Metadata['ChildExitCode'] = 17
            Prefix = Log[:Log.index('[Qualification:ClockAnchor] case=aggregate-overload')].rstrip()
            Result = VALIDATOR.Validate(Metadata, Prefix, 'Aggregate32Structural', File)
            self.assertEqual(Result['State'], 'LOSS_FREE_ANCHOR_WINDOW_RETAINED', Result)
            Metadata['ChildExitCode'] = 0
            self.assertEqual(VALIDATOR.Validate(Metadata, Prefix,
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
            Anchors = [Line for Line in Log.splitlines() if Line.startswith('[Qualification:ClockAnchor]')]
            for Bad in (Log.replace('aggregate-baseline', 'recovery'), Log.replace('FULL_RESERVATION', 'POOLED_SERVICE'),
                        '\n'.join(Anchors[2:] + Anchors[:2]), Log + '\n' + Log,
                        Log.replace('phase_index=1', 'phase_index=0'), Log.replace('phase_index=0 ', ''),
                        '\n'.join(Anchors[Index] for Index in (0, 2, 1, 3, 4, 5)),
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

    def test_aggregate_complete_rpc_and_operation_evidence_denials(self):
        with tempfile.TemporaryDirectory() as Directory:
            Metadata, Log, File = self.AggregateFixture(Path(Directory))
            Lines = Log.splitlines()
            def Changed(Label, Old=None, New=None, Drop=False, Duplicate=False):
                Output = list(Lines)
                Index = next(Index for Index, Line in enumerate(Output) if Line.startswith('[Qualification:' + Label + ']'))
                if Drop: Output.pop(Index)
                elif Duplicate: Output.insert(Index, Output[Index])
                else: Output[Index] = Output[Index].replace(Old, New)
                return '\n'.join(Output)
            Mutations = [('\n'.join(Line for Line in Lines if Line.startswith('[Qualification:ClockAnchor]')), 'anchors-only')]
            for Label in ('AggregatePeerEvidence', 'RemoteChronology', 'RemoteSpan', 'RemoteResourceSpan',
                          'AggregateOperationCoverage', 'AggregateOperationSpan', 'Aggregate'):
                Mutations.extend(((Changed(Label, Drop=True), 'dropped-' + Label),
                                  (Changed(Label, Duplicate=True), 'duplicate-' + Label)))
            for Label, Old, New in (
                    ('AggregatePeerEvidence', 'generation=1', 'generation=2'),
                    ('RemoteSpan', 'phase_index=0', 'phase_index=1'),
                    ('RemoteSpan', 'id=1 ', 'id=9 '),
                    ('RemoteSpan', 'terminal=1', 'terminal=0'),
                    ('RemoteSpan', 'observed_ns=20000', 'observed_ns=19999'),
                    ('RemoteResourceSpan', 'endpoint_order_valid=1', 'endpoint_order_valid=0'),
                    ('RemoteResourceSpan', 'start_sample_before_ns=1119', 'start_sample_before_ns=1121'),
                    ('RemoteResourceSpan', 'thread_cpu_lower_100ns=1', 'thread_cpu_lower_100ns=2'),
                    ('RemoteResourceSpan', 'sleep_delta_valid=1', 'sleep_delta_valid=0'),
                    ('RemoteChronology', 'invalid=0', 'invalid=1'),
                    ('RemoteChronology', 'overflow=0', 'overflow=1'),
                    ('RemoteChronology', 'records=4', 'records=5'),
                    ('Aggregate', 'accepted=4', 'accepted=3'),
                    ('Aggregate', 'completed=4', 'completed=3'),
                    ('Aggregate', 'errors=0', 'errors=1'),
                    ('AggregateOperationCoverage', 'overflow=0', 'overflow=1'),
                    ('AggregateOperationCoverage', 'invalid=0', 'invalid=1'),
                    ('AggregateOperationCoverage', 'SAMPLED_NOT_COMPLETE_CAUSAL_PROOF', 'COMPLETE'),
                    ('AggregateOperationCoverage', 'snapshots=0', 'snapshots=1'),
                    ('AggregateOperationSpan', 'timestamps_valid=1', 'timestamps_valid=0'),
                    ('AggregateOperationSpan', 'generation=1', 'generation=2')):
                Mutations.append((Changed(Label, Old, New), Label + ':' + Old))
            for Bad, Name in Mutations:
                with self.subTest(Name=Name):
                    Result = VALIDATOR.Validate(Metadata, Bad, 'Aggregate32Structural', File)
                    self.assertEqual(Result['State'], 'INCOMPLETE', Result)
                    self.assertEqual(Result['CausalVerdict'], 'NOT_CLAIMED')

    def test_aggregate_missing_single_anchor_cli_retains_incomplete_receipt(self):
        for Boundary in ('BEGIN', 'END'):
            with tempfile.TemporaryDirectory() as Directory:
                Root = Path(Directory)
                Metadata, Log, _ = self.AggregateFixture(Root)
                Lines = Log.splitlines()
                Index = next(Index for Index, Line in enumerate(Lines) if
                    Line.startswith('[Qualification:ClockAnchor] case=aggregate-baseline ') and
                    'boundary=' + Boundary in Line)
                Lines.pop(Index)
                (Root / 'metadata.json').write_text(json.dumps(Metadata), encoding='utf-8')
                (Root / 'workload.stdout.txt').write_text('\n'.join(Lines), encoding='utf-8')
                Result = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/ci/SchedulerTraceValidate.py'),
                    '--root', str(Root), '--case', 'Aggregate32Structural'], text=True, capture_output=True,
                    timeout=30, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                self.assertEqual(Result.returncode, 125, Result.stdout + Result.stderr)
                self.assertNotIn('Traceback', Result.stderr)
                self.assertEqual(json.loads((Root / 'coverage.json').read_text())['State'], 'INCOMPLETE')

    def test_aggregate_failed_rpc_outcomes_and_inline_callback_are_retained(self):
        with tempfile.TemporaryDirectory() as Directory:
            Metadata, Log, File = self.AggregateFixture(Path(Directory))
            Prefix = Log[:Log.index('[Qualification:ClockAnchor] case=aggregate-overload')].rstrip()
            for Accepted in (False, True):
                Lines = Prefix.splitlines()
                SpanIndex = next(Index for Index, Line in enumerate(Lines) if Line.startswith('[Qualification:RemoteSpan]'))
                ResourceIndex = SpanIndex + 1
                Span = dict(Part.split('=', 1) for Part in Lines[SpanIndex].split()[1:])
                Resource = dict(Part.split('=', 1) for Part in Lines[ResourceIndex].split()[1:])
                Span.update(outcome='MISSING_AT_CASE_END' if Accepted else 'REJECTED', accepted=str(int(Accepted)),
                            terminal='0', end_step='0', end_ns='0', end_tid='0', terminal_status='-1', payload_matched='0')
                Resource.update(terminal='0', end_ns='0', end_tid='0', endpoint_order_valid='0',
                                end_sample_before_ns='0', end_sample_after_ns='0', sleep_delta_valid='0')
                for Key in list(Resource):
                    if Key.startswith('end_'):
                        Resource[Key] = '0'
                    if '_cpu_lower_' in Key or '_cpu_upper_' in Key or Key.startswith('measured_sleep_'):
                        Resource[Key] = 'NOT_MEASURED'
                Lines[SpanIndex] = '[Qualification:RemoteSpan] ' + ' '.join(Key + '=' + Value for Key, Value in Span.items())
                Lines[ResourceIndex] = '[Qualification:RemoteResourceSpan] ' + ' '.join(Key + '=' + Value for Key, Value in Resource.items())
                SummaryIndex = next(Index for Index, Line in enumerate(Lines) if Line.startswith('[Qualification:Aggregate]') and 'peer=0 ' in Line)
                Lines[SummaryIndex] = Lines[SummaryIndex].replace('completed=4', 'completed=3').replace(
                    'accepted=4', 'accepted=4' if Accepted else 'accepted=3').replace('errors=0', 'errors=0' if Accepted else 'errors=1')
                Result = VALIDATOR.Validate(dict(Metadata, ChildExitCode=17), '\n'.join(Lines), 'Aggregate32Structural', File)
                self.assertEqual(Result['State'], 'LOSS_FREE_ANCHOR_WINDOW_RETAINED', Result)
                self.assertEqual(Result['CausalVerdict'], 'NOT_CLAIMED')
                self.assertEqual(VALIDATOR.Validate(Metadata, '\n'.join(Lines), 'Aggregate32Structural', File)['State'], 'INCOMPLETE')
            # The real StartRequest may invoke an accepted terminal callback
            # synchronously before Submitted observes its return; do not reorder it.
            Lines = Log.splitlines()
            Index = next(Index for Index, Line in enumerate(Lines) if Line.startswith('[Qualification:RemoteSpan]'))
            Lines[Index] = Lines[Index].replace('submitted_step=1', 'submitted_step=3').replace('submitted_ns=1125', 'submitted_ns=1135')
            Result = VALIDATOR.Validate(Metadata, '\n'.join(Lines), 'Aggregate32Structural', File)
            self.assertEqual(Result['State'], 'LOSS_FREE_ANCHOR_WINDOW_RETAINED', Result)

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
