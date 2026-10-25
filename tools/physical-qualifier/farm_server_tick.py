"""Verify bounded Farm32 server work ticks on one monotonic clock.

The measured interval ends before the deliberate 60-Hz pacing sleep. This is
not a cross-host latency measurement or a complete recipient-cadence verdict.
"""

import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path


RECORD = struct.Struct("<BBHIQQQ")
HEADER = re.compile(
    rb"format=GargantuanFarmServerTicksV1\trun=([A-Za-z0-9_-]{1,64})"
    rb"\tcount=([0-9]+)\tdropped=([01])\tinvalid=([01])\n"
)
PHASES = ("baseline", "load", "resident", "evict", "reload")
RECORD_LIMIT = 48_016
FILE_LIMIT = RECORD_LIMIT * RECORD.size + 512
P95_LIMIT_NS = 16_667_000
P99_LIMIT_NS = 33_334_000
MAXIMUM_LIMIT_NS = 100_000_000
MINIMUM_PHASE_NS = 13_000_000_000
MINIMUM_PHASE_TICKS = 240


def Require(Condition, Message):
    if not Condition:
        raise ValueError("[Qualification:ServerTick] " + Message)


def Digest(PathValue):
    Hash = hashlib.sha256()
    with PathValue.open("rb") as Stream:
        while Bytes := Stream.read(1 << 20):
            Hash.update(Bytes)
    return Hash.hexdigest()


def Indexed(IndexPath, Name, MaximumBytes):
    IndexPath = Path(IndexPath)
    Require(IndexPath.name == "evidence-sha256.json" and IndexPath.is_file() and
            not IndexPath.is_symlink() and 0 < IndexPath.stat().st_size <= 65536,
            "server evidence index missing or oversized")
    Index = json.loads(IndexPath.read_text(encoding="utf-8"))
    Require(isinstance(Index, dict) and Index.get("Role") == "Server" and
            isinstance(Index.get("RunId"), str) and
            re.fullmatch(r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}", Index["RunId"]) and
            isinstance(Index.get("Files"), list) and len(Index["Files"]) <= 128,
            "server evidence index identity invalid")
    Members = [Value for Value in Index["Files"] if isinstance(Value, dict) and Value.get("Name") == Name]
    Require(len(Members) == 1, "server tick evidence member missing or duplicated")
    Entry = Members[0]
    Require(set(Entry) == {"Name", "Bytes", "Sha256"} and
            type(Entry["Bytes"]) is int and 0 < Entry["Bytes"] <= MaximumBytes and
            isinstance(Entry["Sha256"], str) and
            re.fullmatch(r"[0-9a-f]{64}", Entry["Sha256"]),
            "server tick evidence member bound invalid")
    File = IndexPath.parent / Name
    Require(File.is_file() and not File.is_symlink() and
            File.stat().st_size == Entry["Bytes"] and Digest(File) == Entry["Sha256"],
            "server tick evidence member hash or size mismatch")
    return Index["RunId"], File, Entry["Sha256"]


def Percentile(Values, Fraction):
    """Match the canonical differential's floor((N-1)*fraction) convention."""
    Ordered = sorted(Values)
    return Ordered[(Fraction * (len(Ordered) - 1)) // 100]


def Analyze(PathValue, RunId, MaximumTicks=48_000):
    PathValue = Path(PathValue)
    Require(PathValue.is_file() and not PathValue.is_symlink() and
            0 < PathValue.stat().st_size <= FILE_LIMIT,
            "bounded server work-tick trace missing or redirected")
    Require(type(MaximumTicks) is int and 7200 <= MaximumTicks <= 48_000,
            "manifest server tick bound invalid")
    Ticks = {}
    Phases = []
    OpenPhase = None
    PreviousEventNs = 0
    PreviousTick = 0
    with PathValue.open("rb") as Stream:
        HeaderLine = Stream.readline(513)
        HeaderMatch = HEADER.fullmatch(HeaderLine)
        Require(HeaderMatch is not None and HeaderMatch.group(1).decode("ascii") == RunId,
                "server work-tick trace header or run identity invalid")
        Count, Dropped, Invalid = map(int, HeaderMatch.groups()[1:])
        Require(0 < Count <= RECORD_LIMIT and Dropped == Invalid == 0 and
                PathValue.stat().st_size == len(HeaderLine) + Count * RECORD.size,
                "server work-tick trace incomplete, overflowed, or malformed")
        for _ in range(Count):
            Bytes = Stream.read(RECORD.size)
            Require(len(Bytes) == RECORD.size, "truncated server work-tick record")
            Kind, Phase, Reserved, Reserved2, Tick, AtNs, EndNs = RECORD.unpack(Bytes)
            Require(Reserved == Reserved2 == 0 and Tick > 0 and AtNs > 0,
                    "server work-tick record has invalid fields")
            if Kind == 1:
                Require(Phase == 0 and EndNs >= AtNs and EndNs >= PreviousEventNs and
                        (PreviousTick == 0 or Tick == PreviousTick + 1) and
                        Tick <= MaximumTicks and Tick not in Ticks,
                        "server work ticks are duplicated, missing, or out of order")
                Ticks[Tick] = (AtNs, EndNs)
                PreviousTick = Tick
                PreviousEventNs = EndNs
            elif Kind in (2, 3):
                Require(Phase < len(PHASES) and EndNs == 0 and AtNs >= PreviousEventNs,
                        "phase marker is invalid or out of time order")
                if Kind == 2:
                    Require(OpenPhase is None and Phase == len(Phases),
                            "phase start is duplicated or out of order")
                    OpenPhase = (Phase, Tick, AtNs)
                else:
                    Require(OpenPhase is not None and OpenPhase[0] == Phase and
                            Tick > OpenPhase[1] and AtNs - OpenPhase[2] > MINIMUM_PHASE_NS,
                            "phase end is missing, wrong, or too early")
                    Phases.append((OpenPhase[1], Tick, OpenPhase[2], AtNs))
                    OpenPhase = None
                PreviousEventNs = AtNs
            else:
                Require(False, "unknown server work-tick record kind")
        Require(Stream.read(1) == b"", "server work-tick trace has trailing bytes")
    Require(OpenPhase is None and len(Phases) == len(PHASES) and len(Ticks) > 0,
            "five server phases or completed work ticks are missing")
    Results = []
    for Phase, (StartTick, EndTick, StartNs, FinishNs) in enumerate(Phases):
        Require(StartTick in Ticks and EndTick in Ticks and
                Ticks[StartTick][0] <= StartNs <= Ticks[StartTick][1] and
                Ticks[EndTick][0] <= FinishNs <= Ticks[EndTick][1] and
                EndTick - StartTick >= MINIMUM_PHASE_TICKS,
                "phase markers are not bracketed by completed server ticks")
        Durations = [Ticks[Tick][1] - Ticks[Tick][0]
                     for Tick in range(StartTick, EndTick + 1)]
        P95 = Percentile(Durations, 95)
        P99 = Percentile(Durations, 99)
        Maximum = max(Durations)
        Pass = (P95 <= P95_LIMIT_NS and P99 <= P99_LIMIT_NS and Maximum <= MAXIMUM_LIMIT_NS)
        Results.append({"Phase": PHASES[Phase], "StartTick": StartTick,
                        "EndTick": EndTick, "Ticks": len(Durations),
                        "ElapsedNanoseconds": FinishNs - StartNs,
                        "P95WorkNanoseconds": P95, "P99WorkNanoseconds": P99,
                        "MaximumWorkNanoseconds": Maximum,
                        "Status": "MEASURED_PASS" if Pass else "MEASURED_FAIL"})
    return {"Format": "GargantuanFarmServerWorkTicks", "Version": 1,
            "RunId": RunId, "CompletedTicks": len(Ticks), "Phases": Results,
            "Status": "MEASURED_PASS" if all(Value["Status"] == "MEASURED_PASS" for Value in Results)
            else "MEASURED_FAIL", "CrossHostLatency": "NOT_MEASURED"}


def AnalyzeIndexed(ServerIndexPath):
    RunId, Manifest, ManifestHash = Indexed(ServerIndexPath, "run-manifest.json", 65536)
    RunId2, Trace, TraceHash = Indexed(ServerIndexPath, "server-work-ticks.bin", FILE_LIMIT)
    Require(RunId == RunId2, "manifest and tick trace run identities differ")
    ManifestValue = json.loads(Manifest.read_text(encoding="utf-8"))
    Require(isinstance(ManifestValue, dict) and ManifestValue.get("RunId") == RunId and
            ManifestValue.get("ScaleWorkload") is True,
            "server work-tick trace has no scale workload manifest")
    Result = Analyze(Trace, RunId, ManifestValue.get("ServerTicks"))
    Require(Digest(Manifest) == ManifestHash and Digest(Trace) == TraceHash,
            "server work-tick evidence changed during analysis")
    Result["ServerIndexSha256"] = Digest(Path(ServerIndexPath))
    Result["ManifestSha256"] = ManifestHash
    Result["TraceSha256"] = TraceHash
    Result["AnalyzerSha256"] = Digest(Path(__file__))
    return Result


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("server_index")
    Arguments = Parser.parse_args()
    try:
        print(json.dumps(AnalyzeIndexed(Arguments.server_index), sort_keys=True))
    except (ValueError, OSError, json.JSONDecodeError) as Error:
        print(str(Error), file=sys.stderr)
        raise SystemExit(1)
