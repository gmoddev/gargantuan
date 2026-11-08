"""Requalify the maintained native API using the existing unchanged physical gates."""
import hashlib
import json
import os
import pathlib
import platform
import subprocess
import sys
import time

Evaluation = pathlib.Path(__file__).resolve().parents[1]
Engine = Evaluation.parents[1]
Jolt = Engine / '.foundation-jolt'
ObservedJolt = Engine / '.foundation-jolt-observed'
Evidence = Engine / 'foundation-evidence'
Evidence.mkdir(exist_ok=True)
Convergence = os.environ.get('GANTRIA_CONVERGENCE_SCREEN') == '1'
ConvergenceQualification = Convergence and os.environ.get('GANTRIA_CONVERGENCE_KIND') == 'qualify'
SleepQualification = os.environ.get('GANTRIA_SLEEP_QUALIFICATION') == '1'
assert not SleepQualification or ConvergenceQualification
ConvergenceIterations = int(os.environ.get('GANTRIA_CONVERGENCE_ITERATIONS', '6'))
assert ConvergenceIterations in (5, 6)
Expected = ('d8c46588b23700d24ebdaeada0ff460d34b1f830' if SleepQualification else
            '25658bcf45f21bb2674d47952638c31c7d0b1467' if Convergence else
            '109be79fa82be0b306c56e4b4357675ed7843767')
Receipts = []

def Run(Name, Command):
    Start = time.monotonic()
    with (Evidence / (Name + '.log')).open('w', encoding='utf-8') as Output:
        Process = subprocess.run([str(Value) for Value in Command], cwd=Engine, stdout=Output,
                                 stderr=subprocess.STDOUT, timeout=1800)
    Receipts.append({'Name': Name, 'Command': [str(Value) for Value in Command],
                     'ExitCode': Process.returncode, 'Seconds': time.monotonic() - Start})
    (Evidence / 'ExecutionReceipts.json').write_text(json.dumps(Receipts, indent=2), encoding='utf-8')
    print(f'[Physics:Foundation] {Name}: exit {Process.returncode}', flush=True)
    if Process.returncode:
        print((Evidence / (Name + '.log')).read_text(encoding='utf-8')[-10000:], flush=True)
    Process.check_returncode()

Actual = subprocess.check_output(['git', '-C', str(Jolt), 'rev-parse', 'HEAD'], text=True).strip()
assert Actual == Expected, (Actual, Expected)
Source = {'Engine': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=Engine, text=True).strip(),
          'Jolt': Actual, 'Upstream': 'e77f175595e64cb44218cc9d9d56fc365ad0e36a',
          'Platform': platform.platform(), 'LibraryProfile': 'SSE2 deterministic, two compiler jobs',
          'RetainedLambdaConvergence': Convergence,
          'FullUpdateSleepClock': SleepQualification,
          'CandidateSoftIterations': ConvergenceIterations if ConvergenceQualification else None}
(Evidence / 'Source.json').write_text(json.dumps(Source, indent=2), encoding='utf-8')
Run('ObserverWorktree', ['git', '-C', Jolt, 'worktree', 'add', '--detach', ObservedJolt, Actual])
Patch = Evidence / 'EnergyObservation.patch'
Run('GenerateObserver', [sys.executable, Evaluation / 'MakeEnergyObservation.py', ObservedJolt,
                         Patch, '--expected-commit', Actual])
Run('ApplyObserver', ['git', '-C', ObservedJolt, 'apply', Patch])

Builds = {}
for Mode, SourcePath in [('Pure', Jolt), ('Observed', ObservedJolt)]:
    Build = Engine / ('build-foundation-' + Mode.lower())
    Builds[Mode] = Build
    Options = ['cmake', '-S', Evaluation / 'Foundation', '-B', Build, '-G', 'Ninja',
               '-DCMAKE_BUILD_TYPE=Release', '-DGANTRIA_JOLT_ROOT=' + str(SourcePath),
               '-DGANTRIA_JOLT_COMMIT=' + Actual, '-DGANTRIA_NUMERICS=ON',
               '-DGANTRIA_OBSERVER=' + ('ON' if Mode == 'Observed' else 'OFF')]
    if Convergence:
        Options += ['-DGANTRIA_CONVERGENCE=ON']
    if SleepQualification:
        Options += ['-DGANTRIA_SLEEP_FIX=ON']
    if platform.system() != 'Windows':
        Options += ['-DCMAKE_CXX_COMPILER=clang++-19',
                    '-DCMAKE_CXX_FLAGS=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/14']
    else:
        Options += ['-DCMAKE_CXX_COMPILER=cl.exe']
    Run('Configure' + Mode, Options)
    Run('Build' + Mode, ['cmake', '--build', Build, '--parallel', '2', '--target',
                         'PhysicsJoltScheduledCouplingDiagnostic', 'PhysicsJoltUnifiedCouplingQualification'])

Suffix = '.exe' if os.name == 'nt' else ''
Native = 'PhysicsJoltScheduledCouplingDiagnostic' + Suffix
Unified = 'PhysicsJoltUnifiedCouplingQualification' + Suffix
if ConvergenceQualification:
    SleepOptions = ['--full-update-sleep-clock'] if SleepQualification else []
    if not SleepQualification:
        Run('ConvergenceRefinement', [sys.executable, Evaluation / 'Optimization/RunCadenceScreen.py',
            '--pure', Builds['Pure'] / Native, '--observed', Builds['Observed'] / Native,
            '--output', Evidence / 'ConvergenceRefinement', '--screen', 'convergence-refine'])
    Run('NativeConvergenceQualification', [sys.executable, Evaluation / 'RunScheduledProfileQualification.py',
        '--pristine', Builds['Pure'] / Native, '--observed', Builds['Observed'] / Native,
        '--output', Evidence / 'Native', '--world-updates', '2', '--steps', '8',
        '--iterations', str(ConvergenceIterations), '--compliance', '1e-6', '--velocity-reconstruction', 'delta',
        '--constraint-sweeps', '2', '--compress-raw', *SleepOptions])
    Run('UnifiedConvergenceQualification', [sys.executable, Evaluation / 'RunUnifiedScheduledQualification.py',
        '--pristine', Builds['Pure'] / Unified, '--observed', Builds['Observed'] / Unified,
        '--output', Evidence / 'Unified', '--world-updates', '2', '--collision-steps', '8',
        '--soft-iterations', str(ConvergenceIterations), '--compliance', '1e-6', '--velocity-reconstruction', 'delta',
        '--constraint-sweeps', '2', '--compress-raw', *SleepOptions])
elif Convergence or os.environ.get('GANTRIA_CADENCE_SCREEN') == '1':
    Run('CadenceScreen', [sys.executable, Evaluation / 'Optimization/RunCadenceScreen.py',
        '--pure', Builds['Pure'] / Native, '--observed', Builds['Observed'] / Native,
        '--output', Evidence / 'CadenceScreen',
        '--screen', 'convergence' if Convergence else
        os.environ.get('GANTRIA_CADENCE_KIND', 'soft-budget')])
else:
    Run('NativeQualification', [sys.executable, Evaluation / 'RunScheduledProfileQualification.py',
        '--pristine', Builds['Pure'] / Native, '--observed', Builds['Observed'] / Native,
        '--output', Evidence / 'Native', '--world-updates', '2', '--steps', '8',
        '--iterations', '12', '--compliance', '1e-6', '--velocity-reconstruction', 'delta', '--compress-raw'])
    Run('UnifiedQualification', [sys.executable, Evaluation / 'RunUnifiedScheduledQualification.py',
        '--pristine', Builds['Pure'] / Unified, '--observed', Builds['Observed'] / Unified,
        '--output', Evidence / 'Unified', '--world-updates', '2', '--collision-steps', '8',
        '--soft-iterations', '12', '--compliance', '1e-6', '--velocity-reconstruction', 'delta', '--compress-raw'])
Identities = []
for Mode, Build in Builds.items():
    for Path in [Build / Native, Build / Unified, Build / 'JoltBuild/Jolt.lib', Build / 'JoltBuild/libJolt.a']:
        if Path.exists():
            with Path.open('rb') as Stream:
                Digest = hashlib.file_digest(Stream, 'sha256').hexdigest()
            Identities.append({'Mode': Mode, 'Path': str(Path.relative_to(Engine)), 'Sha256': Digest})
    (Evidence / (Mode + 'CMakeCache.txt')).write_bytes((Build / 'CMakeCache.txt').read_bytes())
(Evidence / 'BuildIdentities.json').write_text(json.dumps(Identities, indent=2), encoding='utf-8')
