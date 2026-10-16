import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from farm_server_tick import Analyze, AnalyzeIndexed, RECORD


class FarmServerTickTests(unittest.TestCase):
    def setUp(self):
        self.Temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.Temp.cleanup)
        self.Root = Path(self.Temp.name)
        self.RunId = "00000000-0000-4000-8000-000000000001"
        self.Rows = self.ValidRows()

    @staticmethod
    def ValidRows(DurationAt=None):
        Rows = []
        Tick = 0
        for Phase in range(5):
            for Offset in range(262):
                Tick += 1
                Started = Tick * 150_000_000
                Duration = DurationAt(Tick, Phase, Offset) if DurationAt else 1_000_000
                if Offset == 0:
                    Rows.append((2, Phase, 0, 0, Tick, Started + 100_000, 0))
                if Offset == 261:
                    Rows.append((3, Phase, 0, 0, Tick, Started + 900_000, 0))
                Rows.append((1, 0, 0, 0, Tick, Started, Started + Duration))
        return Rows

    def Write(self, Rows=None, Dropped=0, Invalid=0):
        Rows = self.Rows if Rows is None else Rows
        PathValue = self.Root / "server-work-ticks.bin"
        Header = (f"format=GargantuanFarmServerTicksV1\trun={self.RunId}"
                  f"\tcount={len(Rows)}\tdropped={Dropped}\tinvalid={Invalid}\n").encode()
        PathValue.write_bytes(Header + b"".join(RECORD.pack(*Row) for Row in Rows))
        return PathValue

    def test_valid_complete_five_phase_trace(self):
        Result = Analyze(self.Write(), self.RunId)
        self.assertEqual(Result["Status"], "MEASURED_PASS")
        self.assertEqual(Result["CompletedTicks"], 1310)
        self.assertEqual([Value["Ticks"] for Value in Result["Phases"]], [262] * 5)
        self.assertEqual(Result["CrossHostLatency"], "NOT_MEASURED")

    def test_one_over_limit_tick_fails_without_changing_quantiles(self):
        self.Rows = self.ValidRows(lambda Tick, Phase, Offset: 100_000_001 if Phase == 2 and Offset == 120 else 1_000_000)
        Result = Analyze(self.Write(), self.RunId)
        self.assertEqual(Result["Status"], "MEASURED_FAIL")
        self.assertEqual(Result["Phases"][2]["MaximumWorkNanoseconds"], 100_000_001)
        self.assertEqual(Result["Phases"][2]["Status"], "MEASURED_FAIL")

    def test_canonical_p95_and_p99_bounds(self):
        self.Rows = self.ValidRows(lambda Tick, Phase, Offset:
                                   17_000_000 if Phase == 0 and Offset < 20 else
                                   34_000_000 if Phase == 1 and Offset < 5 else 1_000_000)
        Result = Analyze(self.Write(), self.RunId)
        self.assertGreater(Result["Phases"][0]["P95WorkNanoseconds"], 16_667_000)
        self.assertGreater(Result["Phases"][1]["P99WorkNanoseconds"], 33_334_000)
        self.assertEqual([Value["Status"] for Value in Result["Phases"]],
                         ["MEASURED_FAIL", "MEASURED_FAIL", "MEASURED_PASS", "MEASURED_PASS", "MEASURED_PASS"])

    def test_exactly_at_limits_passes(self):
        self.Rows = self.ValidRows(lambda Tick, Phase, Offset:
                                   100_000_000 if Phase == 0 and Offset == 120 else
                                   33_334_000 if Phase == 1 and Offset < 5 else
                                   16_667_000 if Phase == 2 and Offset < 20 else 1_000_000)
        self.assertEqual(Analyze(self.Write(), self.RunId)["Status"], "MEASURED_PASS")

    def test_missing_tick_and_marker_rejected(self):
        MissingTick = [Row for Row in self.Rows if not (Row[0] == 1 and Row[4] == 100)]
        with self.assertRaisesRegex(ValueError, "missing|bracketed"):
            Analyze(self.Write(MissingTick), self.RunId)
        MissingMarker = [Row for Row in self.Rows if not (Row[0] == 3 and Row[1] == 4)]
        with self.assertRaisesRegex(ValueError, "missing"):
            Analyze(self.Write(MissingMarker), self.RunId)

    def test_overflow_invalid_and_reserved_rejected(self):
        with self.assertRaisesRegex(ValueError, "overflowed"):
            Analyze(self.Write(Dropped=1), self.RunId)
        with self.assertRaisesRegex(ValueError, "overflowed"):
            Analyze(self.Write(Invalid=1), self.RunId)
        Rows = list(self.Rows)
        Rows[5] = (*Rows[5][:2], 1, *Rows[5][3:])
        with self.assertRaisesRegex(ValueError, "invalid fields"):
            Analyze(self.Write(Rows), self.RunId)

    def test_indexed_trace_hash_and_run_pin(self):
        Trace = self.Write()
        Manifest = self.Root / "run-manifest.json"
        Manifest.write_text(json.dumps({"RunId": self.RunId, "ScaleWorkload": True,
                                        "ServerTicks": 9000}), encoding="utf-8")
        def Entry(PathValue):
            Bytes = PathValue.read_bytes()
            return {"Name": PathValue.name, "Bytes": len(Bytes),
                    "Sha256": hashlib.sha256(Bytes).hexdigest()}
        Index = self.Root / "evidence-sha256.json"
        Index.write_text(json.dumps({"Role": "Server", "RunId": self.RunId,
                                     "Files": [Entry(Trace), Entry(Manifest)]}), encoding="utf-8")
        self.assertEqual(AnalyzeIndexed(Index)["Status"], "MEASURED_PASS")
        Trace.write_bytes(Trace.read_bytes() + b"x")
        with self.assertRaisesRegex(ValueError, "hash or size"):
            AnalyzeIndexed(Index)


if __name__ == "__main__":
    unittest.main()
