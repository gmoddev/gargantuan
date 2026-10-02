"""Mock transfer tests for fixed Farm32 two-host staging."""

import base64
import ctypes
import hashlib
import json
from pathlib import Path
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
from test_farm_ticket_staging import FarmTicketStagingTests  # noqa: E402
REAL_WORKER_SANDBOX = Outer.WorkerSandbox


class MockTransport:
    def __init__(self):
        self.ControlRules = []
        self.RetiredNodeRoots = []
        self.RetiredTlsRoots = []

    def AddControlFirewall(self, RunId):
        self.ControlRules.append(("add", RunId))

    def RemoveControlFirewall(self, RunId):
        self.ControlRules.append(("remove", RunId))

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
    def test_staged_entrypoints_import_siblings_with_isolated_python(self):
        for Name, Arguments in (("farm_outer_endpoint.py", ["--help"]),
                                ("farm_campaign_runner.py", ["--help"]),
                                ("farm_lifecycle.py", [])):
            with self.subTest(Name=Name):
                Result = subprocess.run([sys.executable, "-I", "-B", str(ROOT / Name),
                                         *Arguments], capture_output=True, text=True,
                                        timeout=10, check=False)
                self.assertEqual(Result.returncode, 0, Result.stderr)

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

    def test_recovery_manifest_profile_is_explicit_and_bounded(self):
        self.assertEqual((9000, 10000,
                          ["-ScaleWorkload", "-ClientFrames", "9000",
                           "-ServerTicks", "10000"]), Outer.WorkloadProfile(False))
        self.assertEqual((18000, 19000,
                          ["-ScaleWorkload", "-ClientFrames", "18000",
                           "-ServerTicks", "19000", "-RecoveryWorkload"]),
                         Outer.WorkloadProfile(True))
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

    def test_node_token_retired_after_failed_launch_barrier(self):
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
