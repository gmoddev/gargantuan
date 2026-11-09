"""Compare nested Jolt profile work; never use instrumented frame timings for ranking."""

from __future__ import annotations

import argparse
import json
import hashlib
import pathlib
import re


Parser = argparse.ArgumentParser(description=__doc__)
Parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path(__file__).parent.parent,
                    help="directory containing raw dated profile and qualification folders")
Parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path(__file__).parent,
                    help="directory to receive ProfileComparison.md/json")
Args = Parser.parse_args()
HERE = Args.output.resolve()
ROOT = Args.root.resolve()
OLD = ROOT / "Hoist-Profile-20261109T055147"
FINAL = ROOT / "Final-Profile-20261109T073222"
LONG = ROOT / "C-Long-20261109T070355"
GUARDS = ROOT / "Final-Guards-20261109T072412"
CASES = (("cloth16k", 1), ("cloth16k", 8), ("rubber4k", 8))
SCOPES = ("SoftSimulate", "SoftEdges", "SoftVolumes", "SoftFinishIteration",
          "SoftIntegrate", "Barrier", "ClearLambdas")


def Require(Condition, Message):
    if not Condition:
        raise ValueError(Message)


def CheckReceipt(Path):
    Receipt = json.loads(Path.with_suffix(".receipt.json").read_text())
    Stderr = Path.with_suffix(".stderr")
    Require(Receipt["ExitCode"] == 0, f"{Path}: nonzero exit")
    Require(hashlib.sha256(Path.read_bytes()).hexdigest() == Receipt["StdoutSha256"],
            f"{Path}: stdout SHA-256 mismatch")
    Require(hashlib.sha256(Stderr.read_bytes()).hexdigest() == Receipt["StderrSha256"],
            f"{Path}: stderr SHA-256 mismatch")


def Read(Path):
    CheckReceipt(Path)
    Lines = Path.read_text(encoding="utf-8").splitlines()
    Require("timing_instrumented=1" in Lines[0], f"{Path}: missing instrumented marker")
    Profile = [json.loads(Line)["ProfileWorkNanoseconds"] for Line in Lines
               if Line.startswith('{"ProfileWorkNanoseconds":')]
    Require(len(Profile) == 1, f"{Path}: expected one nested profile summary")
    States = [(int(Match.group(1)), Match.group(2)) for Line in Lines
              if (Match := re.search(r"state_frame=(\d+) soft_trajectory_hash=(\d+)", Line))]
    Require(len(States) == 40 and [Index for Index, _ in States] == list(range(40)),
            f"{Path}: expected 40 sequential state hashes")
    return Profile[0], States


def ReadStates(Path):
    CheckReceipt(Path)
    States = [(int(Match.group(1)), Match.group(2)) for Line in Path.read_text(encoding="utf-8").splitlines()
              if (Match := re.search(r"state_frame=(\d+) soft_trajectory_hash=(\d+)", Line))]
    Require(len(States) >= 40, f"{Path}: fewer than 40 hashes")
    return States[:40]


def Main():
    HERE.mkdir(parents=True, exist_ok=True)
    Source = json.loads((FINAL / "Source.json").read_text())
    Require(Source["NativeProfile"] is True and Source["LargeClothGroupBatch"] == 1024,
            "final profile provenance or group size mismatch")
    Require("JPH_USE_EXTERNAL_PROFILE:BOOL=ON" in (FINAL / "CMakeCache.txt").read_text(),
            "diagnostic binary profile flag missing")
    Require("JPH_USE_EXTERNAL_PROFILE:BOOL=OFF" in (FINAL / "RestoredCache.txt").read_text(),
            "final unprofiled worker config not restored")
    RestoredQualifications = []
    for Name in ("PhysicsJoltProviderQualification", "PhysicsJoltAdmissionCacheQualification",
                 "PhysicsJoltSchedulingQualification"):
        Path = FINAL / f"Restored-{Name}.jsonl"
        CheckReceipt(Path)
        RestoredQualifications.append(Name)
    Rows = []
    HashChecks = []
    for Scene, Workers in CASES:
        Label = f"{Scene}-W{Workers}"
        OldProfile, _ = Read(OLD / f"Solver-{Label}.jsonl")
        FinalProfile, FinalStates = Read(FINAL / f"{Label}.jsonl")
        for Scope in SCOPES:
            Before = OldProfile.get(Scope, {"Count": 0, "Nanoseconds": 0})
            After = FinalProfile.get(Scope, {"Count": 0, "Nanoseconds": 0})
            BeforeMs = Before["Nanoseconds"] / 1e6
            AfterMs = After["Nanoseconds"] / 1e6
            Rows.append({"Case": Label, "Scope": Scope,
                         "OldCount": Before["Count"], "FinalCount": After["Count"],
                         "OldWorkMs": BeforeMs, "FinalWorkMs": AfterMs,
                         "WorkDeltaPercent": 100 * (AfterMs / BeforeMs - 1) if BeforeMs else None})
        if Workers == 8:
            ForLong = LONG / f"{Scene}.jsonl"
            ForGuards = GUARDS / f"{Scene}-W8.jsonl"
            for Reference in [ForLong, ForGuards] if ForGuards.exists() else [ForLong]:
                Exact = FinalStates == ReadStates(Reference)
                HashChecks.append({"Case": Label, "Reference": Reference.parent.name + "/" + Reference.name,
                                   "Frames": 40, "AllExact": Exact})
                Require(Exact, f"{Label}: 40-frame hash mismatch against {Reference}")
    (HERE / "ProfileComparison.json").write_text(json.dumps({"Rows": Rows, "HashChecks": HashChecks,
                                                                "RestoredQualifications": RestoredQualifications,
                                                                "FinalSource": Source}, indent=2)
                                                        + "\n", encoding="utf-8")
    Lines = ["# Diagnostic Jolt nested work", "",
             "40 frames per case, including 10 warmup frames. Both binaries have native profiling enabled. "
             "These scopes overlap and sum worker work; they are not wall time or a critical path. "
             "Only the separate unprofiled B/C receipts rank frame timing. "
             "Captured source is native `7e59df5f`, guarded Engine `3c864c58`, group size 1024; "
             "the worker restored external profiling OFF and all three qualification receipts exited zero.", "",
             "| Case | Scope | Old 256 count | Final count | Old work ms | Final work ms | Work change |",
             "| --- | --- | ---: | ---: | ---: | ---: | ---: |"]
    for Row in Rows:
        Change = f"{Row['WorkDeltaPercent']:+.1f}%" if Row["WorkDeltaPercent"] is not None else "—"
        Lines.append(f"| {Row['Case']} | {Row['Scope']} | {Row['OldCount']:,} | {Row['FinalCount']:,} | "
                     f"{Row['OldWorkMs']:,.1f} | {Row['FinalWorkMs']:,.1f} | {Change} |")
    Lines.extend(["", "The final 1024-group cloth cases issue fewer edge/volume scope calls. "
                  "The eight-worker cloth FinishIteration work also falls; this supports the reduced serial-group "
                  "cost hypothesis, while the nested profile alone cannot allocate a wall-time gain. "
                  "Rubber keeps group size 256, so its scope counts should stay equal.", "",
                  "## Exact 40-frame state checks", "",
                  "| Profile case | Reference | Frames | Result |", "| --- | --- | ---: | --- |"])
    for Check in HashChecks:
        Lines.append(f"| {Check['Case']} | {Check['Reference']} | {Check['Frames']} | exact |")
    (HERE / "ProfileComparison.md").write_text("\n".join(Lines) + "\n", encoding="utf-8")
    print(json.dumps({"Scopes": len(Rows), "HashChecks": HashChecks,
                      "RestoredQualifications": RestoredQualifications}, indent=2))


if __name__ == "__main__":
    Main()

