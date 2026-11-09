"""Build and exercise the real Engine through the isolated shared Jolt provider."""
import json
import math
import os
import pathlib
import platform
import re
import subprocess
import struct
import sys
import time

from CompareBenchmark import ReadSummary

Engine = pathlib.Path(__file__).resolve().parents[3]
Jolt = Engine / '.provider-jolt'
Evidence = Engine / 'provider-evidence'
Evidence.mkdir(exist_ok=True)
Build = Engine / 'build-provider'
AttachmentMode = os.environ.get('GANTRIA_ATTACHMENT_QUALIFICATION', '1')
if AttachmentMode not in ('0', '1'):
    raise ValueError('GANTRIA_ATTACHMENT_QUALIFICATION must be 0 or 1')
AttachmentQualification = AttachmentMode == '1'
ConvergenceMode = os.environ.get('GANTRIA_PROVIDER_CONVERGENCE', '0')
if ConvergenceMode not in ('0', '1'):
    raise ValueError('GANTRIA_PROVIDER_CONVERGENCE must be 0 or 1')
ConvergenceQualification = ConvergenceMode == '1'
if ConvergenceQualification and not AttachmentQualification:
    raise ValueError('The combined convergence candidate requires attachments enabled')
SleepMode = os.environ.get('GANTRIA_PROVIDER_SLEEP_FIX', '0')
if SleepMode not in ('0', '1'):
    raise ValueError('GANTRIA_PROVIDER_SLEEP_FIX must be 0 or 1')
SleepQualification = SleepMode == '1'
if SleepQualification and not AttachmentQualification:
    raise ValueError('The sleep-clock candidate requires attachments enabled')
if SleepQualification and not ConvergenceQualification:
    raise ValueError('The hosted sleep-clock profile requires convergence enabled')
Pin = ('d8c46588b23700d24ebdaeada0ff460d34b1f830' if SleepQualification else
       'e167ba589787fb76f5d9718ae348d4ac1c3b19ee' if ConvergenceQualification else
       'cd095d408a1cd231ad43fa8725d06a818fd63b13' if AttachmentQualification else
       '109be79fa82be0b306c56e4b4357675ed7843767')
FinalCommit = os.environ.get('GANTRIA_FINAL_JOLT_COMMIT')
GroupBatch = os.environ.get('GANTRIA_PROVIDER_GROUP_BATCH')
if FinalCommit is not None:
    if not re.fullmatch(r'[0-9a-f]{40}', FinalCommit):
        raise ValueError('GANTRIA_FINAL_JOLT_COMMIT must be an exact lowercase SHA-1')
    if not (AttachmentQualification and ConvergenceQualification and SleepQualification):
        raise ValueError('Final Jolt provider requires attachment, convergence and sleep-clock gates')
    Pin = FinalCommit
if GroupBatch is not None:
    if FinalCommit is None or GroupBatch not in ('256', '512', '1024'):
        raise ValueError('Final grouping requires an exact candidate and batch 256, 512 or 1024')
Receipts = []

def Run(Name, Command, Timeout=1800):
    Command = [str(Value) for Value in Command]
    Started = time.monotonic()
    TimedOut = False
    with (Evidence / (Name + '.stdout')).open('wb') as Output, (Evidence / (Name + '.stderr')).open('wb') as Error:
        try:
            Process = subprocess.run(Command, cwd=Engine, stdout=Output, stderr=Error, timeout=Timeout)
        except subprocess.TimeoutExpired:
            # subprocess.run kills and waits for the direct child before raising.
            TimedOut = True
    ExitCode = None if TimedOut else Process.returncode
    Receipts.append({'Name': Name, 'Command': Command, 'ExitCode': ExitCode,
                     'TimedOut': TimedOut, 'DirectProcessTerminated': TimedOut,
                     'TimeoutSeconds': Timeout, 'Seconds': time.monotonic() - Started})
    (Evidence / 'Receipts.json').write_text(json.dumps(Receipts, indent=2), encoding='utf-8')
    print(f'[Physics:Provider] {Name}: ' + ('timed out' if TimedOut else f'exit {ExitCode}'), flush=True)
    if TimedOut or ExitCode:
        print((Evidence / (Name + '.stdout')).read_text(encoding='utf-8', errors='replace')[-6000:])
        print((Evidence / (Name + '.stderr')).read_text(encoding='utf-8', errors='replace')[-6000:])
    if TimedOut:
        raise TimeoutError(f'[Physics:Provider] {Name} exceeded {Timeout} seconds; direct process terminated')
    Process.check_returncode()

def CheckLargeClothTrace(Path, Log, Workers):
    Frames = 30
    Resolution = 128
    Header = struct.Struct('<8sIIIIIf')
    Vertex = struct.Struct('<fff')
    FrameBytes = Resolution * Resolution * Vertex.size
    if Path.stat().st_size != Header.size + Frames * FrameBytes:
        raise ValueError('Large-cloth trace byte count is incomplete')
    Summary = ReadSummary(Log)
    Expected = {'backend': 'jolt', 'scene': 'cloth16k', 'coupling': 'none',
        'frames': str(Frames), 'workers_requested': str(Workers),
        'jolt_world_updates': '2', 'jolt_collision_steps_per_update': '8',
        'jolt_soft_iterations_per_collision_step': '5',
        'jolt_constraint_sweeps_per_iteration': '2',
        'jolt_soft_integrations_per_frame': '80', 'jolt_full_update_sleep_clock': '1',
        'jolt_large_cloth_group_batch': GroupBatch}
    if any(Summary.get(Key) != Value for Key, Value in Expected.items()):
        raise ValueError('Large-cloth summary does not match final source profile')
    RestEdge = 8.0 / (Resolution - 1)
    PeakStrain = 0.0
    PeakPinError = 0.0
    FinalStrain = 0.0
    with Path.open('rb') as Stream:
        Prefix = Header.unpack(Stream.read(Header.size))
        if Prefix[:6] != (b'PJTRACE1', 1, 1, Resolution * Resolution, Frames, 0) or \
                not math.isfinite(Prefix[6]) or abs(Prefix[6] - 1.0 / 60.0) > 1e-7:
            raise ValueError('Large-cloth trace header disagrees with fixture')
        for Frame in range(Frames):
            Positions = list(struct.iter_unpack('<fff', Stream.read(FrameBytes)))
            if len(Positions) != Resolution * Resolution or \
                    any(not all(math.isfinite(Value) for Value in Position)
                        for Position in Positions):
                raise ValueError('Large-cloth frame missing or nonfinite')
            FrameStrain = 0.0
            for Y in range(Resolution):
                for X in range(Resolution):
                    Index = Y * Resolution + X
                    Position = Positions[Index]
                    if Y == 0:
                        Pin = (-4.0 + 8.0 * X / (Resolution - 1), 7.0, 0.0)
                        PeakPinError = max(PeakPinError, math.dist(Position, Pin))
                    for Neighbor in ((Index + 1,) if X + 1 < Resolution else ()) + \
                            ((Index + Resolution,) if Y + 1 < Resolution else ()):
                        Strain = abs(math.dist(Position, Positions[Neighbor]) / RestEdge - 1.0)
                        FrameStrain = max(FrameStrain, Strain)
            PeakStrain = max(PeakStrain, FrameStrain)
            FinalStrain = FrameStrain
    Result = {'Workers': Workers, 'Frames': Frames, 'PeakPinErrorM': PeakPinError,
        'PeakEdgeStrain': PeakStrain, 'FinalEdgeStrain': FinalStrain,
        'MaximumPinErrorM': 1e-4, 'MaximumPeakEdgeStrain': 0.05,
        'MaximumFinalEdgeStrain': 0.02, 'Source': Source}
    (Evidence / f'LargeCloth-W{Workers}-Quality.json').write_text(
        json.dumps(Result, indent=2), encoding='utf-8')
    if PeakPinError > 1e-4 or PeakStrain > 0.05 or FinalStrain > 0.02:
        raise ValueError('Large-cloth grouping exceeded fixed physical envelope')

Actual = subprocess.check_output(['git', '-C', str(Jolt), 'rev-parse', 'HEAD'], text=True).strip()
assert Actual == Pin
Source = {'Engine': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=Engine, text=True).strip(),
          'Jolt': Actual, 'Platform': platform.platform(), 'Sanitizers': platform.system() != 'Windows',
          'Attachments': AttachmentQualification, 'Convergence': ConvergenceQualification,
          'FullUpdateSleepClock': SleepQualification, 'FinalCandidate': FinalCommit is not None,
          'GroupBatch': int(GroupBatch) if GroupBatch is not None else None}
(Evidence / 'Source.json').write_text(json.dumps(Source, indent=2), encoding='utf-8')
Run('ClassGeneration', ['lute', 'tools/classgen'])
Options = ['cmake', '-S', Engine, '-B', Build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
           '-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON', '-DGARGANTUAN_HEADLESS_GEOMETRY_QUALIFICATION=ON',
           '-DGARGANTUAN_TRACY=OFF', '-DGARGANTUAN_WITH_GNS=OFF', '-DBUILD_TESTING=ON',
           '-DGARGANTUAN_BUILD_GLAZE_SERIALIZATION_PROTOTYPE=OFF',
           '-DGARGANTUAN_BUILD_PHYSICS_JOLT_EVALUATION=ON', '-DPHYSICS_EVALUATION_GANTRIA_NUMERICS=ON',
           '-DPHYSICS_EVALUATION_GANTRIA_ATTACHMENTS=' + ('ON' if AttachmentQualification else 'OFF'),
           '-DPHYSICS_EVALUATION_GANTRIA_CONVERGENCE=' + ('ON' if ConvergenceQualification else 'OFF'),
           '-DPHYSICS_EVALUATION_GANTRIA_SLEEP_FIX=' + ('ON' if SleepQualification else 'OFF'),
           '-DPHYSICS_EVALUATION_GANTRIA_GROUPING=' + ('ON' if GroupBatch is not None else 'OFF'),
           '-DPHYSICS_EVALUATION_LARGE_CLOTH_GROUP_BATCH=' + (GroupBatch or '256'),
           '-DPHYSICS_EVALUATION_JOLT_ROOT=' + str(Jolt), '-DPHYSICS_EVALUATION_JOLT_COMMIT=' + Actual]
if platform.system() == 'Windows':
    Options += ['-DCMAKE_C_COMPILER=cl.exe', '-DCMAKE_CXX_COMPILER=cl.exe']
else:
    Flags = '--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/14 -fsanitize=address,undefined -fno-omit-frame-pointer'
    Options += ['-DCMAKE_C_COMPILER=clang-19', '-DCMAKE_CXX_COMPILER=clang++-19',
                '-DCMAKE_C_FLAGS=' + Flags, '-DCMAKE_CXX_FLAGS=' + Flags,
                '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined',
                '-DCPP_RTTI_ENABLED=ON',
                '-DLINKER_SUPPORTS_WL_NO_UNDEFINED=OFF']
Run('Configure', Options)
Targets = ['PhysicsJoltProviderQualification']
if FinalCommit is not None:
    Targets += ['PhysicsJoltAdmissionCacheQualification',
        'PhysicsJoltSchedulingQualification', 'PhysicsJoltProviderBenchmark']
Run('Build', ['cmake', '--build', Build, '--parallel', '2', '--target', *Targets])
BinaryDirectory = Build / 'experiments/PhysicsJoltEvaluation'
Suffix = '.exe' if platform.system() == 'Windows' else ''
Binary = BinaryDirectory / ('PhysicsJoltProviderQualification' + Suffix)
for Repeat in range(1, 4):
    Run('PhysicalFixtures-R' + str(Repeat), [Binary], 180)
    assert b'[Physics:JoltProvider] qualification passed' in (
        Evidence / ('PhysicalFixtures-R' + str(Repeat) + '.stdout')).read_bytes()
if FinalCommit is not None:
    for Target, Marker in [
        ('PhysicsJoltAdmissionCacheQualification', b'[Physics:AdmissionCache] PASS'),
        ('PhysicsJoltSchedulingQualification', b'[Physics:JoltScheduling] qualification passed')]:
        for Repeat in range(1, 4):
            Name = Target + '-R' + str(Repeat)
            Run(Name, [BinaryDirectory / (Target + Suffix)], 180)
            assert Marker in (Evidence / (Name + '.stdout')).read_bytes()
    for Workers in (1, 8):
        Name = f'LargeCloth-W{Workers}'
        Trace = Evidence / (Name + '.trace')
        Run(Name, [BinaryDirectory / ('PhysicsJoltProviderBenchmark' + Suffix),
            'jolt', 'cloth16k', 'none', '30', str(Workers), Name, '0', '1', Trace], 600)
        CheckLargeClothTrace(Trace, Evidence / (Name + '.stdout'), Workers)
(Evidence / 'CMakeCache.txt').write_bytes((Build / 'CMakeCache.txt').read_bytes())
(Evidence / 'Verdict.json').write_text(json.dumps({'RequiredFixturesPass': True,
    'Repeats': 3, 'AdditionalFinalTargets': Targets[1:],
    'LargeClothWorkerCaps': [1, 8] if FinalCommit is not None else [], 'Source': Source,
    'Scope': 'Only the Engine-integrated fixture assertions in this source; not all production backend features'}, indent=2), encoding='utf-8')
