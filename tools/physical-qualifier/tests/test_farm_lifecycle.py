"""No-capture fixed-operation tests for the 32-client farm lifecycle adapter."""

import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import uuid


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from dependency import GetRoot  # noqa: E402

sys.path.insert(0, str(GetRoot()))
from agent_coordinator.workflow import Operation, Workflow  # noqa: E402
from agent_coordinator.control import Assignments, Host, Join  # noqa: E402
from agent_coordinator.transport import Journal  # noqa: E402
import farm_lifecycle as Farm  # noqa: E402


MOCK_SUPERVISOR = r'''
#requires -Version 7.0
param(
    [ValidateSet('Server','Clients')][string]$Role,
    [string]$ManifestPath, [string]$ManifestSha256,
    [string]$PackageRoot, [string]$EvidenceRoot,
    [string]$RunRegistryRoot, [string]$NodeRootCertificatePath
)
$ErrorActionPreference = 'Stop'
$Manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
[void][IO.Directory]::CreateDirectory($EvidenceRoot)
if ($Role -eq 'Server') {
    @{RunId=$Manifest.RunId;Role='Server';Pid=$PID;Endpoint=$Manifest.Endpoint;
      ReadyUtc=[DateTimeOffset]::UtcNow.ToString('O')} |
      ConvertTo-Json | Set-Content -LiteralPath (Join-Path $EvidenceRoot 'server-ready.json')
} else {
    "[Qualification:Client] event=ready run_id=$($Manifest.RunId) slot=0 nonce=$($Manifest.Nonces[0]) connection_slot=1 connection_generation=1" |
      Set-Content -LiteralPath (Join-Path $EvidenceRoot 'client-00.stdout.log')
}
if (Test-Path -LiteralPath (Join-Path $PackageRoot 'fail')) { Start-Sleep -Milliseconds 250; exit 7 }
if (Test-Path -LiteralPath (Join-Path $PackageRoot 'hold')) {
    $Child = Start-Process -FilePath (Join-Path $PSHOME 'pwsh.exe') -WindowStyle Hidden -PassThru `
      -ArgumentList @('-NoProfile','-NonInteractive','-Command','Start-Sleep -Seconds 120')
    [IO.File]::WriteAllText((Join-Path $EvidenceRoot 'child.pid'), [string]$Child.Id)
    Start-Sleep -Seconds 120
}
Start-Sleep -Milliseconds 350
@{RunId=$Manifest.RunId;Role=$Role;Provider=$Manifest.Provider;
  ManifestSha256=$ManifestSha256;Status='PASS'} |
  ConvertTo-Json | Set-Content -LiteralPath (Join-Path $EvidenceRoot 'result.json')
'''


def Hash(File):
    return hashlib.sha256(File.read_bytes()).hexdigest()


def Alive(Pid):
    if os.name != "nt":
        try:
            os.kill(Pid, 0)
            return True
        except ProcessLookupError:
            return False
    Query = subprocess.run(["tasklist.exe", "/FI", "PID eq " + str(Pid), "/FO", "CSV", "/NH"],
                           capture_output=True, text=True, timeout=5, check=False,
                           creationflags=subprocess.CREATE_NO_WINDOW)
    return '"' + str(Pid) + '"' in Query.stdout


class FarmLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.Temporary = tempfile.TemporaryDirectory()
        self.Root = Path(self.Temporary.name)
        self.Supervisor = self.Root / "PhysicalGameSessionFarmEndpoint.ps1"
        self.Supervisor.write_text(MOCK_SUPERVISOR, encoding="utf-8")
        self.Manifest = self.Root / "run-manifest.json"
        self.RunId = str(uuid.uuid4())
        self.CoordinatorRunId = str(uuid.uuid4())
        self.SourceCommit = "a" * 40
        self.Manifest.write_text(json.dumps({
            "Format": "GargantuanPhysicalFarmEndpoint", "Version": 1,
            "RunId": self.RunId, "SourceCommit": self.SourceCommit,
            "Endpoint": "10.253.3.2:39450", "Provider": "Local",
            "Nonces": [str((123456789 << 32) | (Slot + 1)) for Slot in range(32)],
        }), encoding="utf-8")
        self.PowerShell = shutil.which("pwsh")

    def tearDown(self):
        self.Temporary.cleanup()

    def Config(self, Role, Name=None):
        if self.PowerShell is None:
            self.skipTest("PowerShell unavailable")
        Directory = self.Root / (Name or Role.lower())
        Directory.mkdir()
        Package = Directory / "package"
        Package.mkdir()
        EvidenceParent = Directory / "evidence"
        EvidenceParent.mkdir()
        Config = {
            "Role": Role, "CoordinatorRunId": self.CoordinatorRunId, "RunId": self.RunId,
            "SourceCommit": self.SourceCommit,
            "PowerShellPath": str(Path(self.PowerShell).resolve()),
            "PowerShellSHA256": Hash(Path(self.PowerShell)),
            "SupervisorPath": str(self.Supervisor), "SupervisorSHA256": Hash(self.Supervisor),
            "ManifestPath": str(self.Manifest), "ManifestSHA256": Hash(self.Manifest),
            "PackageRoot": str(Package), "EvidenceRoot": str(EvidenceParent / self.RunId),
            "RunRegistryRoot": str(Directory / "registry"),
        }
        File = Directory / "farm-local.json"
        File.write_text(json.dumps(Config), encoding="utf-8")
        return File, Config, Package

    def Context(self, Seconds=10):
        return Operation(time.monotonic() + Seconds)

    def test_schema_is_closed_bounded_and_requires_exact_roles(self):
        Data = json.loads((ROOT / "workflows/thirty-two-client-farm-lifecycle.json").read_text())
        Schema = Workflow(Data)
        self.assertLessEqual(Schema.Value["ExecutionTimeout"], 600)
        self.assertTrue(all(Step["Timeout"] <= 90 and not Step["Parameters"] for Step in Data["Transitions"]))
        self.assertEqual({"SERVER", "CLIENT"}, set(Data["Roles"]))
        self.assertNotIn("physical.server-run.v1", Data["Roles"]["SERVER"])
        self.assertNotIn("capture.pktmon.v1", Data["Roles"]["SERVER"])

    def test_pin_and_manifest_identity_rejections(self):
        File, Config, _ = self.Config("SERVER")
        Config["SupervisorSHA256"] = "0" * 64
        File.write_text(json.dumps(Config), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "supervisor pin changed"):
            Farm.FarmLifecycle(File)
        Config["SupervisorSHA256"] = Hash(self.Supervisor)
        Config["RunId"] = str(uuid.uuid4())
        File.write_text(json.dumps(Config), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "run manifest mismatch"):
            Farm.FarmLifecycle(File)
        Config["RunId"] = self.RunId
        Config["SourceCommit"] = "b" * 40
        File.write_text(json.dumps(Config), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "run manifest mismatch"):
            Farm.FarmLifecycle(File)
        Config["SourceCommit"] = self.SourceCommit
        Config["InjectedCommand"] = "calc.exe"
        File.write_text(json.dumps(Config), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "config fields"):
            Farm.FarmLifecycle(File)

    @unittest.skipUnless(os.name == "nt", "actual PowerShell process test is Windows-only")
    def test_actual_child_launch_barriers_results_and_cleanup(self):
        ServerFile, _, _ = self.Config("SERVER")
        ClientFile, _, _ = self.Config("CLIENT")
        Server = Farm.FarmLifecycle(ServerFile)
        Client = Farm.FarmLifecycle(ClientFile)
        try:
            Schema = Workflow(json.loads((ROOT / "workflows/thirty-two-client-farm-lifecycle.json").read_text()))
            self.assertEqual(set(Server.Catalog().Advertise(Schema, "SERVER")), set(Schema.Value["Roles"]["SERVER"]))
            self.assertEqual(set(Client.Catalog().Advertise(Schema, "CLIENT")), set(Schema.Value["Roles"]["CLIENT"]))
            self.assertTrue(Server.ServerLaunch({}, self.Context())["Success"])
            self.assertEqual(1, len(Server.ServerReady({}, self.Context())["Evidence"]))
            self.assertTrue(Client.ClientLaunch({}, self.Context())["Success"])
            self.assertEqual(1, len(Client.ClientReady({}, self.Context())["Evidence"]))
            Farm.POLL_SECONDS = 0.2
            self.assertTrue(Server.Progress({}, self.Context())["Success"])
            self.assertTrue(Client.Progress({}, self.Context())["Success"])
            self.assertEqual(1, len(Server.Result({}, self.Context())["Evidence"]))
            self.assertEqual(1, len(Client.Result({}, self.Context())["Evidence"]))
            self.assertEqual(0, Server.Process.returncode)
            self.assertEqual(0, Client.Process.returncode)
        finally:
            Server.Cleanup()
            Client.Cleanup()

    @unittest.skipUnless(os.name == "nt", "actual PowerShell process test is Windows-only")
    def test_failure_after_launch_and_abort_kills_only_owned_tree(self):
        File, _, Package = self.Config("SERVER")
        (Package / "fail").write_text("x")
        Failed = Farm.FarmLifecycle(File)
        try:
            self.assertTrue(Failed.ServerLaunch({}, self.Context())["Success"])
            with self.assertRaisesRegex(RuntimeError, "supervisor exited 7"):
                Failed.Result({}, self.Context())
        finally:
            Failed.Cleanup()

        # A separate staged config avoids the consumed first run's local logs.
        HoldFile, HoldConfig, HoldPackage = self.Config("SERVER", "hold-server")
        (HoldPackage / "hold").write_text("x")
        Unrelated = subprocess.Popen([self.PowerShell, "-NoProfile", "-NonInteractive", "-Command", "Start-Sleep -Seconds 120"],
                                     creationflags=subprocess.CREATE_NO_WINDOW)
        Owned = Farm.FarmLifecycle(HoldFile)
        try:
            Owned.ServerLaunch({}, self.Context())
            Owned.ServerReady({}, self.Context())
            ChildFile = Path(HoldConfig["EvidenceRoot"]) / "child.pid"
            Until = time.monotonic() + 5
            while not ChildFile.exists() and time.monotonic() < Until:
                time.sleep(0.05)
            self.assertTrue(ChildFile.exists())
            ChildPid = int(ChildFile.read_text())
            self.assertTrue(Alive(ChildPid))
            Owned.Cleanup()
            Until = time.monotonic() + 5
            while Alive(ChildPid) and time.monotonic() < Until:
                time.sleep(0.05)
            self.assertFalse(Alive(ChildPid))
            self.assertIsNone(Unrelated.poll())
        finally:
            Owned.Cleanup()
            Unrelated.terminate()
            Unrelated.wait(timeout=5)

    @unittest.skipUnless(os.name == "nt", "actual PowerShell process test is Windows-only")
    def test_pinned_coordinator_two_role_actual_child_lifecycle(self):
        ServerFile, ServerConfig, _ = self.Config("SERVER")
        ClientFile, ClientConfig, _ = self.Config("CLIENT")
        Value = json.loads((ROOT / "workflows/thirty-two-client-farm-lifecycle.json").read_text())
        Value["RegistrationTimeout"] = 10
        Value["ExecutionTimeout"] = 15
        Schema = Workflow(Value)
        Bootstrap = {Role: {"EndpointId": Role, "PeerIp": "127.0.0.1",
                            "RunId": str(uuid.uuid4()), "Token": secrets.token_hex(32)}
                     for Role in ("SERVER", "CLIENT")}
        AssignmentSet = Assignments(Schema, Bootstrap, {})
        for File, Config in ((ServerFile, ServerConfig), (ClientFile, ClientConfig)):
            Config["CoordinatorRunId"] = AssignmentSet.RunId
            File.write_text(json.dumps(Config), encoding="utf-8")
        Adapters = {"SERVER": Farm.FarmLifecycle(ServerFile), "CLIENT": Farm.FarmLifecycle(ClientFile)}
        Farm.POLL_SECONDS = 0.2
        Codes = {}
        Ready = threading.Event()
        Port = {}

        def Listen(Number):
            Port["Value"] = Number
            Ready.set()

        def RunHost():
            Codes["HOST"] = Host("127.0.0.1", 0, AssignmentSet, Journal(self.Root / "host"), Listen)

        def RunEndpoint(Role):
            Config = {**Bootstrap[Role], "CoordinatorHost": "127.0.0.1", "Port": Port["Value"]}
            Codes[Role] = Join(Config, {(Schema.Value["SchemaId"], 1): Schema},
                               Adapters[Role].Catalog(), Journal(self.Root / "journals" / Role))

        HostThread = threading.Thread(target=RunHost)
        HostThread.start()
        self.assertTrue(Ready.wait(3))
        Endpoints = [threading.Thread(target=RunEndpoint, args=(Role,)) for Role in ("SERVER", "CLIENT")]
        for Thread in Endpoints:
            Thread.start()
        for Thread in (*Endpoints, HostThread):
            Thread.join(12)
            self.assertFalse(Thread.is_alive())
        self.assertEqual({"SERVER": 0, "CLIENT": 0, "HOST": 0}, Codes)
        for Role in ("SERVER", "CLIENT"):
            self.assertEqual(0, Adapters[Role].Process.returncode)
            self.assertEqual("PASS", json.loads((Adapters[Role].Evidence / "result.json").read_text())["Status"])


if __name__ == "__main__":
    unittest.main()
