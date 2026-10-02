"""No capture process: sealed Farm32 cross-endpoint identity fixtures."""

from datetime import datetime, timedelta, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).parents[1]
sys.path.insert(0, str(ROOT))
SPEC = importlib.util.spec_from_file_location("farm_capture_acceptance", ROOT / "farm_capture_acceptance.py")
ACCEPTANCE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ACCEPTANCE)
SPEC = importlib.util.spec_from_file_location("test_farm_capture_directions", Path(__file__).with_name("test_farm_capture_directions.py"))
FIXTURE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FIXTURE)
RUN = FIXTURE.RUN_ID
COORDINATOR = FIXTURE.COORDINATOR_ID
NOW = datetime(2026, 10, 2, tzinfo=timezone.utc)


def Save(File, Row):
    File.write_text(json.dumps(Row), encoding="utf-8")


def Index(Root, Name, Format, Role, Extra):
    Rows = [{"Name": File.name, "Bytes": File.stat().st_size,
             "Sha256": hashlib.sha256(File.read_bytes()).hexdigest()}
            for File in sorted(Root.iterdir()) if File.is_file() and File.name != Name]
    Row = {"Format": Format, "Version": 1, "RunId": RUN, "Role": Role, "Files": Rows}
    Row.update(Extra)
    File = Root / Name
    Save(File, Row)
    return File


class FarmCaptureAcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.Temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.Temp.cleanup)
        self.Root = Path(self.Temp.name)
        self.ServerRole = self.Root / "server-role"
        self.ClientRole = self.Root / "client-role"
        self.ServerCapture = self.Root / "server-capture"
        self.ClientCapture = self.Root / "client-capture"
        for Directory in (self.ServerRole, self.ClientRole, self.ServerCapture, self.ClientCapture):
            Directory.mkdir()
        self.Ports = list(range(49000, 49032))
        self.Nonces = [str(1000000 + Slot) for Slot in range(32)]
        self.Manifest = {"RunId": RUN, "Endpoint": "10.253.3.2:39450",
                         "Provider": "Local", "Nonces": self.Nonces}
        for Directory in (self.ServerRole, self.ClientRole):
            Save(Directory / "run-manifest.json", self.Manifest)
            Save(Directory / "result.json", {"RunId": RUN, "Status": "PASS",
                                             "StartedUtc": (NOW + timedelta(seconds=2)).isoformat(),
                                             "CompletedUtc": (NOW + timedelta(seconds=5)).isoformat()})
        Ready = [f"[Qualification:Server] event=ready run={RUN} nonce={Nonce} "
                 f"connection_slot={Slot+1} connection_generation=1 session_epoch=1 "
                 f"player_id={Slot+1} monotonic_us={Slot+1} client_port={self.Ports[Slot]}"
                 for Slot, Nonce in enumerate(self.Nonces)]
        (self.ServerRole / "server.stdout.log").write_text("\n".join(Ready) + "\n", encoding="utf-8")
        self.ServerRoleIndex = Index(self.ServerRole, "evidence-sha256.json", "role", "Server", {})
        self.ClientRoleIndex = Index(self.ClientRole, "evidence-sha256.json", "role", "Clients", {})
        (self.ServerCapture / "farm32-worker-capture.pcapng").write_bytes(FIXTURE.Pcap(self.Ports))
        (self.ServerCapture / "farm32-worker-capture.etl").write_bytes(b"etl")
        Save(self.ServerCapture / "farm32-netsh-owner.json", {
            "Profile": "Farm32Capture16GiB-v2",
            "CapturePort": 39450, "MiniportIfIndex": 19,
            "CaptureLayers": ["NDIS physical miniport"],
            "TraceMaximumMiB": 16384, "NoWrapThresholdMiB": 15360})
        (self.ClientCapture / "farm32-client-capture.pcapng").write_bytes(FIXTURE.Pcap(self.Ports))
        (self.ServerCapture / "farm32-capture-summary.txt").write_text("Total Events  Lost  0\n", encoding="utf-8")
        Save(self.ClientCapture / "farm32-client-capture.json", {
            "Format": "GargantuanFarm32Dumpcap", "Version": 1, "RunId": RUN,
            "Profile": "Farm32Capture16GiB-v2",
            "Filter": "udp port 39450 and host 10.253.3.2",
            "DurationSeconds": 600, "AutostopKilobytes": 16777216,
            "CompletenessBytes": 15 * 1024 * 1024 * 1024, "RequestedBufferMiB": 64,
            "DumpcapSha256": "a" * 64})
        (self.ClientCapture / "farm32-dumpcap-error.txt").write_text(
            "Packets captured: 64\nPackets received/dropped on interface "
            "'\\Device\\NPF_{TEST}': 64/0 (100.0%)\n", encoding="utf-8")
        self.SealCaptures()
        self.CoordinatorResult = self.Root / "coordinator.json"
        Save(self.CoordinatorResult, {"RunId": COORDINATOR, "Success": True})
        self.Outer = self.Root / "outer.json"
        self.SealOuter()

    def SealCaptures(self):
        Extra = {"CoordinatorRunId": COORDINATOR, "State": "SEALED_UNQUALIFIED",
                 "Profile": "Farm32Capture16GiB-v2",
                 "ReadyUtc": (NOW + timedelta(seconds=1)).isoformat(),
                 "StoppedUtc": (NOW + timedelta(seconds=6)).isoformat()}
        self.ServerCaptureIndex = Index(self.ServerCapture, "capture-sha256.json",
                                        "GargantuanFarm32CaptureEvidence", "SERVER",
                                        {**Extra, "RoleIndexSha256": ACCEPTANCE.directions.Digest(self.ServerRoleIndex)})
        self.ClientCaptureIndex = Index(self.ClientCapture, "capture-sha256.json",
                                        "GargantuanFarm32CaptureEvidence", "CLIENT",
                                        {**Extra, "RoleIndexSha256": ACCEPTANCE.directions.Digest(self.ClientRoleIndex)})

    def SealOuter(self):
        Save(self.Outer, {"Format": "GargantuanFarm32OuterReceipt", "Version": 1,
                          "RunId": RUN, "CoordinatorRunId": COORDINATOR,
                          "State": "SEALED_UNQUALIFIED",
                          "CoordinatorResultSha256": ACCEPTANCE.directions.Digest(self.CoordinatorResult),
                          "Roles": {Role: {
                              "RoleIndexSha256": ACCEPTANCE.directions.Digest(RoleIndex),
                              "CaptureIndexSha256": ACCEPTANCE.directions.Digest(CaptureIndex)}
                              for Role, RoleIndex, CaptureIndex in (
                                  ("SERVER", self.ServerRoleIndex, self.ServerCaptureIndex),
                                  ("CLIENT", self.ClientRoleIndex, self.ClientCaptureIndex))}})

    def Analyze(self):
        return ACCEPTANCE.Analyze(self.ServerCaptureIndex, self.ClientCaptureIndex,
                                  self.ServerRoleIndex, self.ClientRoleIndex,
                                  self.Outer, self.CoordinatorResult)

    def test_sealed_nonce_bound_bidirectional_zero_loss(self):
        Result = self.Analyze()
        self.assertEqual(Result["Status"], "MEASURED_PASS")
        self.assertEqual(Result["NonceBoundTuples"], 32)
        self.assertEqual(Result["Loss"]["ClientDroppedPackets"], 0)

    def test_missing_nonce_bound_port_fails_closed(self):
        Path = self.ServerRole / "server.stdout.log"
        Path.write_text(Path.read_text().replace(" client_port=49007", ""), encoding="utf-8")
        self.ServerRoleIndex = Index(self.ServerRole, "evidence-sha256.json", "role", "Server", {})
        self.SealCaptures()
        self.SealOuter()
        with self.assertRaisesRegex(ValueError, "nonce-to-port"):
            self.Analyze()

    def test_wrong_nonce_port_binding_fails_even_when_capture_has_32_tuples(self):
        Path = self.ServerRole / "server.stdout.log"
        Path.write_text(Path.read_text().replace("client_port=49007", "client_port=49999"), encoding="utf-8")
        self.ServerRoleIndex = Index(self.ServerRole, "evidence-sha256.json", "role", "Server", {})
        self.SealCaptures()
        self.SealOuter()
        with self.assertRaisesRegex(ValueError, "captured UDP tuples differ"):
            self.Analyze()

    def test_duplicate_nonce_port_fails(self):
        Path = self.ServerRole / "server.stdout.log"
        Path.write_text(Path.read_text().replace("client_port=49007", "client_port=49006"), encoding="utf-8")
        self.ServerRoleIndex = Index(self.ServerRole, "evidence-sha256.json", "role", "Server", {})
        self.SealCaptures()
        self.SealOuter()
        with self.assertRaisesRegex(ValueError, "duplicate or invalid"):
            self.Analyze()

    def test_missing_direction_and_truncated_capture_fail(self):
        Pcap = self.ServerCapture / "farm32-worker-capture.pcapng"
        Pcap.write_bytes(FIXTURE.Pcap(self.Ports, MissingPort=49007))
        self.SealCaptures()
        self.SealOuter()
        with self.assertRaisesRegex(ValueError, "lacks a direction"):
            self.Analyze()
        Pcap.write_bytes(FIXTURE.Pcap(self.Ports, Truncated=True))
        self.SealCaptures()
        self.SealOuter()
        with self.assertRaisesRegex(ValueError, "truncated"):
            self.Analyze()

    def test_nonzero_worker_or_client_loss_fails(self):
        Summary = self.ServerCapture / "farm32-capture-summary.txt"
        Summary.write_text("Total Events Lost 1\n", encoding="utf-8")
        self.SealCaptures()
        self.SealOuter()
        with self.assertRaisesRegex(ValueError, "worker capture loss"):
            self.Analyze()
        Summary.write_text("Total Events Lost 0\n", encoding="utf-8")
        Diagnostic = self.ClientCapture / "farm32-dumpcap-error.txt"
        Diagnostic.write_text(Diagnostic.read_text().replace("64/0", "64/1"), encoding="utf-8")
        self.SealCaptures()
        self.SealOuter()
        with self.assertRaisesRegex(ValueError, "dropped packets"):
            self.Analyze()

    def test_missing_drop_counter_fails(self):
        Diagnostic = self.ClientCapture / "farm32-dumpcap-error.txt"
        Diagnostic.write_text("Packets captured: 64\n", encoding="utf-8")
        self.SealCaptures()
        self.SealOuter()
        with self.assertRaisesRegex(ValueError, "counters are missing"):
            self.Analyze()

    def test_v3_npcap_loss_fails_despite_zero_dumpcap_drops(self):
        Diagnostic = self.ClientCapture / "farm32-dumpcap-error.txt"
        Diagnostic.write_text(
            "Packets captured: 6113310\n"
            "Packets received/dropped on interface 'Ethernet 3': 6113310/1022 "
            "(pcap:1022/dumpcap:0/flushed:0/ps_ifdrop:0) (100.0%)\n", encoding="utf-8")
        self.SealCaptures()
        self.SealOuter()
        with self.assertRaisesRegex(ValueError, "dropped packets"):
            self.Analyze()

    def test_client_requested_buffer_pin_fails_closed(self):
        Marker = self.ClientCapture / "farm32-client-capture.json"
        Original = json.loads(Marker.read_text())
        for Value in (None, 2, 63, 65, "64"):
            Row = {**Original, "RequestedBufferMiB": Value}
            if Value is None:
                del Row["RequestedBufferMiB"]
            Save(Marker, Row)
            self.SealCaptures()
            self.SealOuter()
            with self.subTest(Value=Value), self.assertRaisesRegex(ValueError, "client capture marker"):
                self.Analyze()

    def test_worker_owner_port_or_etl_is_not_assumed(self):
        Marker = self.ServerCapture / "farm32-netsh-owner.json"
        Row = json.loads(Marker.read_text())
        Row["CapturePort"] = 39452
        Save(Marker, Row)
        self.SealCaptures()
        self.SealOuter()
        with self.assertRaisesRegex(ValueError, "ownership or completeness"):
            self.Analyze()
        Row["CapturePort"] = 39450
        Save(Marker, Row)
        (self.ServerCapture / "farm32-worker-capture.etl").write_bytes(b"")
        self.SealCaptures()
        self.SealOuter()
        with self.assertRaisesRegex(ValueError, "ownership or completeness"):
            self.Analyze()

    def test_old_or_mixed_capture_profiles_fail_closed(self):
        for Root, Name, Fields in (
            (self.ServerCapture, "farm32-netsh-owner.json", {"TraceMaximumMiB": 1024, "NoWrapThresholdMiB": 960}),
            (self.ClientCapture, "farm32-client-capture.json", {"AutostopKilobytes": 1048576}),
            (self.ServerCapture, "farm32-netsh-owner.json", {"Profile": "old"}),
            (self.ClientCapture, "farm32-client-capture.json", {"Profile": "old"}),
        ):
            File = Root / Name
            Original = json.loads(File.read_text())
            Save(File, {**Original, **Fields})
            self.SealCaptures()
            self.SealOuter()
            with self.subTest(Name=Name, Fields=Fields), self.assertRaisesRegex(ValueError, "marker is invalid"):
                self.Analyze()
            Save(File, Original)

    def test_outer_receipt_and_role_hash_mismatch_fail(self):
        Outer = json.loads(self.Outer.read_text())
        Outer["Roles"]["CLIENT"]["CaptureIndexSha256"] = "b" * 64
        Save(self.Outer, Outer)
        with self.assertRaisesRegex(ValueError, "not bound"):
            self.Analyze()
        self.SealOuter()
        Path = self.ServerRole / "server.stdout.log"
        Path.write_text(Path.read_text() + "changed\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "changed indexed"):
            self.Analyze()

    def test_capture_did_not_cover_role_fails(self):
        Result = self.ServerRole / "result.json"
        Row = json.loads(Result.read_text())
        Row["CompletedUtc"] = (NOW + timedelta(seconds=7)).isoformat()
        Save(Result, Row)
        self.ServerRoleIndex = Index(self.ServerRole, "evidence-sha256.json", "role", "Server", {})
        self.SealCaptures()
        self.SealOuter()
        with self.assertRaisesRegex(ValueError, "did not cover"):
            self.Analyze()


if __name__ == "__main__":
    unittest.main()
