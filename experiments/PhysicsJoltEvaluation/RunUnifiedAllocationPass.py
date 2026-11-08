"""Separate native Jolt allocation census for a selected experimental profile.

The instrumented pass includes harness allocation work and is never a timing
ranking. Its captured resource endpoint precedes result output and teardown.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess


Cases = ("RigidIntoCloth", "Multiple", "RubberPush", "SphereOnCloth", "Repeated")
IgnoredFrameFields = {"StepMs", "FrameWallMs", "AcceptedUpdates", "ObservedInternalCallbacks"}
CreationFlags = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def HashFile(PathName):
    Digest = hashlib.sha256()
    with PathName.open("rb") as Source:
        for Chunk in iter(lambda: Source.read(1024 * 1024), b""):
            Digest.update(Chunk)
    return Digest.hexdigest()


def PhysicalFrames(Result):
    return [{Key: Value for Key, Value in Frame.items() if Key not in IgnoredFrameFields}
            for Frame in Result["Frames"]]


def Run(Binary, Output, Name, Options):
    StdoutPath = Output / f"{Name}.json"
    StderrPath = Output / f"{Name}.stderr.txt"
    with StdoutPath.open("w", encoding="utf-8") as Stdout, StderrPath.open("w", encoding="utf-8") as Stderr:
        Process = subprocess.run([str(Binary), *Options], stdout=Stdout, stderr=Stderr,
                                 timeout=1200, creationflags=CreationFlags)
    if Process.returncode != 0:
        raise RuntimeError(f"[Physics:UnifiedAllocation] {Name} exited {Process.returncode}; see {StderrPath}")
    Result = json.loads(StdoutPath.read_text(encoding="utf-8"))
    return Result, {"ExitCode": Process.returncode, "Raw": StdoutPath.name,
                    "Stderr": StderrPath.name, "RawSha256": HashFile(StdoutPath)}


def ProfileMatches(Result, Profile):
    return (Result.get("Backend") == "Jolt" and
            Result.get("WorldUpdatesPerFrame") == Profile["WorldUpdatesPerFrame"] and
            Result.get("CollisionSteps") == Profile["CollisionSteps"] and
            Result.get("SolverIterations") == Profile["SoftIterations"] and
            math.isclose(float(Result.get("SelectedStretchCompliance", float("nan"))),
                         Profile["StretchCompliance"], rel_tol=1.0e-6, abs_tol=0.0) and
            Result.get("VelocityReconstruction", "difference") == Profile["VelocityReconstruction"] and
            len(Result.get("Frames", [])) == Profile["Frames"] and
            Result.get("EnergyObserverEnabled") is False and
            Result.get("AcceptedObservationCoverage") is False)


def ValidResources(Resources, Instrumented):
    if not isinstance(Resources, dict) or Resources.get("AllocationInstrumentation") is not Instrumented:
        return False
    if not Instrumented:
        return Resources.get("AllocationCalls") is None
    Required = ("AllocationCalls", "FreeCalls", "AllocatedBytes", "FreedBytes",
                "LiveBytesDelta", "TrackedPeakProcessLifetimeBytes")
    if not all(isinstance(Resources.get(Key), int) for Key in Required):
        return False
    return (all(Resources[Key] >= 0 for Key in Required if Key != "LiveBytesDelta") and
            isinstance(Resources.get("AllocationScope"), str) and
            "Jolt hooks" in Resources["AllocationScope"])


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--allocations", type=Path, required=True)
    Parser.add_argument("--pristine", type=Path, required=True)
    Parser.add_argument("--output", type=Path, required=True)
    Parser.add_argument("--world-updates", type=int, default=1)
    Parser.add_argument("--collision-steps", type=int, default=8)
    Parser.add_argument("--soft-iterations", type=int, default=24)
    Parser.add_argument("--velocity-reconstruction",
                        choices=("difference", "delta", "applieddelta"), default="difference")
    Parser.add_argument("--compliance", type=float, default=1.0e-6)
    Arguments = Parser.parse_args()
    if (not 1 <= Arguments.world_updates <= 384 or
            not 1 <= Arguments.collision_steps <= 8 or
            not 1 <= Arguments.soft_iterations <= 64 or
            Arguments.world_updates * Arguments.collision_steps * Arguments.soft_iterations > 2304 or
            not math.isfinite(Arguments.compliance) or Arguments.compliance <= 0.0):
        Parser.error("[Physics:UnifiedAllocation] Selected profile outside bounded native diagnostic range")
    Allocations = Arguments.allocations.resolve(strict=True)
    Pristine = Arguments.pristine.resolve(strict=True)
    Output = Arguments.output
    Output.mkdir(parents=True, exist_ok=False)
    SelectedProfile = {"WorldUpdatesPerFrame": Arguments.world_updates,
                       "CollisionSteps": Arguments.collision_steps,
                       "SoftIterations": Arguments.soft_iterations,
                       "StretchCompliance": Arguments.compliance,
                       "VelocityReconstruction": Arguments.velocity_reconstruction,
                       "Frames": 150, "Workers": 1, "ObserveAccepted": False}
    ModeOptions = [] if Arguments.velocity_reconstruction == "difference" else [
        "--velocity-reconstruction", Arguments.velocity_reconstruction]
    Receipts = []
    for Case in Cases:
        Options = ["--backend", "Jolt", "--case", Case, "--workers", "1",
                   "--frames", "150", "--world-updates", str(Arguments.world_updates),
                   "--collision-steps", str(Arguments.collision_steps),
                   "--soft-iterations", str(Arguments.soft_iterations),
                   "--stretch-compliance", format(Arguments.compliance, ".12g"),
                   "--observe-accepted", "0", *ModeOptions]
        Counted, CountedRun = Run(Allocations, Output, f"{Case}-Allocations", Options)
        Plain, PlainRun = Run(Pristine, Output, f"{Case}-Pristine", Options)
        CountedFrames = PhysicalFrames(Counted)
        PlainFrames = PhysicalFrames(Plain)
        CountedControl = Counted.get("ContactDisabledControl", {})
        PlainControl = Plain.get("ContactDisabledControl", {})
        Checks = {
            "ProfileMetadata": all(ProfileMatches(Result, SelectedProfile) for Result in
                                   (Counted, Plain, CountedControl, PlainControl)),
            "InstrumentedAllocationHooks": ValidResources(Counted.get("Resources"), True),
            "PristineUninstrumented": ValidResources(Plain.get("Resources"), False),
            "ExactFrameParity": CountedFrames == PlainFrames,
            "ExactContactDisabledFrameParity": PhysicalFrames(CountedControl) == PhysicalFrames(PlainControl),
        }
        Receipt = {"Case": Case, "SelectedProfile": SelectedProfile, "Options": Options,
                   "AllocationsRun": CountedRun,
                   "PristineRun": PlainRun, "Checks": Checks,
                   "AllocationResources": Counted.get("Resources"),
                   "CapturedLiveBytesDeltaBeforeOutputAndTeardown":
                       Counted.get("Resources", {}).get("LiveBytesDelta"),
                   "Scope": "One captured main-run resource delta encompasses contact-enabled and contact-disabled worlds plus harness allocation work after Jolt runtime initialization through result construction. It cannot apportion either world separately. Capture precedes JSON output, result destruction and Jolt runtime teardown. Tracked peak is process lifetime. Separate allocation pass; timing is not ranked.",
                   "QualificationPass": all(Checks.values())}
        Receipts.append(Receipt)
        (Output / "Receipts.json").write_text(json.dumps(Receipts, indent=2, allow_nan=False), encoding="utf-8")
        print(json.dumps({"Case": Case, "Pass": Receipt["QualificationPass"],
                          "AllocatedBytes": Receipt["AllocationResources"].get("AllocatedBytes")}), flush=True)
    Verdict = {"Type": "UnifiedNativeAllocationPass", "Profile": SelectedProfile,
               "SelectedProfile": SelectedProfile,
               "Cases": len(Receipts), "AllRequiredChecksPass": all(Item["QualificationPass"] for Item in Receipts),
               "AllocationBinarySha256": HashFile(Allocations),
               "PristineBinarySha256": HashFile(Pristine),
               "Scope": "Separate instrumented native Jolt allocation census with exact frame-state parity; no timing or backend allocation ranking."}
    (Output / "Verdict.json").write_text(json.dumps(Verdict, indent=2, allow_nan=False), encoding="utf-8")
    if not Verdict["AllRequiredChecksPass"]:
        raise SystemExit(3)


if __name__ == "__main__":
    Main()
