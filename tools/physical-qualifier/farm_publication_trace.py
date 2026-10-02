"""Strict, streaming validator for farm-only native publication evidence.

This validates retention and identities, not cross-host latency. In particular,
the native timestamps in two different processes have unrelated origins.
"""

import argparse
import re
import struct
from pathlib import Path


RECORD = struct.Struct("<HHIIIIIQQQQQQQ")
HEADER = re.compile(
    rb"format=GargantuanFarmPublicationV1\trun=([A-Za-z0-9_-]{1,64})"
    rb"\trole=(SERVER|CLIENT)\tslot=(-1|[0-9]{1,2})\tnonce=([0-9]+)"
    rb"\tcount=([0-9]+)\tdropped=([0-9]+)\tdecode_failures=([0-9]+)\n"
)
SERVER_STAGES = set(range(1, 9))
CLIENT_STAGES = {9, 10}
SERVER_CAP = 4_194_304
CLIENT_CAP = 131_072


def Require(Condition, Message):
    if not Condition:
        raise ValueError("[Qualification:Publication] " + Message)


def Validate(PathValue, ExpectedRunId, ExpectedRole, ExpectedSlot=-1, ExpectedNonce=0):
    PathValue = Path(PathValue)
    Require(PathValue.is_file() and not PathValue.is_symlink(), "trace is missing or redirected")
    with PathValue.open("rb") as Stream:
        HeaderLine = Stream.readline(513)
        Match = HEADER.fullmatch(HeaderLine)
        Require(Match is not None, "invalid bounded trace header")
        RunId = Match.group(1).decode("ascii")
        Role = Match.group(2).decode("ascii")
        Slot, Nonce, Count, Dropped, DecodeFailures = map(int, Match.groups()[2:])
        Require((RunId, Role, Slot, Nonce) ==
                (ExpectedRunId, ExpectedRole, ExpectedSlot, ExpectedNonce), "trace identity mismatch")
        Cap = SERVER_CAP if Role == "SERVER" else CLIENT_CAP
        Require(0 < Count <= Cap and Dropped == 0 and DecodeFailures == 0,
                "trace empty, overflowed, or failed decoding")
        Require(PathValue.stat().st_size == len(HeaderLine) + Count * RECORD.size,
                "trace length differs from declared records")
        Stages = SERVER_STAGES if Role == "SERVER" else CLIENT_STAGES
        PreviousNs = 0
        Counts = {}
        for _ in range(Count):
            Bytes = Stream.read(RECORD.size)
            Require(len(Bytes) == RECORD.size, "truncated trace record")
            (Stage, Flags, ConnectionSlot, ConnectionGeneration, ObjectSlot, ObjectGeneration,
             ServiceBytes, Nanoseconds, Tick, Sequence, DueTick, ControlEpoch,
             MaterializationEpoch, FrameSequence) = RECORD.unpack(Bytes)
            Require(Stage in Stages and Flags <= 1 and Nanoseconds >= PreviousNs,
                    "invalid stage, flags, or process-local time order")
            PreviousNs = Nanoseconds
            if Stage >= 8:
                Require(ConnectionSlot > 0 and ConnectionGeneration > 0 and ObjectSlot > 0 and
                        ObjectGeneration > 0 and Tick > 0 and Sequence > 0 and ControlEpoch > 0 and
                        MaterializationEpoch > 0 and FrameSequence > 0 and ServiceBytes > 0,
                        "packet stage lacks full Character identity")
            if Role == "CLIENT":
                Require(Stage in CLIENT_STAGES, "server-only stage in client trace")
            Counts[Stage] = Counts.get(Stage, 0) + 1
        Require(Stream.read(1) == b"", "unexpected trailing trace bytes")
    Require((8 in Counts if Role == "SERVER" else 9 in Counts and 10 in Counts),
            "required packet stages absent")
    return {"RunId": RunId, "Role": Role, "Slot": Slot, "Nonce": Nonce,
            "Records": Count, "Stages": Counts, "Bytes": PathValue.stat().st_size,
            "CrossHostLatency": "NOT_MEASURED"}


if __name__ == "__main__":
    Parser = argparse.ArgumentParser()
    Parser.add_argument("path")
    Parser.add_argument("run_id")
    Parser.add_argument("role", choices=("SERVER", "CLIENT"))
    Parser.add_argument("--slot", type=int, default=-1)
    Parser.add_argument("--nonce", type=int, default=0)
    Arguments = Parser.parse_args()
    print(Validate(Arguments.path, Arguments.run_id, Arguments.role,
                   Arguments.slot, Arguments.nonce))
