"""Bounded A/B/C/D order screen. Failed physical results remain evidence."""
import hashlib
import json
import os
import pathlib
import platform
import subprocess
import sys
import time

Engine = pathlib.Path(__file__).resolve().parents[3]
Evidence = pathlib.Path(os.environ.get('GANTRIA_CONTACT_EVIDENCE', Engine / 'contact-evidence')).resolve()
Build = pathlib.Path(os.environ.get('GANTRIA_CONTACT_BUILD', Engine / 'build-contact')).resolve()
Jolt = pathlib.Path(os.environ.get('GANTRIA_CONTACT_JOLT', Engine / '.provider-jolt')).resolve()
Evidence.mkdir(parents=True, exist_ok=True)
Receipts = []

def Run(Name, Command, Environment=None, Timeout=1800):
    Command = [str(Value) for Value in Command]
    Started = time.monotonic()
    Code = 124
    with (Evidence / (Name + '.stdout')).open('wb') as Output, (Evidence / (Name + '.stderr')).open('wb') as Error:
        try:
            Code = subprocess.run(Command, cwd=Engine, env=Environment, stdout=Output,
                                  stderr=Error, timeout=Timeout).returncode
        except subprocess.TimeoutExpired:
            pass
    Record = {'Name': Name, 'Command': Command, 'ExitCode': Code, 'Seconds': time.monotonic() - Started,
              'StdoutSha256': hashlib.sha256((Evidence / (Name + '.stdout')).read_bytes()).hexdigest(),
              'StderrSha256': hashlib.sha256((Evidence / (Name + '.stderr')).read_bytes()).hexdigest()}
    Receipts.append(Record)
    (Evidence / 'Receipts.json').write_text(json.dumps(Receipts, indent=2), encoding='utf-8')
    print('[Physics:ContactScreen]', Name, 'exit', Code, flush=True)
    if Code:
        print((Evidence / (Name + '.stdout')).read_text(errors='replace')[-5000:])
        print((Evidence / (Name + '.stderr')).read_text(errors='replace')[-5000:])
        raise RuntimeError(Name + ' process failed')

Native = subprocess.check_output(['git', '-C', str(Jolt), 'rev-parse', 'HEAD'], text=True).strip()
Source = {'Engine': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=Engine, text=True).strip(),
          'Native': Native, 'Platform': platform.platform(), 'Sanitizers': platform.system() != 'Windows',
          'Scope': 'Causal screen only; a strain pass is not physical qualification',
          'GroupBatch': 1024, 'SeedPolicy': 'No jitter or random perturbation'}
(Evidence / 'Source.json').write_text(json.dumps(Source, indent=2), encoding='utf-8')
if os.environ.get('GANTRIA_CONTACT_SKIP_BUILD') != '1':
    Run('ClassGeneration', ['lute', 'tools/classgen'])
    Options = ['cmake', '-S', Engine, '-B', Build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
               '-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON', '-DGARGANTUAN_HEADLESS_GEOMETRY_QUALIFICATION=ON',
               '-DGARGANTUAN_TRACY=OFF', '-DGARGANTUAN_WITH_GNS=OFF', '-DBUILD_TESTING=ON',
               '-DGARGANTUAN_BUILD_GLAZE_SERIALIZATION_PROTOTYPE=OFF',
               '-DGARGANTUAN_BUILD_PHYSICS_JOLT_EVALUATION=ON', '-DPHYSICS_EVALUATION_GANTRIA_NUMERICS=ON',
               '-DPHYSICS_EVALUATION_GANTRIA_ATTACHMENTS=ON', '-DPHYSICS_EVALUATION_GANTRIA_CONVERGENCE=ON',
               '-DPHYSICS_EVALUATION_GANTRIA_SLEEP_FIX=ON', '-DPHYSICS_EVALUATION_GANTRIA_GROUPING=ON',
               '-DPHYSICS_EVALUATION_LARGE_CLOTH_GROUP_BATCH=1024', '-DPHYSICS_EVALUATION_NATIVE_PROFILE=OFF',
               '-DPHYSICS_EVALUATION_JOLT_ROOT=' + str(Jolt), '-DPHYSICS_EVALUATION_JOLT_COMMIT=' + Native]
    if platform.system() == 'Windows':
        Options += ['-DCMAKE_C_COMPILER=cl.exe', '-DCMAKE_CXX_COMPILER=cl.exe']
    else:
        Flags = '--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/14 -fsanitize=address,undefined -fno-omit-frame-pointer'
        Options += ['-DCMAKE_C_COMPILER=clang-19', '-DCMAKE_CXX_COMPILER=clang++-19',
                    '-DCMAKE_C_FLAGS=' + Flags, '-DCMAKE_CXX_FLAGS=' + Flags,
                    '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined',
                    '-DCPP_RTTI_ENABLED=ON', '-DLINKER_SUPPORTS_WL_NO_UNDEFINED=OFF']
    Run('Configure', Options)
    Run('Build', ['cmake', '--build', Build, '--parallel', '2', '--target',
        'PhysicsJoltProviderLongRun', 'PhysicsJoltXpbdContactProbe', 'PhysicsJoltProviderQualification'])
Suffix = '.exe' if platform.system() == 'Windows' else ''
BinaryDirectory = Build / 'experiments/PhysicsJoltEvaluation'
Run('ProviderRegression', [BinaryDirectory / ('PhysicsJoltProviderQualification' + Suffix)], Timeout=180)
Cases = [('jolt', 'A', 1, 0), ('jolt', 'B', 1, 0), ('jolt', 'C', 1, 0),
         ('jolt', 'C', 4, 0), ('jolt', 'D', 4, 0), ('jolt', 'E', 1, 0),
         ('xpbd', 'A', 1, 0), ('xpbd', 'B', 1, 0), ('xpbd', 'C', 1, 0)]
for Backend, Mode, Passes, Offset in Cases:
    Name = f'{Backend}-{Mode}{Passes}-Plane'
    Environment = {Key: Value for Key, Value in os.environ.items()
                   if not Key.startswith(('GANTRIA_PROBE_', 'GANTRIA_CONTACT_ORDER', 'GANTRIA_CONTACT_PASSES',
                                          'GANTRIA_XPBD_CONTACT_ORDER', 'GANTRIA_XPBD_VELOCITY_'))}
    Environment.update({'GANTRIA_CONTACT_ORDER': Mode, 'GANTRIA_CONTACT_PASSES': str(Passes),
                        'GANTRIA_XPBD_CONTACT_ORDER': Mode,
                        'GANTRIA_CONTACT_VELOCITY_TRACE': str(Evidence / (Name + '.velocity.f32le')),
                        'GANTRIA_CONTACT_VELOCITY_STRIDE': '32',
                        'GANTRIA_CONTACT_STAGES': '1',
                        'GANTRIA_XPBD_STAGE_STRIDE': '32'})
    Target = 'PhysicsJoltXpbdContactProbe'
    Run(Name, [BinaryDirectory / (Target + Suffix), Backend, 'cloth16k', 'driven-sphere', '40', '8',
               Name, '0', '32' if Backend == 'xpbd' else '1', Evidence / (Name + '.trace')],
        Environment, Timeout=300)
    (Evidence / (Name + '.config.json')).write_text(json.dumps({
        'Backend': Backend, 'Mode': Mode, 'Passes': Passes, 'OffsetM': Offset,
        'Source': Source, 'Frames': 40, 'Workers': 8}, indent=2), encoding='utf-8')
(Evidence / 'CMakeCache.txt').write_bytes((Build / 'CMakeCache.txt').read_bytes())
