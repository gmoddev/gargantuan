"""Fresh, bounded rigid-cadence screen with paired same-host timing controls.

Runs the frozen neutral rigid benchmark. Quality failures and unsupported cases
remain evidence; only process, schema, and input-integrity failures stop the
harness. Timing ratios include only paired quality-passing rows.
"""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import struct
import subprocess
import sys
import time


Cases = ('static-world', 'stack', 'dense-contacts', 'mostly-static', 'body-churn',
         'sleep-wake', 'mesh-contact', 'advanced-queries', 'mesh-query-dedup',
         'filter-updates', 'moving-ccd-off', 'moving-ccd-on', 'joint-motors',
         'spring-distance', 'articulated-chain', 'concurrent-admission')
CostCases = ('stack', 'dense-contacts', 'mostly-static')
CreationFlags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def HashFile(PathName):
    Digest = hashlib.sha256()
    with PathName.open('rb') as Stream:
        for Block in iter(lambda: Stream.read(1024 * 1024), b''):
            Digest.update(Block)
    return Digest.hexdigest()


def ImportedLibraries(PathName):
    """Read PE/ELF direct dynamic imports without loading the executable."""
    Data = PathName.read_bytes()
    if Data[:2] == b'MZ':
        Header = struct.unpack_from('<I', Data, 0x3c)[0]
        if Data[Header:Header + 4] != b'PE\0\0':
            raise ValueError('[Physics:RigidCost] Invalid PE signature')
        Coff = Header + 4
        SectionCount = struct.unpack_from('<H', Data, Coff + 2)[0]
        OptionalSize = struct.unpack_from('<H', Data, Coff + 16)[0]
        Optional = Coff + 20
        Magic = struct.unpack_from('<H', Data, Optional)[0]
        Directory = Optional + (112 if Magic == 0x20b else 96 if Magic == 0x10b else -1)
        if Directory < Optional:
            raise ValueError('[Physics:RigidCost] Unsupported PE optional header')
        ImportRva = struct.unpack_from('<I', Data, Directory + 8)[0]
        SectionBase = Optional + OptionalSize
        Sections = []
        for Index in range(SectionCount):
            Offset = SectionBase + 40 * Index
            VirtualSize, VirtualAddress, RawSize, RawStart = struct.unpack_from(
                '<IIII', Data, Offset + 8)
            Sections.append((VirtualAddress, max(VirtualSize, RawSize), RawStart))

        def FileOffset(Rva):
            for VirtualAddress, Size, RawStart in Sections:
                if VirtualAddress <= Rva < VirtualAddress + Size:
                    return RawStart + Rva - VirtualAddress
            raise ValueError(f'[Physics:RigidCost] PE import RVA unmapped: {Rva}')

        Libraries = []
        if ImportRva:
            Offset = FileOffset(ImportRva)
            for _ in range(1024):
                Values = struct.unpack_from('<IIIII', Data, Offset)
                if not any(Values):
                    break
                Start = FileOffset(Values[3])
                End = Data.index(b'\0', Start)
                Libraries.append(Data[Start:End].decode('ascii'))
                Offset += 20
            else:
                raise ValueError('[Physics:RigidCost] PE import table unbounded')
        return {'Format': 'PE', 'DirectLibraries': Libraries}
    if Data[:4] == b'\x7fELF':
        if Data[4] != 2 or Data[5] != 1:
            raise ValueError('[Physics:RigidCost] Expected little-endian ELF64')
        ProgramOffset = struct.unpack_from('<Q', Data, 32)[0]
        ProgramEntrySize, ProgramCount = struct.unpack_from('<HH', Data, 54)
        LoadSegments = []
        Dynamic = None
        for Index in range(ProgramCount):
            Offset = ProgramOffset + Index * ProgramEntrySize
            Kind, _, FileStart, VirtualAddress, _, FileSize, _, _ = struct.unpack_from(
                '<IIQQQQQQ', Data, Offset)
            if Kind == 1:
                LoadSegments.append((VirtualAddress, FileSize, FileStart))
            elif Kind == 2:
                Dynamic = (FileStart, FileSize)
        if Dynamic is None:
            return {'Format': 'ELF64', 'DirectLibraries': []}

        def FileOffset(Address):
            for VirtualAddress, FileSize, FileStart in LoadSegments:
                if VirtualAddress <= Address < VirtualAddress + FileSize:
                    return FileStart + Address - VirtualAddress
            raise ValueError(f'[Physics:RigidCost] ELF address unmapped: {Address}')

        Needed = []
        StringAddress = None
        for Offset in range(Dynamic[0], Dynamic[0] + Dynamic[1], 16):
            Tag, Value = struct.unpack_from('<qQ', Data, Offset)
            if Tag == 0:
                break
            if Tag == 1:
                Needed.append(Value)
            elif Tag == 5:
                StringAddress = Value
        if StringAddress is None:
            raise ValueError('[Physics:RigidCost] ELF dynamic string table absent')
        StringOffset = FileOffset(StringAddress)
        Libraries = []
        for Value in Needed:
            Start = StringOffset + Value
            End = Data.index(b'\0', Start)
            Libraries.append(Data[Start:End].decode('ascii'))
        return {'Format': 'ELF64', 'DirectLibraries': Libraries}
    raise ValueError('[Physics:RigidCost] Unsupported executable format')


def Definitions():
    for Backend in ('box3d', 'jolt'):
        for Case in Cases:
            yield ('Screen', Backend, Case, 1, 2, 8, 0)
    for Repeat in (1, 2, 3):
        for Backend in ('box3d', 'jolt'):
            for Case in CostCases:
                yield ('Cost', Backend, Case, 1, 1, 4, Repeat)
                yield ('Cost', Backend, Case, 1, 2, 8, Repeat)
        for Workers in (2, 8):
            for Case in CostCases:
                yield ('Thread', 'jolt', Case, Workers, 2, 8, Repeat)


def PhysicalRow(Row):
    TimingAndResources = {'Step', 'InitMs', 'TeardownMs', 'ResourcesInit',
                          'ResourcesRun', 'ResourcesRetire', 'Resources',
                          'TotalResources'}
    return {Key: Value for Key, Value in Row.items()
            if Key not in TimingAndResources}


def RunCase(Definition, Executable, Output, Environment, TimeoutSeconds):
    Phase, Backend, Case, Workers, Updates, Steps, Repeat = Definition
    Name = f'{Phase}-{Backend}-{Case}-T{Workers}-U{Updates}C{Steps}-R{Repeat}'
    Options = [f'--backend={Backend}', f'--case={Case}', f'--workers={Workers}',
               f'--world-updates={Updates}', f'--collision-steps={Steps}']
    StdoutPath = Output / f'{Name}.jsonl'
    StderrPath = Output / f'{Name}.stderr.txt'
    Started = time.monotonic()
    with StdoutPath.open('wb') as Stdout, StderrPath.open('wb') as Stderr:
        try:
            Process = subprocess.run([str(Executable), *Options], stdout=Stdout,
                                     stderr=Stderr, env=Environment,
                                     timeout=TimeoutSeconds,
                                     creationflags=CreationFlags, check=False)
        except subprocess.TimeoutExpired as Error:
            raise RuntimeError(f'[Physics:RigidCost] {Name} timed out') from Error
    Rows = [json.loads(Line) for Line in StdoutPath.read_text(encoding='utf-8').splitlines()]
    if len(Rows) != 2 or Rows[0].get('Type') != 'PhysicsJoltRigidBenchmark':
        raise ValueError(f'[Physics:RigidCost] {Name} output schema/row count mismatch')
    Config, Row = Rows
    if (Config.get('Backend') != Backend or Config.get('WorldUpdatesPerFrame') != Updates or
            Config.get('CollisionSubsteps') != Steps or
            Config.get('WorkerBudgetTotal') != Workers or
            Row.get('Case') != Case or Row.get('Backend') != Backend):
        raise ValueError(f'[Physics:RigidCost] {Name} profile metadata mismatch')
    Quality = Row.get('Quality', {}).get('Status')
    if Quality not in ('pass', 'fail', 'unsupported'):
        raise ValueError(f'[Physics:RigidCost] {Name} missing quality disposition')
    ExpectedExit = 1 if Quality == 'fail' else 0
    if Process.returncode != ExpectedExit:
        raise ValueError(f'[Physics:RigidCost] {Name} exit {Process.returncode}, expected {ExpectedExit}')
    Step = Row.get('Step')
    HasStepSamples = (isinstance(Step, dict) and
                      isinstance(Step.get('P50Ms'), (float, int)) and
                      isinstance(Step.get('P95Ms'), (float, int)) and
                      Step['P50Ms'] > 0 and Step['P95Ms'] > 0)
    if Phase != 'Screen' and Quality == 'pass' and not HasStepSamples:
        raise ValueError(f'[Physics:RigidCost] {Name} passing row lacks step samples')
    TimingAdmitted = Quality == 'pass' and HasStepSamples
    return {'Name': Name, 'Phase': Phase, 'Backend': Backend, 'Case': Case,
            'Workers': Workers, 'WorldUpdates': Updates, 'CollisionSteps': Steps,
            'Repeat': Repeat, 'Options': Options, 'ExitCode': Process.returncode,
            'WallSeconds': time.monotonic() - Started,
            'OutputSha256': HashFile(StdoutPath),
            'StderrSha256': HashFile(StderrPath),
            'QualityStatus': Quality, 'Status': Row.get('Status'),
            'TimingAdmitted': TimingAdmitted,
            'P50Ms': Step['P50Ms'] if TimingAdmitted else None,
            'P95Ms': Step['P95Ms'] if TimingAdmitted else None,
            'MeasuredFrames': Row.get('MeasuredFrames'),
            'PhysicalRowSha256': hashlib.sha256(json.dumps(
                PhysicalRow(Row), sort_keys=True, separators=(',', ':')).encode()).hexdigest(),
            'Quality': Row.get('Quality'),
            'Resources': Row.get('TotalResources'),
            'Row': Row}


def Summarize(Receipts):
    Paired = []
    for Backend in ('box3d', 'jolt'):
        for Case in CostCases:
            Pairs = []
            Excluded = []
            for Repeat in (1, 2, 3):
                Base = next(R for R in Receipts if R['Phase'] == 'Cost' and
                            R['Backend'] == Backend and R['Case'] == Case and
                            R['Repeat'] == Repeat and R['WorldUpdates'] == 1)
                Candidate = next(R for R in Receipts if R['Phase'] == 'Cost' and
                                 R['Backend'] == Backend and R['Case'] == Case and
                                 R['Repeat'] == Repeat and R['WorldUpdates'] == 2)
                if Base['TimingAdmitted'] and Candidate['TimingAdmitted']:
                    Pairs.append({'Repeat': Repeat, 'BaselineP50Ms': Base['P50Ms'],
                                  'CandidateP50Ms': Candidate['P50Ms'],
                                  'CandidateOverBaseline': Candidate['P50Ms'] / Base['P50Ms']})
                else:
                    Excluded.append({'Repeat': Repeat, 'BaselineQuality': Base['QualityStatus'],
                                     'CandidateQuality': Candidate['QualityStatus']})
            Paired.append({'Backend': Backend, 'Case': Case, 'AdmittedPairs': Pairs,
                           'ExcludedPairs': Excluded,
                           'MedianCandidateOverBaseline': statistics.median(
                               Pair['CandidateOverBaseline'] for Pair in Pairs) if Pairs else None,
                           'MedianBaselineP50Ms': statistics.median(
                               Pair['BaselineP50Ms'] for Pair in Pairs) if Pairs else None,
                           'MedianCandidateP50Ms': statistics.median(
                               Pair['CandidateP50Ms'] for Pair in Pairs) if Pairs else None})
    Threads = []
    for Case in CostCases:
        for Workers in (1, 2, 8):
            Phase = 'Cost' if Workers == 1 else 'Thread'
            Rows = [R for R in Receipts if R['Phase'] == Phase and
                    R['Backend'] == 'jolt' and R['Case'] == Case and
                    R['Workers'] == Workers and R['WorldUpdates'] == 2]
            Accepted = [R['P50Ms'] for R in Rows if R['TimingAdmitted']]
            Threads.append({'Case': Case, 'Workers': Workers,
                            'Admitted': len(Accepted), 'Total': len(Rows),
                            'MedianP50Ms': statistics.median(Accepted) if Accepted else None})
    return {'ProcessCount': len(Receipts), 'ExpectedProcessCount': 86,
            'CandidateAllCases': [
                {'Backend': R['Backend'], 'Case': R['Case'],
                 'QualityStatus': R['QualityStatus'], 'TimingAdmitted': R['TimingAdmitted'],
                 'P50Ms': R['P50Ms'], 'PhysicalRowSha256': R['PhysicalRowSha256']}
                for R in Receipts if R['Phase'] == 'Screen'],
            'QualityFailures': [
                {'Name': R['Name'], 'Quality': R['Quality'], 'Status': R['Status']}
                for R in Receipts if R['QualityStatus'] != 'pass'],
            'SameHostPairedCost': Paired,
            'JoltCandidateThreadCost': Threads,
            'TimingRule': 'Only rows whose existing Quality.Status is pass enter cost summaries; raw timings and all failed/unsupported rows remain in receipts.',
            'Scope': 'Frozen neutral rigid benchmark; global U2/C8 cadence versus U1/C4. This is not a soft/rigid coupled cost claim.'}


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument('--exe', type=Path, required=True)
    Parser.add_argument('--output', type=Path, required=True)
    Parser.add_argument('--runtime-dir', type=Path, action='append', default=[])
    Parser.add_argument('--runtime-file', type=Path, action='append', default=[])
    Parser.add_argument('--expected-exe-sha256')
    Parser.add_argument('--timeout-seconds', type=int, default=600)
    Parser.add_argument('--total-timeout-seconds', type=int, default=7200)
    Arguments = Parser.parse_args()
    if not 30 <= Arguments.timeout_seconds <= 1800:
        Parser.error('[Physics:RigidCost] --timeout-seconds must be 30..1800')
    if not 600 <= Arguments.total_timeout_seconds <= 14400:
        Parser.error('[Physics:RigidCost] --total-timeout-seconds must be 600..14400')
    Executable = Arguments.exe.resolve(strict=True)
    Output = Arguments.output
    Output.mkdir(parents=True, exist_ok=False)
    RuntimeDirs = [Item.resolve(strict=True) for Item in Arguments.runtime_dir]
    RuntimeFiles = {Item.resolve(strict=True) for Item in Arguments.runtime_file}
    for Directory in RuntimeDirs:
        RuntimeFiles.update(Item for Item in Directory.iterdir() if Item.is_file() and
                            (Item.suffix.lower() == '.dll' or '.so' in Item.name))
    BinaryHash = HashFile(Executable)
    if (Arguments.expected_exe_sha256 and
            BinaryHash.lower() != Arguments.expected_exe_sha256.lower()):
        raise ValueError('[Physics:RigidCost] Frozen executable hash mismatch')
    RuntimeHashes = {str(Item): HashFile(Item) for Item in sorted(RuntimeFiles)}
    ScriptHash = HashFile(Path(__file__))
    Environment = os.environ.copy()
    LibraryVariable = 'PATH' if os.name == 'nt' else 'LD_LIBRARY_PATH'
    if RuntimeDirs:
        Environment[LibraryVariable] = (
            os.pathsep.join(str(Item) for Item in RuntimeDirs) + os.pathsep +
            Environment.get(LibraryVariable, ''))
    Metadata = {'Executable': str(Executable), 'ExecutableSha256': BinaryHash,
                'ImportedLibraries': ImportedLibraries(Executable),
                'RuntimeFilesSha256': RuntimeHashes,
                'RuntimeDirs': [str(Item) for Item in RuntimeDirs],
                'LibrarySearchVariable': LibraryVariable,
                'ScriptSha256': ScriptHash,
                'Platform': platform.platform(), 'Python': sys.version,
                'StartedUtc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                'PerProcessTimeoutSeconds': Arguments.timeout_seconds,
                'TotalTimeoutSeconds': Arguments.total_timeout_seconds}
    (Output / 'Metadata.json').write_text(json.dumps(Metadata, indent=2) + '\n',
                                          encoding='utf-8')
    Receipts = []
    Started = time.monotonic()
    for Definition in Definitions():
        if time.monotonic() - Started >= Arguments.total_timeout_seconds:
            raise RuntimeError('[Physics:RigidCost] Total bounded runtime exceeded')
        Receipt = RunCase(Definition, Executable, Output, Environment,
                          min(Arguments.timeout_seconds, max(1, int(
                              Arguments.total_timeout_seconds - (time.monotonic() - Started)))))
        Receipts.append(Receipt)
        (Output / 'Receipts.json').write_text(json.dumps(Receipts, indent=2,
                                                       allow_nan=False) + '\n',
                                              encoding='utf-8')
        print(json.dumps({'Name': Receipt['Name'], 'Quality': Receipt['QualityStatus'],
                          'P50Ms': Receipt['P50Ms']}), flush=True)
    if len(Receipts) != 86:
        raise ValueError('[Physics:RigidCost] Case matrix incomplete')
    if HashFile(Executable) != BinaryHash or HashFile(Path(__file__)) != ScriptHash or any(
            HashFile(Path(Name)) != Digest for Name, Digest in RuntimeHashes.items()):
        raise RuntimeError('[Physics:RigidCost] Executable, runtime, or runner changed')
    Summary = Summarize(Receipts)
    Summary['InputsUnchanged'] = True
    Summary['CompletedUtc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    (Output / 'Summary.json').write_text(json.dumps(Summary, indent=2,
                                                  allow_nan=False) + '\n',
                                         encoding='utf-8')
    if Summary['ProcessCount'] != Summary['ExpectedProcessCount']:
        raise RuntimeError('[Physics:RigidCost] Process count mismatch')


if __name__ == '__main__':
    Main()
