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
    def HostedAppraisal(self, Mode, Environment):
        Workflow = (ROOT / '.github/workflows/native-ci.yml').read_text()
        Start = Workflow.index('      - name: Quiesce hosted compatibility appraisal')
        Step = Workflow[Start:Workflow.index('      - name:', Start + 20)]
        self.assertIn('        timeout-minutes: 1\n', Step)
        self.assertLess(Workflow.index('      - name: Verify recursive dependency checkout'), Start)
        self.assertLess(Start, Workflow.index('      - name: Install pinned repository tools'))
        Body = Step[Step.index('        run: |\n') + len('        run: |\n'):]
        Body = '\n'.join(Line[10:] for Line in Body.splitlines())
        for Forbidden in ('Stop-Process', 'taskkill', 'Set-Service', 'PriorityClass', 'ProcessorAffinity'):
            self.assertNotIn(Forbidden, Body)
        Prelude = r'''
$ErrorActionPreference='Stop'
$Calls=[Collections.Generic.List[string]]::new()
$Disabled=$false
function Get-ScheduledTask {
    [CmdletBinding()] param([string]$TaskPath,[string]$TaskName)
    $Calls.Add('Get')
    if($TaskPath -cne '\Microsoft\Windows\Application Experience\' -or $TaskName -cne 'Microsoft Compatibility Appraiser') { throw 'Wrong task selection' }
    if($env:APPRAISAL_TEST_MODE -eq 'get-error') { throw 'Supplied task query failure' }
    if($env:APPRAISAL_TEST_MODE -eq 'missing') { return }
    $Name=if($env:APPRAISAL_TEST_MODE -eq 'wrong'){'Other task'}else{$TaskName}
    $State=if($Disabled -and $env:APPRAISAL_TEST_MODE -ne 'enabled'){'Disabled'}else{'Ready'}
    [pscustomobject]@{TaskPath=$TaskPath;TaskName=$Name;State=$State}
    if($env:APPRAISAL_TEST_MODE -eq 'duplicate') { [pscustomobject]@{TaskPath=$TaskPath;TaskName=$Name;State=$State} }
}
function Disable-ScheduledTask {
    [CmdletBinding()] param([string]$TaskPath,[string]$TaskName)
    $Calls.Add('Disable')
    if($TaskPath -cne '\Microsoft\Windows\Application Experience\' -or $TaskName -cne 'Microsoft Compatibility Appraiser') { throw 'Wrong disable target' }
    if($env:APPRAISAL_TEST_MODE -eq 'disable-error') { throw 'Supplied disable failure' }
    $script:Disabled=$true
}
function Stop-ScheduledTask {
    [CmdletBinding()] param([string]$TaskPath,[string]$TaskName)
    $Calls.Add('Stop')
    if($TaskPath -cne '\Microsoft\Windows\Application Experience\' -or $TaskName -cne 'Microsoft Compatibility Appraiser') { throw 'Wrong stop target' }
    if($env:APPRAISAL_TEST_MODE -eq 'stop-error') { throw 'Supplied stop failure' }
}
function Get-CimInstance {
    [CmdletBinding()] param([string]$ClassName,[string]$Filter)
    $Calls.Add('Cim')
    if($ClassName -cne 'Win32_Process' -or $Filter -cne "Name='CompatTelRunner.exe'") { throw 'Wrong census scope' }
    if($env:APPRAISAL_TEST_MODE -eq 'cim-error') { throw 'Supplied CIM failure' }
    if($env:APPRAISAL_TEST_MODE -eq 'persistent') { [pscustomobject]@{Name='CompatTelRunner.exe';ProcessId=123} }
}
$Failed=$false
$Reason=''
try {
'''
        Trailer = r'''
} catch { $Failed=$true; $Reason=$_.Exception.Message }
Write-Output ('HOST_BOUNDARY_RESULT=' + ([ordered]@{Failed=$Failed;Reason=$Reason;Calls=@($Calls)}|ConvertTo-Json -Compress))
'''
        Env = dict(os.environ, APPRAISAL_TEST_MODE=Mode)
        for Name in ('GITHUB_ACTIONS', 'RUNNER_ENVIRONMENT', 'RUNNER_OS'):
            Env.pop(Name, None)
        Env.update(Environment)
        with tempfile.TemporaryDirectory() as Directory:
            Script = Path(Directory) / 'mock-host.ps1'
            Script.write_text(Prelude + Body + Trailer)
            Result = subprocess.run(['pwsh', '-NoProfile', '-NonInteractive', '-File', str(Script)],
                env=Env, text=True, capture_output=True, timeout=30,
                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        self.assertEqual(Result.returncode, 0, Result.stdout + Result.stderr)
        Row = next(Line.removeprefix('HOST_BOUNDARY_RESULT=') for Line in Result.stdout.splitlines()
                   if Line.startswith('HOST_BOUNDARY_RESULT='))
        return json.loads(Row), Result.stdout

    def test_hosted_appraisal_guards_precede_all_task_queries_and_mutations(self):
        Hosted = dict(GITHUB_ACTIONS='true', RUNNER_ENVIRONMENT='github-hosted', RUNNER_OS='Windows')
        for Name, Bad in (('GITHUB_ACTIONS', 'false'), ('GITHUB_ACTIONS', 'True'),
                          ('RUNNER_ENVIRONMENT', 'self-hosted'), ('RUNNER_OS', 'Linux')):
            with self.subTest(Name=Name, Bad=Bad):
                Row, _ = self.HostedAppraisal('happy', dict(Hosted, **{Name: Bad}))
                self.assertTrue(Row['Failed'])
                self.assertEqual(Row['Calls'], [])
        Row, _ = self.HostedAppraisal('happy', {})
        self.assertTrue(Row['Failed'])
        self.assertEqual(Row['Calls'], [])

    def test_hosted_appraisal_exact_task_disabled_and_absent_or_fail_closed(self):
        Hosted = dict(GITHUB_ACTIONS='true', RUNNER_ENVIRONMENT='github-hosted', RUNNER_OS='Windows')
        Row, Log = self.HostedAppraisal('happy', Hosted)
        self.assertFalse(Row['Failed'], Row)
        self.assertEqual(Row['Calls'], ['Get', 'Disable', 'Stop', 'Get', 'Cim'])
        self.assertIn('[CI:HostResources] task_path=\\Microsoft\\Windows\\Application Experience\\ task_name=Microsoft Compatibility Appraiser task_state=Disabled process_count=0', Log)
        for Mode in ('wrong', 'missing', 'duplicate', 'get-error', 'enabled', 'disable-error', 'stop-error', 'cim-error', 'persistent'):
            with self.subTest(Mode=Mode):
                Row, _ = self.HostedAppraisal(Mode, Hosted)
                self.assertTrue(Row['Failed'], Row)
                if Mode in ('wrong', 'missing', 'duplicate', 'get-error'):
                    self.assertEqual(Row['Calls'], ['Get'])
                if Mode == 'persistent':
                    self.assertIn('within 15 seconds; process_count=1', Row['Reason'])
                    self.assertGreater(Row['Calls'].count('Cim'), 1)

    def FullFixture(self, Root, Exit=0, Cases=8, Case='Full'):
        Metadata, Aggregate, File = self.AggregateFixture(Root)
        Metadata.update(WorkloadCase=Case, WorkloadArguments=['--reliable-workload'], ChildExitCode=Exit)
        if Case == 'PooledFull':
            Metadata['WorkloadArguments'].insert(0, '--pooled')
        Templates = {Label: next(dict(Part.split('=', 1) for Part in Line.split()[1:])
            for Line in Aggregate.splitlines() if Line.startswith('[Qualification:' + Label + ']'))
            for Label in ('RemoteSpan', 'RemoteResourceSpan')}
        Lines = []
        for Index, Name in enumerate(VALIDATOR.FULL_CASES[:Cases]):
            Begin, End = 10 + Index * 110, 100 + Index * 110
            def Emit(Label, **Fields):
                Lines.append('[Qualification:' + Label + '] case=' + Name + ' ' +
                             ' '.join(Key + '=' + str(Value) for Key, Value in Fields.items()))
            for Boundary, Tick in (('BEGIN', Begin), ('END', End)):
                Emit('ClockAnchor', profile='FULL_RESERVATION', boundary=Boundary, pid=42, native_tid=43,
                     native_valid=1, steady_ns=Tick * 100, qpc_before=Tick, qpc_after=Tick + 1,
                     qpc_frequency=10000000)
            Emit('ReliableGameplay', rpc_count=1, event_count=1, action_count=1, rejected=0, errors=0)
            Emit('RemoteChronology', profile='FULL_RESERVATION', records=2, capacity=1062, invalid=0, overflow=0)
            for Offset, Kind in enumerate(('RPC', 'EVENT')):
                Start = Begin * 100 + 100 + Offset * 20
                Finished = Start + 10
                for Label in ('RemoteSpan', 'RemoteResourceSpan'):
                    Row = {Key: Value for Key, Value in Templates[Label].items()
                           if Key not in ('case', 'phase_index', 'peer', 'slot', 'generation')}
                    Row.update(id=Index * 2 + Offset + 1, kind=Kind, start_ns=Start, end_ns=Finished)
                    if Label == 'RemoteSpan':
                        Row.update(submitted_ns=Start + 5, observed_ns=End * 100 + 1,
                                   terminal_status=0 if Kind == 'RPC' else -1)
                    else:
                        Row.update(start_sample_before_ns=Start - 1, start_sample_after_ns=Start + 1,
                                   end_sample_before_ns=Finished - 1, end_sample_after_ns=Finished + 1)
                    Emit(Label, **Row)
            Emit('ActionChronology', profile='FULL_RESERVATION', records=1, capacity=29, invalid=0, overflow=0)
            Emit('ActionSpan', profile='FULL_RESERVATION', sequence=Index + 1, recovery_probe=0,
                 outcome='COMPLETED', start_step=1, observed_end_step=2, start_ns=Begin * 100 + 50,
                 observed_end_ns=Begin * 100 + 60, wall_ms=.00001, thread_cpu_ms=0, process_cpu_ms=0)
            for Phase in ('step_interval', 'client_runtime', 'server_runtime', 'server_poll', 'client_poll',
                          'server_session', 'client_session', 'observer', 'sleep'):
                Emit('WorkloadCpu', phase=Phase, step=1, wall_ms=0, thread_cpu_ms=0, process_cpu_ms=0,
                     requested_sleep_ms=0, actual_sleep_ms=0)
                Emit('WorkloadPhaseSpan', profile='FULL_RESERVATION', phase=Phase, step=1, native_tid=43,
                     start_ns=Begin * 100 + 50, end_ns=Begin * 100 + 60, timestamps_valid=1)
        Log = '\n'.join(Lines)
        if Case == 'PooledFull':
            Log = Log.replace('profile=FULL_RESERVATION', 'profile=POOLED_SERVICE')
        return Metadata, Log, File

    def test_pooled_full_preserves_failure_prefix_and_strict_case_arguments_profile(self):
        with tempfile.TemporaryDirectory() as Directory:
            Metadata, Log, File = self.FullFixture(Path(Directory), Exit=17, Cases=4, Case='PooledFull')
            Result = VALIDATOR.Validate(Metadata, Log, 'PooledFull', File)
            self.assertEqual(Result['State'], 'LOSS_FREE_ANCHOR_WINDOW_RETAINED', Result)
            self.assertEqual(Result['CompleteFullCases'], 4)
            self.assertEqual(Result['FixedWorkloadArgument'], '--pooled --reliable-workload')
            self.assertEqual(Result['CausalVerdict'], 'NOT_CLAIMED')
            self.assertEqual(Metadata['ChildExitCode'], 17)
            self.assertEqual(VALIDATOR.Validate(dict(Metadata, ChildExitCode=0), Log, 'PooledFull', File)['State'], 'INCOMPLETE')
            for Arguments in (['--reliable-workload'], ['--pooled', '--reliable-workload-32-structural'],
                              ['--reliable-workload', '--pooled'], ['--pooled', '--reliable-workload', '--extra']):
                with self.subTest(Arguments=Arguments):
                    self.assertEqual(VALIDATOR.Validate(dict(Metadata, WorkloadArguments=Arguments), Log,
                        'PooledFull', File)['State'], 'INCOMPLETE')
            for Changed in (Log.replace('profile=POOLED_SERVICE', 'profile=FULL_RESERVATION'),
                            Log.replace('profile=POOLED_SERVICE', 'profile=FULL_RESERVATION', 1)):
                self.assertEqual(VALIDATOR.Validate(Metadata, Changed, 'PooledFull', File)['State'], 'INCOMPLETE')
            self.assertEqual(VALIDATOR.Validate(Metadata, Log, 'Full', File)['State'], 'INCOMPLETE')

    def test_pooled_full_complete_success_cannot_substitute_unpooled_profile(self):
        with tempfile.TemporaryDirectory() as Directory:
            Metadata, Log, File = self.FullFixture(Path(Directory), Case='PooledFull')
            Result = VALIDATOR.Validate(Metadata, Log, 'PooledFull', File)
            self.assertEqual(Result['State'], 'LOSS_FREE_ANCHOR_WINDOW_RETAINED', Result)
            self.assertEqual(Result['CompleteFullCases'], 8)
            Metadata, Log, File = self.FullFixture(Path(Directory))
            self.assertEqual(VALIDATOR.Validate(dict(Metadata, WorkloadArguments=['--pooled', '--reliable-workload']),
                Log, 'Full', File)['State'], 'INCOMPLETE')
            self.assertEqual(VALIDATOR.Validate(Metadata, Log.replace('profile=FULL_RESERVATION', 'profile=POOLED_SERVICE'),
                'Full', File)['State'], 'INCOMPLETE')

    def AggregateFixture(self, Root, Exit=0, SameLocalIdentity=False, Case='Aggregate32Structural'):
        Metadata, _ = self.Fixture()
        Metadata.update(WorkloadCase=Case, ChildExitCode=Exit, DecodedRows=4,
                        WorkloadArguments=['--reliable-workload-32' if Case == 'Aggregate32' else '--reliable-workload-32-structural'])
        if Case == 'PooledAggregate32Structural':
            Metadata['WorkloadArguments'].insert(0, '--pooled')
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
                         slot=0 if Peer == 32 else 1 if SameLocalIdentity else Peer + 1, generation=0 if Peer == 32 else 1,
                         coverage='phase_maximum', tick=1, subphase=Kind, start_ns=Begin * 100 + 50,
                         end_ns=Begin * 100 + 60, native_tid=43, timestamps_valid=1, thread_cpu_ms=0,
                         process_cpu_ms=0, before_thread_100ns=1, after_thread_100ns=1,
                         before_process_100ns=1, after_process_100ns=1, before_thread_valid=1,
                         after_thread_valid=1, before_process_valid=1, after_process_valid=1)
            Active = 32 if Index == 1 else 8
            Count = 16 if Index == 1 else 4
            for Peer in range(32):
                Context = dict(peer=Peer, slot=1 if SameLocalIdentity else Peer + 1, generation=1)
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
        Log = '\n'.join(Lines)
        if Case == 'PooledAggregate32Structural':
            Log = Log.replace('profile=FULL_RESERVATION', 'profile=POOLED_SERVICE')
        return Metadata, Log, File

    def test_pooled_fixed_case_retains_exact_arguments_profile_and_failure(self):
        with tempfile.TemporaryDirectory() as Directory:
            Metadata, Log, File = self.AggregateFixture(Path(Directory), Exit=17, Case='PooledAggregate32Structural')
            Result = VALIDATOR.Validate(Metadata, Log, 'PooledAggregate32Structural', File)
            self.assertEqual(Result['State'], 'LOSS_FREE_ANCHOR_WINDOW_RETAINED', Result)
            self.assertEqual(Result['FixedWorkloadArgument'], '--pooled --reliable-workload-32-structural')
            self.assertEqual(Result['CompleteRpcRecords'], 576)
            self.assertEqual(Metadata['ChildExitCode'], 17)
            self.assertEqual(Result['CausalVerdict'], 'NOT_CLAIMED')
            for Arguments in (['--reliable-workload-32-structural'], ['--pooled', '--reliable-workload-32'],
                              ['--reliable-workload-32-structural', '--pooled'], ['--pooled', '--reliable-workload-32-structural', '--extra']):
                with self.subTest(Arguments=Arguments):
                    self.assertEqual(VALIDATOR.Validate(dict(Metadata, WorkloadArguments=Arguments), Log,
                        'PooledAggregate32Structural', File)['State'], 'INCOMPLETE')
            for Changed in (Log.replace('profile=POOLED_SERVICE', 'profile=FULL_RESERVATION'),
                            Log.replace('profile=POOLED_SERVICE', 'profile=FULL_RESERVATION', 1)):
                self.assertEqual(VALIDATOR.Validate(Metadata, Changed, 'PooledAggregate32Structural', File)['State'], 'INCOMPLETE')
            self.assertEqual(VALIDATOR.Validate(Metadata, Log, 'Aggregate32Structural', File)['State'], 'INCOMPLETE')

    def test_unpooled_fixed_case_rejects_pooled_substitution(self):
        with tempfile.TemporaryDirectory() as Directory:
            Metadata, Log, File = self.AggregateFixture(Path(Directory))
            self.assertEqual(VALIDATOR.Validate(dict(Metadata, WorkloadArguments=['--pooled', '--reliable-workload-32-structural']),
                Log, 'Aggregate32Structural', File)['State'], 'INCOMPLETE')
            self.assertEqual(VALIDATOR.Validate(Metadata, Log.replace('profile=FULL_RESERVATION', 'profile=POOLED_SERVICE'),
                'Aggregate32Structural', File)['State'], 'INCOMPLETE')

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

    def test_aggregate32_exact_fourth_case_coverage_and_substitution_denial(self):
        with tempfile.TemporaryDirectory() as Directory:
            Metadata, Log, File = self.AggregateFixture(Path(Directory), Case='Aggregate32')
            Result = VALIDATOR.Validate(Metadata, Log, 'Aggregate32', File)
            self.assertEqual(Result['State'], 'LOSS_FREE_ANCHOR_WINDOW_RETAINED', Result)
            self.assertEqual(Result['FixedWorkloadArgument'], '--reliable-workload-32')
            self.assertEqual(Result['CompleteAggregatePhases'], 3)
            self.assertEqual(Result['CausalVerdict'], 'NOT_CLAIMED')
            for Argument in ('--reliable-workload', '--reliable-workload-32-structural'):
                self.assertEqual(VALIDATOR.Validate(dict(Metadata, WorkloadArguments=[Argument]),
                    Log, 'Aggregate32', File)['State'], 'INCOMPLETE')
            self.assertEqual(VALIDATOR.Validate(Metadata, Log, 'Aggregate32Structural', File)['State'], 'INCOMPLETE')
            Prefix = Log[:Log.index('[Qualification:ClockAnchor] case=aggregate-overload')].rstrip()
            self.assertEqual(VALIDATOR.Validate(dict(Metadata, ChildExitCode=17), Prefix,
                'Aggregate32', File)['State'], 'LOSS_FREE_ANCHOR_WINDOW_RETAINED')
            self.assertEqual(VALIDATOR.Validate(Metadata, Prefix, 'Aggregate32', File)['State'], 'INCOMPLETE')
            for Field, Value in (('EventsLost', 1), ('ChildPriorityClass', 16384),
                                 ('PriorityVerifiedBeforeResume', False), ('ChildTreeReaped', False)):
                self.assertEqual(VALIDATOR.Validate(dict(Metadata, **{Field: Value}),
                    Log, 'Aggregate32', File)['State'], 'INCOMPLETE')

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
        for Case, Valid in ((Case, Valid) for Case in ('Aggregate32', 'Aggregate32Structural') for Valid in (True, False)):
            with tempfile.TemporaryDirectory() as Directory:
                Root = Path(Directory)
                Metadata, Log, _ = self.AggregateFixture(Root, Case=Case)
                if not Valid:
                    Metadata['WorkloadArguments'] = ['--reliable-workload-32-structural', '--pooled']
                (Root / 'metadata.json').write_text(json.dumps(Metadata), encoding='utf-8')
                (Root / 'workload.stdout.txt').write_text(Log, encoding='utf-8')
                Result = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/ci/SchedulerTraceValidate.py'), '--root', str(Root),
                    '--case', Case], text=True, capture_output=True, timeout=30,
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

    def test_aggregate_connection_identity_is_local_to_each_peer_transport(self):
        with tempfile.TemporaryDirectory() as Directory:
            Metadata, Log, File = self.AggregateFixture(Path(Directory), SameLocalIdentity=True)
            Result = VALIDATOR.Validate(Metadata, Log, 'Aggregate32Structural', File)
            self.assertEqual(Result['State'], 'LOSS_FREE_ANCHOR_WINDOW_RETAINED', Result)
            Lines = Log.splitlines()
            for Index, Line in enumerate(Lines):
                if 'case=aggregate-overload ' in Line and 'peer=0 ' in Line:
                    Lines[Index] = Line.replace('generation=1', 'generation=2')
            Result = VALIDATOR.Validate(Metadata, '\n'.join(Lines), 'Aggregate32Structural', File)
            self.assertEqual(Result['State'], 'INCOMPLETE', Result)

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

    def test_normal_ci_preserves_order_with_full_and_aggregate_traced_once(self):
        Workflow = (ROOT / '.github/workflows/native-ci.yml').read_text()
        Start = Workflow.index('      - name: Run qualified reliable workload timing in Release')
        End = Workflow.index('      - name:', Start + 20)
        Commands = Workflow[Start:End]
        for Flags in ('SchedulerTrace.ps1 -Case PooledFull\n', 'SchedulerTrace.ps1 -Case PooledAggregate32Structural\n',
                      'SchedulerTrace.ps1 -Case Full\n', 'SchedulerTrace.ps1 -Case Aggregate32\n'):
            self.assertEqual(Commands.count(Flags), 1)
        self.assertEqual(Commands.count('SchedulerTrace.ps1 -Case Aggregate32Structural'), 1)
        self.assertEqual(Commands.count('if not %errorlevel%==0 exit /b %errorlevel%'), 5)
        self.assertNotIn('if errorlevel 1', Commands)
        Positions = [Commands.index(Flags) for Flags in ('SchedulerTrace.ps1 -Case PooledFull\n',
            'SchedulerTrace.ps1 -Case PooledAggregate32Structural\n', 'SchedulerTrace.ps1 -Case Full\n',
            'SchedulerTrace.ps1 -Case Aggregate32\n', 'SchedulerTrace.ps1 -Case Aggregate32Structural')]
        self.assertEqual(Positions, sorted(Positions))
        self.assertNotIn('tests.exe --reliable-workload-32-structural', Commands)
        self.assertNotIn('tests.exe --reliable-workload\n', Commands)
        self.assertNotIn('tests.exe --reliable-workload-32\n', Commands)
        self.assertNotIn('tests.exe --pooled --reliable-workload-32-structural', Commands)
        self.assertNotIn('tests.exe --pooled --reliable-workload', Commands)
        self.assertIn('if not "%SCHEDULER_TRACE%"=="true" (\n            pwsh -NoProfile -File tools\\ci\\SchedulerTrace.ps1 -Case Full\n          )', Commands)
        Manual = Workflow[Workflow.index('      - name: Trace one FULL'):Workflow.index('      - name: Trace fixed FULL')]
        self.assertIn("github.event_name == 'workflow_dispatch' && inputs.scheduler_trace", Manual)
        self.assertEqual(Manual.count('& tools/ci/SchedulerTrace.ps1'), 1)
        Build = Workflow[Workflow.index('      - name: Build bounded scheduler diagnostic'):Workflow.index('      - name: Trace one FULL')]
        self.assertNotIn('        if:', Build)
        self.assertIn('test_scheduler_trace.py', Build)
        self.assertIn('SCHEDULER_TRACE_TEST_HELPER=', Build)
        self.assertEqual(Build.count('if not %errorlevel%==0 exit /b %errorlevel%'), 3)
        self.assertNotIn('if errorlevel 1', Build)
        Upload = Workflow[Workflow.index('      - name: Upload native CI diagnostics'):]
        self.assertIn('        if: always()', Upload)
        self.assertIn('            build-ci/scheduler-trace/\n', Upload)
        self.assertIn('            build-ci/scheduler-trace-aggregate32-structural/', Upload)
        self.assertIn('            build-ci/scheduler-trace-aggregate32/\n', Upload)
        self.assertIn('            build-ci/scheduler-trace-pooled-aggregate32-structural/\n', Upload)
        self.assertIn('            build-ci/scheduler-trace-pooled-full/\n', Upload)
        self.assertEqual(Upload.count('            build-ci/scheduler-trace.exe\n'), 1)
        self.assertEqual(Upload.count('            build-ci/gargantuan_game_session_real_transport_tests.exe\n'), 1)

    @unittest.skipUnless(os.name == 'nt', 'Windows cmd exit semantics')
    def test_normal_and_manual_full_route_exact_once_without_capture(self):
        Workflow = (ROOT / '.github/workflows/native-ci.yml').read_text()
        Start = Workflow.index('      - name: Run qualified reliable workload timing in Release')
        Block = Workflow[Start:Workflow.index('      - name:', Start + 20)]
        Body = Block[Block.index('        run: |\n') + len('        run: |\n'):]
        Commands = '\n'.join(Line[10:] for Line in Body.splitlines())
        Replacements = (
            ('pwsh -NoProfile -File tools\\ci\\SchedulerTrace.ps1 -Case PooledAggregate32Structural', 'POOLED32'),
            ('pwsh -NoProfile -File tools\\ci\\SchedulerTrace.ps1 -Case PooledFull', 'POOLED'),
            ('pwsh -NoProfile -File tools\\ci\\SchedulerTrace.ps1 -Case Full', 'FULL'),
            ('pwsh -NoProfile -File tools\\ci\\SchedulerTrace.ps1 -Case Aggregate32\n', 'UNPOOLED32\n'),
            ('pwsh -NoProfile -File tools\\ci\\SchedulerTrace.ps1 -Case Aggregate32Structural', 'AGGREGATE'))
        for Command, Label in Replacements:
            self.assertEqual(Commands.count(Command), 1)
            Commands = Commands.replace(Command, 'call :Record ' + Label)
        for Manual in (False, True):
            for Code, FailedCase in ((Code, Case) for Code in (0, 17, -1073740791) for Case in ('POOLED', 'POOLED32', 'FULL', 'UNPOOLED32')):
                with tempfile.TemporaryDirectory() as Directory:
                    Root = Path(Directory)
                    Prelude = '@echo off\nset "SCHEDULER_TRACE=' + str(Manual).lower() + '"\n'
                    if Manual:
                        Prelude += 'call :Record FULL\nif not %errorlevel%==0 exit /b %errorlevel%\n'
                    (Root / 'route.cmd').write_text(Prelude + Commands + '\nexit /b 0\n:Record\n' +
                        'echo %1>>order.txt\nif "%1"=="' + FailedCase + '" exit /b ' + str(Code) + '\nexit /b 0\n')
                    Result = subprocess.run(['cmd', '/d', '/c', str(Root / 'route.cmd')], cwd=Root,
                        text=True, capture_output=True, timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
                    self.assertEqual(Result.returncode & 0xffffffff, Code & 0xffffffff, Result.stdout + Result.stderr)
                    Expected = ['FULL', 'POOLED', 'POOLED32', 'UNPOOLED32', 'AGGREGATE'] if Manual else [
                        'POOLED', 'POOLED32', 'FULL', 'UNPOOLED32', 'AGGREGATE']
                    if Code:
                        Expected = Expected[:Expected.index(FailedCase) + 1]
                    self.assertEqual((Root / 'order.txt').read_text().splitlines(), Expected)

    @unittest.skipUnless(os.name == 'nt', 'Windows cmd exit semantics')
    def test_cmd_guard_preserves_signed_native_failures_before_later_success(self):
        for Code in (0, 17, -1073740791, -1):
            with tempfile.TemporaryDirectory() as Directory:
                Root = Path(Directory)
                Script = Root / 'guard.cmd'
                Script.write_text('@echo off\npwsh -NoProfile -Command "exit ' + str(Code) + '"\n' +
                    'if not %errorlevel%==0 exit /b %errorlevel%\necho continued>continued.txt\nexit /b 0\n')
                Result = subprocess.run(['cmd', '/d', '/c', str(Script)], cwd=Root, text=True,
                    capture_output=True, timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
                self.assertEqual(Result.returncode & 0xffffffff, Code & 0xffffffff,
                                 Result.stdout + Result.stderr)
                self.assertEqual((Root / 'continued.txt').exists(), Code == 0)
        # Reproduce the original cmd guard defect without running a workload.
        with tempfile.TemporaryDirectory() as Directory:
            Root = Path(Directory)
            Script = Root / 'old-guard.cmd'
            Script.write_text('@echo off\npwsh -NoProfile -Command "exit -1073740791"\n' +
                'if errorlevel 1 exit /b %errorlevel%\necho continued>continued.txt\nexit /b 0\n')
            Result = subprocess.run(['cmd', '/d', '/c', str(Script)], cwd=Root, text=True,
                capture_output=True, timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
            self.assertEqual(Result.returncode, 0)
            self.assertTrue((Root / 'continued.txt').is_file())
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
                          ['-Pair', '-Case', 'Aggregate32Structural'], ['-Pair', '-Case', 'Aggregate32'],
                          ['-Pair', '-Case', 'PooledAggregate32Structural'], ['-Pair', '-Case', 'PooledFull']):
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
        Metadata.update(RequestedChildCreationFlags=134742052, ControllerPriorityClass=16384,
                        ChildPriorityClass=32, ChildThreadPriority=0, PriorityQueryError=0,
                        PriorityVerifiedBeforeResume=True,
                        ChildLaunchAttempts=1, ClockType=1, QpcFrequency=10000000, ControllerQpcFrequency=10000000,
                        ControllerStartQpc=1, AfterTraceStartQpc=2, BeforeChildResumeQpc=3, AfterChildExitQpc=1000,
                        ControllerEndQpc=1001, HeaderStartFileTime=1, HeaderEndFileTime=2,
                        ChildPid=42, ChildMainTid=43, MainFirstQpc=4, MainLastQpc=999)
        Lines = []
        for Boundary, Tick in (("BEGIN", 10), ("END", 900)):
            Lines.append(f"[Qualification:ClockAnchor] case=recovery profile=FULL_RESERVATION boundary={Boundary} pid=42 native_tid=43 native_valid=1 steady_ns={Tick * 100} qpc_before={Tick} qpc_after={Tick + 1} qpc_frequency=10000000")
        return Metadata, "\n".join(Lines)

    def test_loss_free_fixture_window_retained_without_causal_claim(self):
        with tempfile.TemporaryDirectory() as Directory:
            Metadata, Log, File = self.FullFixture(Path(Directory))
            Result = VALIDATOR.Validate(Metadata, Log, 'Full', File)
            self.assertEqual(Result["State"], "LOSS_FREE_ANCHOR_WINDOW_RETAINED", Result)
            self.assertEqual(Result["CausalVerdict"], "NOT_CLAIMED")
            self.assertEqual(Result['CompleteFullCases'], 8)
            self.assertEqual(Result['CompleteRemoteRecords'], 16)
            self.assertEqual(Result['ActionNativeTid'], 'NOT_MEASURED')

    def test_full_complete_ordered_prefix_retains_original_failure_only(self):
        with tempfile.TemporaryDirectory() as Directory:
            Metadata, Log, File = self.FullFixture(Path(Directory), Exit=17, Cases=2)
            Result = VALIDATOR.Validate(Metadata, Log, 'Full', File)
            self.assertEqual(Result['State'], 'LOSS_FREE_ANCHOR_WINDOW_RETAINED', Result)
            self.assertEqual(Result['NativeQualification'], 'CHILD_FAILED')
            self.assertEqual(Result['NativeChildExitCode'], 17)
            self.assertEqual(VALIDATOR.Validate(dict(Metadata, ChildExitCode=0), Log, 'Full', File)['State'], 'INCOMPLETE')
            for Bad in (Log.replace('case=small', 'case=recovery'), Log + '\n' + Log,
                        Log.replace('boundary=END', 'boundary=BEGIN'), Log.replace('case=upper', 'case=mixed')):
                self.assertEqual(VALIDATOR.Validate(Metadata, Bad, 'Full', File)['State'], 'INCOMPLETE')

    def test_full_missing_duplicate_overflow_and_endpoint_denials(self):
        with tempfile.TemporaryDirectory() as Directory:
            Metadata, Log, File = self.FullFixture(Path(Directory))
            for Old, New in (('capacity=1062', 'capacity=208'), ('capacity=29', 'capacity=30'),
                             ('invalid=0', 'invalid=1'), ('overflow=0', 'overflow=1'),
                             ('records=2', 'records=1'), ('rpc_count=1', 'rpc_count=2'),
                             ('action_count=1', 'action_count=0'), ('start_tid=43', 'start_tid=44'),
                             ('submitted_ns=1105', 'submitted_ns=1099'),
                             ('end_sample_after_ns=1111', 'end_sample_after_ns=1109'),
                             ('thread_cpu_lower_100ns=1', 'thread_cpu_lower_100ns=2'),
                             ('observed_ns=10001', 'observed_ns=9999'),
                             ('phase=observer', 'phase=sleep'), ('native_tid=43 start_ns', 'native_tid=44 start_ns')):
                with self.subTest(Old=Old):
                    self.assertIn(Old, Log)
                    self.assertEqual(VALIDATOR.Validate(Metadata, Log.replace(Old, New), 'Full', File)['State'], 'INCOMPLETE')
            for Label in ('RemoteSpan', 'RemoteResourceSpan', 'ActionSpan', 'WorkloadPhaseSpan', 'WorkloadCpu'):
                Lines = Log.splitlines()
                Removed = next(Line for Line in Lines if Line.startswith('[Qualification:' + Label + ']'))
                Lines.remove(Removed)
                self.assertEqual(VALIDATOR.Validate(Metadata, '\n'.join(Lines), 'Full', File)['State'], 'INCOMPLETE')
                self.assertEqual(VALIDATOR.Validate(Metadata, Log + '\n' + Removed, 'Full', File)['State'], 'INCOMPLETE')
            self.assertEqual(VALIDATOR.Validate(Metadata, Log, 'Full', None)['State'], 'INCOMPLETE')
            File.write_text(File.read_text().replace(',36,5,', ',36,99,'))
            self.assertEqual(VALIDATOR.Validate(Metadata, Log, 'Full', File)['State'], 'INCOMPLETE')

    def test_full_inline_callback_and_unmeasured_action_cpu_are_not_retimed(self):
        with tempfile.TemporaryDirectory() as Directory:
            Metadata, Log, File = self.FullFixture(Path(Directory))
            Lines = Log.splitlines()
            Index = next(Index for Index, Line in enumerate(Lines) if Line.startswith('[Qualification:RemoteSpan]'))
            Lines[Index] = Lines[Index].replace('submitted_ns=1105', 'submitted_ns=1115').replace('submitted_step=1', 'submitted_step=3')
            Log = '\n'.join(Lines)
            Log = Log.replace('thread_cpu_ms=0', 'thread_cpu_ms=NOT_MEASURED')
            Result = VALIDATOR.Validate(Metadata, Log, 'Full', File)
            self.assertEqual(Result['State'], 'LOSS_FREE_ANCHOR_WINDOW_RETAINED', Result)
            self.assertIn('thread_cpu_ms', Result['UnknownResourceFields'])

    def test_full_initial_cross_case_and_action_identity_gaps_fail(self):
        with tempfile.TemporaryDirectory() as Directory:
            Metadata, Log, File = self.FullFixture(Path(Directory))
            for Old, New in ((' id=1 ', ' id=99 '), (' id=3 ', ' id=99 '),
                             ('sequence=1 ', 'sequence=99 '), ('sequence=2 ', 'sequence=99 ')):
                self.assertIn(Old, Log)
                Result = VALIDATOR.Validate(Metadata, Log.replace(Old, New), 'Full', File)
                self.assertEqual(Result['State'], 'INCOMPLETE', Result)

    def test_full_final_post_anchor_observation_bound_uses_measured_child_exit(self):
        with tempfile.TemporaryDirectory() as Directory:
            Metadata, Log, File = self.FullFixture(Path(Directory))
            # Last END is steady_ns=87000/qpc_before=870; child exit=1000
            # and QPC frequency=10MHz give conservative upper endpoint100000ns.
            Changed = Log.replace('observed_ns=87001', 'observed_ns=100000')
            Result = VALIDATOR.Validate(Metadata, Changed, 'Full', File)
            self.assertEqual(Result['State'], 'LOSS_FREE_ANCHOR_WINDOW_RETAINED', Result)
            self.assertEqual(VALIDATOR.Validate(Metadata, Changed.replace('observed_ns=100000',
                'observed_ns=100001'), 'Full', File)['State'], 'INCOMPLETE')
            for Bad in (0, -1, None, True):
                Result = VALIDATOR.Validate(dict(Metadata, QpcFrequency=Bad), Log, 'Full', File)
                self.assertEqual(Result['State'], 'INCOMPLETE', Result)
            self.assertEqual(VALIDATOR.Validate(Metadata, Log.replace('steady_ns=87000',
                'steady_ns=87101'), 'Full', File)['State'], 'INCOMPLETE')
            Metadata, Log, File = self.FullFixture(Path(Directory), Exit=17, Cases=1)
            Lines = Log.splitlines()
            Index = next(Index for Index, Line in enumerate(Lines) if Line.startswith('[Qualification:ActionSpan]'))
            Lines[Index] = Lines[Index].replace('outcome=COMPLETED', 'outcome=MISSING_AT_CASE_END').replace(
                'observed_end_ns=1060', 'observed_end_ns=100001')
            Lines = [Line.replace('action_count=1', 'action_count=0').replace('observed_ns=10001', 'observed_ns=100001')
                     for Line in Lines]
            self.assertEqual(VALIDATOR.Validate(Metadata, '\n'.join(Lines), 'Full', File)['State'], 'INCOMPLETE')

    def test_full_failed_missing_or_rejected_original_endpoints_remain_unmeasured(self):
        with tempfile.TemporaryDirectory() as Directory:
            Metadata, Log, File = self.FullFixture(Path(Directory), Exit=17, Cases=1)
            for Accepted in (False, True):
                Lines = Log.splitlines()
                SpanIndex = next(Index for Index, Line in enumerate(Lines) if Line.startswith('[Qualification:RemoteSpan]'))
                ResourceIndex = SpanIndex + 1
                Span = dict(Part.split('=', 1) for Part in Lines[SpanIndex].split()[1:])
                Resource = dict(Part.split('=', 1) for Part in Lines[ResourceIndex].split()[1:])
                Span.update(outcome='MISSING_AT_CASE_END' if Accepted else 'REJECTED', accepted=str(int(Accepted)),
                            terminal='0', end_step='0', end_ns='0', end_tid='0', terminal_status='-1', payload_matched='0')
                Resource.update(terminal='0', endpoint_order_valid='0', sleep_delta_valid='0')
                for Key in list(Resource):
                    if Key.startswith('end_'):
                        Resource[Key] = '0'
                    if '_cpu_lower_' in Key or '_cpu_upper_' in Key or Key.startswith('measured_sleep_'):
                        Resource[Key] = 'NOT_MEASURED'
                for Index, Label, Row in ((SpanIndex, 'RemoteSpan', Span), (ResourceIndex, 'RemoteResourceSpan', Resource)):
                    Lines[Index] = '[Qualification:' + Label + '] ' + ' '.join(Key + '=' + Value for Key, Value in Row.items())
                Lines = [Line.replace('rpc_count=1', 'rpc_count=0').replace('rejected=0', 'rejected=' + str(int(not Accepted)))
                         if Line.startswith('[Qualification:ReliableGameplay]') else Line for Line in Lines]
                Result = VALIDATOR.Validate(Metadata, '\n'.join(Lines), 'Full', File)
                self.assertEqual(Result['State'], 'LOSS_FREE_ANCHOR_WINDOW_RETAINED', Result)
                self.assertEqual(Result['NativeQualification'], 'CHILD_FAILED')
                self.assertEqual(Result['PendingObservedRecords'], int(Accepted))
                Bad = '\n'.join(Lines).replace('end_sample_after_ns=0', 'end_sample_after_ns=1111')
                self.assertEqual(VALIDATOR.Validate(Metadata, Bad, 'Full', File)['State'], 'INCOMPLETE')
            Lines = Log.splitlines()
            Index = next(Index for Index, Line in enumerate(Lines) if Line.startswith('[Qualification:ActionSpan]'))
            Lines[Index] = Lines[Index].replace('outcome=COMPLETED', 'outcome=MISSING_AT_CASE_END').replace(
                'observed_end_ns=1060', 'observed_end_ns=10001').replace('observed_end_step=2', 'observed_end_step=480')
            Lines = [Line.replace('action_count=1', 'action_count=0') if Line.startswith('[Qualification:ReliableGameplay]')
                     else Line for Line in Lines]
            Result = VALIDATOR.Validate(Metadata, '\n'.join(Lines), 'Full', File)
            self.assertEqual(Result['State'], 'LOSS_FREE_ANCHOR_WINDOW_RETAINED', Result)
            self.assertEqual(Result['PendingObservedRecords'], 1)
            self.assertEqual(Result['ObservationBoundary'], 'POST_END_ANCHOR_NO_LATENCY_SUBTRACTION')

    def test_full_validator_cli_requires_actual_raw_csv_and_evidence(self):
        for Valid in (True, False):
            with tempfile.TemporaryDirectory() as Directory:
                Root = Path(Directory)
                Metadata, Log, _ = self.FullFixture(Root)
                if not Valid:
                    Log = Log.replace('capacity=1062', 'capacity=208')
                (Root / 'metadata.json').write_text(json.dumps(Metadata))
                (Root / 'workload.stdout.txt').write_text(Log)
                Result = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/ci/SchedulerTraceValidate.py'),
                    '--root', str(Root), '--case', 'Full'], text=True, capture_output=True, timeout=30)
                self.assertEqual(Result.returncode, 0 if Valid else 125, Result.stdout + Result.stderr)

    def test_loss_clock_coverage_and_cleanup_mutations_fail(self):
        with tempfile.TemporaryDirectory() as Directory:
            Metadata, Log, File = self.FullFixture(Path(Directory))
            self.assertEqual(VALIDATOR.Validate(Metadata, Log, 'Full', File)['State'], 'LOSS_FREE_ANCHOR_WINDOW_RETAINED')
            for Name, Value in (("EventsLost", 1), ("HeaderBuffersLost", 1), ("MainFirstQpc", 11),
                                ("MainLastQpc", 869), ("ChildPid", 9), ("ChildMainTid", 9),
                                ("ControllerQpcFrequency", 1), ("ClockType", 2), ("CsvCapped", True),
                                ("ChildTreeReaped", False), ("TimedOut", True), ("ChildLogCapped", True),
                                ("UnsupportedEvents", 1), ("StopStatus", 1), ("BeforeChildResumeQpc", 11),
                                ("WorkloadCase", "AckStats"), ("WorkloadCase", None)):
                Changed = copy.deepcopy(Metadata)
                Changed[Name] = Value
                with self.subTest(Name=Name):
                    self.assertEqual(VALIDATOR.Validate(Changed, Log, 'Full', File)["State"], "INCOMPLETE")

    def test_missing_malformed_or_duplicate_anchors_fail(self):
        with tempfile.TemporaryDirectory() as Directory:
            Metadata, Log, File = self.FullFixture(Path(Directory))
            self.assertEqual(VALIDATOR.Validate(Metadata, Log, 'Full', File)['State'], 'LOSS_FREE_ANCHOR_WINDOW_RETAINED')
            for Bad in ("", Log.splitlines()[0], Log + "\n" + Log, Log.replace("native_valid=1", "native_valid=0"),
                        Log.replace("qpc_before=10", "qpc_before=INVALID")):
                self.assertEqual(VALIDATOR.Validate(Metadata, Bad, 'Full', File)["State"], "INCOMPLETE")
    def test_wrapper_exit_precedence_without_operational_calls(self):
        Result = subprocess.run(
            ["pwsh", "-NoProfile", "-File", str(ROOT / "tools/ci/SchedulerTrace.ps1"), "-SelfTest"],
            text=True, capture_output=True, timeout=30,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        self.assertEqual(Result.returncode, 0, Result.stdout + Result.stderr)
        self.assertIn("wrapper-self-test=PASS no-session-or-child-created", Result.stdout)
        self.assertIn("dword-exit-normalization=PASS raw-status-preserved", Result.stdout)

    def test_decoder_keeps_byte_time_bounds_and_decode_only_has_no_capture_or_child(self):
        Source = (ROOT / 'tools/ci/SchedulerTrace.cpp').read_text()
        self.assertIn('constexpr std::uint64_t CsvLimit = 512ULL * 1024 * 1024;', Source)
        self.assertIn('constexpr ULONGLONG DecodeLimitMs = 180000;', Source)
        self.assertNotIn('RowLimit', Source)
        self.assertIn('Bytes > CsvLimit || Text.size() > CsvLimit - Bytes', Source)
        Body = Source[Source.index('int DecodeOnly('):Source.index('int SelfTest()')]
        for Call in ('StartTraceW(', 'ControlTraceW(', 'CreateProcessW(', 'Run(', 'Cleanup('):
            self.assertNotIn(Call, Body)
        self.assertIn('Decode(Etl, Decoded)', Body)
        self.assertIn('fs::create_directory(Output)', Body)
        self.assertIn('RequirePlainLocalPath(Destination.parent_path())', Body)
        Validator = (ROOT / 'tools/ci/SchedulerTraceValidate.py').read_text()
        self.assertNotIn('5_000_000', Validator)
        self.assertEqual(Validator.count('Count <= CsvBytes'), 2)

    def test_ordinary_child_priority_metadata_is_required_without_latency_discount(self):
        with tempfile.TemporaryDirectory() as Directory:
            Metadata, Log, File = self.FullFixture(Path(Directory))
            self.assertEqual(VALIDATOR.Validate(Metadata, Log, 'Full', File)['State'], 'LOSS_FREE_ANCHOR_WINDOW_RETAINED')
            for Name, Values in {
                'RequestedChildCreationFlags': (None, 134742020, '134742052', True),
                'ControllerPriorityClass': (None, 0, True),
                'ChildPriorityClass': (None, 0, 16384, 128, True),
                'ChildThreadPriority': (None, -1, 1, 2147483647, False),
                'PriorityQueryError': (None, 5, False),
                'PriorityVerifiedBeforeResume': (None, False, 1),
            }.items():
                for Value in Values:
                    with self.subTest(Name=Name, Value=Value):
                        Changed = dict(Metadata)
                        if Value is None:
                            del Changed[Name]
                        else:
                            Changed[Name] = Value
                        self.assertEqual(VALIDATOR.Validate(Changed, Log, 'Full', File)['State'], 'INCOMPLETE')
            Failed, FailedLog, FailedFile = self.FullFixture(Path(Directory), Exit=1, Cases=2)
            Verdict = VALIDATOR.Validate(Failed, FailedLog, 'Full', FailedFile)
            self.assertEqual(Verdict['State'], 'LOSS_FREE_ANCHOR_WINDOW_RETAINED')
            self.assertEqual(Failed['ChildExitCode'], 1)

    @unittest.skipUnless(os.environ.get('SCHEDULER_TRACE_TEST_HELPER'), 'native helper not supplied')
    def test_native_priority_inheritance_controls_and_ordinary_child_without_capture(self):
        Result = subprocess.run([os.environ['SCHEDULER_TRACE_TEST_HELPER'], '--priority-self-test'],
                                text=True, capture_output=True, timeout=100,
                                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        self.assertEqual(Result.returncode, 0, Result.stdout + Result.stderr)
        for Part in ('priority-self-test=PASS', 'parents=3', 'default-inheritance-controls=3',
                     'explicit-normal-children=3', 'query-denials=PASS', 'own-children-reaped=PASS',
                     'controller-unchanged=PASS', 'no-session-or-workload-created'):
            self.assertIn(Part, Result.stdout)
        Rows = [dict(Part.split('=', 1) for Part in Line.split()[1:]) for Line in Result.stdout.splitlines()
                if Line.startswith('[Qualification:PriorityCase] ')]
        self.assertEqual(len(Rows), 9)
        Controls = [Row for Row in Rows if Row['argument'] == '--priority-probe' and Row['requested_class'] == '0']
        Fixed = [Row for Row in Rows if Row['argument'] == '--priority-probe' and Row['requested_class'] == '32']
        self.assertEqual({(Row['parent_class'], Row['actual_class']) for Row in Controls},
                         {('32', '32'), ('16384', '16384'), ('64', '64')})
        self.assertEqual({(Row['parent_class'], Row['actual_class']) for Row in Fixed},
                         {('32', '32'), ('16384', '32'), ('64', '32')})
        self.assertTrue(all(Row['relative_priority'] == '0' and
                            all(Row[Name] == '1' for Name in ('assigned', 'root_reaped', 'tree_reaped', 'passed'))
                            for Row in Rows))

    @unittest.skipUnless(os.environ.get("SCHEDULER_TRACE_TEST_HELPER"), "native compile-only helper not supplied")
    def test_native_payload_bounds_and_ownership_without_capture(self):
        Result = subprocess.run(
            [os.environ["SCHEDULER_TRACE_TEST_HELPER"], "--self-test"],
            text=True, capture_output=True, timeout=30,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        self.assertEqual(Result.returncode, 0, Result.stdout + Result.stderr)
        self.assertIn("self-test=PASS v5-layout=PASS no-session-or-child-created", Result.stdout)
        self.assertIn('rows-over-five-million=PASS byte-cap=PASS overflow-denial=PASS', Result.stdout)

    @unittest.skipUnless(os.environ.get('SCHEDULER_TRACE_TEST_HELPER'), 'native compile-only helper not supplied')
    def test_native_decode_rejects_invalid_arguments_without_output_or_capture(self):
        with tempfile.TemporaryDirectory() as Directory:
            Root = Path(Directory)
            Source, Output = Root / 'input.etl', Root / 'output'
            Source.write_bytes(b'not-an-etl')
            for Tid in ('0', '-1', '+1', '4294967296', '17x', ''):
                Result = subprocess.run([os.environ['SCHEDULER_TRACE_TEST_HELPER'], '--decode',
                    str(Source), str(Output), Tid], text=True, capture_output=True, timeout=30,
                    creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                self.assertEqual(Result.returncode, 125)
                self.assertIn('positive uint32 main thread ID', Result.stderr)
                self.assertFalse(Output.exists())
            for Args in ([str(Source), str(Output)], [str(Source), str(Output), '17', '--run']):
                Result = subprocess.run([os.environ['SCHEDULER_TRACE_TEST_HELPER'], '--decode', *Args],
                    text=True, capture_output=True, timeout=30, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                self.assertEqual(Result.returncode, 125)
                self.assertIn('invalid arguments', Result.stderr)
                self.assertFalse(Output.exists())

    @unittest.skipUnless(os.environ.get('SCHEDULER_TRACE_TEST_HELPER'), 'native compile-only helper not supplied')
    def test_native_decode_exclusive_and_plain_path_denials_preserve_inputs(self):
        with tempfile.TemporaryDirectory() as Directory:
            Root = Path(Directory)
            Source, Existing = Root / 'input.etl', Root / 'existing'
            Original = b'not-an-etl'
            Source.write_bytes(Original)
            Existing.mkdir()
            Marker = Existing / 'original.txt'
            Marker.write_bytes(b'preserved')
            Linked, Target = Root / 'linked', Root / 'target'
            Target.mkdir()
            (Target / 'input.etl').write_bytes(Original)
            Cases = [(Source, Existing), (Source, Root / 'missing-parent' / 'output'),
                     (Source, Root / 'inside-original'), (Root / 'missing.etl', Root / 'output')]
            # Both source and output parent must reject actual Windows junctions.
            if os.name == 'nt':
                Cases.append((Source, Path(str(Root).upper()) / 'inside-original-case'))
                Result = subprocess.run(['cmd', '/d', '/c', 'mklink', '/J', str(Linked), str(Target)],
                    text=True, capture_output=True, timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
                self.assertEqual(Result.returncode, 0, Result.stdout + Result.stderr)
                Cases.extend(((Linked / 'input.etl', Root / 'output'), (Source, Linked / 'output')))
            try:
                for Input, Output in Cases:
                    Result = subprocess.run([os.environ['SCHEDULER_TRACE_TEST_HELPER'], '--decode',
                        str(Input), str(Output), '17'], text=True, capture_output=True, timeout=30,
                        creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                    self.assertEqual(Result.returncode, 125, Result.stdout + Result.stderr)
                    if Output != Existing:
                        self.assertFalse(Output.exists(), str(Output))
                self.assertEqual(Source.read_bytes(), Original)
                self.assertEqual(Marker.read_bytes(), b'preserved')
                self.assertEqual((Target / 'input.etl').read_bytes(), Original)
            finally:
                if Linked.exists():
                    os.rmdir(Linked) # Remove only this test-owned junction, never its target.

    @unittest.skipUnless(os.environ.get("SCHEDULER_TRACE_TEST_HELPER"), "native compile-only helper not supplied")
    def test_native_rejects_reserved_guid_before_any_trace(self):
        for Command in ('--run', '--run-ack-stats', '--run-aggregate32', '--run-aggregate32-structural', '--run-pooled-aggregate32-structural', '--run-pooled-full'):
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
                          ['--run-aggregate32-structural', str(ROOT), '11111111-2222-4333-8444-555555555555', '--pooled'],
                          ['--run-aggregate32', str(ROOT), '11111111-2222-4333-8444-555555555555', '--pooled'],
                          ['--run-pooled-aggregate32-structural', str(ROOT), '11111111-2222-4333-8444-555555555555', '--extra'],
                          ['--run-pooled-full', str(ROOT), '11111111-2222-4333-8444-555555555555', '--extra']):
            Result = subprocess.run([os.environ['SCHEDULER_TRACE_TEST_HELPER'], *Arguments], text=True,
                capture_output=True, timeout=30, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            self.assertEqual(Result.returncode, 125)
            self.assertIn('invalid arguments', Result.stderr)


if __name__ == "__main__":
    unittest.main()
