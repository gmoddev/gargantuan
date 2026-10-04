"""Mock transfer tests for fixed Farm32 two-host staging."""

import base64
import ctypes
import hashlib
import gzip
import json
from pathlib import Path, PureWindowsPath
import shutil
import subprocess
import sys
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import farm_outer_campaign as Outer  # noqa: E402
import farm_outer_endpoint as Endpoint  # noqa: E402
import farm_campaign_runner as Campaign  # noqa: E402
import farm_capture_campaign as Capture  # noqa: E402
from test_farm_ticket_staging import FarmTicketStagingTests  # noqa: E402
REAL_WORKER_SANDBOX = Outer.WorkerSandbox


class MockTransport:
    def __init__(self):
        self.ControlRules = []
        self.GameRules = []
        self.RetiredNodeRoots = []
        self.RetiredTlsRoots = []

    def AddControlFirewall(self, RunId):
        self.ControlRules.append(("add", RunId))

    def RemoveControlFirewall(self, RunId):
        self.ControlRules.append(("remove", RunId))

    def GameFirewall(self, Plan, Remove=False):
        self.GameRules.append(("remove" if Remove else "add", Plan))

    def WorkerDigest(self, File):
        if File.endswith("python.exe"):
            return "a" * 64
        if File.endswith("farm_outer_endpoint.py"):
            return Outer.Digest(Outer.SOURCE / "farm_outer_endpoint.py")
        raise AssertionError(File)

    def Remote(self, Action, *Arguments):
        if Action == "prepare":
            Endpoint.Prepare(Arguments[0])
        elif Action == "verify":
            Endpoint.Verify(*Arguments)
        elif Action == "prepare-capture":
            Endpoint.PrepareCapture(*Arguments)
        elif Action == "abort":
            Endpoint.Abort(*Arguments)
        elif Action == "retire-node-token":
            self.RetiredNodeRoots.append(Arguments[0])
        elif Action == "retire-node-tls":
            self.RetiredTlsRoots.append(Arguments[0])
        else:
            raise AssertionError(Action)

    def RemoteCopy(self, Source, Destination):
        shutil.copyfile(Source, Destination)

    def RemoteTree(self, Source, Destination):
        shutil.copytree(Source, Destination)

    def Fetch(self, Source, Destination, Timeout=10):
        shutil.copyfile(Source, Destination)


class OuterCampaignTests(unittest.TestCase):
    def test_runtime_projection_fixed_inputs_and_failure_stops_before_client(self):
        import tempfile
        with tempfile.TemporaryDirectory() as Directory:
            PowerShell = Path(Directory) / 'pwsh.exe'
            PowerShell.write_bytes(b'harmless mocked runtime')
            Spec = {'Roles': {
                'SERVER': {'PackageRoot': r'C:\Sandbox\ServerEnvelope', 'RunRegistryRoot': r'C:\Sandbox\ServerRegistry'},
                'CLIENT': {'PackageRoot': str(Path(Directory) / 'PlayerEnvelope'),
                           'RunRegistryRoot': str(Path(Directory) / 'ClientRegistry'),
                           'PowerShell': {'Path': str(PowerShell), 'Sha256': Outer.Digest(PowerShell)}}}}
            Manifest = {'RunId': '12345678-1234-4234-8234-123456789abc', 'SourceCommit': 'a' * 40,
                        'ServerDeploymentSha256': 'b' * 64, 'PlayerDeploymentSha256': 'c' * 64}
            Transport = mock.Mock()
            with mock.patch.object(Outer, 'Checked') as Checked:
                Outer.PrepareRuntimeProjections(Transport, r'C:\Sandbox\pwsh.exe', r'C:\Sandbox\run', Spec, Manifest)
                WorkerArgs = Transport.WorkerPowerShell.call_args
                self.assertEqual(WorkerArgs.kwargs, {'Timeout': 180})
                self.assertEqual(WorkerArgs.args[0:2], (r'C:\Sandbox\pwsh.exe',
                    r'C:\Sandbox\run\NewPhysicalGameSessionFarmProjection.ps1'))
                self.assertEqual(WorkerArgs.args[2:], ('-PackageRoot', r'C:\Sandbox\ServerEnvelope',
                    '-RunRegistryRoot', r'C:\Sandbox\ServerRegistry', '-RunId', Manifest['RunId'],
                    '-Role', 'Server', '-DeploymentSha256', 'b' * 64, '-SourceCommit', 'a' * 40))
                ClientArgs = Checked.call_args.args[0]
                self.assertEqual(ClientArgs[:5], [str(PowerShell), '-NoProfile', '-NonInteractive', '-File',
                    str(Outer.SOURCE.parent.parent / 'tests' / 'NewPhysicalGameSessionFarmProjection.ps1')])
                self.assertEqual(ClientArgs[5:], ['-PackageRoot', Spec['Roles']['CLIENT']['PackageRoot'],
                    '-RunRegistryRoot', Spec['Roles']['CLIENT']['RunRegistryRoot'], '-RunId', Manifest['RunId'],
                    '-Role', 'Clients', '-DeploymentSha256', 'c' * 64, '-SourceCommit', 'a' * 40])
                self.assertEqual(Checked.call_args.args[1], 180)
            Transport.WorkerPowerShell.side_effect = ValueError('partial source copy')
            with mock.patch.object(Outer, 'Checked') as Checked:
                with self.assertRaisesRegex(ValueError, 'partial source copy'):
                    Outer.PrepareRuntimeProjections(Transport, 'fixed-pwsh', 'fixed-run', Spec, Manifest)
                Checked.assert_not_called()

    def test_runtime_projection_precedes_preflight_and_ticket_sealing(self):
        import inspect
        Source = inspect.getsource(Outer.PrepareInputsOnce)
        self.assertIn('"NewPhysicalGameSessionFarmProjection.ps1",', Source)
        Position = Source.index('PrepareRuntimeProjections(')
        self.assertLess(Position, Source.index('PreflightScript ='))
        self.assertLess(Position, Source.index('Tickets.Seal('))

    def test_offline_budgets_enclose_export_without_extending_live_capture(self):
        self.assertEqual(Capture.ROLE_DEADLINE_SECONDS, 500)
        self.assertEqual(Capture.STOP_TIMEOUT_SECONDS, 80)
        self.assertEqual(Capture.CLIENT_AUTOSTOP_SECONDS, 630)
        self.assertEqual(Capture.FINALIZE_TIMEOUT_SECONDS, 1800)
        self.assertGreater(Campaign.CAPTURE_FINISH_SECONDS,
                           Capture.ROLE_DEADLINE_SECONDS + Capture.STOP_TIMEOUT_SECONDS +
                           10 + Capture.FINALIZE_TIMEOUT_SECONDS)
        self.assertEqual(Endpoint.MAX_SECONDS, Outer.OUTER_FINISH_SECONDS)
        self.assertGreater(Endpoint.MAX_SECONDS, Campaign.CAPTURE_FINISH_SECONDS)

    def test_capture_collection_uses_offline_transfer_budget_and_strict_limit(self):
        Remote = self.Fixture.Root / "capture-transfer"
        Remote.mkdir()
        Member = Remote / "farm32-worker-capture.etl"
        Member.write_bytes(b"small fixture")
        Index = {"RunId": self.Fixture.Identity["RunId"], "Role": "SERVER",
                 "Files": [{"Name": Member.name, "Bytes": Member.stat().st_size,
                            "Sha256": Outer.Digest(Member)}]}
        IndexFile = Remote / "capture-sha256.json"
        IndexFile.write_text(json.dumps(Index))
        Transport = MockTransport()
        with mock.patch.object(Transport, "Fetch", wraps=Transport.Fetch) as Fetch:
            Outer.FetchIndexed(Transport, str(Remote), self.Fixture.Root / "capture-copy",
                               IndexFile.name, self.Fixture.Identity["RunId"], "SERVER",
                               20, Outer.CAPTURE_MEMBER_MAX_BYTES)
            self.assertEqual(Fetch.call_args.kwargs["Timeout"], 1800)
        Index["Files"][0]["Bytes"] = 15 * 1024 ** 3
        IndexFile.write_text(json.dumps(Index))
        with mock.patch.object(Transport, "Fetch", wraps=Transport.Fetch) as Fetch:
            with self.assertRaisesRegex(ValueError, "member invalid"):
                Outer.FetchIndexed(Transport, str(Remote), self.Fixture.Root / "capped-copy",
                                   IndexFile.name, self.Fixture.Identity["RunId"], "SERVER",
                                   20, Outer.CAPTURE_MEMBER_MAX_BYTES)
            self.assertEqual(Fetch.call_count, 1)  # Index only; capped member is never copied.

    def test_staged_entrypoints_import_siblings_with_isolated_python(self):
        for Name, Arguments in (("farm_outer_endpoint.py", ["--help"]),
                                ("farm_campaign_runner.py", ["--help"]),
                                ("farm_capture_campaign.py", ["--help"]),
                                ("farm_lifecycle.py", [])):
            with self.subTest(Name=Name):
                Result = subprocess.run([sys.executable, "-I", "-B", str(ROOT / Name),
                                         *Arguments], capture_output=True, text=True,
                                        timeout=10, check=False)
                self.assertEqual(Result.returncode, 0, Result.stderr)

    def test_actual_role_stages_include_capture_import_closure(self):
        Roots = Outer.Stage(self.Fixture.Private, self.Fixture.SpecFile,
                            r"C:\Python312\python.exe", r"C:\Sandbox\farm_outer_endpoint.py",
                            MockTransport())
        for Role, Directory in Roots.items():
            with self.subTest(Role=Role):
                Stage = Path(Directory)
                Controller = Stage / "farm_capture_campaign.py"
                Dependency = Stage / "farm_capture_directions.py"
                self.assertEqual(Outer.Digest(ROOT / Dependency.name), Outer.Digest(Dependency))
                Endpoint.Verify(Stage, Stage / "stage-index.json")
                Result = subprocess.run([sys.executable, "-I", "-B", str(Controller), "--help"],
                                        cwd=self.Fixture.Root, capture_output=True, text=True, timeout=10)
                self.assertEqual(Result.returncode, 0, Result.stderr)
                self.assertIn("role", Result.stdout)
                # Removing the actual staged dependency must fail both pin
                # verification and execution, even with the checkout available.
                Dependency.rename(Dependency.with_suffix(".held"))
                with self.assertRaises(ValueError):
                    Endpoint.Verify(Stage, Stage / "stage-index.json")
                Missing = subprocess.run([sys.executable, "-I", "-B", str(Controller), "--help"],
                                         cwd=self.Fixture.Root, capture_output=True, text=True, timeout=10)
                self.assertNotEqual(Missing.returncode, 0)
                self.assertIn("farm_capture_directions", Missing.stderr)

    def setUp(self):
        self.Fixture = FarmTicketStagingTests(methodName="test_fresh_secrets_fixed_copy_plan_and_runner_schema")
        self.Fixture.setUp()
        self.addCleanup(self.Fixture.tearDown)
        Sandbox = mock.patch.object(Outer, "WorkerSandbox", side_effect=Outer.RemoteText)
        Sandbox.start()
        self.addCleanup(Sandbox.stop)
        self.Fixture.Spec["Host"]["HostIp"] = Outer.WORKER_IP
        self.Fixture.Spec["Host"]["Port"] = Outer.CONTROL_PORT
        self.Fixture.Seal()
        Outer.WriteNew(self.Fixture.Private / "runtime-pins.json", {
            "Format": "GargantuanFarm32RuntimePins", "Version": 1,
            "WorkerPythonPath": r"C:\Python312\python.exe", "WorkerPythonSha256": "a" * 64,
            "WorkerHelperPath": r"C:\Sandbox\farm_outer_endpoint.py",
            "WorkerHelperSha256": Outer.Digest(Outer.SOURCE / "farm_outer_endpoint.py"),
            "ClientPythonPath": sys.executable,
            "ClientPythonSha256": Outer.Digest(sys.executable),
        })
        for Role in ("SERVER", "CLIENT"):
            Path(self.Fixture.Spec["Roles"][Role]["StageRoot"]).rmdir()
            Path(self.Fixture.Spec["Roles"][Role]["CaptureRoot"]).rmdir()

    def test_stage_provisions_missing_capture_roots_with_real_consumer_validation(self):
        for Role in ("SERVER", "CLIENT"):
            Config = self.Fixture.Private / Role / "capture-config.json"
            with self.assertRaises(FileNotFoundError):
                Capture.Configured(Config)
        # ACL confinement may run its fixed OS utility, but no capture or native
        # workload may start while staging the real consumer configs.
        with mock.patch.object(Capture, "RunRole", side_effect=AssertionError("capture started")), \
                mock.patch.object(Capture.FarmCaptureController, "Start", side_effect=AssertionError("capture started")):
            Roots = Outer.Stage(self.Fixture.Private, self.Fixture.SpecFile,
                                r"C:\Python312\python.exe", r"C:\Sandbox\farm_outer_endpoint.py",
                                MockTransport())
        for Role, Directory in Roots.items():
            Config = Capture.Configured(Path(Directory) / "capture-config.json")
            self.assertTrue(Config["CaptureRoot"].is_dir())
            self.assertFalse(Config["CaptureDirectory"].exists())
            self.assertFalse(Config["RoleEvidenceRoot"].exists())
            # Run the actual isolated staged CLI, not a mocked Configured call.
            Result = subprocess.run([sys.executable, "-I", "-B", str(Path(Directory) /
                                    "farm_capture_campaign.py"), "validate",
                                     str(Path(Directory) / "capture-config.json")],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(Result.returncode, 0, Result.stderr)
            self.assertIn("CONFIGURED_ONLY", Result.stdout)
            self.assertFalse(Config["CaptureDirectory"].exists())
            with self.assertRaisesRegex(ValueError, "already exists"):
                Endpoint.PrepareCapture(Directory, Path(Directory) / "stage-index.json")

    def test_capture_provisioning_rejects_missing_parent_and_protected_overlap(self):
        # Both are invalid sealed inputs; reject before any capture root exists.
        for Variant in ("missing-parent", "overlap", "private-overlap"):
            with self.subTest(Variant=Variant):
                Role = self.Fixture.Spec["Roles"]["SERVER"]
                OriginalRoot = Path(Role["CaptureRoot"])
                CaptureFile = self.Fixture.Private / "SERVER" / "capture-config.json"
                CaptureConfig = json.loads(CaptureFile.read_text())
                CaptureConfig["CaptureRoot"] = (str(self.Fixture.Root / "absent-parent" / "capture")
                    if Variant == "missing-parent" else Role["PackageRoot"] if Variant == "overlap"
                    else str(self.Fixture.Private / "capture"))
                Role["CaptureRoot"] = CaptureConfig["CaptureRoot"]
                self.Fixture.SpecFile.write_text(json.dumps(self.Fixture.Spec))
                CaptureFile.write_text(json.dumps(CaptureConfig))
                TicketFile = self.Fixture.Private / "SERVER" / "ticket.json"
                Ticket = json.loads(TicketFile.read_text())
                Ticket["CaptureConfigSha256"] = Outer.Digest(CaptureFile)
                TicketFile.write_text(json.dumps(Ticket))
                PlanFile = self.Fixture.Private / "copy-plan.json"
                Plan = json.loads(PlanFile.read_text())
                for Row in Plan["Files"]:
                    if Row["Source"] in (str(CaptureFile), str(TicketFile)):
                        Row["Sha256"] = Outer.Digest(Row["Source"])
                PlanFile.write_text(json.dumps(Plan))
                with self.assertRaises(ValueError):
                    Outer.Stage(self.Fixture.Private, self.Fixture.SpecFile,
                                r"C:\Python312\python.exe", r"C:\Sandbox\farm_outer_endpoint.py",
                                MockTransport())
                self.assertFalse(OriginalRoot.exists())
                # The failed Stage consumes its roots; use a fresh fixture for
                # the next separately sealed negative, never reuse that Stage.
                if Variant != "private-overlap":
                    self.Fixture.tearDown()
                    self.setUp()

    def test_capture_preparation_rejects_changed_role_identity_and_stage_path(self):
        Roots = Outer.Stage(self.Fixture.Private, self.Fixture.SpecFile,
                            r"C:\Python312\python.exe", r"C:\Sandbox\farm_outer_endpoint.py",
                            MockTransport())
        Stage = Path(Roots["CLIENT"])
        CaptureRoot = Path(self.Fixture.Spec["Roles"]["CLIENT"]["CaptureRoot"])
        CaptureRoot.rmdir()
        ConfigFile = Stage / "capture-config.json"
        TicketFile = Stage / "ticket.json"
        IndexFile = Stage / "stage-index.json"
        Original = {File: File.read_bytes() for File in (ConfigFile, TicketFile, IndexFile)}
        for Variant in ("role", "run", "config-path", "pin", "index-path", "unlisted-import"):
            with self.subTest(Variant=Variant):
                for File, Bytes in Original.items():
                    File.write_bytes(Bytes)
                Config = json.loads(ConfigFile.read_text())
                Ticket = json.loads(TicketFile.read_text())
                if Variant == "role":
                    Config["Role"] = "SERVER"
                elif Variant == "run":
                    Config["RunId"] = self.Fixture.Identity["CoordinatorRunId"]
                elif Variant == "config-path":
                    Ticket["CaptureConfigPath"] = str(self.Fixture.Private / "CLIENT" / "capture-config.json")
                elif Variant == "pin":
                    Config["DumpcapSha256"] = "0" * 64
                ConfigFile.write_text(json.dumps(Config))
                Ticket["CaptureConfigSha256"] = Outer.Digest(ConfigFile)
                TicketFile.write_text(json.dumps(Ticket))
                Index = json.loads(IndexFile.read_text())
                for Entry in Index["Files"]:
                    if Entry["Name"] in (ConfigFile.name, TicketFile.name):
                        Entry["Sha256"] = Outer.Digest(Stage / Entry["Name"])
                if Variant == "unlisted-import":
                    Index["Files"] = [Entry for Entry in Index["Files"]
                                      if Entry["Name"] != "farm_capture_directions.py"]
                IndexFile.write_text(json.dumps(Index))
                with self.assertRaises(ValueError):
                    Endpoint.PrepareCapture(Stage, IndexFile if Variant != "index-path" else
                                            self.Fixture.Private / "client-stage-index.json")
                self.assertFalse(CaptureRoot.exists())

    def test_capture_preparation_rejects_real_reparse_root_before_any_child(self):
        Roots = Outer.Stage(self.Fixture.Private, self.Fixture.SpecFile,
                            r"C:\Python312\python.exe", r"C:\Sandbox\farm_outer_endpoint.py",
                            MockTransport())
        Stage = Path(Roots["CLIENT"])
        CaptureRoot = Path(self.Fixture.Spec["Roles"]["CLIENT"]["CaptureRoot"])
        CaptureRoot.rmdir()
        Target = self.Fixture.Root / "redirect-target"
        Target.mkdir()
        if Endpoint.os.name == "nt":
            Result = subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(CaptureRoot), str(Target)],
                                    capture_output=True, text=True, timeout=10,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
            self.assertEqual(Result.returncode, 0, Result.stderr)
        else:
            CaptureRoot.symlink_to(Target, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "reparse"):
            Endpoint.PrepareCapture(Stage, Stage / "stage-index.json")
        self.assertEqual(list(Target.iterdir()), [])
        CaptureRoot.rmdir() if Endpoint.os.name == "nt" else CaptureRoot.unlink()

    def test_recovery_manifest_profile_is_explicit_and_bounded(self):
        self.assertEqual((18001, 19001,
                          ["-ScaleWorkload", "-ClientFrames", "18001",
                           "-ServerTicks", "19001"]), Outer.WorkloadProfile(False))
        self.assertEqual((25201, 26201,
                          ["-ScaleWorkload", "-ClientFrames", "25201",
                           "-ServerTicks", "26201", "-RecoveryWorkload"]),
                         Outer.WorkloadProfile(True))
        for Recovery, RuntimeUs in ((False, 300000000), (True, 420000000)):
            Frames, Ticks, _ = Outer.WorkloadProfile(Recovery)
            # Independent runtime contract: the first iteration has no prior
            # cadence sleep. Both ceilings must also satisfy endpoint bounds.
            self.assertGreaterEqual((Frames - 1) * 16667, RuntimeUs)
            self.assertLessEqual(Frames, 36000)
            self.assertGreater(Ticks, Frames + 600)
            self.assertLessEqual(Ticks, 48000)
        for Invalid in (None, 0, 1, "true"):
            with self.assertRaisesRegex(ValueError, "invalid recovery workload"):
                Outer.WorkloadProfile(Invalid)

    def test_stage_rebases_host_tickets_and_checks_each_destination(self):
        Roots = Outer.Stage(self.Fixture.Private, self.Fixture.SpecFile,
                            r"C:\Python312\python.exe", r"C:\Sandbox\farm_outer_endpoint.py",
                            MockTransport())
        for Role in Roots:
            Root = Path(Roots[Role])
            Endpoint.Verify(Root, Root / "stage-index.json")
            self.assertTrue((Root / ".agent-coordinator" / "agent_coordinator" / "control.py").is_file())
            Campaign.VerifyRole(json.loads((Root / "ticket.json").read_text()), Role)
        Host = json.loads((Path(Roots["SERVER"]) / "host-config.json").read_text())
        self.assertEqual(str(Path(Roots["SERVER"]) / "host-client-ticket.json"),
                         Host["Roles"]["CLIENT"]["TicketPath"])
        self.assertEqual(Outer.WORKER_IP, Host["HostIp"])
        for Role in ("SERVER", "CLIENT"):
            File = (Path(Roots["SERVER"]) / ("ticket.json" if Role == "SERVER" else
                                                  "host-client-ticket.json"))
            Campaign.VerifyHostTicket(json.loads(File.read_text()), Role, Host)
        Upstream = Path(Roots["SERVER"]) / ".agent-coordinator" / "agent_coordinator" / "control.py"
        Upstream.write_bytes(Upstream.read_bytes() + b"\n# tampered mock\n")
        with self.assertRaises(ValueError):
            Endpoint.Verify(Roots["SERVER"], Path(Roots["SERVER"]) / "stage-index.json")
        with self.assertRaises(ValueError):
            Outer.Stage(self.Fixture.Private, self.Fixture.SpecFile,
                        r"C:\Python312\python.exe", r"C:\Sandbox\farm_outer_endpoint.py",
                        MockTransport())

    def test_rejects_non_worker_host_or_modified_source(self):
        self.Fixture.Spec["Host"]["HostIp"] = "192.168.0.68"
        self.Fixture.SpecFile.write_text(json.dumps(self.Fixture.Spec), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "worker-host profile mismatch"):
            Outer.Stage(self.Fixture.Private, self.Fixture.SpecFile,
                        r"C:\Python312\python.exe", r"C:\Sandbox\farm_outer_endpoint.py",
                        MockTransport())
        self.Fixture.Spec["Host"]["HostIp"] = Outer.WORKER_IP
        self.Fixture.SpecFile.write_text(json.dumps(self.Fixture.Spec), encoding="utf-8")
        Plan = json.loads((self.Fixture.Private / "copy-plan.json").read_text())
        Plan["Files"][0]["Sha256"] = hashlib.sha256(b"wrong").hexdigest()
        (self.Fixture.Private / "copy-plan.json").write_text(json.dumps(Plan), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "fixed copy plan member changed"):
            Outer.Stage(self.Fixture.Private, self.Fixture.SpecFile,
                        r"C:\Python312\python.exe", r"C:\Sandbox\farm_outer_endpoint.py",
                        MockTransport())

    def test_preflight_ttl_is_rechecked_at_launch_boundary(self):
        for Role in ("SERVER", "CLIENT"):
            Source = Path(self.Fixture.Spec["Roles"][Role]["PreflightSource"])
            Target = self.Fixture.Private / ("server-preflight.json" if Role == "SERVER" else
                                              "client-preflight.json")
            shutil.copyfile(Source, Target)
        Outer.RequireFreshPreflights(self.Fixture.Private, self.Fixture.Identity)
        Target = self.Fixture.Private / "client-preflight.json"
        Row = json.loads(Target.read_text())
        Row["ObservedUtc"] = "2020-01-01T00:00:00Z"
        Target.write_text(json.dumps(Row), encoding="utf-8")
        with self.assertRaises(ValueError):
            Outer.RequireFreshPreflights(self.Fixture.Private, self.Fixture.Identity)

    def test_control_firewall_is_run_bound_and_removed_after_probe_failure(self):
        self.PrepareGameFirewallFixture()
        Transport = MockTransport()
        for Role in ("SERVER", "CLIENT"):
            Source = Path(self.Fixture.Spec["Roles"][Role]["PreflightSource"])
            Target = self.Fixture.Private / ("server-preflight.json" if Role == "SERVER" else
                                             "client-preflight.json")
            shutil.copyfile(Source, Target)
        Outer.Stage(self.Fixture.Private, self.Fixture.SpecFile,
                    r"C:\Python312\python.exe", r"C:\Sandbox\farm_outer_endpoint.py",
                    Transport)
        with mock.patch.object(Outer, "ControlProbe", side_effect=TimeoutError("mock blocked")):
            with self.assertRaisesRegex(TimeoutError, "mock blocked"):
                Outer.Launch(self.Fixture.Private, self.Fixture.SpecFile,
                             r"C:\Python312\python.exe", r"C:\Sandbox\farm_outer_endpoint.py",
                             Transport)
        self.assertEqual([("add", self.Fixture.Identity["RunId"]),
                          ("remove", self.Fixture.Identity["RunId"])], Transport.ControlRules)
        self.assertEqual(["add", "remove"], [Row[0] for Row in Transport.GameRules])
        self.assertEqual(Transport.GameRules[0][1], Transport.GameRules[1][1])

    def PrepareGameFirewallFixture(self):
        Manifest = json.loads(self.Fixture.Manifest.read_text())
        Manifest.update(Endpoint='10.253.3.2:39450', ServerSha256='b' * 64,
                        ServerDeploymentSha256='c' * 64)
        self.Fixture.Manifest.write_text(json.dumps(Manifest), encoding='utf-8')
        ManifestHash = Outer.Digest(self.Fixture.Manifest)
        HostPath = self.Fixture.Private / 'host-config.json'
        Host = json.loads(HostPath.read_text())
        Host['ManifestSha256'] = ManifestHash
        for Role in ('SERVER', 'CLIENT'):
            PreflightPath = Path(self.Fixture.Spec['Roles'][Role]['PreflightSource'])
            Preflight = json.loads(PreflightPath.read_text())
            Label = 'Server' if Role == 'SERVER' else 'Clients'
            Preflight.update(ManifestSha256=ManifestHash, SourceCommit='a' * 40,
                             Endpoint=Manifest['Endpoint'], GameInterfaceAddress='10.253.3.2',
                             RuntimeProjectionRoot=str(PureWindowsPath(
                                 self.Fixture.Spec['Roles'][Role]['RunRegistryRoot']) /
                                 (self.Fixture.Identity['RunId'] + '.' + Label + '.runtime')),
                             RuntimeProjectionReceiptSha256='d' * 64)
            PreflightPath.write_text(json.dumps(Preflight), encoding='utf-8')
            shutil.copyfile(PreflightPath, self.Fixture.Private / (Role.lower() + '-preflight.json'))
            FarmPath = self.Fixture.Private / Role / 'farm-config.json'
            Farm = json.loads(FarmPath.read_text())
            Farm['ManifestSHA256'] = ManifestHash
            FarmPath.write_text(json.dumps(Farm), encoding='utf-8')
            TicketPath = self.Fixture.Private / Role / 'ticket.json'
            Ticket = json.loads(TicketPath.read_text())
            Ticket.update(ManifestSha256=ManifestHash, PreflightSha256=Outer.Digest(PreflightPath),
                          FarmConfigSha256=Outer.Digest(FarmPath))
            TicketPath.write_text(json.dumps(Ticket), encoding='utf-8')
            Host['Roles'][Role]['TicketSha256'] = Outer.Digest(TicketPath)
        HostPath.write_text(json.dumps(Host), encoding='utf-8')
        PlanPath = self.Fixture.Private / 'copy-plan.json'
        Plan = json.loads(PlanPath.read_text())
        for Row in Plan['Files']:
            Row['Sha256'] = Outer.Digest(Row['Source'])
        PlanPath.write_text(json.dumps(Plan), encoding='utf-8')

    def test_game_firewall_plan_joins_sealed_native_projection(self):
        self.PrepareGameFirewallFixture()
        Plan = Outer.GameFirewallPlan(self.Fixture.Private, self.Fixture.Identity)
        ExpectedRoot = str(PureWindowsPath(self.Fixture.Spec['Roles']['SERVER']['RunRegistryRoot']) /
                           (self.Fixture.Identity['RunId'] + '.Server.runtime'))
        self.assertEqual(Plan['Program'], str(PureWindowsPath(ExpectedRoot) / 'GargantuanServer.exe'))
        self.assertEqual(Plan['ProgramSha256'], 'b' * 64)
        self.assertEqual(Plan['RuntimeReceiptSha256'], 'd' * 64)
        self.assertEqual(Plan['Name'], 'Codex-Gargantuan-Farm32-Game-' + self.Fixture.Identity['RunId'])
        self.assertNotEqual(Plan['OwnerId'], self.Fixture.Identity['RunId'])

    def test_game_firewall_denies_changed_projection_manifest_or_farm_pins(self):
        self.PrepareGameFirewallFixture()
        Cases = [('server-preflight.json', 'RuntimeProjectionRoot', r'C:\Sandbox\wrong'),
                 ('server-preflight.json', 'Endpoint', '127.0.0.1:39450'),
                 ('SERVER/farm-config.json', 'RunRegistryRoot', r'C:\Sandbox\wrong'),
                 ('SERVER/ticket.json', 'ManifestSha256', '0' * 64)]
        for Name, Key, Value in Cases:
            with self.subTest(Name=Name, Key=Key):
                File = self.Fixture.Private / Name
                Original = File.read_bytes()
                Row = json.loads(Original)
                Row[Key] = Value
                File.write_text(json.dumps(Row), encoding='utf-8')
                try:
                    with self.assertRaisesRegex(ValueError, 'sealed projection binding'):
                        Outer.GameFirewallPlan(self.Fixture.Private, self.Fixture.Identity)
                finally:
                    File.write_bytes(Original)

    def test_game_firewall_unsafe_or_nonprojected_program_never_invokes_worker(self):
        self.PrepareGameFirewallFixture()
        Plan = Outer.GameFirewallPlan(self.Fixture.Private, self.Fixture.Identity)
        Transport = Outer.Transport(r'C:\Python312\python.exe', r'C:\Sandbox\farm_outer_endpoint.py')
        for Key, Value in (('RunId', 'not-a-run'), ('Name', 'unrelated'),
                           ('Program', r'C:\Sandbox\original\GargantuanServer.exe'),
                           ('ProgramSha256', 'bad'), ('Program', "C:\\Sandbox\\bad'path.exe")):
            with self.subTest(Key=Key, Value=Value), mock.patch.object(Outer, 'Checked') as Checked:
                Changed = dict(Plan, **{Key: Value})
                with self.assertRaises(ValueError):
                    Transport.GameFirewall(Changed)
                Checked.assert_not_called()

    def test_game_firewall_transport_uses_hash_bound_memory_payload_and_bounded_command(self):
        self.PrepareGameFirewallFixture()
        Plan = Outer.GameFirewallPlan(self.Fixture.Private, self.Fixture.Identity)
        Transport = Outer.Transport(r'C:\Python312\python.exe', r'C:\Sandbox\farm_outer_endpoint.py')
        with mock.patch.object(Outer.subprocess, 'run') as Run:
            Transport.GameFirewall(Plan)
        Arguments = Run.call_args.args[0]
        self.assertLessEqual(len(' '.join(Arguments)), 7600)
        Payload = Run.call_args.kwargs['input']
        Packed = base64.b64decode(Payload)
        self.assertLessEqual(len(Packed), 4096)
        Script = gzip.decompress(Packed)
        self.assertEqual(Script.decode('utf-8'), Outer.GameFirewallScript(Plan))
        Bootstrap = base64.b64decode(Arguments[-1]).decode('utf-16le')
        self.assertIn(hashlib.sha256(Script).hexdigest(), Bootstrap)
        self.assertIn('16384', Bootstrap)
        self.assertIn('[Console]::In.ReadToEnd()', Bootstrap)
        self.assertEqual(Run.call_args.kwargs['timeout'], 180)
        for Bad in ('', 'x' * 16385):
            with self.assertRaisesRegex(ValueError, 'size exceeds bound'):
                Outer.GameFirewallPayload(Bad)
        # The actual PowerShell decoder must refuse altered bytes before the
        # script block runs, including a small compressed decompression bomb.
        PowerShell = shutil.which('pwsh') or shutil.which('pwsh.exe')
        self.assertIsNotNone(PowerShell)
        Encoded, Good = Outer.GameFirewallPayload("Write-Output 'VERIFIED_SCRIPT_ONLY'")
        for Input, Success in ((Good, True),
                               (base64.b64encode(gzip.compress(b"Write-Output 'UNTRUSTED_EXECUTION'")).decode(), False),
                               (base64.b64encode(gzip.compress(b'x' * 16385)).decode(), False),
                               (base64.b64encode(b'x' * 4097).decode(), False)):
            with self.subTest(Success=Success, Bytes=len(Input)):
                Result = subprocess.run([PowerShell, '-NoProfile', '-NonInteractive', '-EncodedCommand', Encoded],
                                        input=Input, capture_output=True, text=True, timeout=30,
                                        creationflags=Outer.Hidden())
                self.assertEqual(Result.returncode == 0, Success, Result.stderr)
                self.assertNotIn('UNTRUSTED_EXECUTION', Result.stdout)
                if Success:
                    self.assertIn('VERIFIED_SCRIPT_ONLY', Result.stdout)

    def test_game_firewall_cleanup_on_late_preflight_failure_and_success(self):
        self.PrepareGameFirewallFixture()
        Outer.Stage(self.Fixture.Private, self.Fixture.SpecFile,
                    r'C:\Python312\python.exe', r'C:\Sandbox\farm_outer_endpoint.py', MockTransport())
        for Failure in (None, ValueError('late preflight changed')):
            with self.subTest(Failure=Failure):
                Transport = MockTransport()
                Ownership = self.Fixture.Private / 'game-firewall-ownership.json'
                if Ownership.exists():
                    Ownership.unlink()  # Owned pure fixture only, not a campaign retry.
                def Prepared(*Arguments):
                    if Failure:
                        raise Failure
                    return 0
                with mock.patch.object(Outer, 'LaunchPrepared', side_effect=Prepared):
                    if Failure:
                        with self.assertRaisesRegex(ValueError, 'late preflight changed'):
                            Outer.Launch(self.Fixture.Private, self.Fixture.SpecFile,
                                         r'C:\Python312\python.exe', r'C:\Sandbox\farm_outer_endpoint.py', Transport)
                    else:
                        self.assertEqual(Outer.Launch(self.Fixture.Private, self.Fixture.SpecFile,
                                                     r'C:\Python312\python.exe', r'C:\Sandbox\farm_outer_endpoint.py', Transport), 0)
                self.assertEqual(['add', 'remove'], [Row[0] for Row in Transport.GameRules])
                self.assertEqual(Transport.GameRules[0][1], Transport.GameRules[1][1])
                self.assertEqual(json.loads(Ownership.read_text()), Transport.GameRules[0][1])
                self.assertEqual(['add', 'remove'], [Row[0] for Row in Transport.ControlRules])

    def test_game_firewall_failed_add_still_reconciles_only_its_ownership(self):
        self.PrepareGameFirewallFixture()
        Transport = MockTransport()
        Outer.Stage(self.Fixture.Private, self.Fixture.SpecFile,
                    r'C:\Python312\python.exe', r'C:\Sandbox\farm_outer_endpoint.py', Transport)
        Original = Transport.GameFirewall
        def FailAdd(Plan, Remove=False):
            Original(Plan, Remove)
            if not Remove:
                raise RuntimeError('denied stale or blocked game rule')
        with mock.patch.object(Transport, 'GameFirewall', side_effect=FailAdd), \
                mock.patch.object(Outer, 'LaunchPrepared') as LaunchPrepared:
            with self.assertRaisesRegex(RuntimeError, 'denied stale or blocked'):
                Outer.Launch(self.Fixture.Private, self.Fixture.SpecFile,
                             r'C:\Python312\python.exe', r'C:\Sandbox\farm_outer_endpoint.py', Transport)
            LaunchPrepared.assert_not_called()
        self.assertEqual(['add', 'remove'], [Row[0] for Row in Transport.GameRules])
        self.assertEqual(Transport.GameRules[0][1], Transport.GameRules[1][1])

    def test_game_firewall_stale_ownership_preserves_file_and_original_failure(self):
        self.PrepareGameFirewallFixture()
        Transport = MockTransport()
        Outer.Stage(self.Fixture.Private, self.Fixture.SpecFile,
                    r'C:\Python312\python.exe', r'C:\Sandbox\farm_outer_endpoint.py', Transport)
        Ownership = self.Fixture.Private / 'game-firewall-ownership.json'
        Original = b'prior owned receipt must remain unchanged'
        Ownership.write_bytes(Original)
        def RefuseDifferentOwner(Plan, Remove=False):
            self.assertTrue(Remove)
            raise RuntimeError('different ownership marker; no deletion')
        with mock.patch.object(Transport, 'GameFirewall', side_effect=RefuseDifferentOwner) as GameFirewall, \
                mock.patch.object(Outer, 'LaunchPrepared') as Prepared:
            with self.assertRaisesRegex(RuntimeError, 'no deletion') as Raised:
                Outer.Launch(self.Fixture.Private, self.Fixture.SpecFile,
                             r'C:\Python312\python.exe', r'C:\Sandbox\farm_outer_endpoint.py', Transport)
            self.assertIsInstance(Raised.exception.__context__, FileExistsError)
            self.assertEqual(GameFirewall.call_count, 1)
            Prepared.assert_not_called()
        self.assertEqual(Ownership.read_bytes(), Original)
        self.assertEqual(['add', 'remove'], [Row[0] for Row in Transport.ControlRules])

    def test_game_firewall_cleanup_failure_cannot_report_success(self):
        self.PrepareGameFirewallFixture()
        Transport = MockTransport()
        Outer.Stage(self.Fixture.Private, self.Fixture.SpecFile,
                    r'C:\Python312\python.exe', r'C:\Sandbox\farm_outer_endpoint.py', Transport)
        def FailRemove(Plan, Remove=False):
            if Remove:
                raise RuntimeError('owned rule cleanup unproven')
        for Failure in (None, ValueError('original native launch failure')):
            with self.subTest(Failure=Failure):
                Ownership = self.Fixture.Private / 'game-firewall-ownership.json'
                if Ownership.exists():
                    Ownership.unlink()  # Owned pure fixture only.
                Transport.ControlRules.clear()
                with mock.patch.object(Transport, 'GameFirewall', side_effect=FailRemove), \
                        mock.patch.object(Outer, 'LaunchPrepared', return_value=0, side_effect=Failure):
                    with self.assertRaisesRegex(RuntimeError, 'cleanup unproven') as Raised:
                        Outer.Launch(self.Fixture.Private, self.Fixture.SpecFile,
                                     r'C:\Python312\python.exe', r'C:\Sandbox\farm_outer_endpoint.py', Transport)
                if Failure:
                    self.assertIs(Raised.exception.__context__, Failure)
                self.assertEqual(['add', 'remove'], [Row[0] for Row in Transport.ControlRules])

    def RunMockGameFirewall(self, State):
        """Execute generated PowerShell with every network/security cmdlet mocked."""
        if not (self.Fixture.Private / 'server-preflight.json').exists():
            self.PrepareGameFirewallFixture()
        Plan = Outer.GameFirewallPlan(self.Fixture.Private, self.Fixture.Identity)
        Supervisor = self.Fixture.Root / ('mock-supervisor-' + State + '.ps1')
        Supervisor.write_text('''
function Assert-DeploymentManifest { }
function Assert-ProjectionPlainPath { }
function Read-ProjectionJson { param($Path)
    return @{ RunId=$global:FixedPlan.RunId; SourceCommit=$global:FixedPlan.SourceCommit;
        Endpoint='10.253.3.2:39450'; ServerSha256=$global:FixedPlan.ProgramSha256;
        ServerDeploymentSha256=$global:FixedPlan.DeploymentSha256 }
}
function Get-NativeRuntimePlan { param($LocalRoot,$DeploymentSha256,$SourceCommit,$LocalRole)
    if ($LocalRole -cne 'Server' -or $LocalRoot -cne $global:FixedPlan.PackageRoot -or
        $DeploymentSha256 -cne $global:FixedPlan.DeploymentSha256 -or $SourceCommit -cne $global:FixedPlan.SourceCommit) {
        throw 'mock native plan was not source bound'
    }
    return @{ Binary='GargantuanServer.exe' }
}
function Get-RuntimeProjectionPath { }
function Assert-RuntimeProjection { }
function Assert-PreparedRuntimeProjection { param($LocalRoot,$Registry,$LocalRunId,$LocalRole,$Plan)
    if ($LocalRoot -cne $global:FixedPlan.PackageRoot -or $Registry -cne $global:FixedPlan.RunRegistryRoot -or
        $LocalRunId -cne $global:FixedPlan.RunId -or $LocalRole -cne 'Server') { throw 'mock projection identity differs' }
    return $global:FixedPlan.RuntimeRoot
}
''', encoding='utf-8')
        Plan['SupervisorPath'] = str(Supervisor)
        if State == 'DuplicateValidator':
            Supervisor.write_text(Supervisor.read_text().replace(
                'function Assert-RuntimeProjection { }', 'function Get-NativeRuntimePlan { }'), encoding='utf-8')
        Plan['SupervisorSha256'] = Outer.Digest(Supervisor)
        Add = Outer.GameFirewallScript(Plan)
        Remove = Outer.GameFirewallScript(Plan, Remove=True)
        Prefix = ('$global:FixedPlan=' + "('" + json.dumps(Plan).replace("'", "''") + "'|ConvertFrom-Json);\n" +
                  "$global:State='" + State + "';$global:Rule=$null;$global:Created=0;$global:Removed=0;\n")
        Mocks = r'''
function Get-FileHash { param($LiteralPath,$Algorithm)
    $Hash = switch ($LiteralPath) {
        $global:FixedPlan.SupervisorPath { $global:FixedPlan.SupervisorSha256 }
        $global:FixedPlan.ManifestPath { $global:FixedPlan.ManifestSha256 }
        ($global:FixedPlan.RuntimeRoot + '.json') { $global:FixedPlan.RuntimeReceiptSha256 }
        $global:FixedPlan.Program { if ($global:State -eq 'HashChanged') {'0'*64} else {$global:FixedPlan.ProgramSha256} }
        default { throw 'unrecognized mock file hash' }
    }
    [pscustomobject]@{ Hash=$Hash }
}
function Get-NetIPAddress { param($IPAddress,$AddressFamily)
    [pscustomobject]@{ InterfaceIndex=19; InterfaceAlias='Ethernet 4' }
}
function Get-NetConnectionProfile { param($InterfaceIndex)
    [pscustomobject]@{ NetworkCategory=$(if ($global:State -eq 'PublicInterface') {'Public'} else {'Private'}) }
}
function Get-NetFirewallRule { [CmdletBinding()] param($PolicyStore,$Name,$Enabled,$Direction,$Action)
    if ($Name) {
        if ($global:State -eq 'Stale') { return [pscustomobject]@{ Name=$Name; Description='unrelated owner' } }
        if ($global:State -eq 'MissingEffective' -and $PolicyStore -eq 'ActiveStore') { return }
        return $global:Rule
    }
    if ($Action -eq 'Block' -and $global:State -in @('BlockAny','BlockRange','UnrelatedTcp','UnrelatedProgram')) {
        return [pscustomobject]@{ Profile='Private';
            App=[pscustomobject]@{ Program=$(if($global:State -eq 'UnrelatedProgram') {'C:\unrelated.exe'} else {'Any'}) };
            Port=[pscustomobject]@{ Protocol=$(if($global:State -eq 'UnrelatedTcp') {'TCP'} else {'UDP'});
                LocalPort=$(if($global:State -eq 'BlockRange') {'39000-40000'} else {'Any'}) } }
    }
}
function Get-NetFirewallApplicationFilter { param([Parameter(ValueFromPipeline=$true)]$Rule) process { $Rule.App } }
function Get-NetFirewallPortFilter { param([Parameter(ValueFromPipeline=$true)]$Rule) process { $Rule.Port } }
function Get-NetFirewallAddressFilter { param([Parameter(ValueFromPipeline=$true)]$Rule) process { $Rule.Address } }
function Get-NetFirewallInterfaceFilter { param([Parameter(ValueFromPipeline=$true)]$Rule) process { $Rule.Interface } }
function Get-NetFirewallInterfaceTypeFilter { param([Parameter(ValueFromPipeline=$true)]$Rule) process { $Rule.InterfaceType } }
function Get-NetFirewallServiceFilter { param([Parameter(ValueFromPipeline=$true)]$Rule) process { $Rule.Service } }
function Get-NetFirewallSecurityFilter { param([Parameter(ValueFromPipeline=$true)]$Rule) process { $Rule.Security } }
function New-NetFirewallRule { [CmdletBinding()] param($PolicyStore,$Name,$DisplayName,$Description,$Direction,
    $Action,$Protocol,$LocalPort,$RemotePort,$LocalAddress,$RemoteAddress,$Program,$InterfaceAlias,$Profile,$Enabled)
    if ($PolicyStore -cne 'PersistentStore') { throw 'mock wrong policy store' }
    $global:Created += 1
    $global:Rule=[pscustomobject]@{ Name=$Name; DisplayName=$DisplayName; Description=$Description;
        Direction=$Direction; Action=$Action; Enabled='True'; Profile=$Profile;
        App=[pscustomobject]@{ Program=$Program; Package=$null };
        Port=[pscustomobject]@{ Protocol=$Protocol; LocalPort=$LocalPort; RemotePort=$RemotePort };
        Address=[pscustomobject]@{ LocalAddress=$LocalAddress; RemoteAddress=$RemoteAddress };
        Interface=[pscustomobject]@{ InterfaceAlias=$InterfaceAlias };
        InterfaceType=[pscustomobject]@{ InterfaceType='Any' };
        Service=[pscustomobject]@{ Service='Any' };
        Security=[pscustomobject]@{ Authentication='NotRequired'; Encryption='NotRequired';
            OverrideBlockRules='False'; LocalUser='Any'; RemoteUser='Any'; RemoteMachine='Any' } }
    switch ($global:State) {
        'WrongProgram' { $global:Rule.App.Program='C:\unrelated.exe' }
        'WrongPort' { $global:Rule.Port.LocalPort=39451 }
        'WrongAddress' { $global:Rule.Address.RemoteAddress='Any' }
        'WrongRuleProfile' { $global:Rule.Profile='Public' }
        'WrongInterface' { $global:Rule.Interface.InterfaceAlias='Any' }
        'WrongPackage' { $global:Rule.App.Package='S-1-15-2-1234' }
        'WrongService' { $global:Rule.Service.Service='unrelated' }
        'WrongInterfaceType' { $global:Rule.InterfaceType.InterfaceType='Wireless' }
        'WrongAuthentication' { $global:Rule.Security.Authentication='Required' }
        'WrongEncryption' { $global:Rule.Security.Encryption='Required' }
        'OverrideBlock' { $global:Rule.Security.OverrideBlockRules='True' }
        'WrongUser' { $global:Rule.Security.LocalUser='unrelated' }
        'WrongRemoteUser' { $global:Rule.Security.RemoteUser='unrelated' }
        'WrongRemoteMachine' { $global:Rule.Security.RemoteMachine='unrelated' }
    }
    return $global:Rule
}
function Remove-NetFirewallRule { param([Parameter(ValueFromPipeline=$true)]$Rule) process {
    if ($Rule.Description -cne $global:Rule.Description) { throw 'mock unrelated deletion' }
    $global:Removed += 1;$global:Rule=$null
} }
'''
        Encoded, Payload = Outer.GameFirewallPayload(Add)
        Invocation = ("$Failure=$null;try { & ([scriptblock]::Create([Text.Encoding]::Unicode.GetString(" +
                      "[Convert]::FromBase64String('" + Encoded + "'))));" +
                      "& ([scriptblock]::Create([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('" +
                      base64.b64encode(Remove.encode()).decode() + "')))) } catch { $Failure=[string]$_ };" +
                      "@{ Created=$global:Created; Removed=$global:Removed; Failure=$Failure }|ConvertTo-Json -Compress;" +
                      "if($Failure){exit 1}")
        Script = self.Fixture.Root / ('mock-game-firewall-' + State + '.ps1')
        Script.write_text(Prefix + Mocks + Invocation, encoding='utf-8')
        PowerShell = shutil.which('pwsh') or shutil.which('pwsh.exe')
        self.assertIsNotNone(PowerShell, 'pure generated firewall regression requires PowerShell')
        Result = subprocess.run([PowerShell, '-NoProfile', '-NonInteractive', '-File', str(Script)],
                                input=Payload, capture_output=True, text=True, timeout=30, creationflags=Outer.Hidden())
        Lines = Result.stdout.strip().splitlines()
        self.assertTrue(Lines, Result.stderr)
        return Result.returncode, json.loads(Lines[-1])

    def test_generated_game_rule_denies_stale_block_hash_and_wrong_interface_profile(self):
        for State, Reason in (('Stale', 'stale'), ('BlockAny', 'Block'), ('BlockRange', 'Block'),
                              ('HashChanged', 'projection changed'), ('PublicInterface', 'not Private'),
                              ('DuplicateValidator', 'validators invalid')):
            with self.subTest(State=State):
                Code, Row = self.RunMockGameFirewall(State)
                self.assertNotEqual(Code, 0)
                self.assertIn(Reason, Row['Failure'])
                self.assertEqual(Row['Created'], 0)
                self.assertEqual(Row['Removed'], 0)

    def test_generated_game_rule_requires_exact_effective_filters_and_preserves_changed_rules(self):
        for State in ('WrongProgram', 'WrongPort', 'WrongAddress', 'WrongRuleProfile', 'WrongInterface',
                      'WrongPackage', 'WrongService', 'WrongInterfaceType', 'WrongAuthentication',
                      'WrongEncryption', 'OverrideBlock', 'WrongUser', 'WrongRemoteUser', 'WrongRemoteMachine'):
            with self.subTest(State=State):
                Code, Row = self.RunMockGameFirewall(State)
                self.assertNotEqual(Code, 0)
                self.assertIn('changed', Row['Failure'])
                self.assertEqual(Row['Created'], 1)
                self.assertEqual(Row['Removed'], 0)

    def test_generated_game_rule_cleans_owned_rule_if_effective_publication_fails(self):
        Code, Row = self.RunMockGameFirewall('MissingEffective')
        self.assertNotEqual(Code, 0)
        self.assertEqual(Row['Created'], 1)
        self.assertEqual(Row['Removed'], 1)

    def test_generated_game_rule_normal_cleanup_and_unrelated_blocks(self):
        for State in ('Healthy', 'UnrelatedTcp', 'UnrelatedProgram'):
            with self.subTest(State=State):
                Code, Row = self.RunMockGameFirewall(State)
                self.assertEqual(Code, 0, Row['Failure'])
                self.assertEqual(Row, {'Created': 1, 'Removed': 1, 'Failure': None})

    def test_node_token_retired_after_failed_launch_barrier(self):
        self.PrepareGameFirewallFixture()
        Transport = MockTransport()
        for Role in ("SERVER", "CLIENT"):
            Source = Path(self.Fixture.Spec["Roles"][Role]["PreflightSource"])
            Target = self.Fixture.Private / ("server-preflight.json" if Role == "SERVER" else
                                             "client-preflight.json")
            shutil.copyfile(Source, Target)
        Outer.Stage(self.Fixture.Private, self.Fixture.SpecFile,
                    r"C:\Python312\python.exe", r"C:\Sandbox\farm_outer_endpoint.py", Transport)
        self.Fixture.Spec["Roles"]["SERVER"]["NodeStage"] = {"Path": "pinned", "Sha256": "a" * 64}
        self.Fixture.SpecFile.write_text(json.dumps(self.Fixture.Spec), encoding="utf-8")
        with mock.patch.object(Outer, "ControlProbe", side_effect=TimeoutError("mock blocked")):
            with self.assertRaisesRegex(TimeoutError, "mock blocked"):
                Outer.Launch(self.Fixture.Private, self.Fixture.SpecFile,
                             r"C:\Python312\python.exe", r"C:\Sandbox\farm_outer_endpoint.py",
                             Transport)
        self.assertEqual([r"C:\Sandbox" + "\\" + self.Fixture.Identity["RunId"]],
                         Transport.RetiredNodeRoots)
        self.assertEqual(Transport.RetiredNodeRoots, Transport.RetiredTlsRoots)

    def test_node_preparation_failure_retires_both_worker_secrets(self):
        Transport = MockTransport()
        Config = self.Fixture.Private / "node-preparation.json"
        Config.write_text(json.dumps({"Provider": "Node", "PrivateRoot": str(self.Fixture.Private),
                                      "WorkerToolRoot": r"C:\Sandbox",
                                      "WorkerPython": r"C:\Python312\python.exe",
                                      "WorkerHelper": r"C:\Sandbox\farm_outer_endpoint.py"}), encoding="utf-8")
        def FailAfterIdentity(ConfigPath, TransportInstance, AttemptId):
            Outer.WriteNew(self.Fixture.Private / "node-preparation-attempt.json", {
                "Format": "GargantuanFarmNodePreparationAttempt", "Version": 1,
                "RunId": self.Fixture.Identity["RunId"], "AttemptId": AttemptId})
            raise ValueError("mock late failure")

        with mock.patch.object(Outer, "PrepareInputsOnce", side_effect=FailAfterIdentity):
            with self.assertRaisesRegex(ValueError, "mock late failure"):
                Outer.PrepareInputs(Config, Transport)
        Expected = [r"C:\Sandbox" + "\\" + self.Fixture.Identity["RunId"]]
        self.assertEqual(Expected, Transport.RetiredNodeRoots)
        self.assertEqual(Expected, Transport.RetiredTlsRoots)

    def test_node_preparation_does_not_retire_a_preexisting_run(self):
        Transport = MockTransport()
        Config = self.Fixture.Private / "other-node-preparation.json"
        Config.write_text(json.dumps({"Provider": "Node", "PrivateRoot": str(self.Fixture.Private),
                                      "WorkerToolRoot": r"C:\Sandbox",
                                      "WorkerPython": r"C:\Python312\python.exe",
                                      "WorkerHelper": r"C:\Sandbox\farm_outer_endpoint.py"}), encoding="utf-8")
        with mock.patch.object(Outer, "PrepareInputsOnce", side_effect=ValueError("preexisting root")):
            with self.assertRaisesRegex(ValueError, "preexisting root"):
                Outer.PrepareInputs(Config, Transport)
        self.assertEqual([], Transport.RetiredNodeRoots)
        self.assertEqual([], Transport.RetiredTlsRoots)

    def test_worker_firewall_commands_pin_one_program_port_and_peer(self):
        Calls = []
        def Capture(Arguments, Timeout):
            Calls.append((Arguments, Timeout))
        Transport = Outer.Transport(r"C:\Sandbox\Codex\Tools\physical-qualifier\runtime\python.exe",
                                    r"C:\Sandbox\Codex\Tools\farm\farm_outer_endpoint.py")
        RunId = self.Fixture.Identity["RunId"]
        with mock.patch.object(Outer, "Checked", side_effect=Capture):
            Transport.AddControlFirewall(RunId)
            Transport.RemoveControlFirewall(RunId)
        self.assertEqual(2, len(Calls))
        Add = base64.b64decode(Calls[0][0][-1]).decode("utf-16le")
        Remove = base64.b64decode(Calls[1][0][-1]).decode("utf-16le")
        for Fixed in ("-LocalPort 39451", "-LocalAddress 192.168.0.108",
                      "-RemoteAddress 192.168.0.68", "-Profile Private",
                      "-Program $Program"):
            self.assertIn(Fixed, Add)
        self.assertIn("Codex-Gargantuan-Farm32-Control-" + RunId, Add)
        self.assertIn("Codex-Gargantuan-Farm32-Control-" + RunId, Remove)
        self.assertIn("Remove-NetFirewallRule", Remove)
        with self.assertRaisesRegex(ValueError, "invalid control firewall run identity"):
            Transport.AddControlFirewall("not-a-run")

    def test_collected_worker_index_verifies_every_member(self):
        Remote = self.Fixture.Root / "mock-worker-evidence"
        Remote.mkdir()
        Member = Remote / "result.json"
        Member.write_text("bounded evidence", encoding="utf-8")
        Index = {"RunId": self.Fixture.Identity["RunId"], "Role": "Server",
                 "Files": [{"Name": Member.name, "Bytes": Member.stat().st_size,
                            "Sha256": Outer.Digest(Member)}]}
        (Remote / "evidence-sha256.json").write_text(json.dumps(Index), encoding="utf-8")
        Output = self.Fixture.Root / "copied"
        Outer.FetchIndexed(MockTransport(), str(Remote), Output,
                           "evidence-sha256.json", self.Fixture.Identity["RunId"],
                           "SERVER", 128, 16 * 1024 * 1024)
        self.assertEqual(Member.read_bytes(), (Output / Member.name).read_bytes())
        Index["Files"][0]["Sha256"] = "0" * 64
        (Remote / "evidence-sha256.json").write_text(json.dumps(Index), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "transfer.*hash mismatch"):
            Outer.FetchIndexed(MockTransport(), str(Remote), self.Fixture.Root / "changed",
                               "evidence-sha256.json", self.Fixture.Identity["RunId"],
                               "SERVER", 128, 16 * 1024 * 1024)

    def test_admission_fairness_member_uses_canonical_32_mib_cap(self):
        Remote = self.Fixture.Root / "large-worker-evidence"
        Remote.mkdir()
        Member = Remote / "admission-fairness.tsv"
        with Member.open("wb") as Stream:
            Stream.truncate(17 * 1024 * 1024)
        Index = {"RunId": self.Fixture.Identity["RunId"], "Role": "Server",
                 "Files": [{"Name": Member.name, "Bytes": Member.stat().st_size,
                            "Sha256": Outer.Digest(Member)}]}
        (Remote / "evidence-sha256.json").write_text(json.dumps(Index), encoding="utf-8")
        Outer.FetchIndexed(MockTransport(), str(Remote), self.Fixture.Root / "fairness-copied",
                           "evidence-sha256.json", self.Fixture.Identity["RunId"],
                           "SERVER", 128, 16 * 1024 * 1024)
        self.assertEqual(Member.stat().st_size,
                         (self.Fixture.Root / "fairness-copied" / Member.name).stat().st_size)
        Index["Files"][0]["Name"] = "another.tsv"
        (Remote / "evidence-sha256.json").write_text(json.dumps(Index), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "evidence member invalid"):
            Outer.FetchIndexed(MockTransport(), str(Remote), self.Fixture.Root / "other-copied",
                               "evidence-sha256.json", self.Fixture.Identity["RunId"],
                               "SERVER", 128, 16 * 1024 * 1024)

    def test_recovery_causal_members_use_canonical_32_mib_server_cap(self):
        Remote = self.Fixture.Root / "recovery-worker-evidence"
        Remote.mkdir()
        for Case in ("gameplay", "structural", "mixed"):
            with self.subTest(Case=Case):
                Member = Remote / f"recovery-{Case}.tsv"
                with Member.open("wb") as Stream:
                    Stream.truncate(32 * 1024 * 1024)
                Index = {"RunId": self.Fixture.Identity["RunId"], "Role": "Server",
                         "Files": [{"Name": Member.name, "Bytes": Member.stat().st_size,
                                    "Sha256": Outer.Digest(Member)}]}
                (Remote / "evidence-sha256.json").write_text(json.dumps(Index), encoding="utf-8")
                Output = self.Fixture.Root / f"recovery-{Case}-copied"
                Outer.FetchIndexed(MockTransport(), str(Remote), Output,
                                   "evidence-sha256.json", self.Fixture.Identity["RunId"],
                                   "SERVER", 128, 16 * 1024 * 1024)
                self.assertEqual(Member.stat().st_size, (Output / Member.name).stat().st_size)
                Index["Files"][0]["Bytes"] += 1
                (Remote / "evidence-sha256.json").write_text(json.dumps(Index), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "evidence member invalid"):
                    Outer.FetchIndexed(MockTransport(), str(Remote), self.Fixture.Root / f"oversized-{Case}",
                                       "evidence-sha256.json", self.Fixture.Identity["RunId"],
                                       "SERVER", 128, 16 * 1024 * 1024)

    def test_recovery_cap_does_not_expand_other_members_or_client_evidence(self):
        Remote = self.Fixture.Root / "wrong-recovery-evidence"
        Remote.mkdir()
        for Number, (Name, Role, IndexName) in enumerate((
                ("recovery-other.tsv", "SERVER", "evidence-sha256.json"),
                ("recovery-gameplay.tsv", "CLIENT", "evidence-sha256.json"),
                ("recovery-gameplay.tsv", "SERVER", "capture-sha256.json"))):
            with self.subTest(Name=Name, Role=Role, IndexName=IndexName):
                IndexRole = Role if IndexName == "capture-sha256.json" else (
                    "Server" if Role == "SERVER" else "Clients")
                Index = {"RunId": self.Fixture.Identity["RunId"], "Role": IndexRole,
                         "Files": [{"Name": Name, "Bytes": 17 * 1024 * 1024, "Sha256": "a" * 64}]}
                (Remote / IndexName).write_text(json.dumps(Index), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "evidence member invalid"):
                    Outer.FetchIndexed(MockTransport(), str(Remote), self.Fixture.Root / f"wrong-{Number}",
                                       IndexName, self.Fixture.Identity["RunId"],
                                       Role, 128, 16 * 1024 * 1024)

    def test_failed_launch_aborts_each_run_owned_endpoint_and_binds_reap_receipts(self):
        RunId = self.Fixture.Identity["RunId"]
        Roots = {Role: str(self.Fixture.Root / (Role.lower() + "-cleanup"))
                 for Role in ("SERVER", "CLIENT")}
        for Role, Root in Roots.items():
            Endpoint.Prepare(Root)
            Names = ("host-config.json", "ticket.json") if Role == "SERVER" else ("ticket.json",)
            for Name in Names:
                (Path(Root) / Name).write_text(json.dumps({"Format": "GargantuanFarm32Campaign",
                                                            "Version": 1, "RunId": RunId}), encoding="utf-8")
        class FinishedChild:
            def __init__(self, Root, Action):
                self.Root, self.Action = Root, Action

            def wait(self, timeout):
                Marker = self.Root / (self.Action + ".abort.request")
                if not Marker.is_file():
                    raise AssertionError("missing run-bound abort: " + str(Marker))
                (self.Root / (self.Action + ".terminal.json")).write_text(json.dumps({
                    "Format": "GargantuanFarm32Terminal", "Version": 1,
                    "RunId": RunId, "Action": self.Action,
                    "ChildTreeReaped": True, "Outcome": "ABORTED"}), encoding="utf-8")
                return 1

        Launched = [("SERVER", "host", FinishedChild(Path(Roots["SERVER"]), "host")),
                    ("SERVER", "role", FinishedChild(Path(Roots["SERVER"]), "role")),
                    ("CLIENT", "role", FinishedChild(Path(Roots["CLIENT"]), "role"))]
        Outer.ReapLaunch(MockTransport(), Roots, RunId, self.Fixture.Private, Launched, False)
        self.assertTrue((self.Fixture.Private / "worker-host-terminal.json").is_file())
        self.assertTrue((self.Fixture.Private / "worker-role-terminal.json").is_file())

    def test_failed_launch_without_endpoint_reap_receipt_fails_closed(self):
        RunId = self.Fixture.Identity["RunId"]
        Root = str(self.Fixture.Root / "worker-cleanup")
        Endpoint.Prepare(Root)
        (Path(Root) / "host-config.json").write_text(json.dumps({
            "Format": "GargantuanFarm32Campaign", "Version": 1, "RunId": RunId}),
            encoding="utf-8")
        Process = mock.Mock()
        Process.wait.side_effect = subprocess.TimeoutExpired("mock", 30)
        with self.assertRaisesRegex(RuntimeError, "cleanup unproven"):
            Outer.ReapLaunch(MockTransport(), {"SERVER": Root}, RunId, self.Fixture.Private,
                             [("SERVER", "host", Process)], False)
        Process.terminate.assert_called_once()

    def test_worker_powershell_raises_nonzero_for_script_exception(self):
        PowerShell = shutil.which("pwsh.exe")
        if PowerShell is None:
            self.skipTest("PowerShell 7 is unavailable")
        Script = self.Fixture.Root / "fail.ps1"
        Script.write_text("throw 'mock script exception'\n", encoding="utf-8")
        def LocalChecked(Arguments, Timeout):
            return subprocess.run(Arguments[4:], check=True, capture_output=True,
                                  text=True, timeout=Timeout, creationflags=Outer.Hidden())
        Transport = Outer.Transport(r"C:\Python312\python.exe", r"C:\Sandbox\farm_outer_endpoint.py")
        with mock.patch.object(Outer, "Checked", side_effect=LocalChecked):
            with self.assertRaises(subprocess.CalledProcessError) as Context:
                Transport.WorkerPowerShell(PowerShell, str(Script), Timeout=10)
        self.assertNotEqual(0, Context.exception.returncode)

    def test_real_worker_paths_remain_under_task_sandbox(self):
        self.assertRaisesRegex(ValueError, "outside task sandbox", REAL_WORKER_SANDBOX,
                               r"C:\Windows\System32\something.exe")
        self.assertRaisesRegex(ValueError, "unsafe fixed worker path", Outer.RemoteText,
                               r"C:\Sandbox\Codex\RUNNER~1\stage")

    @unittest.skipUnless(sys.platform == "win32", "Windows 8.3 path alias")
    def test_temp_alias_is_canonicalized_before_fixed_path_fixture(self):
        Buffer = ctypes.create_unicode_buffer(1024)
        Length = ctypes.windll.kernel32.GetShortPathNameW(str(self.Fixture.Root), Buffer, len(Buffer))
        if not Length or Length >= len(Buffer) or Buffer.value.casefold() == str(self.Fixture.Root).casefold():
            self.skipTest("volume has no distinct 8.3 alias")
        self.assertEqual(self.Fixture.Root, Path(Buffer.value).resolve(strict=True))


if __name__ == "__main__":
    unittest.main()
