"""Build and exercise the real Engine through the isolated shared Jolt provider."""
import json
import pathlib
import platform
import subprocess
import sys
import time

Engine = pathlib.Path(__file__).resolve().parents[3]
Jolt = Engine / '.provider-jolt'
Evidence = Engine / 'provider-evidence'
Evidence.mkdir(exist_ok=True)
Build = Engine / 'build-provider'
Pin = '109be79fa82be0b306c56e4b4357675ed7843767'
Receipts = []

def Run(Name, Command, Timeout=1800):
    Command = [str(Value) for Value in Command]
    Started = time.monotonic()
    with (Evidence / (Name + '.stdout')).open('wb') as Output, (Evidence / (Name + '.stderr')).open('wb') as Error:
        Process = subprocess.run(Command, cwd=Engine, stdout=Output, stderr=Error, timeout=Timeout)
    Receipts.append({'Name': Name, 'Command': Command, 'ExitCode': Process.returncode,
                     'Seconds': time.monotonic() - Started})
    (Evidence / 'Receipts.json').write_text(json.dumps(Receipts, indent=2), encoding='utf-8')
    print(f'[Physics:Provider] {Name}: exit {Process.returncode}', flush=True)
    if Process.returncode:
        print((Evidence / (Name + '.stdout')).read_text(encoding='utf-8', errors='replace')[-6000:])
        print((Evidence / (Name + '.stderr')).read_text(encoding='utf-8', errors='replace')[-6000:])
    Process.check_returncode()

Actual = subprocess.check_output(['git', '-C', str(Jolt), 'rev-parse', 'HEAD'], text=True).strip()
assert Actual == Pin
Source = {'Engine': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=Engine, text=True).strip(),
          'Jolt': Actual, 'Platform': platform.platform(), 'Sanitizers': platform.system() != 'Windows'}
(Evidence / 'Source.json').write_text(json.dumps(Source, indent=2), encoding='utf-8')
Run('ClassGeneration', ['lute', 'tools/classgen'])
Options = ['cmake', '-S', Engine, '-B', Build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
           '-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON', '-DGARGANTUAN_HEADLESS_GEOMETRY_QUALIFICATION=ON',
           '-DGARGANTUAN_TRACY=OFF', '-DGARGANTUAN_WITH_GNS=OFF', '-DBUILD_TESTING=ON',
           '-DGARGANTUAN_BUILD_GLAZE_SERIALIZATION_PROTOTYPE=OFF',
           '-DGARGANTUAN_BUILD_PHYSICS_JOLT_EVALUATION=ON', '-DPHYSICS_EVALUATION_GANTRIA_NUMERICS=ON',
           '-DPHYSICS_EVALUATION_JOLT_ROOT=' + str(Jolt), '-DPHYSICS_EVALUATION_JOLT_COMMIT=' + Actual]
if platform.system() == 'Windows':
    Options += ['-DCMAKE_C_COMPILER=cl.exe', '-DCMAKE_CXX_COMPILER=cl.exe']
else:
    Flags = '--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/14 -fsanitize=address,undefined -fno-omit-frame-pointer'
    Options += ['-DCMAKE_C_COMPILER=clang-19', '-DCMAKE_CXX_COMPILER=clang++-19',
                '-DCMAKE_C_FLAGS=' + Flags, '-DCMAKE_CXX_FLAGS=' + Flags,
                '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined']
Run('Configure', Options)
Run('Build', ['cmake', '--build', Build, '--parallel', '2', '--target', 'PhysicsJoltProviderQualification'])
Binary = Build / 'experiments/PhysicsJoltEvaluation' / (
    'PhysicsJoltProviderQualification' + ('.exe' if platform.system() == 'Windows' else ''))
for Repeat in range(1, 4):
    Run('PhysicalFixtures-R' + str(Repeat), [Binary], 180)
    assert b'[Physics:JoltProvider] qualification passed' in (
        Evidence / ('PhysicalFixtures-R' + str(Repeat) + '.stdout')).read_bytes()
(Evidence / 'CMakeCache.txt').write_bytes((Build / 'CMakeCache.txt').read_bytes())
(Evidence / 'Verdict.json').write_text(json.dumps({'RequiredFixturesPass': True,
    'Repeats': 3, 'Source': Source, 'Scope': 'Only the Engine-integrated fixture assertions in this source; not all production backend features'}, indent=2), encoding='utf-8')
