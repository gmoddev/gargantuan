"""Source-only Farm32 capture lifetime tests; no real capture is started."""

from datetime import datetime, timedelta, timezone
import base64
import gzip
import hashlib
import importlib.util
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import uuid


MODULE_PATH = Path(__file__).resolve().parents[1] / "farm_capture_campaign.py"
sys.path.insert(0, str(MODULE_PATH.parent))
SPEC = importlib.util.spec_from_file_location("farm_capture_campaign", MODULE_PATH)
campaign = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(campaign)


def WriteJson(File, Data):
    File.write_text(json.dumps(Data), encoding="utf-8")


class FixedClock:
    def __init__(self):
        self.Value = 0

    def __call__(self):
        return self.Value

    def Sleep(self, Seconds):
        self.Value += Seconds


class CaptureCampaignTests(unittest.TestCase):
    def test_versioned_embedded_exporter_matches_reviewable_source(self):
        Worker = MODULE_PATH.parent / "worker"
        Hook = (Worker / "PktMonFarm32Capture.ps1").read_text(encoding="utf-8-sig")
        Match = re.search(r"\$CompressedDefinition = '([A-Za-z0-9+/=]+)'", Hook)
        self.assertIsNotNone(Match)
        self.assertEqual(gzip.decompress(base64.b64decode(Match[1])).replace(b"\r\n", b"\n"),
                         (Worker / "Farm32NdisExport.cs").read_bytes().replace(b"\r\n", b"\n"))

    def setUp(self):
        self.Temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.Temp.cleanup)
        self.Root = Path(self.Temp.name)
        self.CaptureRoot = self.Root / "capture"
        self.CaptureRoot.mkdir()
        self.RoleRoot = self.Root / "role"
        self.RoleRoot.mkdir()
        self.RunId = str(uuid.uuid4())
        self.CoordinatorRunId = str(uuid.uuid4())
        self.Capture = self.CaptureRoot / self.RunId
        self.Clock = FixedClock()

    def Config(self, Role):
        return {"RunId": self.RunId, "CoordinatorRunId": self.CoordinatorRunId,
                "Role": Role, "CaptureRoot": self.CaptureRoot,
                "CaptureDirectory": self.Capture, "RoleEvidenceRoot": self.RoleRoot,
                "PowerShellPath": self.Root / "pwsh.exe",
                "ServicePath": self.Root / "AgentCoordinator.CaptureFarm32Service.exe",
                "HookPath": self.Root / "CaptureFarm32.ps1",
                "CaptureScriptPath": self.Root / "DumpcapFarm32Capture.ps1",
                "DumpcapPath": self.Root / "dumpcap.exe", "DumpcapSha256": "a" * 64}

    def RoleEvidence(self, Role):
        Result = {"RunId": self.RunId, "Role": "Server" if Role == "SERVER" else "Clients",
                  "Status": "PASS"}
        Now = datetime.now(timezone.utc)
        Result["StartedUtc"] = Now.isoformat()
        Result["CompletedUtc"] = Now.isoformat()
        ResultFile = self.RoleRoot / "result.json"
        WriteJson(ResultFile, Result)
        Index = {"RunId": self.RunId, "Role": Result["Role"],
                 "Files": [{"Name": "result.json", "Bytes": ResultFile.stat().st_size,
                            "Sha256": campaign.Digest(ResultFile)}]}
        WriteJson(self.RoleRoot / "evidence-sha256.json", Index)
        return self.RoleRoot / "evidence-sha256.json"

    def CaptureMarker(self, Role):
        Row = {"Profile": "Farm32Capture16GiB-v2"}
        if Role == "SERVER":
            Name = "farm32-netsh-owner.json"
            Row.update(TraceMaximumMiB=16384, NoWrapThresholdMiB=15360, PerformanceMetadataMerge=False)
            Etl = "C:\\Capture\\" + self.RunId + "\\farm32-worker-capture.etl"
            Row.update(NativeStopRecorded=True, NativeEventsLost=0, NativeLogBuffersLost=0, NativeBuffersWritten=10,
                       StopPolicy="ExactOwnedControlTraceW-v1", TraceIdentityRecorded=True,
                       Etl=Etl, TraceSessionName="NetTrace-test", TraceSessionGuid=str(uuid.uuid4()), TraceSessionHandle="18446744073709551615",
                       ControlSessionName="GargantuanFarm32-" + hashlib.sha256(Etl.upper().encode("utf-8")).hexdigest())
            Row["TraceSessionName"] = "NetTrace-" + Row["ControlSessionName"]
        else:
            Name = "farm32-client-capture.json"
            Row.update(DurationSeconds=600, AutostopKilobytes=16777216,
                       CompletenessBytes=15 * 1024 ** 3, RequestedBufferMiB=64)
        WriteJson(self.Capture / Name, Row)

    def test_worker_performance_metadata_merge_is_explicitly_disabled(self):
        Config = self.Config("SERVER")
        self.Capture.mkdir()
        self.CaptureMarker("SERVER")
        Marker = self.Capture / "farm32-netsh-owner.json"
        Original = json.loads(Marker.read_text())
        campaign.AssertCaptureProfile(Config)
        for Value in (None, True, 0, "false"):
            Row = dict(Original)
            if Value is None:
                Row.pop("PerformanceMetadataMerge")
            else:
                Row["PerformanceMetadataMerge"] = Value
            WriteJson(Marker, Row)
            with self.subTest(Value=Value), self.assertRaises(ValueError):
                campaign.AssertCaptureProfile(Config)

    def test_client_requested_buffer_marker_is_pinned(self):
        Config = self.Config("CLIENT")
        self.Capture.mkdir()
        self.CaptureMarker("CLIENT")
        campaign.AssertCaptureProfile(Config)
        Marker = self.Capture / "farm32-client-capture.json"
        Original = campaign.ReadJson(Marker)
        for Value in (None, 2, 63, 65, "64"):
            Row = {**Original, "RequestedBufferMiB": Value}
            if Value is None:
                del Row["RequestedBufferMiB"]
            WriteJson(Marker, Row)
            with self.subTest(Value=Value), self.assertRaisesRegex(ValueError, "profile marker"):
                campaign.AssertCaptureProfile(Config)

    def test_worker_native_stop_identity_required_before_readiness(self):
        Config = self.Config("SERVER")
        self.Capture.mkdir()
        self.CaptureMarker("SERVER")
        Marker = self.Capture / "farm32-netsh-owner.json"
        Original = campaign.ReadJson(Marker)
        for Key, Value in (("StopPolicy", None), ("StopPolicy", "netsh"), ("TraceIdentityRecorded", False),
                           ("TraceIdentityRecorded", 1), ("TraceSessionName", ""), ("TraceSessionGuid", "stale"),
                           ("TraceSessionHandle", "18446744073709551616"), ("TraceSessionHandle", 1),
                           ("ControlSessionName", "NetTrace"), ("Etl", "C:\\unrelated.etl")):
            with self.subTest(Key=Key, Value=Value):
                WriteJson(Marker, {**Original, Key: Value})
                with self.assertRaises(ValueError):
                    campaign.AssertCaptureProfile(Config)

    def test_bom_marker_is_not_silently_accepted(self):
        Config = self.Config("SERVER")
        self.Capture.mkdir()
        self.CaptureMarker("SERVER")
        Marker = self.Capture / "farm32-netsh-owner.json"
        Marker.write_bytes(b"\xef\xbb\xbf" + Marker.read_bytes())
        with self.assertRaises(json.JSONDecodeError):
            campaign.AssertCaptureProfile(Config)

    def test_active_marker_can_be_ready_but_cannot_be_sealed(self):
        Config = self.Config("SERVER")
        self.Capture.mkdir()
        self.CaptureMarker("SERVER")
        Marker = self.Capture / "farm32-netsh-owner.json"
        Row = campaign.ReadJson(Marker)
        Row.update(NativeStopRecorded=False, NativeEventsLost=None, NativeLogBuffersLost=None, NativeBuffersWritten=None)
        WriteJson(Marker, Row)
        campaign.AssertCaptureProfile(Config)
        with self.assertRaisesRegex(ValueError, "missing or records loss"):
            campaign.AssertCaptureProfile(Config, RequireStopped=True)

    def test_worker_stop_precedes_offline_finalize_and_seals_separate_root(self):
        Config = self.Config("SERVER")
        Calls = []

        def Runner(Command, **_):
            Calls.append([str(Item) for Item in Command])
            if Command[0] == str(Config["ServicePath"]):
                Operation = Command[1]
                if Operation == "start":
                    (self.Capture / "farm32-capture-active.txt").write_text("active")
                    self.CaptureMarker("SERVER")
                return subprocess.CompletedProcess(Command, 0, json.dumps({
                    "Success": True, "Operation": Operation, "RunId": self.RunId,
                    "State": {"start": "running", "stop": "stopped", "status": "idle"}[Operation]}), "")
            self.assertEqual(Command[-1], "Finalize")
            (self.Capture / "farm32-worker-capture.etl").write_bytes(b"etl")
            (self.Capture / "farm32-worker-capture.pcapng").write_bytes(b"\x0a\x0d\x0d\x0a" + b"pcap")
            (self.Capture / "farm32-capture-summary.txt").write_text("Total Events Lost 0\n")
            return subprocess.CompletedProcess(Command, 0, "exported", "")

        Controller = campaign.FarmCaptureController(Config, Runner=Runner,
                                                     Clock=self.Clock, Sleep=self.Clock.Sleep)
        Ready = Controller.Start()
        Index = self.RoleEvidence("SERVER")
        self.assertTrue(Ready.is_file())
        Sealed = Controller.Finish()
        Manifest = campaign.ReadJson(Sealed)
        self.assertEqual(Manifest["State"], "SEALED_UNQUALIFIED")
        self.assertEqual(Manifest["Profile"], "Farm32Capture16GiB-v2")
        self.assertEqual(Manifest["RoleIndexSha256"], campaign.Digest(Index))
        self.assertEqual([Row[1] for Row in Calls[:3]], ["start", "stop", "status"])
        self.assertEqual(Calls[3][-1], "Finalize")

    def FailedServer(self, ExportCode=0, Lost=0, MissingRole=False, WrongRun=False, StopDenied=False, ExportTimeout=False, MissingStop=False, LostStopReply=False):
        Config = self.Config("SERVER")
        Calls = []
        def Runner(Command, **_):
            Calls.append([str(Item) for Item in Command])
            if Command[0] == str(Config["ServicePath"]):
                Operation = Command[1]
                if Operation == "start":
                    (self.Capture / "farm32-capture-active.txt").write_text("active")
                    self.CaptureMarker("SERVER")
                    Marker = self.Capture / "farm32-netsh-owner.json"
                    Row = campaign.ReadJson(Marker)
                    Row.update(NativeStopRecorded=False, NativeEventsLost=None,
                               NativeLogBuffersLost=None, NativeBuffersWritten=None)
                    WriteJson(Marker, Row)
                if StopDenied and Operation in ("stop", "status"):
                    return subprocess.CompletedProcess(Command, 1, "", "stop denied")
                if Operation == "stop" and not MissingStop:
                    Marker = self.Capture / "farm32-netsh-owner.json"
                    Row = campaign.ReadJson(Marker)
                    Row.update(NativeStopRecorded=True, NativeEventsLost=Lost,
                               NativeLogBuffersLost=0, NativeBuffersWritten=10)
                    WriteJson(Marker, Row)
                if Operation == "stop" and LostStopReply:
                    return subprocess.CompletedProcess(Command, 1, "", "stop reply lost after owned stop")
                return subprocess.CompletedProcess(Command, 0, json.dumps({
                    "Success": True, "Operation": Operation, "RunId": self.RunId,
                    "State": {"start": "running", "stop": "stopped", "status": "idle"}[Operation]}), "")
            if ExportTimeout:
                raise subprocess.TimeoutExpired(Command, campaign.FINALIZE_TIMEOUT_SECONDS,
                                                output=b"partial export", stderr=b"partial error\xff")
            if ExportCode:
                return subprocess.CompletedProcess(Command, ExportCode, "export progress", "original export failure")
            (self.Capture / "farm32-worker-capture.etl").write_bytes(b"etl")
            (self.Capture / "farm32-worker-capture.pcapng").write_bytes(b"\x0a\x0d\x0d\x0a" + b"diagnostic")
            (self.Capture / "farm32-capture-summary.txt").write_text("Total Events Lost 0\n")
            return subprocess.CompletedProcess(Command, 0, "exported diagnostic", "")
        if not MissingRole:
            Result = {"RunId": str(uuid.uuid4()) if WrongRun else self.RunId, "Role": "Server",
                      "Status": "FAIL", "Reason": "server exited 7", "CompletedUtc": datetime.now(timezone.utc).isoformat()}
            ResultFile = self.RoleRoot / "result.json"
            WriteJson(ResultFile, Result)
            WriteJson(self.RoleRoot / "evidence-sha256.json", {"RunId": Result["RunId"], "Role": "Server",
                "Files": [{"Name": "result.json", "Bytes": ResultFile.stat().st_size,
                           "Sha256": campaign.Digest(ResultFile)}]})
        Controller = campaign.FarmCaptureController(Config, Runner=Runner, Clock=self.Clock, Sleep=self.Clock.Sleep)
        with mock.patch.object(campaign, "Configured", return_value=Config), \
                mock.patch.object(campaign, "FarmCaptureController", return_value=Controller):
            with self.assertRaises(Exception):
                campaign.RunRole("unused fixed test config")
        return campaign.ReadJson(self.Capture / "capture-sha256.json"), Calls, Controller

    def test_native_fail_without_started_time_exports_diagnostic_and_preserves_failure(self):
        Row, Calls, _ = self.FailedServer()
        self.assertEqual(Row["State"], "FAILED_DIAGNOSTIC")
        self.assertTrue(Row["OwnedStopConfirmed"])
        self.assertTrue(Row["OfflineExportSucceeded"])
        self.assertEqual(Row["RoleIndexSha256"], campaign.Digest(self.RoleRoot / "evidence-sha256.json"))
        self.assertEqual([Call[1] for Call in Calls[:3]], ["start", "stop", "status"])
        self.assertEqual(Calls[3][-1], "Finalize")
        self.assertEqual(campaign.ReadJson(self.RoleRoot / "result.json")["Reason"], "server exited 7")
        self.assertTrue((self.Capture / "capture-controller-incomplete.json").is_file())

    def test_failure_diagnostic_cannot_bind_as_qualified_capture(self):
        self.FailedServer()
        Coordinator = self.Root / "coordinator.json"
        WriteJson(Coordinator, {"RunId": self.CoordinatorRunId, "Success": True})
        with self.assertRaisesRegex(ValueError, "binding mismatch"):
            campaign.BindReceipt(self.RunId, self.CoordinatorRunId, Coordinator,
                self.RoleRoot / "evidence-sha256.json", "unused client",
                self.Capture / "capture-sha256.json", "unused client", self.Root / "receipt.json")
        self.assertFalse((self.Root / "receipt.json").exists())

    def test_diagnostic_missing_role_index_stays_unmeasured(self):
        Row, _, _ = self.FailedServer(MissingRole=True)
        self.assertIsNone(Row["RoleIndexSha256"])
        self.assertEqual(Row["State"], "FAILED_DIAGNOSTIC")
        self.assertGreaterEqual(self.Clock.Value, campaign.ROLE_DEADLINE_SECONDS)

    def test_diagnostic_wrong_run_role_index_is_not_attributed(self):
        Row, _, _ = self.FailedServer(WrongRun=True)
        self.assertIsNone(Row["RoleIndexSha256"])

    def test_diagnostic_export_requires_zero_loss_native_stop(self):
        Row, Calls, _ = self.FailedServer(Lost=1)
        self.assertFalse(Row["OfflineExportSucceeded"])
        self.assertFalse(any(Call[-1] == "Finalize" for Call in Calls))
        self.assertEqual(Row["State"], "FAILED_DIAGNOSTIC")

    def test_diagnostic_export_requires_confirmed_idle(self):
        Row, Calls, _ = self.FailedServer(StopDenied=True)
        self.assertFalse(Row["OwnedStopConfirmed"])
        self.assertFalse(Row["OfflineExportSucceeded"])
        self.assertFalse(any(Call[-1] == "Finalize" for Call in Calls))
        self.assertIn("farm32-netsh-owner.json", Row["UnsealedArtifacts"])
        self.assertTrue(all(Member["Name"].startswith("capture-controller-") for Member in Row["Files"]))

    def test_idle_without_native_stop_receipt_cannot_export(self):
        Row, Calls, _ = self.FailedServer(MissingStop=True)
        self.assertTrue(Row["OwnedStopConfirmed"])
        self.assertFalse(Row["OfflineExportSucceeded"])
        self.assertFalse(any(Call[-1] == "Finalize" for Call in Calls))

    def test_lost_stop_reply_requires_idle_and_real_native_zero_loss_receipt(self):
        Row, Calls, _ = self.FailedServer(LostStopReply=True)
        self.assertTrue(Row["OwnedStopConfirmed"])
        self.assertTrue(Row["OfflineExportSucceeded"])
        self.assertEqual([Call[1] for Call in Calls[:3]], ["start", "stop", "status"])
        self.assertEqual(sum(Call[-1] == "Finalize" for Call in Calls), 1)

    def test_export_timeout_preserves_partial_bytes_and_never_retries(self):
        Row, Calls, Controller = self.FailedServer(ExportTimeout=True)
        Raw = campaign.ReadJson(self.Capture / "capture-controller-finalize-failure.json")
        self.assertEqual(Raw["TimeoutSeconds"], campaign.FINALIZE_TIMEOUT_SECONDS)
        self.assertEqual(Raw["Stdout"], "partial export")
        self.assertEqual(Raw["Stderr"], "partial error\ufffd")
        self.assertIsNone(Raw["ExitCode"])
        self.assertFalse(Row["OfflineExportSucceeded"])
        self.assertTrue(Controller.FinalizeAttempted)
        self.assertEqual(sum(Call[-1] == "Finalize" for Call in Calls), 1)

    def test_export_failure_retains_bounded_raw_result_and_never_retries(self):
        Row, Calls, Controller = self.FailedServer(ExportCode=7)
        self.assertFalse(Row["OfflineExportSucceeded"])
        Raw = campaign.ReadJson(self.Capture / "capture-controller-finalize-failure.json")
        self.assertEqual(Raw["ExitCode"], 7)
        self.assertEqual(Raw["Stderr"], "original export failure")
        self.assertEqual(Raw["Stdout"], "export progress")
        Before = len(Calls)
        with self.assertRaises(ValueError):
            Controller.FailureEvidence(RuntimeError("another caller"))
        self.assertEqual(len(Calls), Before)

    def test_diagnostic_index_hashes_all_retained_members(self):
        Row, _, _ = self.FailedServer()
        for Member in Row["Files"]:
            self.assertEqual(Member["Sha256"], campaign.Digest(self.Capture / Member["Name"]))
            self.assertEqual(Member["Bytes"], (self.Capture / Member["Name"]).stat().st_size)

    def test_fixed_operation_error_caps_retained_raw_output(self):
        Result = subprocess.CompletedProcess([], 1, "x" * (campaign.MAX_TEXT_BYTES + 1), "error")
        Error = campaign.FixedOperationError(Result)
        self.assertEqual(len(Error.Result["Stdout"]), campaign.MAX_TEXT_BYTES)
        self.assertTrue(Error.Result["OutputBoundExceeded"])

    def test_client_capture_is_owned_until_autostop(self):
        Config = self.Config("CLIENT")

        class Child:
            def poll(self):
                return None

            def wait(self, timeout):
                self.Test.assertLessEqual(timeout, campaign.CLIENT_AUTOSTOP_SECONDS)
                return 0

        def Spawner(Command, **_):
            self.CaptureMarker("CLIENT")
            self.assertEqual(Command[0], str(Config["PowerShellPath"]))
            (self.Capture / "farm32-client-capture.pcapng").write_bytes(b"\x0a\x0d\x0d\x0a" + b"x" * 24)
            Child.Test = self
            return Child()

        Controller = campaign.FarmCaptureController(Config, Spawner=Spawner,
                                                     Clock=self.Clock, Sleep=self.Clock.Sleep)
        Controller.Start()
        self.RoleEvidence("CLIENT")
        Manifest = campaign.ReadJson(Controller.Finish())
        self.assertEqual(Manifest["Role"], "CLIENT")
        self.assertEqual(Manifest["State"], "SEALED_UNQUALIFIED")

    def test_client_capture_clean_early_exit_cannot_be_timestamped_after_role(self):
        Config = self.Config("CLIENT")
        self.Capture.mkdir()
        Controller = campaign.FarmCaptureController(Config, Clock=self.Clock, Sleep=self.Clock.Sleep)
        Controller.Started = self.Clock()

        class Child:
            def poll(self):
                return 0

        Controller.Child = Child()
        with self.assertRaisesRegex(RuntimeError, "ended before role completion"):
            Controller.AwaitRole()
        self.assertLess(self.Clock.Value, 1)
        self.RoleEvidence("CLIENT")
        with self.assertRaisesRegex(RuntimeError, "ended before role completion"):
            Controller.Finish()
        self.assertFalse((self.Capture / "capture-sha256.json").exists())

    def test_role_deadline_is_less_than_service_lease(self):
        Config = self.Config("SERVER")
        self.Capture.mkdir()
        Controller = campaign.FarmCaptureController(Config, Clock=self.Clock, Sleep=self.Clock.Sleep)
        Controller.Started = self.Clock()
        with self.assertRaises(TimeoutError):
            Controller.AwaitRole()
        self.assertAlmostEqual(self.Clock.Value, campaign.ROLE_DEADLINE_SECONDS, places=6)
        self.assertLess(campaign.ROLE_DEADLINE_SECONDS + campaign.STOP_TIMEOUT_SECONDS, 600)

    def test_stale_profile_cannot_signal_readiness(self):
        Config = self.Config("SERVER")

        def Runner(Command, **_):
            (self.Capture / "farm32-capture-active.txt").write_text("active")
            WriteJson(self.Capture / "farm32-netsh-owner.json", {
                "TraceMaximumMiB": 1024, "NoWrapThresholdMiB": 960})
            return subprocess.CompletedProcess(Command, 0, json.dumps({
                "Success": True, "Operation": "start", "RunId": self.RunId,
                "State": "running"}), "")

        Controller = campaign.FarmCaptureController(Config, Runner=Runner)
        with self.assertRaisesRegex(ValueError, "profile marker"):
            Controller.Start()
        self.assertTrue(Controller.WorkerActive)  # RunRole must still perform owned Abort.
        self.assertFalse((self.Capture / "capture-controller-ready.json").exists())

    def test_sealing_uses_64_bit_strict_15_gib_size_gate(self):
        # Model file metadata only, retaining tiny real files. This tests the
        # gate, not large-file throughput or physical capacity qualification.
        Config = self.Config("SERVER")
        self.Capture.mkdir()
        self.CaptureMarker("SERVER")
        Pcap = self.Capture / "farm32-worker-capture.pcapng"
        Etl = self.Capture / "farm32-worker-capture.etl"
        Pcap.write_bytes(b"\x0a\x0d\x0d\x0a" + b"pcap")
        Etl.write_bytes(b"etl")
        (self.Capture / "farm32-capture-summary.txt").write_text("Total Events Lost 0\n")
        RoleIndex = self.RoleEvidence("SERVER")
        OriginalStat = Path.stat
        for Bytes in (4 * 1024 ** 3 + 1, 15 * 1024 ** 3 - 1, 15 * 1024 ** 3, 16 * 1024 ** 3):
            def VirtualStat(File, *Args, **Keywords):
                Result = OriginalStat(File, *Args, **Keywords)
                if File in (Pcap, Etl):
                    Values = list(Result)
                    Values[6] = Bytes
                    return os.stat_result(Values)
                return Result
            with self.subTest(Bytes=Bytes), mock.patch.object(Path, "stat", VirtualStat):
                if Bytes < 15 * 1024 ** 3:
                    Index = campaign.SealCapture(Config, "start", "ready", "stop", RoleIndex, 1)
                    self.assertEqual(next(Row["Bytes"] for Row in campaign.ReadJson(Index)["Files"]
                                          if Row["Name"] == Pcap.name), Bytes)
                    Index.unlink()
                else:
                    with self.assertRaisesRegex(ValueError, "capped"):
                        campaign.SealCapture(Config, "start", "ready", "stop", RoleIndex, 1)

    def test_outer_receipt_binds_both_role_and_capture_indices(self):
        Coordinator = self.Root / "coordinator.json"
        WriteJson(Coordinator, {"RunId": self.CoordinatorRunId, "Success": True})
        ServerRole = self.Root / "server-role.json"
        ClientRole = self.Root / "client-role.json"
        ServerCapture = self.Root / "server-capture.json"
        ClientCapture = self.Root / "client-capture.json"
        for Role, RoleFile, CaptureFile in (("SERVER", ServerRole, ServerCapture),
                                            ("CLIENT", ClientRole, ClientCapture)):
            WriteJson(RoleFile, {"RunId": self.RunId, "Role": "Server" if Role == "SERVER" else "Clients"})
            WriteJson(CaptureFile, {"RunId": self.RunId, "CoordinatorRunId": self.CoordinatorRunId,
                                    "Role": Role, "RoleIndexSha256": campaign.Digest(RoleFile),
                                    "Profile": "Farm32Capture16GiB-v2",
                                    "State": "SEALED_UNQUALIFIED"})
        Receipt = self.Root / "outer.json"
        campaign.BindReceipt(self.RunId, self.CoordinatorRunId, Coordinator,
                             ServerRole, ClientRole, ServerCapture, ClientCapture, Receipt)
        Bound = campaign.ReadJson(Receipt)
        self.assertEqual(Bound["CoordinatorResultSha256"], campaign.Digest(Coordinator))
        self.assertEqual(Bound["Roles"]["SERVER"]["CaptureIndexSha256"], campaign.Digest(ServerCapture))
        WriteJson(ClientCapture, {"RunId": self.RunId, "State": "PASS"})
        with self.assertRaises(ValueError):
            campaign.BindReceipt(self.RunId, self.CoordinatorRunId, Coordinator,
                                 ServerRole, ClientRole, ServerCapture, ClientCapture,
                                 self.Root / "untrusted.json")

    def test_fixed_config_rejects_changed_pin_and_overlapping_roots(self):
        Config = {"Format": "GargantuanFarm32CaptureCampaign", "Version": 1,
                  "RunId": self.RunId, "CoordinatorRunId": self.CoordinatorRunId,
                  "Role": "SERVER", "CaptureRoot": str(self.CaptureRoot),
                  "RoleEvidenceRoot": str(self.RoleRoot)}
        for Key, Name in (("PowerShellPath", "pwsh.exe"),
                          ("ServicePath", "AgentCoordinator.CaptureFarm32Service.exe"),
                          ("HookPath", "CaptureFarm32.ps1")):
            File = self.Root / Name
            File.write_bytes(Name.encode("ascii"))
            Config[Key] = str(File)
            Config[Key.removesuffix("Path") + "Sha256"] = campaign.Digest(File)
        ConfigFile = self.Root / "config.json"
        WriteJson(ConfigFile, Config)
        self.assertEqual(campaign.Configured(ConfigFile)["CaptureDirectory"],
                         self.CaptureRoot.resolve(strict=True) / self.RunId)
        (self.Root / "CaptureFarm32.ps1").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "pin changed"):
            campaign.Configured(ConfigFile)
        Config["HookSha256"] = campaign.Digest(self.Root / "CaptureFarm32.ps1")
        Config["RoleEvidenceRoot"] = str(self.CaptureRoot / "role")
        WriteJson(ConfigFile, Config)
        with self.assertRaisesRegex(ValueError, "overlap"):
            campaign.Configured(ConfigFile)


if __name__ == "__main__":
    unittest.main()
