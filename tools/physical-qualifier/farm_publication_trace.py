"""Strict, streaming validator for farm-only native publication evidence.

This validates retention and identities, not cross-host latency. In particular,
the native timestamps in two different processes have unrelated origins.
"""

import argparse
import re
import struct
from pathlib import Path
from typing import NamedTuple


RECORD = struct.Struct("<HHIIIIIQQQQQQQ")
HEADER = re.compile(
    rb"format=GargantuanFarmPublicationV1\trun=([A-Za-z0-9_-]{1,64})"
    rb"\trole=(SERVER|CLIENT)\tslot=(-1|[0-9]{1,2})\tnonce=([0-9]+)"
    rb"\tcount=([0-9]+)\tdropped=([0-9]+)\tdecode_failures=([0-9]+)\n"
)
RPC_STAGES = set(range(14, 24))
TRAFFIC_STAGES = {24, 25, 26, 27}
SERVER_STAGES = set(range(1, 9)) | {11, 12, 13, 28} | RPC_STAGES | TRAFFIC_STAGES
CLIENT_STAGES = {9, 10, 28} | RPC_STAGES | TRAFFIC_STAGES
SERVER_CAP = 4_194_304
CLIENT_CAP = 131_072
RPC_NATIVE_RECEIVE = 28
MAXIMUM_ADAPTER_EVENT_DEPTH = 1_048_576


class PublicationRecord(NamedTuple):
    Stage: int
    Flags: int
    ConnectionSlot: int
    ConnectionGeneration: int
    ObjectSlot: int
    ObjectGeneration: int
    ServiceBytes: int
    Nanoseconds: int
    Tick: int
    Sequence: int
    DueTick: int
    ControlEpoch: int
    MaterializationEpoch: int
    FrameSequence: int

    # Stage 28 metadata is orthogonal to the existing RPC causal stages. The
    # native GNS clock age starts at complete ordered-message readiness, not
    # the first UDP packet. Nanoseconds is sampled after dequeue and fixed
    # prefix selection, before decode/copy and adapter QueueEvent.
    @property
    def RpcReadyQueueAgeMicroseconds(self):
        return self.Tick if self.Stage == RPC_NATIVE_RECEIVE else None

    @property
    def RpcAdapterPendingEventDepth(self):
        return self.DueTick if self.Stage == RPC_NATIVE_RECEIVE else None

    @property
    def RpcAgeSamplingSpanNanoseconds(self):
        return self.FrameSequence - self.Nanoseconds if self.Stage == RPC_NATIVE_RECEIVE else None

    @property
    def RpcNativeMessageNumber(self):
        return self.MaterializationEpoch if self.Stage == RPC_NATIVE_RECEIVE else None


def Require(Condition, Message):
    if not Condition:
        raise ValueError("[Qualification:Publication] " + Message)


def IterRecords(PathValue, ExpectedRunId, ExpectedRole, ExpectedSlot=-1, ExpectedNonce=0):
    """Yield one validated record at a time from the same open evidence file."""
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
            Value = PublicationRecord(*RECORD.unpack(Bytes))
            (Stage, Flags, ConnectionSlot, ConnectionGeneration, ObjectSlot, ObjectGeneration,
             ServiceBytes, Nanoseconds, Tick, Sequence, DueTick, ControlEpoch,
             MaterializationEpoch, FrameSequence) = Value
            Require(Stage in Stages and Flags <= 1 and Nanoseconds >= PreviousNs,
                    "invalid stage, flags, or process-local time order")
            PreviousNs = Nanoseconds
            if Stage in (8, 9, 10):
                Require(ConnectionSlot > 0 and ConnectionGeneration > 0 and ObjectSlot > 0 and
                        ObjectGeneration > 0 and Tick > 0 and Sequence > 0 and ControlEpoch > 0 and
                        MaterializationEpoch > 0 and FrameSequence > 0 and ServiceBytes > 0,
                        "packet stage lacks full Character identity")
            if Stage == 11:
                Require(Flags == 0 and ConnectionSlot > 0 and ConnectionGeneration > 0 and
                        ObjectSlot > 0 and ObjectGeneration > 0 and Nanoseconds > 0 and
                        Tick > 0 and ServiceBytes == Sequence == DueTick == ControlEpoch ==
                        MaterializationEpoch == FrameSequence == 0,
                        "retirement lacks full relationship or carries state-packet fields")
            if Stage in (12, 13):
                Require(ConnectionSlot > 0 and ConnectionGeneration > 0 and
                        ObjectSlot > 0 and ObjectGeneration > 0 and Tick > 0 and
                        Sequence > 0 and MaterializationEpoch > 0 and
                        (DueTick == 0 or DueTick <= Tick) and
                        ServiceBytes == ControlEpoch == FrameSequence == 0,
                        "direct publication disposition lacks full generation/state identity")
            if Stage in range(14, 24):
                Require(ConnectionSlot > 0 and ConnectionGeneration > 0 and
                        ObjectSlot > 0 and ObjectGeneration > 0 and Sequence > 0 and
                        DueTick == ControlEpoch == MaterializationEpoch == FrameSequence == 0 and
                        (ServiceBytes > 0 if Stage in (16, 17, 21, 22) else ServiceBytes == 0) and
                        Tick == 0 and Flags == 0,
                        "RPC stage lacks request/remote identity or carries unrelated fields")
            if Stage == RPC_NATIVE_RECEIVE:
                Require(Flags == 0 and ConnectionSlot > 0 and ConnectionGeneration > 0 and
                        ObjectSlot > 0 and ObjectGeneration > 0 and Sequence > 0 and
                        Nanoseconds > 0 and ServiceBytes >= 32 and
                        Tick <= (1 << 63) - 1 and DueTick <= MAXIMUM_ADAPTER_EVENT_DEPTH and
                        ControlEpoch in (3, 4, 5) and 0 < MaterializationEpoch <= (1 << 63) - 1 and
                        FrameSequence >= Nanoseconds,
                        "RPC native dequeue identity, complete-ready age, event depth or protocol invalid")
                # The next record cannot precede the end of this sampled span.
                # Legacy stages keep their existing point-timestamp frontier.
                PreviousNs = FrameSequence
            if Stage in (24, 25, 27):
                Require(ConnectionSlot > 0 and ConnectionGeneration > 0 and ServiceBytes > 32 and
                        DueTick in (0, 1, 2) and FrameSequence > 0 and
                        ObjectSlot == ObjectGeneration == Tick == MaterializationEpoch == 0 and
                        0 <= ControlEpoch <= 5 and (Stage != 27 or Sequence > 0) and
                        (Flags == 0 or DueTick == 2), "ordinary complete-message record invalid")
            if Stage == 26:
                Require(1 <= Sequence <= 5 and FrameSequence > 0 and
                        ObjectSlot == ObjectGeneration == Tick == ServiceBytes == DueTick == ControlEpoch == MaterializationEpoch == 0 and
                        (Role == "SERVER" or ConnectionSlot > 0 and ConnectionGeneration > 0),
                        "sender-local traffic phase boundary invalid")
            if Role == "CLIENT":
                Require(Stage in CLIENT_STAGES, "server-only stage in client trace")
            Counts[Stage] = Counts.get(Stage, 0) + 1
            yield Value
        Require(Stream.read(1) == b"", "unexpected trailing trace bytes")
    Require((8 in Counts if Role == "SERVER" else 9 in Counts and 10 in Counts),
            "required packet stages absent")


def Validate(PathValue, ExpectedRunId, ExpectedRole, ExpectedSlot=-1, ExpectedNonce=0):
    Counts = {}
    Count = 0
    for Value in IterRecords(PathValue, ExpectedRunId, ExpectedRole, ExpectedSlot, ExpectedNonce):
        Counts[Value.Stage] = Counts.get(Value.Stage, 0) + 1
        Count += 1
    return {"RunId": ExpectedRunId, "Role": ExpectedRole, "Slot": ExpectedSlot,
            "Nonce": ExpectedNonce, "Records": Count, "Stages": Counts,
            "Bytes": Path(PathValue).stat().st_size, "CrossHostLatency": "NOT_MEASURED"}


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
