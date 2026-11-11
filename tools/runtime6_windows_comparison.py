# Copyright (c) 2026 Team Fireworks
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

"""Frozen same-host Windows CPU comparison; physical GPU/network claims are separate."""
import argparse
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import time

if sys.platform != "win32":
    raise RuntimeError("[Runtime6:Compare] This driver requires Windows")

import ctypes
from ctypes import wintypes

ctypes.windll.kernel32.SetErrorMode(0x8003)


class ProcessMemoryCounters(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]


try:
    GetProcessMemoryInfo = ctypes.windll.psapi.GetProcessMemoryInfo
    GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessMemoryCounters), wintypes.DWORD]
    GetProcessMemoryInfo.restype = wintypes.BOOL
except (AttributeError, OSError):
    GetProcessMemoryInfo = None


def PeakWorkingSet(Process):
    if GetProcessMemoryInfo is None:
        return None
    try:
        Counters = ProcessMemoryCounters()
        Counters.cb = ctypes.sizeof(Counters)
        if GetProcessMemoryInfo(int(Process._handle), ctypes.byref(Counters), Counters.cb) and Counters.PeakWorkingSetSize > 0:
            return Counters.PeakWorkingSetSize
    except (AttributeError, OSError, TypeError, ValueError):
        pass
    return None


def Hash(File):
    return hashlib.sha256(File.read_bytes()).hexdigest()


Parser = argparse.ArgumentParser()
Parser.add_argument("--evidence", type=pathlib.Path, required=True)
Parser.add_argument("--suite", choices=("core", "cframe", "compact", "readback", "realistic", "gameplay", "signals", "signals_compare", "dirty_compare", "dirty_drain", "luau", "attribution", "compact_large", "readback_large", "cframe_allocation"), default="core")
Parser.add_argument("--comparison-directory", default="comparison")
Parser.add_argument("--verify-only", action="store_true")
Args = Parser.parse_args()
Root = Args.evidence.resolve()
if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}", Args.comparison_directory) or \
        Args.comparison_directory in ("baseline", "candidate"):
    raise RuntimeError("[Runtime6:Compare] Comparison directory must be a fresh simple basename")
ManifestSuite = {"attribution": "core", "compact_large": "compact", "readback_large": "readback", "cframe_allocation": "cframe"}.get(Args.suite, Args.suite)
Sources = {}
for Arm in ("baseline", "candidate"):
    Directory = Root / Arm
    Manifest = json.loads((Directory / "manifest.json").read_text())
    if not re.fullmatch(r"[0-9a-f]{40}", Manifest["source"]):
        raise RuntimeError(f"[Runtime6:Compare] Invalid source identity: {Arm}")
    if Manifest["suite"] != ManifestSuite:
        raise RuntimeError(f"[Runtime6:Compare] Suite mismatch: {Arm}")
    if not Manifest["binaries"] or not any(Name.lower() == "sdl3.dll" for Name in Manifest["binaries"]):
        raise RuntimeError(f"[Runtime6:Compare] Missing frozen SDL3 runtime: {Arm}")
    for Name, Digest in Manifest["binaries"].items():
        Relative = pathlib.PurePosixPath(Name)
        if Relative.is_absolute() or ".." in Relative.parts or "\\" in Name:
            raise RuntimeError(f"[Runtime6:Compare] Invalid frozen path: {Arm}/{Name}")
        File = Directory / Name
        if not re.fullmatch(r"[0-9a-f]{64}", Digest) or not File.is_file() or File.is_symlink() or Hash(File) != Digest:
            raise RuntimeError(f"[Runtime6:Compare] Changed binary: {Arm}/{Name}")
    Sources[Arm] = Manifest

if Args.verify_only:
    print(json.dumps({"verified": True, "suite": Args.suite,
                      "manifestSuite": ManifestSuite,
                      "sources": {Arm: Sources[Arm]["source"] for Arm in Sources}}), flush=True)
    raise SystemExit(0)

Output = Root / Args.comparison_directory
Output.mkdir(exist_ok=False)
(Output / "sources.json").write_text(json.dumps(Sources, indent=2))
Receipts = []


def Run(Arm, Label, Binary, Options, Timeout=600, Environment=None):
    Executable = Root / Arm / (Binary + ".exe")
    if Executable.name not in Sources[Arm]["binaries"]:
        raise RuntimeError(f"[Runtime6:Compare] Unfrozen executable: {Arm}/{Binary}")
    Name = f"{len(Receipts):03}-{Label}-{Arm}"
    Command = [str(Executable), *Options]
    Start = time.monotonic()
    Code, TimedOut, PeakBytes = None, False, None
    with (Output / (Name + ".stdout")).open("xb") as Stdout, (Output / (Name + ".stderr")).open("xb") as Stderr:
        try:
            Process = subprocess.Popen(Command, cwd=Root / Arm, stdout=Stdout, stderr=Stderr,
                                       stdin=subprocess.DEVNULL,
                                       env=Environment,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            Code = Process.wait(timeout=Timeout)
        except subprocess.TimeoutExpired:
            TimedOut = True
            # Popen owns this exact process handle; do not terminate by image name or scan other PIDs.
            Process.kill()
            Process.wait()
        PeakBytes = PeakWorkingSet(Process)
    Receipt = {"arm": Arm, "source": Sources[Arm]["source"], "label": Label,
               "command": Command, "exit": Code, "timeout": TimedOut,
               "diagnosticEnvironment": {Key: Environment.get(Key) for Key in (
                   "GARGANTUAN_QUALIFIED_SCALE", "GARGANTUAN_CONTENT_TRACE", "GARGANTUAN_PUBLICATION_LATENCY")}
                   if Environment is not None else {},
               "memoryStatus": "measured" if PeakBytes is not None else "NA",
               "peakWorkingSetBytes": PeakBytes,
               "elapsedSeconds": time.monotonic() - Start,
               "stdoutSha256": Hash(Output / (Name + ".stdout")),
               "stderrSha256": Hash(Output / (Name + ".stderr"))}
    Receipts.append(Receipt)
    (Output / "receipts.json").write_text(json.dumps(Receipts, indent=2))
    print(json.dumps(Receipt), flush=True)


if Args.suite == "dirty_drain":
    for Count in (1000, 10000, 50000):
        for Profile in ("off", "on"):
            for Arm in ("baseline", "candidate", "candidate", "baseline"):
                Run(Arm, f"dirty-drain-{Count}-{Profile}", "gargantuan_runtime6_physics_listener_probe",
                    ["--mode=native", f"--count={Count}", f"--changed={Count}", "--frames=12",
                     "--drain-physics", f"--profile={Profile}"])
    raise SystemExit(any(Entry["exit"] != 0 or Entry["timeout"] for Entry in Receipts))

if Args.suite == "luau":
    for Count in (1000, 10000, 50000):
        for Arm in ("baseline", "candidate", "candidate", "baseline"):
            Samples = Output / f"{len(Receipts):03}-live-headless-{Count}-{Arm}.csv"
            Run(Arm, f"live-headless-{Count}", "gargantuan_runtime_foundation6_benchmark",
                ["12", f"--count={Count}", "--density-matrix", "--group-width=256",
                 "--split-construction", "--draw-backend=headless", f"--samples={Samples}"])
    for Count in (1000, 10000):
        for Property in ("cframe", "color"):
            for Listeners in (0, 2):
                for Arm in ("baseline", "candidate", "candidate", "baseline"):
                    Run(Arm, f"luau-{Count}-{Property}-{Listeners}", "gargantuan_runtime6_luau_mutation_benchmark",
                        [f"--count={Count}", "--frames=12", f"--listeners={Listeners}", f"--property={Property}"])
    raise SystemExit(any(Entry["exit"] != 0 or Entry["timeout"] for Entry in Receipts))

if Args.suite in ("signals_compare", "dirty_compare"):
    for Count in (1000, 10000, 50000):
        for Arm in ("baseline", "candidate", "candidate", "baseline"):
            Samples = Output / f"{len(Receipts):03}-signal-off-{Count}-{Arm}.csv"
            Run(Arm, f"signal-off-{Count}", "gargantuan_runtime_foundation6_benchmark",
                ["12", f"--count={Count}", "--density-matrix", "--group-width=256",
                 "--split-construction", "--diagnostic-marks", f"--samples={Samples}"])
        for Mode in ("native", "renderer-only"):
            for Arm in ("baseline", "candidate", "candidate", "baseline"):
                Run(Arm, f"signal-on-{Count}-{Mode}", "gargantuan_runtime6_physics_listener_probe",
                    [f"--mode={Mode}", f"--count={Count}", f"--changed={Count}", "--frames=12"])
    raise SystemExit(any(Entry["exit"] != 0 or Entry["timeout"] for Entry in Receipts))

if Args.suite == "signals":
    if Sources["baseline"]["source"] != Sources["candidate"]["source"]:
        raise RuntimeError("[Runtime6:Compare] Signal attribution requires one common source")
    # Counterfactual listener modes are diagnostics, not candidate engine wins.
    for Count in (1000, 10000, 50000):
        for Changed in (max(1, Count // 100), Count):
            for Enabled, Disabled in (("native", "native-no-listener"),
                                      ("renderer-only", "renderer-only-no-listener")):
                for Mode in (Enabled, Disabled, Disabled, Enabled):
                    Run("candidate", f"signal-{Count}-{Changed}-{Mode}",
                        "gargantuan_runtime6_physics_listener_probe",
                        [f"--mode={Mode}", f"--count={Count}", f"--changed={Changed}", "--frames=12"])
    for Changed in (0, 100, 10000):
        Run("candidate", f"signal-alternatives-{Changed}", "gargantuan_runtime6_extraction_alternatives",
            ["10000", str(Changed), "12", "sequential", "0", "profile-signals"])
    raise SystemExit(any(Entry["exit"] != 0 or Entry["timeout"] for Entry in Receipts))

if Args.suite == "gameplay":
    def CheckGameplayInputs():
        Expected = Sources["baseline"].get("sourceInputs", {})
        if not Expected or Sources["candidate"].get("sourceInputs", {}) != Expected:
            raise RuntimeError("[Runtime6:Compare] Gameplay source assets differ or were not frozen")
        for Arm in ("baseline", "candidate"):
            InputRoot = pathlib.Path(Sources[Arm]["sourceInputRoot"])
            ActualFiles = {File.relative_to(InputRoot).as_posix()
                           for File in (InputRoot / "samples/FirstCompleteGame").rglob("*") if File.is_file()}
            if ActualFiles != set(Expected):
                raise RuntimeError("[Runtime6:Compare] Unexpected or absent gameplay source asset")
            for Name, Digest in Expected.items():
                if Hash(InputRoot / Name) != Digest:
                    raise RuntimeError(f"[Runtime6:Compare] Changed gameplay source asset: {Name}")

    CheckGameplayInputs()
    (Output / "gameplay-source-inputs.json").write_text(json.dumps(Sources["baseline"]["sourceInputs"], indent=2))
    Fixture = Root / "shared-gameplay-fixture"
    Run("baseline", "gameplay-fixture-512", "gargantuan_game_session_benchmark",
        ["--write-content-scale", str(Fixture), "512"])
    if Receipts[-1]["exit"] != 0 or Receipts[-1]["timeout"]:
        raise SystemExit(1)
    FixtureHashes = {File.relative_to(Fixture).as_posix(): Hash(File)
                     for File in Fixture.rglob("*") if File.is_file()}
    if not FixtureHashes:
        raise RuntimeError("[Runtime6:Compare] Empty gameplay fixture")
    (Output / "gameplay-fixture.json").write_text(json.dumps(FixtureHashes, indent=2))
    for Trace in (False, True):
        Environment = os.environ.copy()
        Environment["GARGANTUAN_QUALIFIED_SCALE"] = "1"
        for Key in ("GARGANTUAN_CONTENT_TRACE", "GARGANTUAN_PUBLICATION_LATENCY"):
            Environment.pop(Key, None)
        if Trace:
            Environment["GARGANTUAN_CONTENT_TRACE"] = "1"
        for Arm in ("baseline", "candidate", "candidate", "baseline"):
            CheckGameplayInputs()
            Run(Arm, f"gameplay-32-peers-8-active-trace-{Trace}", "gargantuan_game_session_benchmark",
                ["--content-differential", str(Fixture), "local", "8", "8", "32"], Environment=Environment)
            CheckGameplayInputs()
            Current = {File.relative_to(Fixture).as_posix(): Hash(File)
                       for File in Fixture.rglob("*") if File.is_file()}
            if Current != FixtureHashes:
                raise RuntimeError("[Runtime6:Compare] Gameplay changed its shared fixture")
    # Preserve service health and frame/phase health separately in raw output.
    raise SystemExit(any(Entry["exit"] != 0 or Entry["timeout"] for Entry in Receipts))

if Args.suite == "attribution":
    for Count in (1000, 10000, 50000):
        for Arm in ("baseline", "candidate", "candidate", "baseline"):
            Samples = Output / f"{len(Receipts):03}-attribution-{Count}-{Arm}.csv"
            Run(Arm, f"attribution-{Count}", "gargantuan_runtime_foundation6_benchmark",
                ["12", f"--count={Count}", "--density-matrix", "--group-width=256",
                 "--split-construction", "--diagnostic-marks", "--profile",
                 f"--samples={Samples}"])
    raise SystemExit(any(Entry["exit"] != 0 or Entry["timeout"] for Entry in Receipts))

if Args.suite in ("compact_large", "readback_large"):
    for Static in (9872, 49872):
        for Density in ("0", "0.01", "1"):
            for Lifecycle in (0, 1, 16):
                for Arm in ("baseline", "candidate", "candidate", "baseline"):
                    Run(Arm, f"compact-large-{Static + 128}-{Density}-{Lifecycle}",
                        "gargantuan_runtime6_physics_sync_benchmark",
                        ["--static-count", str(Static), "--dynamic-count", "128", "--frames", "30",
                         "--warmup", "4", "--control-density", Density,
                         "--lifecycle-per-frame", str(Lifecycle), "--publication"])
    raise SystemExit(any(Entry["exit"] != 0 or Entry["timeout"] for Entry in Receipts))

if Args.suite == "realistic":
    for Count, Changed in ((128, 0), (128, 16), (128, 128), (512, 64)):
        for Arm in ("baseline", "candidate", "candidate", "baseline"):
            Run(Arm, f"realistic-{Count}-{Changed}", "gargantuan_runtime6_realistic_publication_probe",
                [f"--count={Count}", f"--changed={Changed}", "--frames=12", "--mode=both"])
    for Label, Binary, Options in (
            ("asset-lifecycle", "gargantuan_asset_foundation_benchmark", []),
            ("animation", "gargantuan_animation_foundation_benchmark", ["--quick"]),
            ("cloth", "gargantuan_soft_body_physics_benchmark", ["--quick"]),
            ("replication", "gargantuan_replication_benchmark", ["--incremental-scaling"])):
        for Arm in ("baseline", "candidate", "candidate", "baseline"):
            Run(Arm, Label, Binary, Options)
    raise SystemExit(any(Entry["exit"] != 0 or Entry["timeout"] for Entry in Receipts))

if Args.suite == "cframe":
    for Count in (1000, 10000, 50000):
        # Stage clocks remain off in the performance comparison.
        for Profile in (False, True):
            for Arm in ("baseline", "candidate", "candidate", "baseline"):
                Samples = Output / f"{len(Receipts):03}-cframe-{Count}-{Profile}-{Arm}.csv"
                Options = ["12", f"--count={Count}", "--density-matrix", "--group-width=256",
                           "--split-construction", "--diagnostic-marks", f"--samples={Samples}"]
                if Profile:
                    Options.append("--profile-cframe")
                Run(Arm, f"cframe-{Count}-profile-{Profile}", "gargantuan_runtime_foundation6_benchmark", Options)
    for Arm in ("baseline", "candidate", "candidate", "baseline"):
        Samples = Output / f"{len(Receipts):03}-cframe-material-control-{Arm}.csv"
        Run(Arm, "cframe-material-control", "gargantuan_runtime_foundation6_benchmark",
            ["12", "--count=1000", "--density-matrix", "--property=material", "--profile-cframe",
             "--split-construction", "--diagnostic-marks", f"--samples={Samples}"])
    raise SystemExit(any(Entry["exit"] != 0 or Entry["timeout"] for Entry in Receipts))

if Args.suite == "cframe_allocation":
    # The frozen allocation binary has its own new/delete counters; keep the
    # optional CFrame and publication stage clocks off for this replay.
    for Count in (1000, 10000, 50000):
        for Arm in ("baseline", "candidate", "candidate", "baseline"):
            Samples = Output / f"{len(Receipts):03}-cframe-allocation-{Count}-{Arm}.csv"
            Run(Arm, f"cframe-allocation-{Count}", "gargantuan_runtime6_allocation_benchmark",
                ["12", f"--count={Count}", "--density-matrix", "--group-width=256",
                 "--split-construction", "--diagnostic-marks", f"--samples={Samples}"])
    raise SystemExit(any(Entry["exit"] != 0 or Entry["timeout"] for Entry in Receipts))

if Args.suite in ("compact", "readback"):
    for Static, Dynamic in ((900, 100), (0, 1000)):
        for Density in ("0", "0.01", "1"):
            for Lifecycle in (0, 1, 16):
                for Arm in ("baseline", "candidate", "candidate", "baseline"):
                    Run(Arm, f"compact-{Static}-{Dynamic}-{Density}-{Lifecycle}",
                        "gargantuan_runtime6_physics_sync_benchmark",
                        ["--static-count", str(Static), "--dynamic-count", str(Dynamic), "--frames", "30",
                         "--warmup", "4", "--control-density", Density,
                         "--lifecycle-per-frame", str(Lifecycle), "--publication"])
    raise SystemExit(any(Entry["exit"] != 0 or Entry["timeout"] for Entry in Receipts))


# A/B/B/A after both builds are complete; no compilation overlaps measurements.
for Count in (1000, 10000, 25000, 50000):
    for Arm in ("baseline", "candidate", "candidate", "baseline"):
        Samples = Output / f"{len(Receipts):03}-transform-{Count}-{Arm}.csv"
        Run(Arm, f"transform-{Count}", "gargantuan_runtime_foundation6_benchmark",
            ["12", f"--count={Count}", "--density-matrix", "--group-width=256",
             "--split-construction", "--diagnostic-marks", f"--samples={Samples}"])
for Property in ("material", "mixed"):
    for Arm in ("baseline", "candidate", "candidate", "baseline"):
        Samples = Output / f"{len(Receipts):03}-{Property}-10000-{Arm}.csv"
        Run(Arm, f"{Property}-10000", "gargantuan_runtime_foundation6_benchmark",
            ["12", "--count=10000", "--density-matrix", f"--property={Property}",
             "--group-width=256", "--split-construction", "--diagnostic-marks", f"--samples={Samples}"])
for Arm in ("baseline", "candidate", "candidate", "baseline"):
    Samples = Output / f"{len(Receipts):03}-repeated-10000-{Arm}.csv"
    Run(Arm, "repeated-10000", "gargantuan_runtime_foundation6_benchmark",
        ["12", "--count=10000", "--changes=1000", "--writes=4",
         "--group-width=256", "--split-construction", "--diagnostic-marks", f"--samples={Samples}"])
# Allocation instrumentation is measured separately from the ordinary timing arms.
for Count in (1000, 10000):
    for Arm in ("baseline", "candidate", "candidate", "baseline"):
        Samples = Output / f"{len(Receipts):03}-allocation-{Count}-{Arm}.csv"
        Run(Arm, f"allocation-{Count}", "gargantuan_runtime6_allocation_benchmark",
            ["12", f"--count={Count}", "--density-matrix", "--group-width=256",
             "--split-construction", "--diagnostic-marks", f"--samples={Samples}"])
for Density in ("0", "0.01", "1"):
    for Arm in ("baseline", "candidate", "candidate", "baseline"):
        Run(Arm, f"physics-{Density}", "gargantuan_runtime6_physics_sync_benchmark",
            ["--static-count", "10000", "--dynamic-count", "128", "--frames", "30",
             "--warmup", "4", "--control-density", Density, "--publication"])
for Arm in ("baseline", "candidate", "candidate", "baseline"):
    Run(Arm, "physics-lifecycle", "gargantuan_runtime6_physics_sync_benchmark",
        ["--static-count", "1000", "--dynamic-count", "128", "--frames", "30",
         "--warmup", "4", "--control-density", "0.25", "--lifecycle-per-frame", "4", "--publication"])
for Changed in (0, 10, 100, 1000, 5000, 10000):
    for Order in ("sequential", "random"):
        Run("candidate", f"alternatives-{Changed}-{Order}", "gargantuan_runtime6_extraction_alternatives",
            ["10000", str(Changed), "12", Order, "0"])
Run("candidate", "alternatives-lifecycle", "gargantuan_runtime6_extraction_alternatives",
    ["1000", "100", "12", "random", "10"])
Run("candidate", "prepared-explicit-contract", "gargantuan_prepared_property_tests", ["--compare", "1000", "3"])
raise SystemExit(any(Entry["exit"] != 0 or Entry["timeout"] for Entry in Receipts))
