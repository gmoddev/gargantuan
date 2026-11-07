"""Collect finite, paired experiment runs; preserve failures and raw output.

This collector does not turn a successful process exit into physical correctness.
Quality failures remain failures in Collection.json and the final exit status.
"""
import argparse
import datetime
import hashlib
import json
import os
import pathlib
import platform
import subprocess
import sys
import time


def Hash(Path):
    Digest = hashlib.sha256()
    with Path.open("rb") as File:
        for Block in iter(lambda: File.read(1024 * 1024), b""):
            Digest.update(Block)
    return Digest.hexdigest()


def Git(Root, *Arguments):
    return subprocess.check_output(["git", "-C", str(Root), *Arguments], text=True).strip()


def Main():
    Parser = argparse.ArgumentParser()
    Parser.add_argument("--build", type=pathlib.Path, required=True)
    Parser.add_argument("--jolt", type=pathlib.Path, required=True)
    Parser.add_argument("--output", type=pathlib.Path, required=True)
    Parser.add_argument("--mode", choices=["diagnostic", "full", "allocations"], default="full")
    Parser.add_argument("--repeats", type=int, default=3)
    Arguments = Parser.parse_args()
    if not 1 <= Arguments.repeats <= 5:
        Parser.error("repeats must be1–5")
    Root = pathlib.Path(__file__).resolve().parents[2]
    Output = Arguments.output.resolve()
    Output.mkdir(parents=True, exist_ok=False)
    Source = Git(Root, "rev-parse", "HEAD")
    JoltSource = Git(Arguments.jolt, "rev-parse", "HEAD")
    if JoltSource != "e77f175595e64cb44218cc9d9d56fc365ad0e36a" or Git(Arguments.jolt, "status", "--porcelain"):
        raise RuntimeError("[Physics:Evaluation] Jolt source pin/cleanliness changed")
    Mainline = "5b6782e35c4e24fd80bc8d5526104c615c72d84d"
    subprocess.run(["git", "-C", str(Root), "merge-base", "--is-ancestor", Mainline, Source], check=True)
    Changed = Git(Root, "diff", "--name-only", "HEAD", "--ignore-submodules=all").splitlines()
    GeneratedPrefixes = ("src/classes/generated/", "src/services/generated/", "include/gargantuan/classes/generated/", "include/gargantuan/services/generated/")
    Unexpected = [Name for Name in Changed if not Name.startswith(GeneratedPrefixes)]
    if Unexpected:
        raise RuntimeError("[Physics:Evaluation] Uncommitted non-generated source: " + repr(Unexpected))
    SourceDiff = Git(Root, "diff", "--binary", "HEAD", "--ignore-submodules=all")
    (Output / "GeneratedSource.patch").write_text(SourceDiff, encoding="utf-8")
    InputFiles = {Root / "CMakeLists.txt"}
    for Directory in ["src", "include", "cmake", "assets/classes", "assets/services", "tools/classgen", "experiments/PhysicsJoltEvaluation"]:
        InputFiles.update(Path for Path in (Root / Directory).rglob("*")
                          if Path.is_file() and "__pycache__" not in Path.parts)
    Inputs = {str(Path.relative_to(Root)).replace("\\", "/"): Hash(Path) for Path in sorted(InputFiles)}
    Metadata = {"Source": Source, "Tree": Git(Root, "rev-parse", "HEAD^{tree}"),
                "MergedMain": Mainline, "Jolt": JoltSource,
                "SourceStatus": Git(Root, "status", "--porcelain", "--untracked-files=no"),
                "GeneratedDiff": Git(Root, "diff", "--", "include/gargantuan/classes/generated", "src/classes/generated"),
                "Platform": platform.platform(), "Machine": platform.machine(),
                "Processor": platform.processor(), "LogicalCpuCount": os.cpu_count(),
                "Affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
                "StartedUtc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "Mode": Arguments.mode, "InputSha256": Inputs, "Binaries": {}, "Runs": []}
    Metadata["FullTrackedDiffSha256"] = hashlib.sha256(SourceDiff.encode("utf-8")).hexdigest()
    Metadata["VendorGitlinks"] = Git(Root, "ls-tree", "HEAD", "vendor/")
    Metadata["InputHashScope"] = "Production src/include/cmake, class/service definitions, classgen, root CMake and experiment files; generated tracked differences captured separately; vendor/Jolt pin and build provenance recorded separately"
    for Name in ["/proc/cpuinfo", "/sys/fs/cgroup/cpu.max", "/sys/fs/cgroup/memory.max"]:
        Path = pathlib.Path(Name)
        if Path.is_file():
            Data = Path.read_text()
            Metadata[Name] = Data if Name != "/proc/cpuinfo" else "\n".join(
                Line for Line in Data.splitlines() if Line.startswith(("model name", "cpu cores", "siblings")))
    Cache = Arguments.build / "CMakeCache.txt"
    if Cache.is_file():
        Metadata["CMakeCacheSha256"] = Hash(Cache)
        Metadata["BuildSettings"] = [Line for Line in Cache.read_text(errors="replace").splitlines()
            if Line.startswith(("CMAKE_BUILD_TYPE:", "CMAKE_CXX_COMPILER:", "CMAKE_C_COMPILER:",
                "CMAKE_CXX_FLAGS", "CMAKE_C_FLAGS", "CMAKE_MSVC_RUNTIME_LIBRARY:", "CROSS_PLATFORM_DETERMINISTIC:",
                "USE_AVX", "USE_SSE", "USE_FMADD:", "GARGANTUAN_HEADLESS_GEOMETRY_QUALIFICATION:"))]
    Suffix = "Allocations" if Arguments.mode == "allocations" else ""
    Extension = ".exe" if os.name == "nt" else ""
    Executables = {}
    for Kind in ["Rigid", "Soft", "Coupling"]:
        Path = Arguments.build / "experiments/PhysicsJoltEvaluation" / ("PhysicsJolt" + Kind + "Evaluation" + Suffix + Extension)
        Executables[Kind] = Path
        Metadata["Binaries"][Kind] = {"Path": str(Path), "Sha256": Hash(Path)}
    Repeats = Arguments.repeats if Arguments.mode == "full" else 1
    Budgets = [1, 2, 8] if Arguments.mode == "full" else [1]
    Jobs = []
    for Repeat in range(1, Repeats + 1):
        for Budget in Budgets:
            RigidCases = ["all"] if Arguments.mode == "diagnostic" else ["static-world", "stack", "dense-contacts",
                "mostly-static", "body-churn", "sleep-wake", "mesh-contact", "advanced-queries", "mesh-query-dedup",
                "filter-updates", "moving-ccd-off", "moving-ccd-on", "joint-motors", "spring-distance", "articulated-chain", "concurrent-admission"]
            for Case in RigidCases:
                for Backend in ["box3d", "jolt"]:
                    Args = [f"--backend={Backend}", f"--case={Case}", f"--workers={Budget}"]
                    if Arguments.mode == "diagnostic": Args.append("--quick")
                    Jobs.append((f"Rigid-{Backend}-{Case}-T{Budget}-R{Repeat}", "Rigid", Args))
            Cases = ["TinyCloth"] if Arguments.mode == "diagnostic" else [
                "TinyCloth", "Cloth4K", "Cloth16K", "FourCloths", "Rubber", "MovingCollider", "HighSpeedCollider", "Attachments", "PointImpulse", "Lifecycle"]
            for Case in Cases:
                for Backend in ["XPBD", "Jolt"]:
                    Jobs.append((f"Soft-{Backend}-{Case}-T{Budget}-R{Repeat}", "Soft", [
                        "--backend", Backend, "--case", Case, "--workers", str(Budget),
                        "--frames", "150", "--warmup", "30", "--track", "FixedTotalMass"]))
            for Case in ["SphereOnCloth", "RubberPush", "RigidIntoCloth", "Repeated", "Multiple"]:
                for Backend in ["XPBD", "Jolt"]:
                    Jobs.append((f"Coupling-{Backend}-{Case}-T{Budget}-R{Repeat}", "Coupling", [
                        "--backend", Backend, "--case", Case, "--workers", str(Budget), "--frames", "150"]))
    HarnessFailures = 0
    QualityFailures = 0
    for Name, Kind, Args in Jobs:
        Command = [str(Executables[Kind]), *Args]
        Start = time.perf_counter()
        TimedOut = False
        try:
            Run = subprocess.run(Command, capture_output=True, timeout=180)
            Stdout, Stderr, Code = Run.stdout, Run.stderr, Run.returncode
        except subprocess.TimeoutExpired as Error:
            Stdout, Stderr, Code, TimedOut = Error.stdout or b"", Error.stderr or b"", None, True
        (Output / (Name + ".stdout.jsonl")).write_bytes(Stdout)
        (Output / (Name + ".stderr.log")).write_bytes(Stderr)
        Rows = []
        ParseErrors = []
        for Line in Stdout.decode("utf-8", errors="replace").splitlines():
            try:
                Row = json.loads(Line)
                if not isinstance(Row, dict):
                    ParseErrors.append(Line)
                else:
                    Rows.append(Row)
            except json.JSONDecodeError:
                ParseErrors.append(Line)
        FailedRows = [Row for Row in Rows if Row.get("QualityQualified") is False or
                      Row.get("Quality", {}).get("Status") == "fail" or Row.get("Status") in ["Failed", "failed"]]
        UnsupportedRows = [Row for Row in Rows if str(Row.get("Status", "")).lower().startswith(("unsupported", "unimplemented")) or Row.get("Quality", {}).get("Status") == "unsupported"]
        CaseRows = [Row for Row in Rows if "Case" in Row]
        UnknownQualityRows = [Row for Row in CaseRows if Row not in UnsupportedRows and
            "QualityQualified" not in Row and Row.get("Quality", {}).get("Status") not in ["pass", "fail"]]
        HarnessFailed = TimedOut or not CaseRows or bool(ParseErrors) or Code is None or Code < 0 or Code not in [0, 1, 3]
        HarnessFailures += int(HarnessFailed)
        GateFailures = len(FailedRows) + len(UnknownQualityRows)
        if Code in [1, 3] and not GateFailures and not UnsupportedRows:
            GateFailures = 1
        QualityFailures += GateFailures
        Entry = {"Name": Name, "Command": Command, "ExitCode": Code, "ElapsedSeconds": time.perf_counter() - Start,
                 "TimedOut": TimedOut, "Rows": len(Rows), "ParseErrors": ParseErrors,
                 "HarnessFailed": HarnessFailed, "QualityFailedRows": GateFailures,
                 "UnknownQualityRows": len(UnknownQualityRows),
                 "UnsupportedRows": len(UnsupportedRows), "StdoutSha256": hashlib.sha256(Stdout).hexdigest(),
                 "StderrSha256": hashlib.sha256(Stderr).hexdigest()}
        Metadata["Runs"].append(Entry)
        (Output / "Collection.json").write_text(json.dumps(Metadata, indent=2), encoding="utf-8")
        print(json.dumps({"Run": Name, "ExitCode": Code, "HarnessFailed": HarnessFailed,
                          "QualityFailures": GateFailures, "UnsupportedRows": len(UnsupportedRows)}), flush=True)
    Metadata["CollectionComplete"] = HarnessFailures == 0
    Metadata["HarnessFailures"] = HarnessFailures
    Metadata["QualityFailures"] = QualityFailures
    Metadata["UnsupportedRows"] = sum(Run["UnsupportedRows"] for Run in Metadata["Runs"])
    Metadata["AllExecutedQualityGatesPassed"] = not HarnessFailures and not QualityFailures and not Metadata["UnsupportedRows"]
    Metadata["FinishedUtc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    Metadata["JoltUnchanged"] = Git(Arguments.jolt, "rev-parse", "HEAD") == JoltSource and not Git(Arguments.jolt, "status", "--porcelain")
    Metadata["ExperimentInputsUnchanged"] = all((Root / Name).is_file() and Hash(Root / Name) == Digest for Name, Digest in Inputs.items())
    Metadata["BinariesUnchanged"] = all(Hash(Executables[Kind]) == Value["Sha256"] for Kind, Value in Metadata["Binaries"].items())
    Metadata["SourceRevisionUnchanged"] = Git(Root, "rev-parse", "HEAD") == Source and Git(Root, "diff", "--binary", "HEAD", "--ignore-submodules=all") == SourceDiff
    (Output / "Collection.json").write_text(json.dumps(Metadata, indent=2), encoding="utf-8")
    if not all(Metadata[Key] for Key in ["JoltUnchanged", "ExperimentInputsUnchanged", "BinariesUnchanged", "SourceRevisionUnchanged"]): return 2
    return 2 if HarnessFailures else 1 if QualityFailures else 0


if __name__ == "__main__":
    sys.exit(Main())
