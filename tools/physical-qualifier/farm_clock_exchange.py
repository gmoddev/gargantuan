"""Bounded four-timestamp join for Farm32's existing ScaleFunction echo.

The timestamps are local steady-clock GNS submission/receive boundaries, not
packet-wire timestamps. The result is a causal cross-host offset interval at
each probe, not an exact one-way latency or a phase-long drift guarantee.
"""

from collections import defaultdict
import re

NATIVE_PREFIX = "[Qualification:FarmClock] event=native "
TERMINAL_PREFIX = "[Qualification:FarmClock] event=terminal "
COMPLETE_PREFIX = "[Qualification:FarmClock] event=calibration_complete "
SERVER_READY_PREFIX = "[Qualification:Server] event=ready "
CLIENT_READY_PREFIX = "[Qualification:Client] event=ready "
PHASE_START_PREFIX = "[Qualification:Scale] event=phase_start "
PHASE_END_PREFIX = "[Qualification:Scale] event=phase_end "
CALIBRATION_START_PREFIX = "[Qualification:FarmClock] event=calibration_start "
PHASE_NAMES = ("baseline", "load", "resident", "evict", "reload")
FIELDS = re.compile(r"([A-Za-z_]+)=([^\s]+)")


def Require(Condition, Message):
    if not Condition:
        raise ValueError("[Qualification:FarmClock] " + Message)


def Fields(Line, Prefix):
    return dict(FIELDS.findall(Line[len(Prefix):])) if Line.startswith(Prefix) else None


def Number(Row, Key):
    try:
        Value = int(Row[Key])
    except (KeyError, TypeError, ValueError) as Error:
        raise ValueError("[Qualification:FarmClock] invalid " + Key) from Error
    Require(Value >= 0, "negative " + Key)
    return Value


def OffsetInterval(T1, T2, T3, T4):
    """Return inclusive client-minus-server nanosecond bounds.

    T1/T4 are from one client; T2/T3 are from the server. No path-symmetry
    assumption is needed. The bound includes native queue and polling time.
    """
    Require(T1 < T4 and T2 <= T3, "native timestamp order invalid")
    Lower = T1 - T2
    Upper = T4 - T3
    Require(Lower <= Upper, "causal clock interval is empty")
    return Lower, Upper


def Analyze(ServerLines, ClientLinesBySlot, RunId, Peers=32, Epochs=5, Probes=4):
    Require(Peers > 0 and Epochs > 0 and Probes > 0, "invalid expected dimensions")
    Require(set(ClientLinesBySlot) == set(range(Peers)), "client slot set incomplete")
    ServerConnections = {}
    ServerNonces = {}
    ServerRows = []
    PhaseStarts = {}
    PhaseEnds = {}
    CalibrationStarts = {}
    CompleteEpochs = set()
    ServerTerminal = []
    for Line in ServerLines:
        if (Row := Fields(Line, SERVER_READY_PREFIX)) is not None and Row.get("run") == RunId:
            Nonce = Number(Row, "nonce")
            Slot = (Nonce & 0xffffffff) - 1
            Require(0 <= Slot < Peers, "server nonce slot invalid")
            Key = (Number(Row, "connection_slot"), Number(Row, "connection_generation"))
            Require(Key not in ServerConnections and Slot not in ServerConnections.values(),
                    "duplicate server peer identity")
            ServerConnections[Key] = Slot
            ServerNonces[Slot] = Nonce
        elif (Row := Fields(Line, PHASE_START_PREFIX)) is not None and Row.get("run") == RunId:
            Phase = Row.get("phase")
            Require(Phase in PHASE_NAMES[:Epochs] and Phase not in PhaseStarts,
                    "duplicate or unexpected phase start")
            PhaseStarts[Phase] = Number(Row, "monotonic_us") * 1000
        elif (Row := Fields(Line, PHASE_END_PREFIX)) is not None and Row.get("run") == RunId:
            Phase = Row.get("phase")
            Require(Phase in PHASE_NAMES[:Epochs] and Phase not in PhaseEnds,
                    "duplicate or unexpected phase end")
            PhaseEnds[Phase] = Number(Row, "monotonic_us") * 1000
        elif (Row := Fields(Line, CALIBRATION_START_PREFIX)) is not None and Row.get("run") == RunId:
            Epoch = Number(Row, "epoch")
            Require(1 <= Epoch <= Epochs and Epoch not in CalibrationStarts and
                    Row.get("next_phase") == PHASE_NAMES[Epoch - 1],
                    "calibration start identity invalid")
            CalibrationStarts[Epoch] = Number(Row, "monotonic_us") * 1000
        elif (Row := Fields(Line, NATIVE_PREFIX)) is not None and Row.get("run") == RunId:
            Require(Row.get("role") == "server" and Row.get("slot") == "-1",
                    "server native role invalid")
            ServerRows.append(Row)
        elif (Row := Fields(Line, COMPLETE_PREFIX)) is not None and Row.get("run") == RunId:
            Epoch = Number(Row, "epoch")
            Require(Epoch not in CompleteEpochs and Number(Row, "count") == Peers,
                    "calibration completion invalid")
            CompleteEpochs.add(Epoch)
        elif (Row := Fields(Line, TERMINAL_PREFIX)) is not None and Row.get("run") == RunId:
            ServerTerminal.append(Row)
    Require(len(ServerConnections) == Peers, "server ready identities incomplete")
    Require(set(PhaseStarts) == set(PHASE_NAMES[:Epochs]), "phase starts incomplete")
    Require(set(PhaseEnds) == set(PHASE_NAMES[:Epochs]) and
            set(CalibrationStarts) == set(range(1, Epochs + 1)),
            "phase or calibration boundaries incomplete")
    for Epoch in range(1, Epochs + 1):
        Phase = PHASE_NAMES[Epoch - 1]
        Require(CalibrationStarts[Epoch] < PhaseStarts[Phase] < PhaseEnds[Phase],
                "phase calibration boundary order invalid")
        if Epoch > 1:
            Require(PhaseEnds[PHASE_NAMES[Epoch - 2]] <= CalibrationStarts[Epoch],
                    "calibration overlapped prior measured phase")
    Require(CompleteEpochs == set(range(1, Epochs + 1)),
            "all-peer calibration barrier incomplete")
    Require(len(ServerTerminal) == 1 and Number(ServerTerminal[0], "records") == len(ServerRows) and
            Number(ServerTerminal[0], "overflow") == 0, "server native trace incomplete")

    ClientRows = []
    for Slot, Lines in ClientLinesBySlot.items():
        Ready = []
        Terminal = []
        for Line in Lines:
            if (Row := Fields(Line, CLIENT_READY_PREFIX)) is not None and Row.get("run_id") == RunId:
                Ready.append(Row)
            elif (Row := Fields(Line, NATIVE_PREFIX)) is not None and Row.get("run") == RunId:
                Require(Row.get("role") == "client" and Number(Row, "slot") == Slot,
                        "client native role or slot invalid")
                ClientRows.append((Slot, Row))
            elif (Row := Fields(Line, TERMINAL_PREFIX)) is not None and Row.get("run") == RunId:
                Terminal.append(Row)
        Require(len(Ready) == 1 and Number(Ready[0], "slot") == Slot and
                Number(Ready[0], "nonce") == ServerNonces[Slot],
                "client ready identity invalid")
        Count = sum(1 for ItemSlot, _ in ClientRows if ItemSlot == Slot)
        Require(len(Terminal) == 1 and Number(Terminal[0], "slot") == Slot and
                Number(Terminal[0], "records") == Count and
                Number(Terminal[0], "overflow") == 0, "client native trace incomplete")

    Samples = defaultdict(dict)

    def Add(Slot, Row):
        Epoch, Sequence = Number(Row, "epoch"), Number(Row, "sequence")
        Require(1 <= Epoch <= Epochs and 1 <= Sequence <= Probes,
                "probe epoch or sequence invalid")
        Request = Number(Row, "request")
        Require(Request > 0, "request identity invalid")
        Kind, Stage = Number(Row, "kind"), Row.get("stage")
        Require(Kind in (3, 4) and Stage in ("GnsBefore", "GnsQueued", "GnsReceive"),
                "native stage or kind invalid")
        Key = (Slot, Epoch, Sequence, Request)
        Label = ("request" if Kind == 3 else "response") + ":" + Stage
        Require(Label not in Samples[Key], "duplicate native probe event")
        Samples[Key][Label] = (Number(Row, "monotonic_ns"), Number(Row, "result")
                               if Stage == "GnsQueued" else None)

    for Row in ServerRows:
        Key = (Number(Row, "connection_slot"), Number(Row, "connection_generation"))
        Require(Key in ServerConnections, "server native connection has no ready identity")
        Add(ServerConnections[Key], Row)
    for Slot, Row in ClientRows:
        Add(Slot, Row)

    ExpectedLabels = {"request:GnsBefore", "request:GnsQueued", "request:GnsReceive",
                      "response:GnsBefore", "response:GnsQueued", "response:GnsReceive"}
    Require(len(Samples) == Peers * Epochs * Probes, "native probe count incomplete")
    Result = []
    RequestIds = defaultdict(set)
    for (Slot, Epoch, Sequence, Request), Events in sorted(Samples.items()):
        Require(Request not in RequestIds[Slot], "client request identity reused")
        RequestIds[Slot].add(Request)
        Require(set(Events) == ExpectedLabels, "native four-timestamp path incomplete")
        T1 = Events["request:GnsBefore"][0]
        T2 = Events["request:GnsReceive"][0]
        T3 = Events["response:GnsBefore"][0]
        T4 = Events["response:GnsReceive"][0]
        Require(Events["request:GnsQueued"][1] == 1 and
                Events["response:GnsQueued"][1] == 1,
                "native send was not accepted")
        Require(T1 <= Events["request:GnsQueued"][0] < T4 and
                T2 <= T3 <= Events["response:GnsQueued"][0],
                "per-host native event order invalid")
        Require(T2 < PhaseStarts[PHASE_NAMES[Epoch - 1]] and
                T3 < PhaseStarts[PHASE_NAMES[Epoch - 1]],
                "clock probe entered measured phase")
        Require(CalibrationStarts[Epoch] <= T2 <= T3,
                "clock probe preceded its calibration window")
        Lower, Upper = OffsetInterval(T1, T2, T3, T4)
        Result.append({"Slot": Slot, "Epoch": Epoch, "Sequence": Sequence,
                       "Request": Request, "LowerNs": Lower, "UpperNs": Upper,
                       "WidthNs": Upper - Lower})
    return {"Format": "GargantuanFarm32NativeClockExchange", "Version": 1,
            "RunId": RunId, "Status": "BOUNDED_AT_PROBE", "Samples": Result,
            "PhaseLongOffset": "NOT_MEASURED", "OneWayLatency": "NOT_MEASURED"}
