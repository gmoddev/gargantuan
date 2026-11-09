"""Run cold Engine physics-call pairs without implying material equivalence.

Each attempt is a fresh process. The six-argument EngineBenchmark has no
run-pair or warmup CLI, so pairing and source identity live in this runner.
"""

import argparse
import json
import math
from pathlib import Path
import platform
import shutil
import sys
import time

from RunPerformanceQualification import (
    COUPLINGS, JOLT_PROFILE_FIELDS, SCENES, HashFile, PlanMatrix, RemainingTimeout, RunProcess,
    WriteJson, ValidateTimingCache,
)


QUALITY_FIELDS = ('mean_y', 'center_y', 'max_pin_error', 'peak_pin_error',
                  'mean_edge_strain', 'max_edge_strain', 'peak_edge_strain',
                  'mean_volume_ratio')
TIME_FIELDS = ('setup_ms', 'semantic_destroy_ms', 'owner_release_ms',
               'mean_physics_step_wall_ms', 'median_physics_step_wall_ms',
               'p95_physics_step_wall_ms')


def ParseRaw(PathName, Backend, Scene, Coupling, Frames, Workers):
    Lines = PathName.read_text(encoding='utf-8').splitlines()
    Prefix = '[Physics:EngineBenchmark] '
    Headers = [Line[len(Prefix):] for Line in Lines
               if Line.startswith(Prefix + 'backend=')]
    Summaries = [Line[len(Prefix + 'summary '):] for Line in Lines
                 if Line.startswith(Prefix + 'summary ')]
    FrameLines = [Line for Line in Lines if Line.startswith(Prefix + 'frame=')]
    if (len(Headers), len(Summaries), len(FrameLines)) != (1, 1, Frames):
        raise ValueError('Incomplete Engine header, summary, or frame coverage')
    Header = dict(Token.split('=', 1) for Token in Headers[0].split())
    Summary = dict(Token.split('=', 1) for Token in Summaries[0].split())
    for Row in (Header, Summary):
        if (Row.get('backend'), Row.get('scene'), Row.get('coupling')) != \
                (Backend, Scene, Coupling) or \
                int(Row.get('frames', -1)) != Frames or \
                int(Row.get('workers_requested', -1)) != Workers:
            raise ValueError('Engine scene, backend, or worker identity changed')
    JoltProfile = {Field: int(Summary[Field]) for Field in JOLT_PROFILE_FIELDS}
    if any(int(Header[Field]) != Value for Field, Value in JoltProfile.items()):
        raise ValueError('Compiled Jolt profile changed between Engine header and summary')
    if Backend == 'jolt':
        if any(Value <= 0 for Field, Value in JoltProfile.items()
               if Field != 'jolt_full_update_sleep_clock') or \
                JoltProfile['jolt_full_update_sleep_clock'] not in (0, 1) or \
                JoltProfile['jolt_soft_integrations_per_frame'] != \
                JoltProfile['jolt_world_updates'] * \
                JoltProfile['jolt_collision_steps_per_update'] * \
                JoltProfile['jolt_soft_iterations_per_collision_step']:
            raise ValueError('Invalid compiled Engine Jolt solver profile')
    elif any(JoltProfile.values()):
        raise ValueError('Engine XPBD row reports a Jolt solver profile')
    for Index, Line in enumerate(FrameLines):
        if not Line.startswith(Prefix + f'frame={Index} '):
            raise ValueError('Engine frame chronology changed')
        Fields = dict(Token.split('=', 1) for Token in Line[len(Prefix):].split())
        if not math.isfinite(float(Fields['physics_step_wall_ms'])) or \
                float(Fields['physics_step_wall_ms']) < 0:
            raise ValueError('Invalid Engine frame time')
    for Field in QUALITY_FIELDS + TIME_FIELDS:
        if not math.isfinite(float(Summary[Field])):
            raise ValueError(f'Nonfinite Engine {Field}')
    if any(float(Summary[Field]) < 0 for Field in TIME_FIELDS):
        raise ValueError('Negative Engine timing field')
    if int(Summary['vertices']) <= 0 or int(Summary['constraints']) <= 0:
        raise ValueError('Engine scene lost vertices or constraints')
    if Coupling == 'static-sphere' and \
            float(Summary['initial_min_surface_distance']) <= 0:
        raise ValueError('Engine coupled scene began in penetration')
    return {'Header': Header, 'Summary': Summary, 'JoltSolverProfile': JoltProfile,
            'PhysicalGateScope': 'Per-frame finite, revision, frozen, and drop checks in native executable'}


def WriteProgress(Output, Plan, Receipts, Pairs, Errors, Started, TotalTimeout):
    Complete = not Errors and len(Pairs) == len(Plan)
    WriteJson(Output / 'Summary.json', {
        'PlannedPairs': len(Plan), 'CompletedPairs': len(Pairs),
        'CompletedProcesses': len(Receipts), 'HarnessErrors': Errors,
        'TotalTimeoutSeconds': TotalTimeout,
        'ElapsedSeconds': time.monotonic() - Started,
        'QualificationPass': Complete,
        'Status': 'FAILED' if Errors else 'COMPLETE' if Complete else 'RUNNING',
        'TimingWinner': None,
        'Scope': 'Cold Engine WorldRoot physics-call pairs; quality differences retained; no winner'})


def Main(Arguments=None):
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument('--binary', type=Path, required=True)
    Parser.add_argument('--cmake-cache', type=Path, required=True)
    Parser.add_argument('--engine-source', required=True)
    Parser.add_argument('--jolt-source', required=True)
    Parser.add_argument('--output', type=Path, required=True)
    Parser.add_argument('--scenes', nargs='+', choices=SCENES, default=SCENES)
    Parser.add_argument('--couplings', nargs='+', choices=COUPLINGS,
                        default=COUPLINGS)
    Parser.add_argument('--workers', nargs='+', type=int, default=(1, 2, 8))
    Parser.add_argument('--frames', type=int, default=30)
    Parser.add_argument('--repetitions', type=int, default=3)
    Parser.add_argument('--timeout-seconds', type=int, default=900)
    Parser.add_argument('--total-timeout-seconds', type=int, default=900)
    Options = Parser.parse_args(Arguments)
    if not 1 <= Options.frames <= 30 or \
            not 1 <= Options.repetitions <= 4 or \
            not 30 <= Options.timeout_seconds <= 3600 or \
            not 60 <= Options.total_timeout_seconds <= 7200 or \
            any(Worker < 1 or Worker > 8 for Worker in Options.workers) or \
            len(set(Options.workers)) != len(Options.workers) or \
            len(set(Options.scenes)) != len(Options.scenes) or \
            len(set(Options.couplings)) != len(Options.couplings):
        Parser.error('Invalid frame, worker, repeat, or timeout budget')
    Binary = Options.binary.resolve(strict=True)
    Cache = Options.cmake_cache.resolve(strict=True)
    ValidateTimingCache(Cache)
    Output = Options.output
    Output.mkdir(parents=True, exist_ok=False)
    Started = time.monotonic()
    Deadline = Started + Options.total_timeout_seconds
    shutil.copyfile(Cache, Output / 'CMakeCache.txt')
    Metadata = {'Binary': str(Binary), 'BinarySha256': HashFile(Binary),
                'CMakeCacheSha256': HashFile(Output / 'CMakeCache.txt'),
                'EngineSource': Options.engine_source,
                'JoltSource': Options.jolt_source,
                'Host': platform.node(), 'Platform': platform.platform(),
                'Python': sys.version, 'Frames': Options.frames,
                'TotalTimeoutSeconds': Options.total_timeout_seconds,
                'TimingScope': 'Cold WorldRoot::StepPhysics; Engine setup and teardown separate',
                'QualityScope': 'Physical fields reported; no cross-material winner'}
    WriteJson(Output / 'Metadata.json', Metadata)
    Plan = PlanMatrix(Options.scenes, Options.couplings, Options.workers,
                      Options.repetitions, ('cold',))
    WriteJson(Output / 'Plan.json', Plan)
    Receipts = []
    Pairs = []
    Errors = []
    Stop = False
    WriteProgress(Output, Plan, Receipts, Pairs, Errors, Started,
                  Options.total_timeout_seconds)
    for Item in Plan:
        PairRuns = {}
        for Backend in Item['Order']:
            Name = f"{len(Receipts):04d}-{Item['Pair']}-{Backend}"
            Command = [str(Binary), Backend, Item['Scene'], Item['Coupling'],
                       str(Options.frames), str(Item['Workers'])]
            Timeout = RemainingTimeout(Deadline, Options.timeout_seconds)
            if Timeout <= 0:
                Errors.append({'Name': Name, 'Reason': 'Total deadline elapsed before native launch'})
                WriteProgress(Output, Plan, Receipts, Pairs, Errors, Started,
                              Options.total_timeout_seconds)
                Stop = True
                break
            Result = RunProcess(Command, Timeout)
            StdoutPath = Output / f'{Name}.stdout.txt'
            StderrPath = Output / f'{Name}.stderr.txt'
            StdoutPath.write_bytes(Result.pop('Stdout'))
            StderrPath.write_bytes(Result.pop('Stderr'))
            Receipt = {'Name': Name, 'Pair': Item['Pair'], 'Backend': Backend,
                       'Scene': Item['Scene'], 'Coupling': Item['Coupling'],
                       'Workers': Item['Workers'], 'Repeat': Item['Repeat'],
                       'Command': Command, 'BinarySha256': Metadata['BinarySha256'],
                       'Stdout': StdoutPath.name, 'StdoutSha256': HashFile(StdoutPath),
                       'Stderr': StderrPath.name, 'StderrSha256': HashFile(StderrPath),
                       **Result}
            Receipts.append(Receipt)
            WriteJson(Output / 'Receipts.json', Receipts)
            if Receipt['ExitCode'] != 0 or Receipt['TimedOut']:
                Errors.append({'Name': Name, 'Reason': 'Process failed or timed out'})
                WriteProgress(Output, Plan, Receipts, Pairs, Errors, Started,
                              Options.total_timeout_seconds)
                Stop = True
                break
            try:
                Receipt['Parsed'] = ParseRaw(StdoutPath, Backend, Item['Scene'],
                                             Item['Coupling'], Options.frames,
                                             Item['Workers'])
            except Exception as Error:
                Receipt['ParseError'] = str(Error)
                Errors.append({'Name': Name, 'Reason': str(Error)})
                Stop = True
                WriteJson(Output / 'Receipts.json', Receipts)
                WriteProgress(Output, Plan, Receipts, Pairs, Errors, Started,
                              Options.total_timeout_seconds)
                break
            WriteJson(Output / 'Receipts.json', Receipts)
            WriteProgress(Output, Plan, Receipts, Pairs, Errors, Started,
                          Options.total_timeout_seconds)
            PairRuns[Backend] = Receipt
        if Stop:
            break
        Jolt = PairRuns['jolt']['Parsed']['Summary']
        Xpbd = PairRuns['xpbd']['Parsed']['Summary']
        JoltMs = float(Jolt['mean_physics_step_wall_ms'])
        XpbdMs = float(Xpbd['mean_physics_step_wall_ms'])
        if JoltMs <= 0 or XpbdMs <= 0:
            Errors.append({'Name': Item['Pair'], 'Reason': 'Nonpositive Engine mean step time'})
            WriteProgress(Output, Plan, Receipts, Pairs, Errors, Started,
                          Options.total_timeout_seconds)
            break
        Pair = {**Item, 'JoltReceipt': PairRuns['jolt']['Name'],
                'XpbdReceipt': PairRuns['xpbd']['Name'],
                'JoltSolverProfile': PairRuns['jolt']['Parsed']['JoltSolverProfile'],
                'JoltMeanStepMs': JoltMs, 'XpbdMeanStepMs': XpbdMs,
                'XpbdOverJoltStepCost': XpbdMs / JoltMs,
                'QualityAbsoluteErrors': {Field: abs(float(Jolt[Field]) - float(Xpbd[Field]))
                                          for Field in QUALITY_FIELDS},
                'ConstraintCountMatched': Jolt['constraints'] == Xpbd['constraints'],
                'JoltObservedContactBand': int(Jolt['observed_contact_band']),
                'XpbdObservedContactBand': int(Xpbd['observed_contact_band']),
                'QualityComparable': None, 'TimingWinner': None,
                'Scope': 'Engine physics-call cost; no matched material/contact envelope'}
        Pairs.append(Pair)
        WriteJson(Output / 'Pairs.json', Pairs)
        if time.monotonic() >= Deadline:
            Errors.append({'Name': Item['Pair'],
                           'Reason': 'Total deadline elapsed during pair processing'})
        WriteProgress(Output, Plan, Receipts, Pairs, Errors, Started,
                      Options.total_timeout_seconds)
        if Errors:
            break
        print(json.dumps({'Pair': Item['Pair'], 'Scene': Item['Scene'],
                          'Coupling': Item['Coupling'], 'Workers': Item['Workers'],
                          'JoltMeanStepMs': JoltMs, 'XpbdMeanStepMs': XpbdMs,
                          'TimingWinner': None}), flush=True)
    if not Errors and time.monotonic() >= Deadline:
        Errors.append({'Name': 'matrix', 'Reason': 'Total deadline elapsed before completion'})
    WriteProgress(Output, Plan, Receipts, Pairs, Errors, Started,
                  Options.total_timeout_seconds)
    if Errors:
        raise SystemExit(2)


if __name__ == '__main__':
    Main()
