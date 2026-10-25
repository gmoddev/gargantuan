"""Offline custody and full-matrix checks for actual Farm32 clock receipts."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / "farm_clock_exchange.py"
SPEC = importlib.util.spec_from_file_location("clock_receipt", SOURCE)
Clock = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(Clock)
FIXTURE_SPEC = importlib.util.spec_from_file_location("clock_fixture", Path(__file__).with_name("make_farm_clock_fixture.py"))
Fixture = importlib.util.module_from_spec(FIXTURE_SPEC)
FIXTURE_SPEC.loader.exec_module(Fixture)
RUN = "12345678-1234-1234-1234-123456789abc"


class ClockReceiptTests(unittest.TestCase):
    def setUp(self):
        self.Temp = tempfile.TemporaryDirectory(prefix="clock-receipt-")
        self.addCleanup(self.Temp.cleanup)
        self.Root = Path(self.Temp.name)
        self.Server, self.Clients = self.Root / "server", self.Root / "clients"
        self.Server.mkdir(); self.Clients.mkdir()
        self.Manifest = self.Root / "manifest.json"
        self.Save(self.Manifest, {"Format": "GargantuanPhysicalFarmEndpoint", "Version": 1,
                  "ScaleWorkload": True, "RunId": RUN,
                  "Nonces": [str((123 << 32) + Slot + 1) for Slot in range(32)]})
        (self.Server / "server.stdout.log").write_text("")
        for Slot in range(32): (self.Clients / f"client-{Slot:02d}.stdout.log").write_text("")
        Fixture.Append(self.Manifest, self.Server, self.Clients)
        self.Index()

    def Save(self, File, Value):
        File.write_text(json.dumps(Value), encoding="utf-8")

    def Index(self):
        for Root, Role in ((self.Server, "Server"), (self.Clients, "Clients")):
            self.Save(Root / "evidence-sha256.json", {"RunId": RUN, "Role": Role, "Files": [
                {"Name": File.name, "Bytes": File.stat().st_size,
                 "Sha256": hashlib.sha256(File.read_bytes()).hexdigest()}
                for File in sorted(Root.glob("*.log"))]})

    def Analyze(self):
        return Clock.AnalyzeIndexed(self.Server / "evidence-sha256.json",
                                    self.Clients / "evidence-sha256.json", self.Manifest)

    def Change(self, File, Before, After, Reindex=True):
        Text = File.read_text()
        self.assertIn(Before, Text)
        File.write_text(Text.replace(Before, After, 1), encoding="utf-8")
        if Reindex: self.Index()

    def test_full_matrix_and_pins(self):
        Result = self.Analyze()
        self.assertEqual(Result["Status"], "BOUNDED_AT_PROBE")
        self.assertEqual(len(Result["Samples"]), 640)
        self.assertEqual(len(Result["Sources"]), 33)
        self.assertEqual(Result["Samples"][0]["LowerNs"], -50)
        self.assertEqual(Result["Samples"][0]["UpperNs"], 100)
        self.assertEqual(Result["PhaseLongOffset"], "NOT_MEASURED")
        self.assertEqual(Result["OneWayLatency"], "NOT_MEASURED")
        self.assertEqual(Result["AnalyzerSha256"], hashlib.sha256(SOURCE.read_bytes()).hexdigest())
        for Role in ("Server", "Client"):
            Root = self.Server if Role == "Server" else self.Clients
            self.assertEqual(Result[Role + "IndexSha256"], hashlib.sha256((Root / "evidence-sha256.json").read_bytes()).hexdigest())

    def test_cli_matches_callable(self):
        Result = subprocess.run([sys.executable, str(SOURCE), str(self.Server / "evidence-sha256.json"),
                                 str(self.Clients / "evidence-sha256.json"), str(self.Manifest)],
                                capture_output=True, text=True, timeout=10, check=True)
        self.assertEqual(json.loads(Result.stdout), self.Analyze())

    def test_changed_log_without_index_refresh_rejected(self):
        self.Change(self.Clients / "client-00.stdout.log", "bytes=80", "bytes=81", False)
        with self.assertRaisesRegex(ValueError, "hash"): self.Analyze()

    def test_rehashed_missing_native_stage_rejected(self):
        File = self.Clients / "client-31.stdout.log"
        self.Change(File, "stage=GnsReceive", "stage=missing")
        with self.assertRaisesRegex(ValueError, "native stage"): self.Analyze()

    def test_equal_total_cannot_hide_missing_probe_coordinate(self):
        for Root in (self.Server, self.Clients):
            for File in Root.glob("*.log"):
                File.write_text(File.read_text().replace("epoch=1 sequence=4 request=4",
                                                        "epoch=1 sequence=3 request=4"))
        self.Index()
        with self.assertRaisesRegex(ValueError, "matrix coverage"): self.Analyze()

    def test_rehashed_manifest_nonce_mismatch_rejected(self):
        Manifest = json.loads(self.Manifest.read_text())
        Manifest["Nonces"] = [str((124 << 32) + Slot + 1) for Slot in range(32)]
        self.Save(self.Manifest, Manifest)
        with self.assertRaisesRegex(ValueError, "nonce mismatch"): self.Analyze()

    def test_foreign_clock_run_rejected_even_with_valid_complete_trace(self):
        File = self.Server / "server.stdout.log"
        with File.open("a") as Stream:
            Stream.write("[Qualification:FarmClock] event=terminal run=foreign\n")
        self.Index()
        with self.assertRaisesRegex(ValueError, "foreign run"): self.Analyze()

    def test_historical_absent_clock_is_not_measured(self):
        for Root in (self.Server, self.Clients):
            for File in Root.glob("*.log"):
                File.write_text("".join(Line for Line in File.read_text().splitlines(True)
                                       if not Line.startswith(Clock.CLOCK_PREFIX)))
        self.Index()
        Result = self.Analyze()
        self.assertEqual(Result["Status"], "NOT_MEASURED")
        self.assertEqual(Result["Samples"], [])
        self.assertEqual(len(Result["Sources"]), 33)

    def test_partial_clock_never_legacy_fallback(self):
        for File in self.Clients.glob("*.log"):
            File.write_text("".join(Line for Line in File.read_text().splitlines(True)
                                   if not Line.startswith(Clock.CLOCK_PREFIX)))
        self.Index()
        with self.assertRaisesRegex(ValueError, "trace incomplete"): self.Analyze()

    def test_wrong_index_role_rejected(self):
        File = self.Clients / "evidence-sha256.json"
        Row = json.loads(File.read_text()); Row["Role"] = "Server"; self.Save(File, Row)
        with self.assertRaisesRegex(ValueError, "index identity"): self.Analyze()

    def test_duplicate_index_member_rejected(self):
        File = self.Clients / "evidence-sha256.json"
        Row = json.loads(File.read_text()); Row["Files"].append(Row["Files"][0]); self.Save(File, Row)
        with self.assertRaisesRegex(ValueError, "duplicate"): self.Analyze()

    def test_unindexed_client_rejected(self):
        File = self.Clients / "evidence-sha256.json"
        Row = json.loads(File.read_text()); Row["Files"].pop(); self.Save(File, Row)
        with self.assertRaisesRegex(ValueError, "required clock log"): self.Analyze()

    def test_metadata_and_file_bounds(self):
        File = self.Clients / "client-00.stdout.log"
        File.write_text((f"[Qualification:FarmClock] event=terminal run={RUN}\n") * (Clock.RETAINED_LINES + 1))
        self.Index()
        with self.assertRaisesRegex(ValueError, "metadata bound"): self.Analyze()
        Index = self.Clients / "evidence-sha256.json"
        Row = json.loads(Index.read_text()); Row["Files"][0]["Bytes"] = Clock.LOG_LIMIT + 1; self.Save(Index, Row)
        with self.assertRaisesRegex(ValueError, "clock log missing or invalid"): self.Analyze()


if __name__ == "__main__": unittest.main()
