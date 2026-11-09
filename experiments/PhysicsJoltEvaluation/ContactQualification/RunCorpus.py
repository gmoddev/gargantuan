"""Run the prespecified corpus and preserve failures, traces and provenance."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import struct
import subprocess
import sys
import time

from CreateCases import Cases
from ValidateContactTrace import Inspect


def Digest(PathValue):
    with PathValue.open('rb') as Stream:
        return hashlib.file_digest(Stream, 'sha256').hexdigest()


def PackTrace(Directory):
    Raw = Directory / 'ContactTraceV1.jsonl'
    Packed = Directory / 'ContactTraceV1.jsonl.gz'
    RawDigest = Digest(Raw)
    FrameHashes = []
    with Raw.open('rb') as Input, Packed.open('wb') as Output, \
            gzip.GzipFile(filename='', fileobj=Output, mode='wb', compresslevel=1, mtime=0) as Compressed:
        for Line in Input:
            Row = json.loads(Line)
            assert Row['Frame'] == len(FrameHashes)
            Bits = {}
            for Name in ('Positions', 'Velocities'):
                Values = Row[Name]
                Bytes = struct.pack('<' + str(len(Values)) + 'f', *Values)
                Bits[Name + 'Float32Sha256'] = hashlib.sha256(Bytes).hexdigest()
            FrameHashes.append({'Frame': Row['Frame'], **Bits})
            Compressed.write(Line)
    with gzip.open(Packed, 'rb') as Verify:
        assert hashlib.file_digest(Verify, 'sha256').hexdigest() == RawDigest
    (Directory / 'FrameHashes.json').write_text(json.dumps(FrameHashes, indent=2), encoding='utf-8')
    # This disposable file is losslessly preserved by the checked gzip stream.
    assert Raw.resolve().parent == Directory.resolve() and Raw.name == 'ContactTraceV1.jsonl'
    Raw.unlink()
    return RawDigest


def Main():
    Parser = argparse.ArgumentParser()
    Parser.add_argument('--binary', type=Path, required=True)
    Parser.add_argument('--engine', type=Path, required=True)
    Parser.add_argument('--native', type=Path, required=True)
    Parser.add_argument('--output', type=Path, required=True)
    Parser.add_argument('--variants', nargs='+', default=['jolt:A', 'jolt:F4G', 'xpbd:A', 'xpbd:D0', 'xpbd:E'])
    Parser.add_argument('--cases', nargs='*')
    Parser.add_argument('--manifest', type=Path)
    Parser.add_argument('--workers', type=int, choices=(1,8), default=1)
    Parser.add_argument('--face-cull', type=int, choices=(0,1), default=0)
    Args = Parser.parse_args()
    assert not Args.output.exists(), 'Evidence directories are immutable once created'
    Args.output.mkdir(parents=True)
    if Args.manifest:
        assert Args.manifest.stat().st_size < 4 * 1024 * 1024
        Definitions = json.loads(Args.manifest.read_text(encoding='utf-8'))
        assert isinstance(Definitions, list) and 1 <= len(Definitions) <= 256
    else:
        Definitions = Cases()
    Selected = [Case for Case in Definitions if Args.cases is None or Case['CaseId'] in Args.cases]
    assert Selected and (Args.cases is None or {Case['CaseId'] for Case in Selected} == set(Args.cases))
    Manifest = {'Definitions': Selected, 'Variants': Args.variants, 'Workers': Args.workers,'FaceCull':Args.face_cull,
                'Scope': 'Mechanism and physical geometry screen; absent impulse/work/self-contact evidence remains INCOMPLETE',
                'LegacyAxialGate': .1, 'AdditionalTriangleShellPenetrationGateM': .001}
    ManifestPath = Args.output / 'Manifest.json'
    ManifestPath.write_text(json.dumps(Manifest, indent=2), encoding='utf-8')
    EngineHead = subprocess.check_output(['git','-C',str(Args.engine),'rev-parse','HEAD'], text=True).strip()
    NativeHead = subprocess.check_output(['git','-C',str(Args.native),'rev-parse','HEAD'], text=True).strip()
    CompilerFiles = list(Args.binary.parents[2].glob('CMakeFiles/*/CMakeCXXCompiler.cmake'))
    Compiler = CompilerFiles[0].read_text(errors='replace') if CompilerFiles else 'Unavailable'
    Compiler = '; '.join(re.findall(r'set\(CMAKE_CXX_COMPILER(?:_VERSION|_ID)? "([^"]+)"\)', Compiler))
    Summary = []
    for Variant in Args.variants:
        Backend, Mode = Variant.split(':')
        assert Backend in ('jolt','xpbd') and Mode in ('A','AH','AS','AG','B','C','D0','E','EH','ES','G','GH','GS','H','I','IG','F1G','F4G','F1','F4')
        for Definition in Selected:
            Name = Backend + '-' + Mode + '-' + Definition['CaseId']
            Directory = Args.output / Name
            Directory.mkdir()
            Config = {**Definition, 'CaseId': Name, 'Backend': Backend, 'Workers': Args.workers}
            ConfigPath = Directory / 'Case.json'
            ConfigPath.write_text(json.dumps(Config, indent=2), encoding='utf-8')
            Environment = {Key: Value for Key, Value in os.environ.items() if not Key.startswith(
                ('GANTRIA_PROBE_', 'GANTRIA_CONTACT_', 'GANTRIA_XPBD_CONTACT_', 'GANTRIA_XPBD_STAGE_', 'GANTRIA_XPBD_VELOCITY_'))}
            Environment.update({'GANTRIA_CONTACT_ORDER':Mode[0], 'GANTRIA_XPBD_CONTACT_ORDER':Mode[0],
                'GANTRIA_CONTACT_PASSES':'4' if '4' in Mode else '1',
                'GANTRIA_CONTACT_KINEMATIC_RELATIVE':'1' if Mode.endswith('G') else '0',
                'GANTRIA_CONTACT_FACE_AABB_CULL':str(Args.face_cull),
                'GANTRIA_XPBD_IDEAL_VELOCITY':('2' if Mode.endswith('S') else '1' if Mode.endswith('H') else '0') if Backend=='xpbd' else '0',
                'GANTRIA_XPBD_CONTACT_NORMAL_COMPLIANCE':'0', 'SDL_VIDEODRIVER':'dummy'})
            Command = [str(Args.binary.resolve()), str(ConfigPath.resolve()), str(Directory.resolve())]
            Started = time.monotonic()
            try:
                Process = subprocess.run(Command, env=Environment, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, timeout=300,
                    **({'creationflags':subprocess.CREATE_NO_WINDOW} if platform.system() == 'Windows' else {}))
                Code, Output, Error = Process.returncode, Process.stdout, Process.stderr
            except subprocess.TimeoutExpired as Failure:
                Code, Output, Error = 124, Failure.stdout or b'', Failure.stderr or b''
            (Directory / 'stdout.txt').write_bytes(Output)
            (Directory / 'stderr.txt').write_bytes(Error)
            Receipt = {'Command':Command, 'ExitCode':Code, 'Seconds':time.monotonic()-Started,
                'StdoutSha256':hashlib.sha256(Output).hexdigest(), 'StderrSha256':hashlib.sha256(Error).hexdigest(),
                'SolverEnvironment':{Key:Value for Key,Value in Environment.items() if Key.startswith('GANTRIA_')}}
            ReceiptPath = Directory / 'Receipt.json'
            ReceiptPath.write_text(json.dumps(Receipt, indent=2), encoding='utf-8')
            Row = {'CaseId':Name, 'ProcessExitCode':Code, 'ReceiptSha256':Digest(ReceiptPath)}
            if Backend == 'xpbd' and any(Sphere['Motion']=='dynamic' for Sphere in Config['Spheres']):
                Row['Status'] = 'UNSUPPORTED_EXPECTED' if Code == 1 and b'cannot provide reciprocal dynamic coupling' in Error else 'HARNESS_FAILURE'
            elif Code == 0:
                RawDigest = PackTrace(Directory)
                Source = {'Engine':EngineHead, 'Native':NativeHead, 'SolverVariant':Variant,
                    'OS':platform.platform(), 'Compiler':Compiler or 'Unavailable', 'Workers':Args.workers,
                    'SeedPolicy':'No random jitter', 'ManifestSha256':Digest(ManifestPath),
                    'FixtureSha256':Digest(Directory/'ContactFixtureV1.json'),
                    'TraceSha256':Digest(Directory/'ContactTraceV1.jsonl.gz'),
                    'UncompressedTraceSha256':RawDigest, 'ReceiptSha256':Digest(ReceiptPath),
                    'BinarySha256':Digest(Args.binary)}
                (Directory/'Source.json').write_text(json.dumps(Source, indent=2), encoding='utf-8')
                try:
                    Verdict = Inspect(Directory)
                    (Directory/'Verdict.json').write_text(json.dumps(Verdict, indent=2), encoding='utf-8')
                    Row.update(Status='GEOMETRY_PASS' if Verdict['GeometryGatePass'] else 'GEOMETRY_FAIL',
                        AxialStrain=Verdict['MaximumAxialStrain'], PenetrationM=Verdict['MaximumSpherePenetrationM'],
                        PhysicalQualification=Verdict['FullPhysicalQualification'])
                except (ValueError, OSError, KeyError, TypeError) as Failure:
                    Row.update(Status='VALIDATION_FAILURE', Error=str(Failure))
            else:
                Row['Status'] = 'PROCESS_FAILURE'
            Summary.append(Row)
            (Args.output/'Summary.json').write_text(json.dumps(Summary, indent=2), encoding='utf-8')
            print('[Physics:ContactCorpus]', Name, Row['Status'], flush=True)
    return int(any(Row['Status'] in ('HARNESS_FAILURE','PROCESS_FAILURE','VALIDATION_FAILURE') for Row in Summary))


if __name__ == '__main__':
    raise SystemExit(Main())
