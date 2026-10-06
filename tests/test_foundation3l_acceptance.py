import copy
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock
import zipfile

import foundation3l_acceptance as A


SOURCE = "1" * 40


def Put(Value, PathValue, Item):
    Parts = PathValue.split(".")
    for Part in Parts[:-1]:
        Value = Value.setdefault(Part, {})
    Value[Parts[-1]] = Item


def Providers():
    Value = {"Format": "GargantuanPhysicalFarmAcceptanceObservation", "Version": 1,
             "Status": "INCOMPLETE", "Foundation3LQualification": "NOT CLAIMED",
             "WorkloadPinParity": {"State": "MEASURED", "SourceCommit": SOURCE},
             "LocalRunId": "11111111-1111-4111-8111-111111111111",
             "NodeRunId": "22222222-2222-4222-8222-222222222222"}
    for Role in ("Local", "Node"):
        Value[Role] = {"AcceptedBytes": 123, "RetiredBytes": 123}
        for Key, Expected in A.PROVIDER_GATES.items():
            Put(Value[Role], Key, Expected)
        Put(Value[Role], "Recovery.ExactRetainedWorkBytes", 123)
    Put(Value, "Local.Provider.State", "LOCAL_PINNED_PACKAGE_ONLY")
    Put(Value, "Node.Provider.State", "AUTHENTICATED_MANIFEST_RPC_MEASURED")
    Put(Value, "Node.Provider.RealTls", "NEGOTIATED_TLS_MANIFEST_RPC_MEASURED")
    Put(Value, "Node.Provider.ProcessResources.State", "MEASURED")
    return Value


def Four():
    return {"State": "MEASURED_PASS", "SourceCommit": SOURCE,
            "RunId": "33333333-3333-4333-8333-333333333333",
            "LifecycleRunId": "44444444-4444-4444-8444-444444444444"}


def Xml(Names, Suffix=""):
    return ('<testsuite tests="%d" failures="0" errors="0">' % len(Names) +
            "".join(f'<testcase name="{Name}" status="run">{Suffix}</testcase>' for Name in sorted(Names)) +
            '</testsuite>').encode()


class AnalyzerProvenanceTests(unittest.TestCase):
    AUTHOR = ("-c", "user.email=foundation3l-fixture@users.noreply.github.com",
              "-c", "user.name=Foundation3L Fixture")

    def setUp(self):
        self.Temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.Temporary.cleanup)
        self.Root = Path(self.Temporary.name)
        subprocess.run(["git", "init", "-q", str(self.Root)], check=True, capture_output=True)
        (self.Root / ".git/fixture-hooks").mkdir()
        for Name in A.ANALYZER_FILES:
            File = self.Root / Name
            File.parent.mkdir(parents=True, exist_ok=True)
            File.write_text("frozen A\n", encoding="utf-8")
        self.Git("add", "--", *A.ANALYZER_FILES)
        self.Git(*self.AUTHOR, "commit", "-qm", "A")
        self.Source = self.Git("rev-parse", "HEAD").strip()
        (self.Root / "tests/foundation3l_four_client.py").write_text("reviewed B\n", encoding="utf-8")
        self.Git("add", "--", "tests/foundation3l_four_client.py")
        self.Git(*self.AUTHOR, "commit", "-qm", "B")
        self.Analyzer = self.Git("rev-parse", "HEAD").strip()

    def Git(self, *Arguments):
        Result = subprocess.run(["git", "-c", f"core.hooksPath={self.Root / '.git/fixture-hooks'}",
                                 *Arguments], cwd=self.Root,
                                capture_output=True, text=True)
        self.assertEqual(Result.returncode, 0, Result.stderr or Result.stdout)
        return Result.stdout

    def test_clean_descendant_b_records_exact_analyzer_without_relabeling_a(self):
        Result = A.VerifyAnalyzerCheckout(self.Root, self.Source, self.Analyzer)
        self.assertEqual(Result["ExecutionSourceCommit"], self.Source)
        self.assertEqual(Result["AnalyzerCommit"], self.Analyzer)
        self.assertEqual([Row["Path"] for Row in Result["Files"]], list(A.ANALYZER_FILES))
        for Row in Result["Files"]:
            self.assertEqual(Row["Sha256"], A.Digest((self.Root / Row["Path"]).read_bytes()))
        # Omitting the explicit B pin must retain the old exact-A-head rule.
        with self.assertRaisesRegex(A.EvidenceError, "exact reviewed analyzer commit"):
            A.VerifyAnalyzerCheckout(self.Root, self.Source, self.Source)

    def test_wrong_head_divergent_source_and_dirty_analyzer_fail(self):
        with self.assertRaisesRegex(A.EvidenceError, "exact reviewed analyzer commit"):
            A.VerifyAnalyzerCheckout(self.Root, self.Source, "f" * 40)
        Tree = self.Git("rev-parse", "HEAD^{tree}").strip()
        Unrelated = self.Git(*self.AUTHOR, "commit-tree", Tree, "-m", "unrelated").strip()
        with self.assertRaisesRegex(A.EvidenceError, "not a descendant"):
            A.VerifyAnalyzerCheckout(self.Root, Unrelated, self.Analyzer)
        (self.Root / "tests/foundation3l_four_client.py").write_text("uncommitted bypass\n", encoding="utf-8")
        with self.assertRaisesRegex(A.EvidenceError, "tracked analyzer source is dirty"):
            A.VerifyAnalyzerCheckout(self.Root, self.Source, self.Analyzer)

    def test_missing_tracked_analyzer_dependency_fails_in_clean_descendant(self):
        Missing = A.ANALYZER_FILES[-1]
        self.Git("rm", "--", Missing)
        self.Git(*self.AUTHOR, "commit", "-qm", "remove required analyzer")
        Head = self.Git("rev-parse", "HEAD").strip()
        with self.assertRaisesRegex(A.EvidenceError, "untracked fixed analyzer dependency"):
            A.VerifyAnalyzerCheckout(self.Root, self.Source, Head)

    def test_execution_source_is_analyzer_when_head_is_a(self):
        self.Git("checkout", "-q", "--detach", self.Source)
        Result = A.VerifyAnalyzerCheckout(self.Root, self.Source, self.Source)
        self.assertEqual(Result["ExecutionSourceCommit"], self.Source)
        self.assertEqual(Result["AnalyzerCommit"], self.Source)


class FinalTests(unittest.TestCase):
    def Final(self, Farm=None, CI=None, Physical=None):
        return A.FinalObservation(SOURCE, Farm or Providers(), CI or
                                  {"State": "MEASURED_PASS", "SourceCommit": SOURCE}, Physical or Four(),
                                  {'Package': {'State': 'MEASURED_PASS'}, 'Sequence': {'State': 'MEASURED_PASS'}})

    def test_all_typed_conjuncts_are_required(self):
        self.assertEqual(self.Final()["Status"], "PASS")
        for Role in ("Local", "Node"):
            for Key in A.PROVIDER_GATES:
                with self.subTest(Role=Role, Gate=Key):
                    Farm = Providers()
                    Put(Farm[Role], Key, None)
                    # A fabricated top-level result never repairs missing evidence.
                    Farm["Status"] = "PASS"
                    self.assertEqual(self.Final(Farm)["Status"], "INCOMPLETE")

    def test_observed_failure_wins_over_missing(self):
        Farm = Providers()
        Put(Farm, "Local.F1.State", "MEASURED_FAIL")
        Put(Farm, "Node.Publication.OrdinaryDemand.State", None)
        self.assertEqual(self.Final(Farm)["Status"], "FAIL")

    def test_ci_and_fresh_four_are_required(self):
        self.assertEqual(self.Final(CI={"State": "NOT_MEASURED"})["Status"], "INCOMPLETE")
        self.assertEqual(self.Final(Physical={"State": "NOT_MEASURED"})["Status"], "INCOMPLETE")
        self.assertEqual(A.FinalObservation(SOURCE, Providers(),
                         {"State": "MEASURED_PASS", "SourceCommit": SOURCE}, Four())['Status'], 'INCOMPLETE')

    def test_wrong_source_or_reused_identity_rejected(self):
        for Mutator in (lambda P: Put(P, "WorkloadPinParity.SourceCommit", "2" * 40),
                        lambda P: P.update(NodeRunId=P["LocalRunId"]),
                        lambda P: Put(P, "Local.RetiredBytes", 0),
                        lambda P: Put(P, "Node.Recovery.ExactRetainedWorkBytes", True)):
            Farm = Providers()
            Mutator(Farm)
            with self.assertRaises((ValueError, A.EvidenceError)):
                self.Final(Farm)
        BadFour = Four()
        BadFour["RunId"] = Providers()["LocalRunId"]
        with self.assertRaises(A.EvidenceError):
            self.Final(Physical=BadFour)

    def test_no_invented_cpu_or_rss_percentage_gate(self):
        Farm = Providers()
        Put(Farm, "Node.Resources.ServerHost.MaximumObservedHostCpuPercent", 100)
        Put(Farm, "Node.Provider.ProcessResources.HeadroomThreshold", "NOT_DEFINED")
        self.assertEqual(self.Final(Farm)["Status"], "PASS")

    def test_tls_and_node_process_are_independent(self):
        for Key in ("Node.Provider.RealTls", "Node.Provider.ProcessResources.State"):
            Farm = Providers()
            Put(Farm, Key, "NOT_MEASURED")
            self.assertEqual(self.Final(Farm)["Status"], "INCOMPLETE")

    def test_raw_map_cannot_replace_fixed_replayer(self):
        with self.assertRaises(A.EvidenceError):
            A.ReplayProviders(Path('.'), {"Script": "fake.ps1", "Status": "PASS"}, Path('ignored.json'), None)

    def test_powershell_is_explicit_and_hash_pinned(self):
        with tempfile.TemporaryDirectory() as Root:
            Executable = Path(Root) / 'pwsh.exe'
            Executable.write_bytes(b'pinned-runtime-fixture')
            Hash = A.Digest(Executable.read_bytes())
            self.assertEqual(A.PinnedPowerShell(str(Executable), Hash), Executable)
            for PathValue, Pin in ((str(Executable), '0' * 64), ('pwsh', Hash),
                                   (str(Path(Root) / 'script.ps1'), Hash)):
                with self.assertRaises(A.EvidenceError):
                    A.PinnedPowerShell(PathValue, Pin)

    def test_replayer_uses_explicit_runtime_and_fixed_script(self):
        with tempfile.TemporaryDirectory() as Root:
            Root = Path(Root)
            Arguments = {Key: str(Root / Key) for Key in (
                'LocalReportPath', 'LocalServerEvidenceRoot', 'LocalClientEvidenceRoot',
                'NodeReportPath', 'NodeServerEvidenceRoot', 'NodeClientEvidenceRoot')}
            Destination = Root / 'result.json'
            Runtime = Root / 'pwsh.exe'
            def Run(Command, **Options):
                self.assertEqual(Command[0], str(Runtime))
                self.assertEqual(Command[Command.index('-File') + 1], str(Root / 'tests/PhysicalGameSessionFarmAcceptance.ps1'))
                self.assertTrue(Options['check'])
                Destination.write_text(json.dumps(Providers()))
            with mock.patch.object(A.subprocess, 'run', side_effect=Run):
                self.assertEqual(A.ReplayProviders(Root, Arguments, Destination, Runtime), Providers())

    def test_four_replay_has_no_summary_bypass(self):
        with tempfile.TemporaryDirectory() as Root:
            with self.assertRaises(A.EvidenceError):
                A.ReplayFourClient(Path(Root), {"Status": "PASS"}, SOURCE)


class CITests(unittest.TestCase):
    def setUp(self):
        self.Temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.Temporary.cleanup)
        self.Root = Path(self.Temporary.name)
        self.Entries = []
        self.Documents = {}
        self.Archives = {}
        for Number, (Name, (Workflow, Artifact, Required, Step)) in enumerate(A.CI_JOBS.items()):
            RunId = 101 if Workflow.endswith('native-ci.yml') else 102
            Run = {"id": RunId, "run_attempt": 1, "repository": {"full_name": "gmoddev/gargantuan"},
                   "head_sha": SOURCE, "path": Workflow, "event": "workflow_dispatch",
                   "status": "completed", "conclusion": "success"}
            Job = {"name": Name, "run_id": RunId, "run_attempt": 1, "head_sha": SOURCE,
                   "status": "completed", "conclusion": "success",
                   "started_at": "2026-10-02T01:00:00Z", "completed_at": "2026-10-02T02:00:00Z",
                   "steps": [{"name": Step, "status": "completed", "conclusion": "success"}]}
            Jobs = {"total_count": 1, "jobs": [Job]}
            Archive = self.MakeArchive(Required)
            Metadata = {"id": Number + 1, "name": Artifact, "expired": False,
                        "created_at": "2026-10-02T01:59:00Z",
                        "workflow_run": {"id": RunId, "head_sha": SOURCE},
                        "digest": "sha256:" + A.Digest(Archive), "size_in_bytes": len(Archive)}
            Entry = {"Name": Name}
            for Key, Object in (("Run", Run), ("Jobs", Jobs), ("ArtifactMetadata", Metadata)):
                FileName = f'{Number}-{Key}.json'
                self.Documents[FileName] = Object
                Entry[Key] = {"Path": FileName, "Sha256": ""}
            FileName = f'{Number}-artifact.zip'
            self.Archives[FileName] = Archive
            Entry["Archive"] = {"Path": FileName, "Sha256": ""}
            self.Entries.append(Entry)
        NativeJobs = [self.Documents[f'{Number}-Jobs.json']['jobs'][0] for Number in (0, 1)]
        for Number in (0, 1):
            self.Documents[f'{Number}-Jobs.json'] = {'total_count': 2, 'jobs': NativeJobs}

    def MakeArchive(self, Required, Source=SOURCE, XmlValue=None):
        Buffer = io.BytesIO()
        with zipfile.ZipFile(Buffer, 'w') as Zip:
            Zip.writestr('build/ctest-results.xml', XmlValue or Xml(Required))
            Zip.writestr('qualified-source-commit.txt', Source + '\n')
        return Buffer.getvalue()

    def Write(self):
        for Entry in self.Entries:
            for Key in ('Run', 'Jobs', 'ArtifactMetadata', 'Archive'):
                Item = Entry[Key]
                Data = (self.Archives[Item['Path']] if Key == 'Archive' else
                        json.dumps(self.Documents[Item['Path']]).encode())
                (self.Root / Item['Path']).write_bytes(Data)
                Item['Sha256'] = A.Digest(Data)
        Data = json.dumps({"Format": "GargantuanFoundation3LCI", "Version": 1, "Jobs": self.Entries}).encode()
        Index = self.Root / 'index.json'
        Index.write_bytes(Data)
        return Index, A.Digest(Data)

    def Verify(self):
        Index, Pin = self.Write()
        return A.VerifyCI(Index, Pin, SOURCE)

    def test_exact_three_successful_jobs_and_raw_junit(self):
        Result = self.Verify()
        self.assertEqual(Result['State'], 'MEASURED_PASS')
        self.assertEqual(len(Result['Jobs']), 3)

    def DiagnosticMetadataReader(self, Reads, Sizes=None, ExtraNames=('diagnostics/scheduler.etl',), SourceData=None):
        # Model central-directory expanded sizes without allocating or reading
        # a large diagnostic payload. Required members remain real ZIP data.
        OriginalZip = zipfile.ZipFile

        def Open(*Arguments, **Keywords):
            Zip = OriginalZip(*Arguments, **Keywords)
            Infos = Zip.infolist()
            for Info in Infos:
                if Sizes and Info.filename in Sizes:
                    Info.file_size = Sizes[Info.filename]
            for Name in ExtraNames:
                Info = zipfile.ZipInfo(Name)
                # Preserve the supplied central-directory name in this mock;
                # ZipInfo's Windows constructor otherwise normalizes '\\'.
                Info.filename = Name
                Info.file_size = 286761989
                Infos.append(Info)
            Zip.infolist = mock.Mock(return_value=Infos)
            OriginalOpen = Zip.open

            def SelectedOpen(Member, *Arguments, **Keywords):
                Name = Member.filename if isinstance(Member, zipfile.ZipInfo) else Member
                self.assertIn(Name, ('build/ctest-results.xml', 'qualified-source-commit.txt'),
                              'unused diagnostic must never be opened')
                Cap = 128 if Name == 'qualified-source-commit.txt' else 16 * 1024 * 1024
                self.assertLessEqual(Member.file_size, Cap)
                Reads.append(Name)
                Stream = (io.BytesIO(SourceData) if Name == 'qualified-source-commit.txt' and SourceData is not None else
                          OriginalOpen(Member, *Arguments, **Keywords))
                Owner = self

                class SelectedStream:
                    def __enter__(self):
                        return self

                    def __exit__(self, *Arguments):
                        Stream.close()

                    def read(self, Maximum):
                        Owner.assertEqual(Maximum, Cap + 1, 'selected read must have an explicit cap')
                        return Stream.read(Maximum)

                return SelectedStream()

            Zip.open = SelectedOpen
            return Zip

        return Open

    def test_large_unused_diagnostic_is_not_read_or_an_expanded_size_gate(self):
        Reads = []
        with mock.patch.object(A.zipfile, 'ZipFile', side_effect=self.DiagnosticMetadataReader(Reads)):
            Result = self.Verify()
        self.assertEqual(Result['State'], 'MEASURED_PASS')
        self.assertEqual(len(Result['Jobs']), 3)
        self.assertEqual(Reads, ['qualified-source-commit.txt', 'build/ctest-results.xml'] * 3)

    def test_required_size_caps_and_all_unused_names_still_reject(self):
        for Sizes, Names, Error in (
            ({'build/ctest-results.xml': 16 * 1024 * 1024 + 1}, ('diagnostics/scheduler.etl',), 'JUnit evidence'),
            ({'qualified-source-commit.txt': 129}, ('diagnostics/scheduler.etl',), 'actual CI checkout'),
            (None, ('../unused.etl',), 'invalid ZIP member'),
            (None, ('/unused.etl',), 'invalid ZIP member'),
            (None, ('C:/unused.etl',), 'invalid ZIP member'),
            (None, ('diagnostics\\unused.etl',), 'invalid ZIP member'),
            (None, ('diagnostics/unused.etl', 'diagnostics/unused.etl'), 'duplicate ZIP member'),
            (None, tuple(f'diagnostics/{Index}.etl' for Index in range(9999)), 'unbounded CI artifact'),
        ):
            with self.subTest(Sizes=Sizes, Names=Names[:2]):
                Reads = []
                with mock.patch.object(A.zipfile, 'ZipFile', side_effect=self.DiagnosticMetadataReader(Reads, Sizes, Names)):
                    with self.assertRaisesRegex(A.EvidenceError, Error):
                        self.Verify()
                self.assertEqual(Reads, [])
        Reads = []
        with mock.patch.object(A.zipfile, 'ZipFile', side_effect=self.DiagnosticMetadataReader(Reads, SourceData=b'x' * 129)):
            with self.assertRaisesRegex(A.EvidenceError, 'actual CI checkout'):
                self.Verify()
        self.assertEqual(Reads, ['qualified-source-commit.txt'])

    def test_successful_diagnostic_dispatch_is_not_native_qualification(self):
        # Even a successful diagnostic with the correct source/artifact pins
        # cannot stand in for the complete Windows and Linux qualification.
        Windows = self.Documents['0-Jobs.json']['jobs'][0]
        Windows['name'] = 'Windows causal diagnostic - not qualification'
        with self.assertRaisesRegex(A.EvidenceError, 'unexpected/missing workflow jobs'):
            self.Verify()
        Windows['name'] = self.Entries[0]['Name']
        Windows['steps'][0]['conclusion'] = 'skipped'
        with self.assertRaisesRegex(A.EvidenceError, 'required CTest step missing/failed'):
            self.Verify()

    def test_metadata_failures(self):
        Changes = [('0-Run.json', 'head_sha', '2' * 40), ('0-Run.json', 'conclusion', 'failure'),
                   ('0-Run.json', 'event', 'pull_request'), ('0-Run.json', 'status', 'in_progress'),
                   ('0-ArtifactMetadata.json', 'expired', True),
                   ('0-ArtifactMetadata.json', 'created_at', '2026-10-01T01:59:00Z'),
                   ('0-ArtifactMetadata.json', 'digest', 'sha256:' + '0' * 64)]
        for File, Key, Value in Changes:
            with self.subTest(File=File, Key=Key):
                Previous = self.Documents[File][Key]
                self.Documents[File][Key] = Value
                with self.assertRaises(A.EvidenceError):
                    self.Verify()
                self.Documents[File][Key] = Previous

    def test_omitted_duplicate_and_truncated_jobs(self):
        Original = copy.deepcopy(self.Entries)
        for Entries in (Original[:2], [Original[0], Original[0], Original[2]]):
            self.Entries = Entries
            with self.assertRaises(A.EvidenceError):
                self.Verify()
        self.Entries = Original
        self.Documents['0-Jobs.json']['total_count'] = 3
        with self.assertRaises(A.EvidenceError):
            self.Verify()

    def test_wrong_actual_checkout_despite_matching_api_head(self):
        self.Archives['0-artifact.zip'] = self.MakeArchive(A.CI_JOBS[self.Entries[0]['Name']][2], Source='2' * 40)
        Metadata = self.Documents['0-ArtifactMetadata.json']
        Metadata['digest'] = 'sha256:' + A.Digest(self.Archives['0-artifact.zip'])
        Metadata['size_in_bytes'] = len(self.Archives['0-artifact.zip'])
        with self.assertRaisesRegex(A.EvidenceError, 'actual CI checkout'):
            self.Verify()

    def test_index_and_file_pins_are_enforced(self):
        Index, Pin = self.Write()
        with self.assertRaises(A.EvidenceError):
            A.VerifyCI(Index, '0' * 64, SOURCE)
        (self.Root / '0-Run.json').write_text('{}')
        with self.assertRaises(A.EvidenceError):
            A.VerifyCI(Index, Pin, SOURCE)

    def test_wrong_attempt_and_failed_test_step(self):
        Job = self.Documents['0-Jobs.json']['jobs'][0]
        Job['run_attempt'] = 2
        with self.assertRaises(A.EvidenceError):
            self.Verify()
        Job['run_attempt'] = 1
        Job['steps'][0]['conclusion'] = 'skipped'
        with self.assertRaises(A.EvidenceError):
            self.Verify()

    def test_junit_required_failed_skipped_duplicate_missing(self):
        Required = {'Required'}
        for Data in (Xml({'Other'}), Xml(Required, '<skipped/>'), Xml(Required, '<failure/>'),
                     b'<testsuite><testcase name="Required"/><testcase name="Required"/></testsuite>',
                     b'<!DOCTYPE testsuite><testsuite/>'):
            with self.assertRaises(A.EvidenceError):
                A.ReadJUnit(Data, Required)
        # Existing optional platform skips do not create a new qualification SLA.
        self.assertEqual(A.ReadJUnit(b'<testsuite><testcase name="Required"/>'
                                     b'<testcase name="Optional"><skipped/></testcase></testsuite>', Required), 2)

    def test_path_redirect_and_duplicate_json(self):
        for PathValue in ('../x', '/x', 'C:/x', 'a\\x', './x'):
            with self.assertRaises(A.EvidenceError):
                A.Pinned(self.Root, {'Path': PathValue, 'Sha256': '0' * 64})
        with self.assertRaises(A.EvidenceError):
            A.JsonData(b'{"Status":"PASS","Status":"FAIL"}')


class ProvenanceTests(unittest.TestCase):
    def setUp(self):
        self.Temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.Temporary.cleanup)
        self.Root = Path(self.Temporary.name)
        self.CI = {'State': 'MEASURED_PASS', 'Jobs': [{
            'Name': next(iter(A.CI_JOBS)), 'Event': 'workflow_dispatch', 'RunId': 101,
            'StartedUtc': '2026-10-02T01:00:00Z', 'CompletedUtc': '2026-10-02T02:00:00Z'}]}
        self.Files = {}
        self.Files['qualification-descriptor.json'] = json.dumps({
            'format': 'GargantuanQualifiedScalePackage', 'version': 1, 'expected_clients': 32,
            'content_objects': 512, 'content_bytes': 273032,
            'phases': ['baseline', 'load', 'resident', 'evict', 'reload']}).encode()
        self.Files['fixture/content/content.manifest.json'] = b'{}'
        self.Files['fixture/content/regions/scale.instance.json'] = b'{}'
        self.Pins = {}
        for Role in ('Player', 'Server'):
            Records = []
            for Name in (f'Gargantuan{Role}.exe', 'content/content.manifest.json'):
                Data = (Role + Name).encode()
                self.Files[f'{Role}/{Name}'] = Data
                Records.append({'Path': Name, 'Bytes': len(Data), 'Sha256': A.Digest(Data)})
            Content = [{'Path': Record['Path'], 'Size': Record['Bytes'], 'Sha256': Record['Sha256'],
                        'Category': 'Runtime' if Record['Path'].endswith('.exe') else 'Project'}
                       for Record in Records]
            Table = json.dumps(Content, ensure_ascii=False, separators=(',', ':')).encode()
            Package = {'Format': 'GargantuanGamePackage', 'PackageFormatVersion': 2,
                       'RuntimeCompatibility': 1, 'ProjectId': 'a' * 32, 'DisplayName': 'Test',
                       'Configuration': 'Release', 'Revision': 1, 'UnsavedChanges': False,
                       'Player': f'Gargantuan{Role}.exe', 'Startup': {},
                       'ContentTableSha256': A.Digest(Table), 'Content': Content}
            Data = json.dumps(Package).encode()
            self.Files[f'{Role}/game.package.json'] = Data
            Records.insert(1, {'Path': 'game.package.json', 'Bytes': len(Data), 'Sha256': A.Digest(Data)})
            Data = json.dumps({'Format': 'GargantuanFarmDeployment', 'Version': 1,
                               'SourceCommit': SOURCE, 'Files': Records}).encode()
            self.Files[f'{Role}/deployment-sha256.json'] = Data
            for Record, Suffix in zip(Records, ('Sha256', 'PackageSha256', 'ContentManifestSha256')):
                self.Pins[Role + Suffix] = Record['Sha256']
            self.Pins[Role + 'DeploymentSha256'] = A.Digest(Data)

    def Package(self):
        File = self.Root / 'qualified.zip'
        with zipfile.ZipFile(File, 'w') as Zip:
            for Name, Data in self.Files.items():
                Zip.writestr(Name, Data)
        Hash = A.Digest(File.read_bytes())
        Metadata = {'id': 9, 'name': 'qualified-scale-' + SOURCE, 'expired': False,
                    'workflow_run': {'id': 101, 'head_sha': SOURCE}, 'created_at': '2026-10-02T01:59:00Z',
                    'digest': 'sha256:' + Hash, 'size_in_bytes': File.stat().st_size}
        Raw = json.dumps(Metadata).encode()
        (self.Root / 'metadata.json').write_bytes(Raw)
        return {'ArtifactMetadata': {'Path': 'metadata.json', 'Sha256': A.Digest(Raw)},
                'Archive': {'Path': 'qualified.zip', 'Sha256': Hash}}

    def test_actual_ci_package_complete_byte_join(self):
        Result = A.VerifyQualifiedPackage(self.Root, self.Package(), SOURCE, self.CI)
        self.assertEqual(Result['Pins'], self.Pins)
        self.assertEqual(Result['State'], 'MEASURED_PASS')
        self.assertEqual(Result['RuntimeInventories']['Server']['Files'], 3)
        self.assertEqual(Result['RuntimeInventories']['Server']['Layout'],
                         'DEPLOYMENT_ENVELOPE_REQUIRES_RUNTIME_PROJECTION')

    def test_deployment_cannot_extend_native_content_closure(self):
        # This is hash-consistent deployment evidence, but the native host
        # would reject its extra file. Inventory hashes alone are insufficient.
        Name = 'Server/extra.dll'
        self.Files[Name] = b'new deployment member'
        Manifest = json.loads(self.Files['Server/deployment-sha256.json'])
        Manifest['Files'].append({'Path': 'extra.dll', 'Bytes': len(self.Files[Name]),
                                  'Sha256': A.Digest(self.Files[Name])})
        self.Files['Server/deployment-sha256.json'] = json.dumps(Manifest).encode()
        with self.assertRaisesRegex(A.EvidenceError, 'outside native runtime closure'):
            A.VerifyQualifiedPackage(self.Root, self.Package(), SOURCE, self.CI)

    def test_native_table_and_exact_deployment_basis(self):
        Original = json.loads(self.Files['Server/game.package.json'])
        Deployment = json.loads(self.Files['Server/deployment-sha256.json'])['Files']
        for Mode in ('table', 'size', 'digest', 'missing', 'duplicate', 'order', 'redirect', 'bool-size'):
            with self.subTest(Mode=Mode):
                Package = json.loads(json.dumps(Original))
                if Mode == 'table':
                    Package['ContentTableSha256'] = '0' * 64
                elif Mode == 'size':
                    Package['Content'][0]['Size'] += 1
                elif Mode == 'digest':
                    Package['Content'][0]['Sha256'] = '0' * 64
                elif Mode == 'missing':
                    Package['Content'].pop()
                elif Mode == 'duplicate':
                    Package['Content'].append(dict(Package['Content'][0]))
                elif Mode == 'order':
                    Package['Content'].reverse()
                elif Mode == 'redirect':
                    Package['Content'][0]['Path'] = '../escape'
                else:
                    Package['Content'][0]['Size'] = True
                if Mode != 'table':
                    Package['ContentTableSha256'] = A.Digest(json.dumps(
                        Package['Content'], ensure_ascii=False, separators=(',', ':')).encode())
                with self.assertRaises(A.EvidenceError):
                    A.VerifyNativeRuntimeInventory(json.dumps(Package).encode(), Deployment)

    def test_native_inventory_rejects_nonobject_and_boolean_compatibility(self):
        Deployment = json.loads(self.Files['Server/deployment-sha256.json'])['Files']
        for Value in (None, [], ['Content']):
            with self.assertRaises(A.EvidenceError):
                A.VerifyNativeRuntimeInventory(json.dumps(Value).encode(), Deployment)
        Package = json.loads(self.Files['Server/game.package.json'])
        Package['RuntimeCompatibility'] = True
        with self.assertRaises(A.EvidenceError):
            A.VerifyNativeRuntimeInventory(json.dumps(Package).encode(), Deployment)

    def test_extra_omitted_corrupt_and_case_alias_members(self):
        for Mode in ('extra', 'missing', 'corrupt', 'alias', 'redirect'):
            with self.subTest(Mode=Mode):
                Original = dict(self.Files)
                if Mode == 'extra':
                    self.Files['Server/unknown.exe'] = b'extra'
                elif Mode == 'missing':
                    del self.Files['Server/GargantuanServer.exe']
                elif Mode == 'corrupt':
                    self.Files['Server/GargantuanServer.exe'] += b'bad'
                elif Mode == 'alias':
                    self.Files['Server/GARGANTUANSERVER.exe'] = b'alias'
                else:
                    self.Files['Server/../escape'] = b'escape'
                with self.assertRaises(A.EvidenceError):
                    A.VerifyQualifiedPackage(self.Root, self.Package(), SOURCE, self.CI)
                self.Files = Original

    def test_package_requires_verified_dispatch(self):
        Entry = self.Package()
        with self.assertRaises(A.EvidenceError):
            A.VerifyQualifiedPackage(self.Root, Entry, SOURCE, {'State': 'NOT_MEASURED'})
        self.CI['Jobs'][0]['Event'] = 'push'
        with self.assertRaises(A.EvidenceError):
            A.VerifyQualifiedPackage(self.Root, Entry, SOURCE, self.CI)

    def Provenance(self, Completed='2026-10-02T02:30:00Z'):
        Index = {'Format': 'GargantuanFoundation3LProvenance', 'Version': 1, 'QualifiedPackage': self.Package()}
        Inputs = {}
        for Role in ('Local', 'Node'):
            Directory = self.Root / Role
            Directory.mkdir(exist_ok=True)
            Manifest = {'SourceCommit': SOURCE, 'Provider': Role, 'RunId': Providers()[Role + 'RunId'], **self.Pins}
            Raw = json.dumps(Manifest).encode()
            (Directory / 'run-manifest.json').write_bytes(Raw)
            Inputs[Role + 'ClientEvidenceRoot'] = str(Directory)
            Stage = {'Format': 'GargantuanFarm32Campaign', 'Version': 1, 'Role': 'CLIENT',
                     'SourceCommit': SOURCE, 'RunId': Manifest['RunId'], 'ManifestSha256': A.Digest(Raw),
                     'CoordinatorRunId': '55555555-5555-4555-8555-555555555555',
                     'CreatedUtc': '2026-10-02T03:00:00Z' if Role == 'Local' else '2026-10-02T04:00:00Z'}
            Raw = json.dumps(Stage).encode()
            (self.Root / f'{Role}-stage.json').write_bytes(Raw)
            Index[Role + 'ClientStage'] = {'Path': f'{Role}-stage.json', 'Sha256': A.Digest(Raw)}
        Terminal = {'Format': 'GargantuanFarm32Terminal', 'Version': 1,
                    'RunId': Providers()['LocalRunId'], 'Action': 'host', 'ChildExitCode': 0,
                    'ChildTreeReaped': True, 'Outcome': 'COMPLETED', 'EndedUtc': '2026-10-02T03:30:00Z'}
        Raw = json.dumps(Terminal).encode()
        (self.Root / 'local-terminal.json').write_bytes(Raw)
        Index['LocalHostTerminal'] = {'Path': 'local-terminal.json', 'Sha256': A.Digest(Raw)}
        Coordinator = self.Root / 'local-coordinator.json'
        Coordinator.write_text(json.dumps({'RunId': '55555555-5555-4555-8555-555555555555', 'Success': True}))
        Inputs['LocalCoordinatorResultPath'] = str(Coordinator)
        Raw = json.dumps(Index).encode()
        IndexPath = self.Root / 'provenance.json'
        IndexPath.write_bytes(Raw)
        Physical = {**Four(), 'CompletedUtc': Completed, 'CompletionClockDomain': 'CONTROLLING_HOST_UTC'}
        return IndexPath, A.Digest(Raw), Inputs, Physical

    def test_sequence_uses_controller_utc_and_pinned_stage(self):
        Index, Hash, Inputs, Physical = self.Provenance()
        Result = A.VerifyProvenance(Index, Hash, Inputs, self.CI, Physical, SOURCE)
        self.assertEqual(Result['Sequence']['State'], 'MEASURED_PASS')
        Physical['CompletedUtc'] = '2026-10-02T03:00:01Z'
        with self.assertRaises(A.EvidenceError):
            A.VerifyProvenance(Index, Hash, Inputs, self.CI, Physical, SOURCE)
        del Physical['CompletedUtc']
        self.assertEqual(A.VerifyProvenance(Index, Hash, Inputs, self.CI, Physical, SOURCE)
                         ['Sequence']['State'], 'NOT_MEASURED')

    def test_package_must_match_replayed_deployment_not_claimed_commit_only(self):
        Index, Hash, Inputs, Physical = self.Provenance()
        PathValue = Path(Inputs['NodeClientEvidenceRoot']) / 'run-manifest.json'
        Manifest = json.loads(PathValue.read_bytes())
        Manifest['ServerSha256'] = '0' * 64
        PathValue.write_text(json.dumps(Manifest))
        with self.assertRaisesRegex(A.EvidenceError, 'differs from CI package'):
            A.VerifyProvenance(Index, Hash, Inputs, self.CI, Physical, SOURCE)

    def test_local_completion_before_node_is_required(self):
        Index, Hash, Inputs, Physical = self.Provenance()
        Value = json.loads(Index.read_bytes())
        del Value['LocalHostTerminal']
        Raw = json.dumps(Value).encode()
        Index.write_bytes(Raw)
        Result = A.VerifyProvenance(Index, A.Digest(Raw), Inputs, self.CI, Physical, SOURCE)
        self.assertEqual(Result['Sequence']['State'], 'NOT_MEASURED')
        for Change in ({'EndedUtc': '2026-10-02T04:00:01Z'}, {'ChildExitCode': 1},
                       {'ChildTreeReaped': False}, {'RunId': Providers()['NodeRunId']}):
            Index, Hash, Inputs, Physical = self.Provenance()
            Value = json.loads(Index.read_bytes())
            TerminalPath = self.Root / 'local-terminal.json'
            Terminal = json.loads(TerminalPath.read_bytes())
            Terminal.update(Change)
            Raw = json.dumps(Terminal).encode()
            TerminalPath.write_bytes(Raw)
            Value['LocalHostTerminal']['Sha256'] = A.Digest(Raw)
            Raw = json.dumps(Value).encode()
            Index.write_bytes(Raw)
            with self.assertRaises(A.EvidenceError):
                A.VerifyProvenance(Index, A.Digest(Raw), Inputs, self.CI, Physical, SOURCE)


if __name__ == '__main__':
    unittest.main()
