"""Build stock, maintained and independently profiled scheduling experiments."""
import json
import pathlib
import platform
import subprocess
import sys
import time

Evaluation = pathlib.Path(__file__).resolve().parents[1]
Engine = Evaluation.parents[1]
Evidence = Engine / 'optimization-evidence'
Evidence.mkdir(exist_ok=True)
Receipts = []

def Run(Name, Command):
    Start = time.monotonic()
    Command = [str(Value) for Value in Command]
    with (Evidence / (Name + '.log')).open('w', encoding='utf-8') as Output:
        Result = subprocess.run(Command, cwd=Engine, stdout=Output, stderr=subprocess.STDOUT, timeout=1200)
    Receipts.append({'Name': Name, 'Command': Command, 'ExitCode': Result.returncode,
                     'Seconds': time.monotonic() - Start})
    (Evidence / 'ExecutionReceipts.json').write_text(json.dumps(Receipts, indent=2), encoding='utf-8')
    print(f'[Physics:Optimization] {Name}: exit {Result.returncode}', flush=True)
    if Result.returncode:
        print((Evidence / (Name + '.log')).read_text(encoding='utf-8')[-12000:], flush=True)
    Result.check_returncode()

Sources = {'Stock': ('e77f175595e64cb44218cc9d9d56fc365ad0e36a', Engine / '.stock-jolt'),
           'Patched': ('109be79fa82be0b306c56e4b4357675ed7843767', Engine / '.foundation-jolt')}
(Evidence / 'Source.json').write_text(json.dumps({
    'Engine': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=Engine, text=True).strip(),
    'Jolt': {Name: Pin for Name, (Pin, _) in Sources.items()}, 'Platform': platform.platform()}, indent=2), encoding='utf-8')
Binaries = {}
for Mode in ['Stock', 'Patched', 'Profile']:
    Pin, Source = Sources['Patched' if Mode == 'Profile' else Mode]
    assert subprocess.check_output(['git', '-C', str(Source), 'rev-parse', 'HEAD'], text=True).strip() == Pin
    Build = Engine / ('build-optimization-' + Mode.lower())
    Command = ['cmake', '-S', Evaluation / 'Foundation', '-B', Build, '-G', 'Ninja',
               '-DCMAKE_BUILD_TYPE=Release', '-DGANTRIA_JOLT_ROOT=' + str(Source),
               '-DGANTRIA_JOLT_COMMIT=' + Pin,
               '-DGANTRIA_NUMERICS=' + ('OFF' if Mode == 'Stock' else 'ON'),
               '-DGANTRIA_PROFILE=' + ('ON' if Mode == 'Profile' else 'OFF')]
    if platform.system() == 'Windows':
        Command += ['-DCMAKE_CXX_COMPILER=cl.exe']
    else:
        Command += ['-DCMAKE_CXX_COMPILER=clang++-19',
                    '-DCMAKE_CXX_FLAGS=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/14']
    Run('Configure' + Mode, Command)
    Run('Build' + Mode, ['cmake', '--build', Build, '--parallel', '2', '--target', 'PhysicsJoltSelectiveCouplingBenchmark'])
    Binaries[Mode] = Build / ('PhysicsJoltSelectiveCouplingBenchmark' + ('.exe' if platform.system() == 'Windows' else ''))
    (Evidence / (Mode + 'CMakeCache.txt')).write_bytes((Build / 'CMakeCache.txt').read_bytes())
for Mode in ['global', 'selective']:
    Run('Profile-' + Mode, [Binaries['Profile'], '--mode', Mode, '--frames', '120',
                           '--workers', '1', '--isolated-bodies', '128', '--velocity-reconstruction', 'delta'])
Run('Qualification', [sys.executable, Evaluation / 'Optimization/RunSelectiveQualification.py',
                      '--stock', Binaries['Stock'], '--patched', Binaries['Patched'],
                      '--frames', '120', '--isolated-bodies', '128', '--repeats', '4',
                      '--output', Evidence / 'Qualification.json'])
