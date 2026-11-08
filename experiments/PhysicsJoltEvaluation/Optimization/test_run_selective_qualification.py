"""Focused controller tests; no native benchmark process is launched."""

import contextlib
import hashlib
import importlib.util
import io
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock


Spec = importlib.util.spec_from_file_location(
    "SelectiveRunner", Path(__file__).with_name("RunSelectiveQualification.py"))
Runner = importlib.util.module_from_spec(Spec)
Spec.loader.exec_module(Runner)


class RunnerTests(unittest.TestCase):
    def test_parsed_arguments_and_failed_physical_run_are_preserved(self):
        with tempfile.TemporaryDirectory() as Temporary:
            Root = Path(Temporary)
            Stock = Root / "stock.exe"
            Patched = Root / "patched.exe"
            Stock.write_bytes(b"stock binary")
            Patched.write_bytes(b"patched binary")
            Output = Root / "evidence"
            FailedOnce = False

            def FakeRun(Command, **Keywords):
                nonlocal FailedOnce
                self.assertTrue(Keywords["capture_output"])
                Mode = Command[Command.index("--mode") + 1]
                Workers = int(Command[Command.index("--workers") + 1])
                if Mode.startswith("refusal-"):
                    Row = {"RefusedBeforeUpdate": True}
                    return subprocess.CompletedProcess(Command, 0, json.dumps(Row) + "\n", "")
                Backend = "Stock" if Command[0] == str(Stock) else "Patched"
                Layout = Command[Command.index("--layout") + 1]
                Fail = Backend == "Stock" and Mode == "global" and Workers == 1 and not FailedOnce
                FailedOnce |= Fail
                Row = {"BenchmarkQualityPass": not Fail,
                       "Layout": Layout, "InitialQualityPass": True,
                       "DenseContactPass": True, "RigidContactFrames": 1,
                       "IsolatedFiniteEveryFrame": not Fail,
                       "CoupledFiniteEveryFrame": True,
                       "RequiresFullInternalCouplingQualification": True,
                       "CoupledTrajectoryHash": 111 if Backend == "Stock" else 222,
                       "TotalUpdateMs": 20.0 if Mode == "global" else 12.0,
                       "GuardMs": 0.1, "QualityMs": 0.2, "SceneSetupMs": 1.0,
                       "MaxIsolatedSpeedAcrossFrames": 1.0,
                       "MaxIsolatedPenetrationAcrossFrames": 0.01,
                       "MaxFloorPenetrationAcrossFrames": 0.0,
                       "MaxEdgeStretchAcrossFrames": 0.02}
                return subprocess.CompletedProcess(Command, 3 if Fail else 0,
                                                   json.dumps(Row) + "\n", "physical fail" if Fail else "")

            with mock.patch.object(Runner.subprocess, "run", side_effect=FakeRun):
                with contextlib.redirect_stdout(io.StringIO()):
                    Runner.Main(["--stock", str(Stock), "--patched", str(Patched),
                                 "--frames", "1", "--isolated-bodies", "1",
                                 "--repeats", "2", "--layout", "dense",
                                 "--output-dir", str(Output)])

            Receipts = json.loads((Output / "Receipts.json").read_text(encoding="utf-8"))
            Summary = json.loads((Output / "Summary.json").read_text(encoding="utf-8"))
            self.assertEqual(len(Receipts["Calls"]), 6 + 2 * 3 * 2 * 2)
            Failed = [Call for Call in Receipts["Calls"] if Call["ExitCode"] == 3]
            self.assertEqual(len(Failed), 1)
            self.assertIn("physical fail", (Output / Failed[0]["Stderr"]).read_text(encoding="utf-8"))
            self.assertIn('"BenchmarkQualityPass": false',
                          (Output / Failed[0]["Stdout"]).read_text(encoding="utf-8"))
            self.assertEqual(Failed[0]["StdoutSha256"], hashlib.sha256(
                (Output / Failed[0]["Stdout"]).read_bytes()).hexdigest())
            self.assertEqual(Failed[0]["StderrSha256"], hashlib.sha256(
                (Output / Failed[0]["Stderr"]).read_bytes()).hexdigest())
            self.assertEqual(Failed[0]["Command"][0], str(Stock))
            self.assertEqual(len(Failed[0]["BinarySha256"]), 64)
            self.assertGreaterEqual(Failed[0]["ElapsedSeconds"], 0.0)
            self.assertFalse(Summary["BenchmarkQualityPassByBackend"]["Stock"])
            self.assertTrue(Summary["BenchmarkQualityPassByBackend"]["Patched"])
            self.assertEqual(Summary["Layout"], "dense")
            self.assertIsNone(next(Row for Row in Summary["Summary"] if Row["Backend"] == "Stock")
                              ["MedianPairedSpeedup"])
            self.assertGreater(next(Row for Row in Summary["Summary"] if Row["Backend"] == "Patched")
                               ["MedianPairedSpeedup"], 1.0)

    def test_process_failure_keeps_raw_receipt_before_raise(self):
        with tempfile.TemporaryDirectory() as Temporary:
            Output = Path(Temporary)
            RecorderValue = Runner.Recorder(Output, {"Stock": "a" * 64})
            with mock.patch.object(Runner.subprocess, "run", return_value=
                                   subprocess.CompletedProcess(["fake"], 9, "broken\n", "crash\n")):
                Receipt = RecorderValue.Run("Stock", Path("fake"), "global", 1, 1, 1,
                                            "difference", "sparse", 0)
            with self.assertRaises(RuntimeError):
                Runner.QualityRow(Receipt)
            self.assertEqual((Output / Receipt["Stdout"]).read_text(encoding="utf-8"), "broken\n")
            self.assertEqual((Output / Receipt["Stderr"]).read_text(encoding="utf-8"), "crash\n")
            self.assertEqual(json.loads((Output / "Receipts.json").read_text(encoding="utf-8"))
                             ["Calls"][0]["ExitCode"], 9)


if __name__ == "__main__":
    unittest.main()
