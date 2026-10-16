"""Exact fixed-workload demand audit at the canonical successful transport send.

Complete bytes include the 32-byte adapter once. Native Windows QPC ticks
authorize aggregation only among senders on the same owning host. No server
timestamp is subtracted from a client's timestamp and no service/latency curve
is inferred from these demand buckets. Replay uses O(32) metadata/merge state.
"""
import heapq
import re
import sqlite3
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


def Offers(Records, Role, Slot, Connections, Database, Coverage=None):
    Phase = 0
    Expected = 1
    Previous = 0
    LastQueued = 0
    for Value in Records:
        if Value.Stage not in (24, 25, 26, 27):
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
        if Value.Stage == 27:
            Require(Value.Sequence == LastQueued + 1 and Value.ControlEpoch == Phase,
                    "ordinary enqueue identity or originating phase mismatch")
            LastQueued = Value.Sequence
            Database.execute("INSERT INTO OrdinaryPending VALUES(?,?,?,?,?,?,?,?,?)",
                             (Role, Slot, Value.Sequence, *Connection, Phase,
                              Value.ServiceBytes, Value.DueTick, Value.Flags))
            if Phase and Coverage is not None:
                Coverage[Slot if Role == "CLIENT" else Connections[Connection]]["QueuedMessages"] += 1
            continue
        Origin = Phase
        if Value.Sequence:
            Pending = Database.execute("SELECT CS,CG,Phase,Bytes,Kind,Forced FROM OrdinaryPending "
                                       "WHERE Role=? AND Slot=? AND Sequence=?",
                                       (Role, Slot, Value.Sequence)).fetchone()
            Require(Pending == (*Connection, Value.ControlEpoch, Value.ServiceBytes, Value.DueTick, Value.Flags),
                    "ordinary handoff lacks exact queued identity/bytes/origin")
            Database.execute("DELETE FROM OrdinaryPending WHERE Role=? AND Slot=? AND Sequence=?",
                             (Role, Slot, Value.Sequence))
            Origin = Value.ControlEpoch
        else:
            Require(Value.ControlEpoch == 0 and Value.DueTick == 0,
                    "direct ordinary control send forged queued phase or gameplay identity")
        # A queued message retains its admitted phase even after phase End or
        # deliberate overload. Direct non-scheduler sends use the current phase.
        # Actual handoff QPC drives the buckets; crossing phases never resets them.
        if not Origin:
            continue
        Require(Value.Stage == 24, "fixed-workload ordinary transport send rejected")
        if Coverage is not None:
            Row = Coverage[Slot if Role == "CLIENT" else Connections[Connection]]
            Row["SuccessfulQueuedMessages" if Value.Sequence else "DirectMessages"] += 1
        if Value.DueTick == 1:
            Require(Value.ServiceBytes - 32 <= 16384, "Remote encoded frame exceeds canonical cap")
        yield (Value.FrameSequence, Slot if Role == "CLIENT" else Connections[Connection],
               Origin, Value.ServiceBytes, Value.Flags)
    Require(Phase == 0 and Expected == 6, "traffic phase evidence incomplete")
    Require(Database.execute("SELECT count(*) FROM OrdinaryPending WHERE Role=? AND Slot=? AND Phase>0",
                             (Role, Slot)).fetchone()[0] == 0,
            "fixed-phase ordinary queued work was dropped or never handed off")


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


def NativeRows(PathValue, Prefix):
    """Reuse bounded indexed stdout; no new trace, payload or evidence storage."""
    PathValue = Path(PathValue)
    Require(PathValue.is_file() and not PathValue.is_symlink() and
            PathValue.stat().st_size <= 16 * 1024 * 1024, "ordinary FIFO log invalid")
    Rows = []
    with PathValue.open(encoding="utf-8") as Stream:
        for Line in Stream:
            if not Line.startswith(Prefix):
                continue
            Row = {}
            for Field in Line[len(Prefix):].strip().split():
                Match = re.fullmatch(r"([a-z_]+)=([^\s=]+)", Field)
                Require(Match is not None and Match[1] not in Row, "malformed ordinary FIFO receipt")
                Row[Match[1]] = Match[2]
            Rows.append(Row)
            Require(len(Rows) <= 32, "ordinary FIFO certificate count exceeds bound")
    return Rows


def NativeNumber(Row, Field):
    Value = Row.get(Field)
    Require(isinstance(Value, str) and re.fullmatch(r"0|[1-9][0-9]*", Value),
            "ordinary FIFO counter missing or malformed: " + Field)
    Value = int(Value)
    Require(Value <= 0xffffffffffffffff, "ordinary FIFO counter overflow")
    return Value


def NativeConnection(Row):
    Value = Row.get("connection", "")
    Match = re.fullmatch(r"([1-9][0-9]*):([1-9][0-9]*)", Value)
    Require(Match is not None, "ordinary FIFO connection identity invalid")
    return int(Match[1]), int(Match[2])


def AnalyzeOrdinaryFIFO(ServerLog, ClientSources, ReadyBySlot, Run, Egress, Ingress):
    """Join positive-native 50-ms certificates to ALL originating normal sends.

    API acceptance/demand replay is only the denominator. Native completion
    certificates supply service truth; no server/client clock subtraction occurs.
    """
    ServerRows = NativeRows(ServerLog, "[Qualification:FarmF1]")
    Clients = {Slot: NativeRows(Log, "[Qualification:OrdinaryFIFO]")
               for Slot, (_, _, Log) in ClientSources.items()}
    Require(all(len(Rows) <= 1 for Rows in Clients.values()), "duplicate ordinary FIFO client certificate")
    if not ServerRows or len(ServerRows) != 32 or any(len(Rows) != 1 for Rows in Clients.values()) or \
            any(Row.get("contract") != "HYBRID_F1_V1" for Row in ServerRows) or \
            any(Rows[0].get("contract") != "HYBRID_F1_V1" for Rows in Clients.values() if Rows):
        return {"State": "NOT_MEASURED", "Contract": "HYBRID_F1_V1",
                "Reason": "all 32 server and 32 client native FIFO certificates required; legacy/unknown contracts are not reinterpreted"}
    Server = {}
    for Row in ServerRows:
        Require(Row.get("event") == "peer" and Row.get("run") == Run,
                "ordinary FIFO server run or event differs")
        Connection = NativeConnection(Row)
        Require(Connection not in Server, "duplicate ordinary FIFO server connection")
        Server[Connection] = Row
    Require(set(Server) == {Value[0] for Value in ReadyBySlot.values()},
            "ordinary FIFO server generations differ from ready peers")
    Summaries = []
    ClientFields = {"event", "contract", "run", "slot", "nonce", "connection", "registered", "completed",
                    "pending", "max_latency_us", "ever_failed", "last_failed", "active_failed", "invalid", "queued_failed"}
    ServerFailures = ("queued_failed", "ordinary_ever_failed", "ordinary_last_completed_failed", "ordinary_active_deadline_failed",
                      "ordinary_evidence_invalid", "ordinary_failure_sequence", "ordinary_failure_phase", "ordinary_failure_message",
                      "ordinary_failure_accepted_us", "ordinary_failure_completed_us", "ordinary_failure_expected_bytes",
                      "ordinary_failure_first_sent_bytes")
    for Slot in range(32):
        ServerRow, ClientRow = Server[ReadyBySlot[Slot][0]], Clients[Slot][0]
        Require(set(ClientRow) == ClientFields and ClientRow["event"] == "client" and ClientRow["run"] == Run and
                NativeNumber(ClientRow, "slot") == Slot and NativeNumber(ClientRow, "nonce") == ClientSources[Slot][1] and
                NativeConnection(ClientRow) == ReadyBySlot[Slot][1], "ordinary FIFO client schema or ready identity differs")
        for Role, Row, Demand, Fields, Failures in (
                ("SERVER", ServerRow, Egress["Peers"][Slot],
                 ("ordinary_registered", "ordinary_completed", "ordinary_pending", "ordinary_max_latency_us"), ServerFailures),
                ("CLIENT", ClientRow, Ingress["Peers"][Slot],
                 ("registered", "completed", "pending", "max_latency_us"),
                 ("ever_failed", "last_failed", "active_failed", "invalid", "queued_failed"))):
            Registered, Completed, Pending, Maximum = [NativeNumber(Row, Field) for Field in Fields]
            Require(Registered > 0 and Registered == Completed == Demand["Messages"] and Pending == 0 and Maximum <= 50000 and
                    all(NativeNumber(Row, Field) == 0 for Field in Failures),
                    "ordinary FIFO native coverage, completion or 50-ms hard service failed")
            Require(Demand["QueuedMessages"] == Demand["SuccessfulQueuedMessages"] and
                    Demand["Messages"] == Demand["SuccessfulQueuedMessages"] + Demand["DirectMessages"],
                    "ordinary FIFO queued/successful denominator differs")
            Summaries.append({"Role": Role, "Slot": Slot, "Registered": Registered, "Completed": Completed,
                              "MaximumLatencyMicroseconds": Maximum, "QueuedMessages": Demand["QueuedMessages"],
                              "DirectMessages": Demand["DirectMessages"]})
    return {"State": "MEASURED_PASS", "Contract": "HYBRID_F1_V1", "ServerConnections": 32, "ClientCertificates": 32,
            "RoleProcesses": 33, "Certificates": 64, "Boundary": "SCHEDULER_ACCEPTED_TO_COMPLETE_POSITIVE_NATIVE_FIRST_SEND",
            "Scope": "ORIGINATING_FIXED_PHASES_1_TO_5", "MaximumLatencyMicroseconds": max(Row["MaximumLatencyMicroseconds"] for Row in Summaries),
            "Observations": Summaries, "CrossHostTiming": "NOT_USED"}


def Analyze(ServerPath, ServerLog, ClientSources, ReadyBySlot, Run, Database=None):
    if Database is None:
        # Standalone deterministic fixtures. Production supplies its existing
        # bounded disk-backed 4-GiB scratch database, not an in-memory queue.
        with sqlite3.connect(":memory:") as Temporary:
            return Analyze(ServerPath, ServerLog, ClientSources, ReadyBySlot, Run, Temporary)
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
    Database.execute("CREATE TABLE OrdinaryPending(Role TEXT,Slot INTEGER,Sequence INTEGER,CS INTEGER,CG INTEGER,"
                     "Phase INTEGER,Bytes INTEGER,Kind INTEGER,Forced INTEGER,PRIMARY KEY(Role,Slot,Sequence)) WITHOUT ROWID")
    def Coverage():
        return [{"QueuedMessages": 0, "SuccessfulQueuedMessages": 0, "DirectMessages": 0} for _ in range(32)]
    EgressCoverage, IngressCoverage = Coverage(), Coverage()
    Egress = Direction([Offers(IterRecords(ServerPath, Run, "SERVER"), "SERVER", -1, Connections, Database, EgressCoverage)],
                       ServerClock["Frequency"])
    Ingress = Direction([Offers(IterRecords(PathValue, Run, "CLIENT", Slot, Nonce), "CLIENT", Slot,
                                 {ReadyBySlot[Slot][1]: Slot}, Database, IngressCoverage)
                         for Slot, (PathValue, Nonce, _) in sorted(ClientSources.items())],
                        ClientClock["Frequency"])
    Require(all(Row["ForcedCharacterMessages"] for Row in Egress["Phases"]),
            "fixed phases lack actual forced reliable Character traffic")
    for DirectionValue, CoverageValue in ((Egress, EgressCoverage), (Ingress, IngressCoverage)):
        for Row, Count in zip(DirectionValue["Peers"], CoverageValue):
            Require(Count["QueuedMessages"] == Count["SuccessfulQueuedMessages"] and
                    Row["Messages"] == Count["SuccessfulQueuedMessages"] + Count["DirectMessages"],
                    "ordinary queued/successful transport coverage differs")
            Row.update(Count)
    OrdinaryFIFO = AnalyzeOrdinaryFIFO(ServerLog, ClientSources, ReadyBySlot, Run, Egress, Ingress)
    return {"State": "MEASURED_PASS", "Contract": "ordinary_sender_demand_v1",
            "Boundary": "SUCCESSFUL_TRANSPORT_SEND", "CompleteByteBasis": "PAYLOAD_PLUS_32_BYTE_ADAPTER",
            "ServerClock": ServerClock, "ClientClock": ClientClock,
            "Egress": Egress, "Ingress": Ingress, "CrossHostTiming": "NOT_USED",
            "StructuralFirstSendService": "SEPARATE_F1_GATE",
            "PacketReserve": "SEPARATE_FUNDED_TRANSPORT_GATE", "OrdinaryFIFO": OrdinaryFIFO}
