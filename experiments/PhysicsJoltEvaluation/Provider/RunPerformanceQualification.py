"""Run bounded fresh-process provider pairs and preserve every raw receipt.

Timing ranking is delegated to CompareBenchmark and requires a reviewed
physical envelope. Cold and warm tails are separate scene populations.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time

from CompareBenchmark import ReadSummary


SCENES = ('cloth4k', 'cloth16k', 'cloth4x4k', 'rubber4k', 'rubber16k')
COUPLINGS = ('none', 'static-sphere')
TAILS = ('cold', 'warm')
JOLT_PROFILE_FIELDS = ('jolt_world_updates', 'jolt_collision_steps_per_update',
                       'jolt_soft_iterations_per_collision_step',
                       'jolt_constraint_sweeps_per_iteration',
                       'jolt_soft_integrations_per_frame',
                       'jolt_full_update_sleep_clock')
XPBD_REFERENCE_FIELDS = ('xpbd_substeps_requested', 'xpbd_substeps_actual',
                         'xpbd_reference', 'xpbd_quality_mode',
                         'xpbd_damping_mode', 'xpbd_substep_damping')


def HashFile(PathName):
    Digest = hashlib.sha256()
    with PathName.open('rb') as Source:
        for Chunk in iter(lambda: Source.read(1024 * 1024), b''):
            Digest.update(Chunk)
    return Digest.hexdigest()


def WriteJson(PathName, Value):
    Temporary = PathName.with_name(PathName.name + '.tmp')
    Temporary.write_text(json.dumps(Value, indent=2, sort_keys=True) + '\n',
                         encoding='utf-8')
    Temporary.replace(PathName)


def PlanMatrix(Scenes, Couplings, Workers, Repetitions, Tails):
    Plan = []
    Group = 0
    for Scene in Scenes:
        for Coupling in Couplings:
            for WorkerCount in Workers:
                for Tail in Tails:
                    for Repeat in range(Repetitions):
                        Order = ('jolt', 'xpbd') if (Group + Repeat) % 2 == 0 \
                            else ('xpbd', 'jolt')
                        Plan.append({'Pair': f'p{len(Plan):04d}',
                                     'Scene': Scene, 'Coupling': Coupling,
                                     'Workers': WorkerCount, 'Tail': Tail,
                                     'Repeat': Repeat, 'Order': Order})
                    Group += 1
    return Plan


def ParseRaw(PathName, Backend, Frames, Warmup, Pair, RequestedXpbdSubsteps=1):
    Lines = PathName.read_text(encoding='utf-8').splitlines()
    Prefix = '[Physics:ProviderBenchmark] '
    Heads = [Line[len(Prefix):] for Line in Lines
             if Line.startswith(Prefix + 'backend=')]
    Summaries = [Line[len(Prefix + 'summary '):] for Line in Lines
                 if Line.startswith(Prefix + 'summary ')]
    Lifecycles = [Line[len(Prefix + 'lifecycle '):] for Line in Lines
                  if Line.startswith(Prefix + 'lifecycle ')]
    Calibrations = [Line[len(Prefix + 'reference_calibration '):] for Line in Lines
                    if Line.startswith(Prefix + 'reference_calibration ')]
    Measured = [Line for Line in Lines if Line.startswith(Prefix + 'frame=')]
    Warm = [Line for Line in Lines if Line.startswith(Prefix + 'warmup_frame=')]
    if (len(Heads), len(Summaries), len(Lifecycles), len(Measured), len(Warm)) != \
            (1, 1, 1, Frames, Warmup):
        raise ValueError('Incomplete provider header, lifecycle, or frame coverage')
    Header = dict(Token.split('=', 1) for Token in Heads[0].split())
    Summary = ReadSummary(PathName)
    Lifecycle = json.loads(Lifecycles[0])
    for Index, Line in enumerate(Measured):
        if not Line.startswith(Prefix + f'frame={Index} '):
            raise ValueError('Measured frame chronology changed')
    for Index, Line in enumerate(Warm):
        if not Line.startswith(Prefix + f'warmup_frame={Index} '):
            raise ValueError('Warmup frame chronology changed')
    for Row in (Header, Summary):
        if Row.get('backend') != Backend or Row.get('run_pair') != Pair or \
                int(Row.get('warmup_frames', -1)) != Warmup:
            raise ValueError('Provider identity or warmup mismatch')
        if int(Row.get('timing_instrumented', -1)) != 0:
            raise ValueError('Allocation-instrumented timing is invalid')
    JoltProfile = {Field: int(Summary[Field]) for Field in JOLT_PROFILE_FIELDS}
    if any(int(Header[Field]) != Value for Field, Value in JoltProfile.items()):
        raise ValueError('Compiled Jolt profile changed between header and summary')
    if Backend == 'jolt':
        if any(Value <= 0 for Value in JoltProfile.values()) or \
                JoltProfile['jolt_soft_integrations_per_frame'] != \
                JoltProfile['jolt_world_updates'] * \
                JoltProfile['jolt_collision_steps_per_update'] * \
                JoltProfile['jolt_soft_iterations_per_collision_step']:
            raise ValueError('Invalid compiled Jolt solver profile')
    elif any(JoltProfile.values()):
        raise ValueError('XPBD row incorrectly reports a Jolt solver profile')
    if any(Header[Field] != Summary[Field] for Field in XPBD_REFERENCE_FIELDS):
        raise ValueError('XPBD reference configuration changed between header and summary')
    if int(Summary['xpbd_substeps_requested']) != RequestedXpbdSubsteps:
        raise ValueError('Requested XPBD reference substeps changed')
    if Backend == 'xpbd':
        Reference = RequestedXpbdSubsteps > 1
        if int(Summary['xpbd_substeps_actual']) != RequestedXpbdSubsteps or \
                int(Summary['xpbd_reference']) != int(Reference) or \
                Summary['xpbd_quality_mode'] != (
                    'high-reference' if Reference else 'automatic-production') or \
                Summary['xpbd_damping_mode'] != (
                    'per-substep-frame-retention' if Reference else 'per-frame-production'):
            raise ValueError('XPBD reference mode misidentified')
        if int(Summary['xpbd_measured_substeps']) != Frames * RequestedXpbdSubsteps:
            raise ValueError('XPBD measured substep coverage incomplete')
        if len(Calibrations) != int(Reference):
            raise ValueError('XPBD reference calibration coverage mismatch')
    elif int(Summary['xpbd_substeps_actual']) != 0 or \
            int(Summary['xpbd_reference']) != 0:
        raise ValueError('Jolt row incorrectly reports XPBD reference work')
    elif Calibrations:
        raise ValueError('Jolt row unexpectedly reports XPBD calibration')
    if int(Summary['frames']) != Frames or \
            Lifecycle.get('MeasuredFrames') != Frames or \
            Lifecycle.get('WarmupFrames') != Warmup or \
            Lifecycle.get('TimingInstrumented') is not False:
        raise ValueError('Lifecycle frame scope or instrumentation mismatch')
    for Field in ('StartupAdmissionMs', 'TeardownMs'):
        if not math.isfinite(float(Lifecycle[Field])) or float(Lifecycle[Field]) < 0:
            raise ValueError(f'Invalid lifecycle {Field}')
    Calibration = json.loads(Calibrations[0]) if Calibrations else None
    if Calibration and (Calibration.get('Pass') is not True or
                        Calibration.get('Substeps') != RequestedXpbdSubsteps or
                        not math.isfinite(float(Calibration['ObservedFrameRetention']))):
        raise ValueError('XPBD reference retention calibration failed')
    return {'Header': Header, 'Summary': Summary, 'Lifecycle': Lifecycle,
            'CompiledJoltProfile': JoltProfile,
            'XpbdReferenceCalibration': Calibration}


def RunProcess(Command, TimeoutSeconds):
    Start = time.monotonic()
    StartedUtc = datetime.now(timezone.utc).isoformat()
    try:
        Process = subprocess.run(Command, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, timeout=TimeoutSeconds,
                                 check=False)
        return {'Stdout': Process.stdout, 'Stderr': Process.stderr,
                'ExitCode': Process.returncode, 'TimedOut': False,
                'StartedUtc': StartedUtc,
                'ElapsedSeconds': time.monotonic() - Start}
    except subprocess.TimeoutExpired as Error:
        return {'Stdout': Error.stdout or b'', 'Stderr': Error.stderr or b'',
                'ExitCode': None, 'TimedOut': True, 'StartedUtc': StartedUtc,
                'ElapsedSeconds': time.monotonic() - Start}
    except OSError as Error:
        return {'Stdout': b'', 'Stderr': str(Error).encode('utf-8'),
                'ExitCode': None, 'TimedOut': False, 'LaunchError': str(Error),
                'StartedUtc': StartedUtc,
                'ElapsedSeconds': time.monotonic() - Start}


def RemainingTimeout(Deadline, PerProcessTimeout):
    return min(float(PerProcessTimeout), Deadline - time.monotonic())


def WriteProgress(Output, Plan, Receipts, Pairs, Errors, Started,
                  TotalTimeout, EnvelopeProvided):
    Complete = not Errors and len(Pairs) == len(Plan)
    WriteJson(Output / 'Summary.json', {
        'PlannedPairs': len(Plan), 'CompletedPairs': len(Pairs),
        'CompletedProcesses': len(Receipts), 'HarnessErrors': Errors,
        'QualityEnvelopeProvided': EnvelopeProvided,
        'TimingWinners': [Pair['Comparison']['timing_winner']
                          for Pair in Pairs if 'Comparison' in Pair],
        'UnrankedWithoutEnvelope': not EnvelopeProvided,
        'TotalTimeoutSeconds': TotalTimeout,
        'ElapsedSeconds': time.monotonic() - Started,
        'QualificationPass': Complete,
        'Status': 'FAILED' if Errors else 'COMPLETE' if Complete else 'RUNNING',
        'Scope': 'Per-pair cost only; no cross-scene or Engine frame winner'})


def Main(Arguments=None):
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument('--binary', type=Path, required=True)
    Parser.add_argument('--cmake-cache', type=Path, required=True)
    Parser.add_argument('--engine-source', required=True)
    Parser.add_argument('--jolt-source', required=True)
    Parser.add_argument('--output', type=Path, required=True)
    Parser.add_argument('--envelope', type=Path)
    Parser.add_argument('--scenes', nargs='+', choices=SCENES, default=SCENES)
    Parser.add_argument('--couplings', nargs='+', choices=COUPLINGS,
                        default=COUPLINGS)
    Parser.add_argument('--workers', nargs='+', type=int, default=(1, 2, 8))
    Parser.add_argument('--tails', nargs='+', choices=TAILS, default=TAILS)
    Parser.add_argument('--frames', type=int, default=30)
    Parser.add_argument('--warmup', type=int, default=10)
    Parser.add_argument('--xpbd-substeps', type=int, choices=(1, 4, 8, 16, 32),
                        default=1)
    Parser.add_argument('--repetitions', type=int, default=3)
    Parser.add_argument('--timeout-seconds', type=int, default=900)
    Parser.add_argument('--total-timeout-seconds', type=int, default=900)
    Options = Parser.parse_args(Arguments)
    if not 1 <= Options.frames <= 30 or not 0 <= Options.warmup <= 30 or \
            ('warm' in Options.tails and Options.warmup == 0) or \
            not 1 <= Options.repetitions <= 4 or \
            not 30 <= Options.timeout_seconds <= 3600 or \
            not 60 <= Options.total_timeout_seconds <= 7200 or \
            not Options.workers or any(Worker < 1 or Worker > 8
                                       for Worker in Options.workers) or \
            len(set(Options.workers)) != len(Options.workers) or \
            len(set(Options.scenes)) != len(Options.scenes) or \
            len(set(Options.couplings)) != len(Options.couplings) or \
            len(set(Options.tails)) != len(Options.tails):
        Parser.error('Frame, warmup, repeat, timeout, or matrix budget invalid')
    Binary = Options.binary.resolve(strict=True)
    Cache = Options.cmake_cache.resolve(strict=True)
    Envelope = Options.envelope.resolve(strict=True) if Options.envelope else None
    Output = Options.output
    Output.mkdir(parents=True, exist_ok=False)
    Started = time.monotonic()
    Deadline = Started + Options.total_timeout_seconds
    shutil.copyfile(Cache, Output / 'CMakeCache.txt')
    if Envelope:
        shutil.copyfile(Envelope, Output / 'QualityEnvelope.json')
    Metadata = {
        'Binary': str(Binary), 'BinarySha256': HashFile(Binary),
        'CMakeCacheSha256': HashFile(Output / 'CMakeCache.txt'),
        'EngineSource': Options.engine_source, 'JoltSource': Options.jolt_source,
        'Host': platform.node(), 'Platform': platform.platform(),
        'Python': sys.version, 'Frames': Options.frames,
        'WarmupFrames': Options.warmup, 'Repetitions': Options.repetitions,
        'RequestedXpbdSubsteps': Options.xpbd_substeps,
        'TotalTimeoutSeconds': Options.total_timeout_seconds,
        'EnvelopeSha256': HashFile(Output / 'QualityEnvelope.json') if Envelope else None,
        'TimingScope': 'Separate cold and post-warmup provider calls; allocation builds rejected',
    }
    WriteJson(Output / 'Metadata.json', Metadata)
    Plan = PlanMatrix(Options.scenes, Options.couplings, Options.workers,
                      Options.repetitions, Options.tails)
    WriteJson(Output / 'Plan.json', Plan)
    Receipts = []
    Pairs = []
    Errors = []
    Stop = False
    WriteProgress(Output, Plan, Receipts, Pairs, Errors, Started,
                  Options.total_timeout_seconds, Envelope is not None)
    for Item in Plan:
        PairRuns = {}
        Warmup = 0 if Item['Tail'] == 'cold' else Options.warmup
        for Backend in Item['Order']:
            Name = f"{len(Receipts):04d}-{Item['Pair']}-{Backend}-{Item['Tail']}"
            Command = [str(Binary), Backend, Item['Scene'], Item['Coupling'],
                       str(Options.frames), str(Item['Workers']), Item['Pair'],
                       str(Warmup), str(Options.xpbd_substeps)]
            Timeout = RemainingTimeout(Deadline, Options.timeout_seconds)
            if Timeout <= 0:
                Errors.append({'Name': Name, 'Reason': 'Total deadline elapsed before native launch'})
                WriteProgress(Output, Plan, Receipts, Pairs, Errors, Started,
                              Options.total_timeout_seconds, Envelope is not None)
                Stop = True
                break
            Result = RunProcess(Command, Timeout)
            StdoutPath = Output / f'{Name}.stdout.txt'
            StderrPath = Output / f'{Name}.stderr.txt'
            StdoutPath.write_bytes(Result.pop('Stdout'))
            StderrPath.write_bytes(Result.pop('Stderr'))
            Receipt = {'Name': Name, 'Pair': Item['Pair'], 'Backend': Backend,
                       'Scene': Item['Scene'], 'Coupling': Item['Coupling'],
                       'Workers': Item['Workers'], 'Tail': Item['Tail'],
                       'Repeat': Item['Repeat'], 'WarmupFrames': Warmup,
                       'RequestedXpbdSubsteps': Options.xpbd_substeps,
                       'Command': Command, 'BinarySha256': Metadata['BinarySha256'],
                       'Stdout': StdoutPath.name, 'StdoutSha256': HashFile(StdoutPath),
                       'Stderr': StderrPath.name, 'StderrSha256': HashFile(StderrPath),
                       **Result}
            Receipts.append(Receipt)
            WriteJson(Output / 'Receipts.json', Receipts)
            if Receipt['ExitCode'] != 0 or Receipt['TimedOut']:
                Errors.append({'Name': Name, 'Reason': 'Process failed or timed out'})
                WriteProgress(Output, Plan, Receipts, Pairs, Errors, Started,
                              Options.total_timeout_seconds, Envelope is not None)
                Stop = True
                break
            try:
                Receipt['Parsed'] = ParseRaw(StdoutPath, Backend, Options.frames,
                                             Warmup, Item['Pair'],
                                             Options.xpbd_substeps)
                if int(Receipt['Parsed']['Summary']['workers_requested']) != Item['Workers']:
                    raise ValueError('Requested worker count changed')
            except Exception as Error:
                Receipt['ParseError'] = str(Error)
                Errors.append({'Name': Name, 'Reason': str(Error)})
                Stop = True
                WriteJson(Output / 'Receipts.json', Receipts)
                WriteProgress(Output, Plan, Receipts, Pairs, Errors, Started,
                              Options.total_timeout_seconds, Envelope is not None)
                break
            WriteJson(Output / 'Receipts.json', Receipts)
            WriteProgress(Output, Plan, Receipts, Pairs, Errors, Started,
                          Options.total_timeout_seconds, Envelope is not None)
            PairRuns[Backend] = Receipt
        if Stop:
            break
        Comparator = Path(__file__).with_name('CompareBenchmark.py')
        ComparisonCommand = [sys.executable, str(Comparator),
                             str(Output / PairRuns['jolt']['Stdout']),
                             str(Output / PairRuns['xpbd']['Stdout'])]
        if Envelope:
            ComparisonCommand += ['--envelope', str(Output / 'QualityEnvelope.json')]
        Timeout = RemainingTimeout(Deadline, Options.timeout_seconds)
        if Timeout <= 0:
            Errors.append({'Name': Item['Pair'],
                           'Reason': 'Total deadline elapsed before comparator launch'})
            WriteProgress(Output, Plan, Receipts, Pairs, Errors, Started,
                          Options.total_timeout_seconds, Envelope is not None)
            break
        Compared = RunProcess(ComparisonCommand, Timeout)
        CompareStdout = Output / f"{Item['Pair']}.compare.stdout.json"
        CompareStderr = Output / f"{Item['Pair']}.compare.stderr.txt"
        CompareStdout.write_bytes(Compared.pop('Stdout'))
        CompareStderr.write_bytes(Compared.pop('Stderr'))
        Pair = {**Item, 'WarmupFrames': Warmup,
                'RequestedXpbdSubsteps': Options.xpbd_substeps,
                'JoltReceipt': PairRuns['jolt']['Name'],
                'XpbdReceipt': PairRuns['xpbd']['Name'],
                'ComparisonCommand': ComparisonCommand,
                'ComparisonStdout': CompareStdout.name,
                'ComparisonStdoutSha256': HashFile(CompareStdout),
                'ComparisonStderr': CompareStderr.name,
                'ComparisonStderrSha256': HashFile(CompareStderr),
                **Compared}
        Pairs.append(Pair)
        WriteJson(Output / 'Pairs.json', Pairs)
        if Pair['ExitCode'] != 0 or Pair['TimedOut']:
            Errors.append({'Name': Item['Pair'], 'Reason': 'Comparator failed or timed out'})
            WriteProgress(Output, Plan, Receipts, Pairs, Errors, Started,
                          Options.total_timeout_seconds, Envelope is not None)
            break
        try:
            Pair['Comparison'] = json.loads(CompareStdout.read_text(encoding='utf-8'))
            Pair['JoltMeanTotalMs'] = float(PairRuns['jolt']['Parsed']['Summary']['mean_total_ms'])
            Pair['XpbdMeanTotalMs'] = float(PairRuns['xpbd']['Parsed']['Summary']['mean_total_ms'])
            Pair['XpbdOverJolt'] = Pair['XpbdMeanTotalMs'] / Pair['JoltMeanTotalMs']
            if not math.isfinite(Pair['XpbdOverJolt']) or Pair['XpbdOverJolt'] <= 0:
                raise ValueError('Invalid paired time ratio')
        except Exception as Error:
            Pair['ParseError'] = str(Error)
            Errors.append({'Name': Item['Pair'], 'Reason': str(Error)})
            WriteJson(Output / 'Pairs.json', Pairs)
            WriteProgress(Output, Plan, Receipts, Pairs, Errors, Started,
                          Options.total_timeout_seconds, Envelope is not None)
            break
        WriteJson(Output / 'Pairs.json', Pairs)
        if time.monotonic() >= Deadline:
            Errors.append({'Name': Item['Pair'],
                           'Reason': 'Total deadline elapsed during pair processing'})
        WriteProgress(Output, Plan, Receipts, Pairs, Errors, Started,
                      Options.total_timeout_seconds, Envelope is not None)
        if Errors:
            break
        print(json.dumps({'Pair': Item['Pair'], 'Scene': Item['Scene'],
                          'Coupling': Item['Coupling'], 'Workers': Item['Workers'],
                          'Tail': Item['Tail'], 'JoltMs': Pair['JoltMeanTotalMs'],
                          'XpbdMs': Pair['XpbdMeanTotalMs'],
                          'TimingWinner': Pair['Comparison']['timing_winner']}),
              flush=True)
    if not Errors and time.monotonic() >= Deadline:
        Errors.append({'Name': 'matrix', 'Reason': 'Total deadline elapsed before completion'})
    WriteProgress(Output, Plan, Receipts, Pairs, Errors, Started,
                  Options.total_timeout_seconds, Envelope is not None)
    if Errors:
        raise SystemExit(2)


if __name__ == '__main__':
    Main()
