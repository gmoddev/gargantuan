"""Non-capture tests. Native helper self-test is opt-in after compile-only validation."""
import os
import importlib.util
import copy
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("SchedulerTraceValidate", ROOT / "tools/ci/SchedulerTraceValidate.py")
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)


class SchedulerTraceTests(unittest.TestCase):
    def Fixture(self):
        Metadata = {"Format": "GargantuanSchedulerTrace", "Version": 1}
        for Name in ("StartStatus", "QueryStatus", "StopStatus", "EventsLost", "LogBuffersLost", "RealTimeBuffersLost",
                     "HeaderEventsLost", "HeaderBuffersLost", "DecodeOpenStatus", "DecodeProcessStatus", "UnsupportedEvents"):
            Metadata[Name] = 0
        for Name in ("DiagnosticComplete", "ChildResumed", "ChildTreeReaped"):
            Metadata[Name] = True
        for Name in ("TimedOut", "ChildLogCapped", "ChildLogFailed", "CsvCapped", "DecodeTimedOut"):
            Metadata[Name] = False
        Metadata.update(ChildLaunchAttempts=1, ClockType=1, QpcFrequency=10000000, ControllerQpcFrequency=10000000,
                        ControllerStartQpc=1, AfterTraceStartQpc=2, BeforeChildResumeQpc=3, AfterChildExitQpc=1000,
                        ControllerEndQpc=1001, HeaderStartFileTime=1, HeaderEndFileTime=2,
                        ChildPid=42, ChildMainTid=43, MainFirstQpc=4, MainLastQpc=999)
        Lines = []
        for Boundary, Tick in (("BEGIN", 10), ("END", 900)):
            Lines.append(f"[Qualification:ClockAnchor] case=recovery profile=FULL_RESERVATION boundary={Boundary} pid=42 native_tid=43 native_valid=1 steady_ns={Tick * 100} qpc_before={Tick} qpc_after={Tick + 1} qpc_frequency=10000000")
        return Metadata, "\n".join(Lines)

    def test_loss_free_fixture_window_retained_without_causal_claim(self):
        Result = VALIDATOR.Validate(*self.Fixture())
        self.assertEqual(Result["State"], "LOSS_FREE_ANCHOR_WINDOW_RETAINED", Result)
        self.assertEqual(Result["CausalVerdict"], "NOT_CLAIMED")

    def test_loss_clock_coverage_and_cleanup_mutations_fail(self):
        Metadata, Log = self.Fixture()
        for Name, Value in (("EventsLost", 1), ("HeaderBuffersLost", 1), ("MainFirstQpc", 11),
                            ("MainLastQpc", 899), ("ChildPid", 9), ("ChildMainTid", 9),
                            ("ControllerQpcFrequency", 1), ("ClockType", 2), ("CsvCapped", True),
                            ("ChildTreeReaped", False), ("TimedOut", True), ("ChildLogCapped", True),
                            ("UnsupportedEvents", 1), ("StopStatus", 1), ("BeforeChildResumeQpc", 11)):
            Changed = copy.deepcopy(Metadata)
            Changed[Name] = Value
            with self.subTest(Name=Name):
                self.assertEqual(VALIDATOR.Validate(Changed, Log)["State"], "INCOMPLETE")

    def test_missing_malformed_or_duplicate_anchors_fail(self):
        Metadata, Log = self.Fixture()
        for Bad in ("", Log.splitlines()[0], Log + "\n" + Log, Log.replace("native_valid=1", "native_valid=0"),
                    Log.replace("qpc_before=10", "qpc_before=INVALID")):
            self.assertEqual(VALIDATOR.Validate(Metadata, Bad)["State"], "INCOMPLETE")
    def test_wrapper_exit_precedence_without_operational_calls(self):
        Result = subprocess.run(
            ["pwsh", "-NoProfile", "-File", str(ROOT / "tools/ci/SchedulerTrace.ps1"), "-SelfTest"],
            text=True, capture_output=True, timeout=30,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        self.assertEqual(Result.returncode, 0, Result.stdout + Result.stderr)
        self.assertIn("wrapper-self-test=PASS no-session-or-child-created", Result.stdout)

    @unittest.skipUnless(os.environ.get("SCHEDULER_TRACE_TEST_HELPER"), "native compile-only helper not supplied")
    def test_native_payload_bounds_and_ownership_without_capture(self):
        Result = subprocess.run(
            [os.environ["SCHEDULER_TRACE_TEST_HELPER"], "--self-test"],
            text=True, capture_output=True, timeout=30,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        self.assertEqual(Result.returncode, 0, Result.stdout + Result.stderr)
        self.assertIn("self-test=PASS v5-layout=PASS no-session-or-child-created", Result.stdout)

    @unittest.skipUnless(os.environ.get("SCHEDULER_TRACE_TEST_HELPER"), "native compile-only helper not supplied")
    def test_native_rejects_reserved_guid_before_any_trace(self):
        Result = subprocess.run(
            [os.environ["SCHEDULER_TRACE_TEST_HELPER"], "--run", str(ROOT), "00000000-0000-0000-0000-000000000000"],
            text=True, capture_output=True, timeout=30,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        self.assertEqual(Result.returncode, 125)
        self.assertIn("reserved session GUID", Result.stderr)


if __name__ == "__main__":
    unittest.main()
