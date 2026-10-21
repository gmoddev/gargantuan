"""Socket-free adversarial tests for Farm32 clock evidence."""

import csv
from datetime import datetime, timedelta, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1] / "farm_clock_correlation.py"
SPEC = importlib.util.spec_from_file_location("farm_clock_correlation", SOURCE)
Clock = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(Clock)
RUN = "12345678-1234-4234-8234-123456789abc"
BASE = datetime(2026, 10, 1, tzinfo=timezone.utc)
PHASES = ("baseline", "load", "resident", "evict", "reload")


class FarmClockTests(unittest.TestCase):
    def setUp(self):
        self.Temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.Temporary.cleanup)
        self.Root = Path(self.Temporary.name)
        self.Server = self.Root / "server"
        self.Client = self.Root / "client"
        self.Server.mkdir()
        self.Client.mkdir()
        self.Build()

    def Write(self, Root, Name, Content):
        File = Root / Name
        File.write_text(Content, encoding="utf-8")

    def Json(self, Root, Name, Value):
        self.Write(Root, Name, json.dumps(Value))

    def Csv(self, Root, Name, Rows):
        File = Root / Name
        with File.open("w", newline="", encoding="utf-8") as Stream:
            Writer = csv.DictWriter(Stream, fieldnames=Rows[0].keys())
            Writer.writeheader()
            Writer.writerows(Rows)

    def ReadCsv(self, Root, Name):
        with (Root / Name).open(newline="", encoding="utf-8") as Stream:
            return list(csv.DictReader(Stream))

    def Seal(self, Root):
        Files = []
        for File in Root.iterdir():
            if File.name == "evidence-sha256.json":
                continue
            Data = File.read_bytes()
            Files.append({"Name": File.name, "Bytes": len(Data),
                          "Sha256": hashlib.sha256(Data).hexdigest()})
        self.Json(Root, "evidence-sha256.json", {"RunId": RUN, "Files": Files})

    def Stamp(self, Seconds):
        return (BASE + timedelta(seconds=Seconds)).isoformat()

    def Build(self):
        self.Json(self.Server, "result.json", {"RunId": RUN, "Role": "Server",
                 "Status": "PASS", "Pids": [400], "StartedUtc": self.Stamp(0),
                 "CompletedUtc": self.Stamp(100)})
        self.Json(self.Client, "result.json", {"RunId": RUN, "Role": "Clients",
                 "Status": "PASS", "Pids": list(range(1000, 1032)),
                 "StartedUtc": self.Stamp(0), "CompletedUtc": self.Stamp(100)})
        self.Json(self.Server, "server-ready.json", {"RunId": RUN, "Role": "Server",
                 "Pid": 400, "ReadyUtc": self.Stamp(2)})
        ServerRows = []
        ClientRows = []
        for Seconds in (3, 23):
            Common = {"RunId": RUN, "Utc": self.Stamp(Seconds),
                      "SupervisorElapsedMilliseconds": Seconds * 1000,
                      "MonotonicTicks": 1_000_000_000 + Seconds * 10_000_000,
                      "MonotonicFrequency": 10_000_000}
            ServerRows.append(dict(Common, Label="server", Pid=400))
            for Slot in range(32):
                ClientRows.append(dict(Common, Label=f"client-{Slot:02d}", Pid=1000 + Slot))
        self.Csv(self.Server, "process-resources.csv", ServerRows)
        self.Csv(self.Client, "process-resources.csv", ClientRows)
        Lines = []
        for Slot in range(32):
            Nonce = 5000 + Slot
            Lines.append(f"[Qualification:Server] event=ready run={RUN} nonce={Nonce} "
                         f"monotonic_us={1_000_000 + Slot * 1000}")
            Identity = f"run_id={RUN} slot={Slot} nonce={Nonce}"
            self.Write(self.Client, f"client-{Slot:02d}.stdout.log", "\n".join((
                f"[Qualification:Client] event=start {Identity} steady_ns={1_000_000_000 + Slot * 1000}",
                f"[Qualification:Client] event=ready {Identity} steady_ns={2_000_000_000 + Slot * 1000}",
                f"[Qualification:Client] event=result {Identity} steady_ns={11_000_000_000 + Slot * 1000}",
            )) + "\n")
        for Index, Phase in enumerate(PHASES):
            Start = 2_000_000 + Index * 15_000_000
            Lines.extend((
                f"[Qualification:Scale] event=phase_start run={RUN} phase={Phase} monotonic_us={Start}",
                f"[Qualification:Scale] event=phase_end run={RUN} phase={Phase} monotonic_us={Start + 14_000_000}",
            ))
        self.Write(self.Server, "server.stdout.log", "\n".join(Lines) + "\n")
        self.Seal(self.Server)
        self.Seal(self.Client)

    def Correlate(self):
        return Clock.Correlate(self.Server, self.Client, RUN)

    def test_complete_32_client_partial_correlation(self):
        Result = self.Correlate()
        self.assertEqual(Result["NativeClientProcesses"], 32)
        self.assertEqual(Result["CrossHostClockOffset"], "NOT_MEASURED")
        self.assertEqual(Result["CrossHostChronology"], "NONCE_CAUSAL_ORDER_ONLY")

    def test_independent_client_wall_clock_offset_is_allowed(self):
        Result = json.loads((self.Client / "result.json").read_text())
        Result["StartedUtc"] = self.Stamp(3600)
        Result["CompletedUtc"] = self.Stamp(3700)
        self.Json(self.Client, "result.json", Result)
        Rows = self.ReadCsv(self.Client, "process-resources.csv")
        for Row in Rows:
            Row["Utc"] = self.Stamp(int(Row["SupervisorElapsedMilliseconds"]) // 1000 + 3600)
        self.Csv(self.Client, "process-resources.csv", Rows)
        self.Seal(self.Client)
        self.assertEqual(self.Correlate()["CrossHostClockOffset"], "NOT_MEASURED")

    def test_native_client_clock_reversal_rejected(self):
        File = self.Client / "client-17.stdout.log"
        File.write_text(File.read_text().replace("steady_ns=2000017000", "steady_ns=1000"))
        self.Seal(self.Client)
        with self.assertRaisesRegex(ValueError, "native clock order"):
            self.Correlate()

    def test_supervisor_elapsed_drift_rejected(self):
        Rows = self.ReadCsv(self.Client, "process-resources.csv")
        Rows[-1]["SupervisorElapsedMilliseconds"] = "26000"
        self.Csv(self.Client, "process-resources.csv", Rows)
        self.Seal(self.Client)
        with self.assertRaisesRegex(ValueError, "Stopwatch/elapsed drift"):
            self.Correlate()

    def test_reassigned_process_pid_rejected(self):
        Rows = self.ReadCsv(self.Client, "process-resources.csv")
        for Row in Rows:
            if Row["Label"] == "client-03":
                Row["Pid"] = "1004"
            elif Row["Label"] == "client-04":
                Row["Pid"] = "1003"
        self.Csv(self.Client, "process-resources.csv", Rows)
        self.Seal(self.Client)
        with self.assertRaisesRegex(ValueError, "PID/slot assignment"):
            self.Correlate()

    def test_same_host_utc_clock_jump_rejected(self):
        Rows = self.ReadCsv(self.Client, "process-resources.csv")
        Rows[-1]["Utc"] = self.Stamp(3600)
        self.Csv(self.Client, "process-resources.csv", Rows)
        self.Seal(self.Client)
        with self.assertRaisesRegex(ValueError, "UTC/monotonic drift"):
            self.Correlate()

    def test_native_log_hash_tamper_rejected(self):
        File = self.Client / "client-01.stdout.log"
        File.write_text(File.read_text().replace("nonce=5001", "nonce=9001"))
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            self.Correlate()

    def test_missing_client_sample_rejected(self):
        Rows = self.ReadCsv(self.Client, "process-resources.csv")
        self.Csv(self.Client, "process-resources.csv",
                 [Row for Row in Rows if Row["Label"] != "client-31"])
        self.Seal(self.Client)
        with self.assertRaisesRegex(ValueError, "missing process samples"):
            self.Correlate()

    def test_reordered_server_phase_rejected(self):
        File = self.Server / "server.stdout.log"
        File.write_text(File.read_text().replace("monotonic_us=17000000", "monotonic_us=1000000"))
        self.Seal(self.Server)
        with self.assertRaisesRegex(ValueError, "phase monotonic order"):
            self.Correlate()


if __name__ == "__main__":
    unittest.main()
