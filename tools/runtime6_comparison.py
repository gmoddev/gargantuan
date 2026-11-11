"""Same-host source-bound CPU comparison; physical GPU/network claims are separate."""
import argparse
import hashlib
import json
import os
import pathlib
import signal
import subprocess
import time


def Hash(File):
    return hashlib.sha256(File.read_bytes()).hexdigest()


Parser = argparse.ArgumentParser()
Parser.add_argument("--evidence", type=pathlib.Path, required=True)
Parser.add_argument("--suite", choices=("core", "cframe", "compact", "realistic"), default="core")
Args = Parser.parse_args()
Root = Args.evidence.resolve()
Sources = {}
for Arm in ("baseline", "candidate"):
    Directory = Root / Arm
    Manifest = json.loads((Directory / "manifest.json").read_text())
    for Name, Digest in Manifest["binaries"].items():
        if Hash(Directory / Name) != Digest:
            raise RuntimeError(f"[Runtime6:Compare] Changed binary: {Arm}/{Name}")
    Sources[Arm] = Manifest

Output = Root / "comparison"
Output.mkdir(exist_ok=False)
(Output / "sources.json").write_text(json.dumps(Sources, indent=2))
Receipts = []


def Run(Arm, Label, Binary, Options, Timeout=600):
    Executable = Root / Arm / Binary
    Name = f"{len(Receipts):03}-{Label}-{Arm}"
    Command = [str(Executable), *Options]
    MemoryFile = Output / (Name + ".resources.txt")
    Invocation = ["/usr/bin/time", "-v", "-o", str(MemoryFile), *Command]
    Start = time.monotonic()
    Code, TimedOut = None, False
    with (Output / (Name + ".stdout")).open("xb") as Stdout, (Output / (Name + ".stderr")).open("xb") as Stderr:
        try:
            Process = subprocess.Popen(Invocation, stdout=Stdout, stderr=Stderr,
                                       stdin=subprocess.DEVNULL, start_new_session=True)
            Code = Process.wait(timeout=Timeout)
        except subprocess.TimeoutExpired:
            TimedOut = True
            os.killpg(Process.pid, signal.SIGKILL)
            Process.wait()
    Receipt = {"arm": Arm, "source": Sources[Arm]["source"], "label": Label,
               "command": Command, "exit": Code, "timeout": TimedOut,
               "invocation": Invocation, "resources": str(MemoryFile),
               "elapsedSeconds": time.monotonic() - Start,
               "stdoutSha256": Hash(Output / (Name + ".stdout")),
               "stderrSha256": Hash(Output / (Name + ".stderr"))}
    Receipts.append(Receipt)
    (Output / "receipts.json").write_text(json.dumps(Receipts, indent=2))
    print(json.dumps(Receipt), flush=True)


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

if Args.suite == "compact":
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
