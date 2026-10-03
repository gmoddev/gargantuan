"""Exact fixed-workload demand audit at the canonical successful transport send.

Complete bytes include the 32-byte adapter once. Native Windows QPC ticks
authorize aggregation only among senders on the same owning host. No server
timestamp is subtracted from a client's timestamp and no service/latency curve
is inferred from these demand buckets. Replay uses O(32) metadata/merge state.
"""
import heapq
import re
from pathlib import Path

from farm_publication_trace import IterRecords, Require

PHASES = ("baseline", "load", "resident", "evict", "reload")
CLOCK = re.compile(r"\[Qualification:TrafficClock\] contract=sender_qpc_v1 run=([A-Za-z0-9_-]+) "
                   r"role=(SERVER|CLIENT) slot=(-1|[0-9]+) nonce=([0-9]+) "
                   r"host=([A-Za-z0-9_-]{1,63}) frequency=([0-9]+)\s*")


class Bucket:
    """Integer equivalent of DueServiceFixture::ServiceBucket.

    Min is min(A(before arrival)*F - R*t); Required is max(A(after)*F-R*t-Min).
    Updating the minimum BEFORE adding the tied arrival counts the full burst.
    """
    def __init__(self, Rate, Burst, Frequency):
        self.Rate, self.Burst, self.Frequency = Rate, Burst, Frequency
        self.Minimum = self.Required = self.Total = 0

    def Add(self, Amount, At):
        self.Minimum = min(self.Minimum, self.Total * self.Frequency - self.Rate * At)
        self.Total += Amount
        self.Required = max(self.Required,
                            self.Total * self.Frequency - self.Rate * At - self.Minimum)
        Require(self.Required <= self.Burst * self.Frequency,
                "ordinary all-interval demand exceeds canonical burst/rate")


def ClockReceipt(PathValue, Run, Role, Slot, Nonce):
    PathValue = Path(PathValue)
    Require(PathValue.is_file() and not PathValue.is_symlink() and
            PathValue.stat().st_size <= 16 * 1024 * 1024, "traffic clock log invalid")
    Rows = []
    with PathValue.open(encoding="utf-8") as Stream:
        for Line in Stream:
            if Line.startswith("[Qualification:TrafficClock]"):
                Match = CLOCK.fullmatch(Line)
                Require(Match is not None, "invalid traffic QPC receipt")
                Identity = Match.group(1), Match.group(2), int(Match.group(3)), int(Match.group(4))
                Require(Identity == (Run, Role, Slot, Nonce), "traffic QPC identity mismatch")
                Require(int(Match.group(6)) > 0 and Match.group(5) != "unknown",
                        "traffic QPC owning-host domain unavailable")
                Rows.append({"Host": Match.group(5).upper(), "Frequency": int(Match.group(6))})
    Require(len(Rows) <= 1, "duplicate traffic QPC receipt")
    return Rows[0] if Rows else None


def Offers(Records, Role, Slot, Connections):
    Phase = 0
    Expected = 1
    Previous = 0
    for Value in Records:
        if Value.Stage not in (24, 25, 26):
            continue
        Require(Value.FrameSequence >= Previous, "sender QPC regressed")
        Previous = Value.FrameSequence
        if Value.Stage == 26:
            if Value.Flags:
                Require(Phase == 0 and Value.Sequence == Expected and Expected <= 5,
                        "traffic phase missing, repeated, or overlapping")
                Phase = Expected
            else:
                Require(Phase != 0 and Value.Sequence == Phase, "traffic phase end without start")
                Phase = 0
                Expected += 1
            continue
        Connection = Value.ConnectionSlot, Value.ConnectionGeneration
        Require(Connection in Connections, "ordinary send has unqualified generation")
        # Bootstrap/calibration and deliberate overload are separate workload
        # domains. Every fixed-phase ordinary send, including control, is charged.
        if not Phase:
            continue
        Require(Value.Stage == 24, "fixed-workload ordinary transport send rejected")
        if Value.DueTick == 1:
            Require(Value.ServiceBytes - 32 <= 16384, "Remote encoded frame exceeds canonical cap")
        yield (Value.FrameSequence, Slot if Role == "CLIENT" else Connections[Connection],
               Phase, Value.ServiceBytes, Value.Flags)
    Require(Phase == 0 and Expected == 6, "traffic phase evidence incomplete")


def Direction(Streams, Frequency):
    Global = (Bucket(512, 128, Frequency), Bucket(262144, 163840, Frequency))
    Peers = [(Bucket(64, 16, Frequency), Bucket(32768, 20480, Frequency)) for _ in range(32)]
    Phases = [{"Phase": Name, "Messages": 0, "CompleteBytes": 0, "ForcedCharacterMessages": 0}
              for Name in PHASES]
    for At, Slot, Phase, Bytes, Forced in heapq.merge(*Streams):
        Require(0 <= Slot < 32 and 1 <= Phase <= 5 and Bytes > 32,
                "invalid ordinary demand identity/bytes")
        for Count, Size in (Global, Peers[Slot]):
            Count.Add(1, At)
            Size.Add(Bytes, At)
        Row = Phases[Phase - 1]
        Row["Messages"] += 1
        Row["CompleteBytes"] += Bytes
        Row["ForcedCharacterMessages"] += Forced
    Require(all(Row["Messages"] for Row in Phases), "ordinary demand has an empty fixed phase")
    def Summary(Pair):
        return {"Messages": Pair[0].Total, "CompleteBytes": Pair[1].Total,
                "RequiredMessageBurstTimesFrequency": Pair[0].Required,
                "RequiredByteBurstTimesFrequency": Pair[1].Required}
    return {"Global": Summary(Global), "Peers": [dict(Slot=Slot, **Summary(Pair))
            for Slot, Pair in enumerate(Peers)], "Phases": Phases}


def Analyze(ServerPath, ServerLog, ClientSources, ReadyBySlot, Run):
    ServerClock = ClockReceipt(ServerLog, Run, "SERVER", -1, 0)
    ClientClocks = {Slot: ClockReceipt(Log, Run, "CLIENT", Slot, Nonce)
                    for Slot, (_, Nonce, Log) in ClientSources.items()}
    if ServerClock is None and all(Value is None for Value in ClientClocks.values()):
        return {"State": "NOT_MEASURED", "Reason": "sender-owned complete-message/QPC receipts absent"}
    Require(len(ClientClocks) == 32 and ServerClock is not None and
            all(Value is not None for Value in ClientClocks.values()), "partial ordinary demand receipts")
    ClientClock = ClientClocks[0]
    Require(all(Value == ClientClock for Value in ClientClocks.values()),
            "client sender clocks do not share one verified host/QPC domain")
    Connections = {Value[0]: Slot for Slot, Value in ReadyBySlot.items()}
    Egress = Direction([Offers(IterRecords(ServerPath, Run, "SERVER"), "SERVER", -1, Connections)],
                       ServerClock["Frequency"])
    Ingress = Direction([Offers(IterRecords(PathValue, Run, "CLIENT", Slot, Nonce), "CLIENT", Slot,
                                 {ReadyBySlot[Slot][1]: Slot})
                         for Slot, (PathValue, Nonce, _) in sorted(ClientSources.items())],
                        ClientClock["Frequency"])
    Require(all(Row["ForcedCharacterMessages"] for Row in Egress["Phases"]),
            "fixed phases lack actual forced reliable Character traffic")
    return {"State": "MEASURED_PASS", "Contract": "ordinary_sender_demand_v1",
            "Boundary": "SUCCESSFUL_TRANSPORT_SEND", "CompleteByteBasis": "PAYLOAD_PLUS_32_BYTE_ADAPTER",
            "ServerClock": ServerClock, "ClientClock": ClientClock,
            "Egress": Egress, "Ingress": Ingress, "CrossHostTiming": "NOT_USED",
            "StructuralFirstSendService": "SEPARATE_F1_GATE",
            "PacketReserve": "SEPARATE_FUNDED_TRANSPORT_GATE"}
