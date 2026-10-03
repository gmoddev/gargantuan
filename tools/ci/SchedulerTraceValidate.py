"""Offline loss and clock-window validation. Never attributes a stall or changes latency."""
import argparse
import csv
import json
import math
from pathlib import Path

CSV_FIELDS = 'Qpc,Processor,Opcode,Version,HeaderPid,HeaderTid,NewTid,OldTid,TargetTid,TargetPid,OldWaitReason,OldWaitMode,OldState,ReadyAdjustReason,ReadyAdjustIncrement,ReadyFlags'.split(',')


def Validate(Metadata, Stdout, Case="Full", CsvPath=None):
    Errors = []

    def Require(Condition, Message):
        if not Condition:
            Errors.append(Message)

    Require(Metadata.get("Format") == "GargantuanSchedulerTrace" and Metadata.get("Version") == 1, "unknown trace metadata")
    Require(Metadata.get("WorkloadCase") == Case or (Case == "Full" and "WorkloadCase" not in Metadata),
            "wrong workload metadata")
    for Name in ("StartStatus", "QueryStatus", "StopStatus", "EventsLost", "LogBuffersLost", "RealTimeBuffersLost",
                 "HeaderEventsLost", "HeaderBuffersLost", "DecodeOpenStatus", "DecodeProcessStatus", "UnsupportedEvents"):
        Require(type(Metadata.get(Name)) is int and Metadata[Name] == 0, Name + " is nonzero or unmeasured")
    for Name, Expected in (("DiagnosticComplete", True), ("ChildResumed", True), ("ChildTreeReaped", True),
                           ("TimedOut", False), ("ChildLogCapped", False), ("ChildLogFailed", False), ("CsvCapped", False), ("DecodeTimedOut", False)):
        Require(type(Metadata.get(Name)) is bool and Metadata[Name] is Expected, Name + " invalid")
    Require(type(Metadata.get("ChildLaunchAttempts")) is int and Metadata["ChildLaunchAttempts"] == 1, "expected one workload launch")
    Require(type(Metadata.get("ClockType")) is int and Metadata["ClockType"] == 1, "trace is not QPC")
    Frequency = Metadata.get("QpcFrequency")
    Require(type(Frequency) is int and Frequency > 0 and Frequency == Metadata.get("ControllerQpcFrequency"), "QPC frequency mismatch")
    Names = ("ControllerStartQpc", "AfterTraceStartQpc", "BeforeChildResumeQpc", "AfterChildExitQpc", "ControllerEndQpc")
    Ticks = [Metadata.get(Name) for Name in Names]
    Require(all(type(Value) is int and Value > 0 for Value in Ticks) and Ticks == sorted(Ticks, key=lambda V: V if type(V) is int else -1), "controller QPC order invalid")
    Require(type(Metadata.get("HeaderStartFileTime")) is int and type(Metadata.get("HeaderEndFileTime")) is int and
            0 < Metadata["HeaderStartFileTime"] <= Metadata["HeaderEndFileTime"], "ETL FILETIME boundaries absent")
    Anchors = []
    for Line in Stdout.splitlines():
        if not Line.startswith("[Qualification:ClockAnchor] "):
            continue
        try:
            Parts = [Part.split("=", 1) for Part in Line.split()[1:]]
            Fields = dict(Parts)
            Require(len(Fields) == len(Parts), "duplicate fixture anchor field")
            for Key in ("pid", "native_tid", "native_valid", "steady_ns", "qpc_before", "qpc_after", "qpc_frequency"):
                Fields[Key] = int(Fields[Key])
            Require(Fields["native_valid"] == 1 and Fields["pid"] == Metadata.get("ChildPid") and
                    Fields["native_tid"] == Metadata.get("ChildMainTid") and Fields["qpc_frequency"] == Frequency,
                    "fixture native identity/clock mismatch")
            Require(Fields.get("profile") == ("ACK_STATS" if Case == "AckStats" else "FULL_RESERVATION"), "unexpected fixture profile")
            Require(Metadata["BeforeChildResumeQpc"] <= Fields["qpc_before"] <= Fields["qpc_after"] <= Metadata["AfterChildExitQpc"], "fixture anchor outside child interval")
            Require(Metadata["MainFirstQpc"] <= Fields["qpc_before"] <= Fields["qpc_after"] <= Metadata["MainLastQpc"], "target thread events do not bracket fixture anchor")
            Anchors.append(Fields)
        except (ValueError, KeyError, TypeError):
            Errors.append("malformed fixture clock anchor")
    Require(bool(Anchors), "no fixture anchors")
    ByCase = {}
    for Anchor in Anchors:
        ByCase.setdefault(Anchor.get("case"), []).append(Anchor)
    for CaseName, Pair in ByCase.items():
        Require(len(Pair) == 2 and [Value.get("boundary") for Value in Pair] == ["BEGIN", "END"], "missing or duplicate anchor pair: " + str(CaseName))
        if len(Pair) == 2:
            Require(Pair[0]["qpc_after"] <= Pair[1]["qpc_before"] and Pair[0]["steady_ns"] <= Pair[1]["steady_ns"], "case clock order invalid")
    Extra = {}
    if Case == "Aggregate32Structural":
        try:
            Expected = ("aggregate-baseline", "aggregate-overload", "aggregate-recovery")
            Require(list(ByCase) == list(Expected[:len(ByCase)]) and 0 < len(ByCase) <= 3,
                    "aggregate phases missing, unexpected or out of order")
            Require([(Value.get("case"), Value.get("boundary")) for Value in Anchors] ==
                    [(Name, Boundary) for Name in Expected[:len(ByCase)] for Boundary in ("BEGIN", "END")],
                    "aggregate anchor sequence is not a complete ordered prefix")
            for Index, Pair in enumerate(ByCase.values()):
                Require(all(Value.get("phase_index") == str(Index) for Value in Pair),
                        "aggregate phase identity differs from fixed case")
                if Index and len(Pair) == 2:
                    Previous = list(ByCase.values())[Index - 1]
                    Require(len(Previous) == 2 and Previous[-1]["qpc_after"] <= Pair[0]["qpc_before"] and
                            Previous[-1]["steady_ns"] <= Pair[0]["steady_ns"], "aggregate phase clocks overlap")
            Require(type(Metadata.get("ChildExitCode")) is int and
                    (Metadata["ChildExitCode"] != 0 or tuple(ByCase) == Expected),
                    "successful aggregate workload must retain all three phases")
            Require(Metadata.get("WorkloadArguments") == ["--reliable-workload-32-structural"],
                    "aggregate workload arguments differ from fixed fifth command")
            Extra = ValidateAggregateCsv(Metadata, CsvPath)
            Extra.update(ValidateAggregateEvidence(Metadata, Stdout, ByCase))
        except (OSError, ValueError, KeyError, TypeError, UnicodeError) as Error:
            Errors.append("aggregate coverage: " + str(Error))
    if Case == "AckStats":
        try:
            Require(set(ByCase) in ({"ackstats-control"}, {"ackstats-control", "ackstats-prompt"}), "unexpected stats arms")
            Require(type(Metadata.get("ChildExitCode")) is int and
                    (Metadata["ChildExitCode"] != 0 or set(ByCase) == {"ackstats-control", "ackstats-prompt"}),
                    "successful stats workload must retain both arms")
            Require(type(Metadata.get("UnsupportedLifecycleEvents")) is int and Metadata["UnsupportedLifecycleEvents"] == 0,
                    "service-thread lifecycle schema incomplete")
            Extra = ValidateStats(Metadata, Stdout, CsvPath, ByCase)
        except (OSError, ValueError, KeyError, TypeError, UnicodeError) as Error:
            Errors.append("stats coverage: " + str(Error))
    return {"Format": "GargantuanSchedulerTraceCoverage", "Version": 1,
            "State": "LOSS_FREE_ANCHOR_WINDOW_RETAINED" if not Errors else "INCOMPLETE",
            "CausalVerdict": "NOT_CLAIMED", "Errors": Errors, "FixtureAnchorCount": len(Anchors),
            "Note": "Window retention does not prove a scheduler cause; inspect the exact CSwitch/ReadyThread chain. Unknown lifecycle versions limit other-thread identity attribution.", **Extra}


def ValidateAggregateCsv(Metadata, CsvPath):
    def Need(Value, Message):
        if not Value:
            raise ValueError(Message)
    Need(CsvPath is not None and CsvPath.is_file() and CsvPath.stat().st_size <= 512 * 1024 * 1024,
         "bounded scheduler CSV absent")
    Count, Last, FirstMain, LastMain, Switches, Readies = 0, 0, 0, 0, 0, 0
    Main = Metadata['ChildMainTid']
    with CsvPath.open('r', encoding='ascii', newline='') as Stream:
        csv.field_size_limit(1024)
        Reader = csv.DictReader(Stream)
        Need(Reader.fieldnames == CSV_FIELDS, "unexpected scheduler CSV schema")
        for Row in Reader:
            Count += 1
            Need(Count <= 5_000_000 and None not in Row and all(V is not None for V in Row.values()),
                 "CSV cap/schema")
            Tick, Opcode = int(Row['Qpc']), int(Row['Opcode'])
            Need(Tick > 0 and Tick >= Last, "unordered scheduler CSV")
            Last = Tick
            IsMain = False
            if Opcode == 36:
                Need(int(Row['Version']) in (2, 5), "unsupported switch schema")
                IsMain = int(Row['NewTid']) == Main or int(Row['OldTid']) == Main
                Switches += int(IsMain)
            elif Opcode == 50:
                Need(int(Row['Version']) == 2, "unsupported ready schema")
                IsMain = int(Row['TargetTid']) == Main
                Readies += int(IsMain)
            else:
                Need(Opcode in (1, 2, 3, 4) and int(Row['Version']) in (2, 3), "unsupported lifecycle schema")
                IsMain = int(Row['TargetTid']) == Main
                if IsMain: Need(int(Row['TargetPid']) == Metadata['ChildPid'], "main lifecycle PID mismatch")
            if IsMain:
                if not FirstMain: FirstMain = Tick
                LastMain = Tick
    Need(Count == Metadata['DecodedRows'] and Switches > 0 and Readies > 0, "main scheduler rows incomplete")
    Need(FirstMain == Metadata['MainFirstQpc'] and LastMain == Metadata['MainLastQpc'], "main coverage metadata differs from raw CSV")
    return {'SchedulerCsvRows': Count, 'MainSwitchRows': Switches, 'MainReadyRows': Readies,
            'FixedWorkloadArgument': '--reliable-workload-32-structural'}


def ValidateAggregateEvidence(Metadata, Stdout, Arms):
    """Check bounded fixture evidence completeness, never latency or causation."""
    def Need(Value, Message):
        if not Value:
            raise ValueError(Message)

    def Number(Row, Name):
        Value = Row[Name]
        Need(Value.isascii() and Value.isdigit() and len(Value) <= 20 and int(Value) <= 2**64 - 1,
             'invalid integer ' + Name)
        return int(Value)

    Need(len(Stdout.encode('utf-8')) <= 32 * 1024 * 1024, 'aggregate stdout cap')
    Rows = {Name: {} for Name in Arms}
    Current = None
    Labels = {'AggregatePeerEvidence', 'RemoteChronology', 'RemoteSpan', 'RemoteResourceSpan',
              'AggregateOperationCoverage', 'AggregateStepSnapshot', 'AggregateOperationSpan', 'Aggregate'}
    for Line in Stdout.splitlines():
        if not Line.startswith('[Qualification:'):
            continue
        Prefix, _, Tail = Line.partition('] ')
        Label = Prefix.removeprefix('[Qualification:')
        if Label not in Labels and Label != 'ClockAnchor':
            continue
        Parts = [Part.split('=', 1) for Part in Tail.split()]
        Need(len(Line) <= 8192 and all(len(Part) == 2 and len(Part[1]) <= 1024 for Part in Parts),
             'bounded fixture evidence fields')
        Row = dict(Parts)
        Need(len(Row) == len(Parts), 'duplicate fixture evidence field')
        if Label == 'ClockAnchor':
            if Row.get('case') in Arms and Row.get('boundary') == 'BEGIN': Current = Row['case']
            continue
        if Label == 'Aggregate':
            Need(Current in Arms, 'aggregate summary outside retained phase')
            Name = Current
        else:
            Name = Row.get('case')
            if Name not in Arms:
                # Earlier single-peer diagnostic cases have separate ownership.
                Need(not str(Name).startswith('aggregate-'), 'unexpected aggregate evidence case')
                continue
            Need(Name == Current and Row.get('profile') == 'FULL_RESERVATION' and
                 Row.get('phase_index') == Arms[Name][0]['phase_index'], 'aggregate evidence phase/profile mismatch')
        Rows[Name].setdefault(Label, []).append(Row)
        Limit = 32 * 208 if Label in ('RemoteSpan', 'RemoteResourceSpan') else 64 * 99 + 99
        Need(len(Rows[Name][Label]) <= Limit, 'aggregate evidence row cap')

    Stable, RecordCount = {}, 0
    for Phase, (Name, Pair) in enumerate(Arms.items()):
        Data = Rows[Name]
        Need(len(Pair) == 2, 'incomplete aggregate anchor pair')
        Begin, End = Pair[0]['steady_ns'], Pair[1]['steady_ns']
        def Context(Row):
            Peer = Number(Row, 'peer')
            Need(Peer < 32, 'peer outside fixed population')
            Identity = (Number(Row, 'slot'), Number(Row, 'generation'))
            Need(Identity[0] > 0 and Identity[1] > 0 and max(Identity) <= 2**32 - 1,
                 'invalid connection identity')
            if Peer in Stable: Need(Stable[Peer] == Identity, 'generation/slot changed across phase')
            else: Stable[Peer] = Identity
            return Peer, Identity

        def PeerRows(Label):
            Result = {}
            for Row in Data.get(Label, []):
                Peer, _ = Context(Row)
                Need(Peer not in Result, 'duplicate ' + Label)
                Result[Peer] = Row
            Need(set(Result) == set(range(32)), 'missing fixed peer ' + Label)
            return Result

        Headers, Chronologies = PeerRows('AggregatePeerEvidence'), PeerRows('RemoteChronology')
        # Each peer owns a separate client transport. Its slot/generation pair
        # is transport-local; the peer index is part of the evidence identity.
        Spans, Resources = {}, {}
        for Label, Target in (('RemoteSpan', Spans), ('RemoteResourceSpan', Resources)):
            for Row in Data.get(Label, []):
                Peer, _ = Context(Row)
                Key = (Peer, Number(Row, 'id'))
                Need(Row.get('kind') == 'RPC' and Key[1] > 0 and Key not in Target, 'RPC identity duplicate/invalid')
                Target[Key] = Row
        Need(Spans.keys() == Resources.keys(), 'RPC/resource record coverage differs')
        Active = 32 if Phase == 1 else 8
        Summaries = {}
        for Row in Data.get('Aggregate', []):
            Peer = Number(Row, 'peer')
            Need(Peer < Active and Peer not in Summaries and Number(Row, 'connected') == 32 and
                 Number(Row, 'active') == Active and Number(Row, 'overload') == int(Phase == 1),
                 'aggregate summary identity duplicate/invalid')
            Summaries[Peer] = Row
        Need(set(Summaries) == set(range(Active)), 'missing active peer summaries')
        for Peer in range(32):
            Header, Chronology = Headers[Peer], Chronologies[Peer]
            Need(Header['configured'] == '1' and Header['invalid'] == '0' and
                 Chronology['invalid'] == Chronology['overflow'] == '0' and Number(Chronology, 'capacity') == 208,
                 'peer evidence invalid/overflow/unconfigured')
            Count = Number(Chronology, 'records')
            Need(Count <= (208 if Phase == 1 else 48) and (Peer < Active or Count == 0), 'RPC count outside reachable bound')
            Need({Id for Owner, Id in Spans if Owner == Peer} == set(range(1, Count + 1)),
                 'RPC chronology dropped/noncontiguous records')
            Accepted = Completed = Errors = 0
            for Id in range(1, Count + 1):
                Span, Resource = Spans[(Peer, Id)], Resources[(Peer, Id)]
                for Field in ('start_ns', 'end_ns', 'start_tid', 'end_tid', 'terminal'):
                    Need(Span[Field] == Resource[Field], 'RPC/resource endpoints differ')
                Start, Submitted, Finished = (Number(Span, Field) for Field in ('start_ns', 'submitted_ns', 'end_ns'))
                Observed = Number(Span, 'observed_ns')
                Need(Begin <= Start <= End and Observed == End and Span['recovery_probe'] == '0' and
                     Number(Span, 'start_tid') == Metadata['ChildMainTid'], 'RPC start/observation boundary invalid')
                Need(Number(Span, 'observed_step') >= max(Number(Span, 'start_step'),
                     Number(Span, 'submitted_step'), Number(Span, 'end_step')), 'RPC step observation reversed')
                Need(Resource['start_sample_before_ns'].isdigit() and
                     Begin <= Number(Resource, 'start_sample_before_ns') <= Start <= Number(Resource, 'start_sample_after_ns') <= End,
                     'RPC original start clock bracket invalid')
                Need(Span['accepted'] in ('0', '1') and Span['terminal'] in ('0', '1'), 'RPC boolean invalid')
                IsAccepted, Terminal = Span['accepted'] == '1', Span['terminal'] == '1'
                Accepted += int(IsAccepted); Completed += int(Terminal)
                Outcome = Span['outcome']
                Need(Outcome in ('TERMINAL', 'REJECTED', 'MISSING_AT_SERVICE_DEADLINE', 'MISSING_AT_CASE_END',
                                 'SUBMISSION_NOT_OBSERVED'), 'RPC outcome invalid')
                if Outcome != 'SUBMISSION_NOT_OBSERVED':
                    Need(Start <= Submitted <= End and Number(Span, 'submitted_tid') == Metadata['ChildMainTid'] and
                         Number(Span, 'submitted_step') >= Number(Span, 'start_step'), 'RPC submission boundary invalid')
                if Terminal:
                    Need(IsAccepted and Outcome == 'TERMINAL' and Start <= Finished <= End and
                         Number(Span, 'end_step') >= Number(Span, 'start_step') and
                         Number(Span, 'end_tid') == Metadata['ChildMainTid'] and Resource['endpoint_order_valid'] == '1',
                         'RPC terminal boundary invalid')
                    Need((Submitted <= Finished and Number(Span, 'submitted_step') <= Number(Span, 'end_step')) or
                         (Finished <= Submitted and Number(Span, 'end_step') <= Number(Span, 'submitted_step')),
                         'RPC callback/submission clock and step order disagree')
                    Need(Number(Resource, 'start_sample_after_ns') <= Number(Resource, 'end_sample_before_ns') <=
                         Finished <= Number(Resource, 'end_sample_after_ns') <= End, 'RPC original terminal clock bracket invalid')
                    Need(Span['payload_matched'] in ('0', '1') and Number(Span, 'terminal_status') <= 6, 'RPC terminal result invalid')
                    Errors += int(Span['terminal_status'] != '0' or Span['payload_matched'] != '1')
                    ValidateAggregateResource(Resource, Number, Need)
                else:
                    Need(Finished == 0 and Number(Span, 'end_tid') == 0 and Span['terminal_status'] == '-1' and
                         Resource['endpoint_order_valid'] == '0', 'missing/rejected RPC fabricated terminal')
                    Need((not IsAccepted and Outcome in ('REJECTED', 'SUBMISSION_NOT_OBSERVED')) or
                         (IsAccepted and Outcome in ('MISSING_AT_SERVICE_DEADLINE', 'MISSING_AT_CASE_END')),
                         'RPC outcome contradicts acceptance')
                    Need(Resource['sleep_delta_valid'] == '0' and
                         all(Resource[Kind + '_cpu_' + Bound + '_100ns'] == 'NOT_MEASURED'
                             for Kind in ('thread', 'process') for Bound in ('lower', 'upper')) and
                         all(Resource['measured_sleep_' + Kind] == 'NOT_MEASURED'
                             for Kind in ('count', 'requested_ns', 'actual_ns')),
                         'nonterminal RPC fabricated resource deltas')
                    Errors += int(Outcome == 'REJECTED')
                if Metadata['ChildExitCode'] == 0:
                    Need(IsAccepted and Terminal and Errors == 0, 'successful child has incomplete/error RPC')
            if Peer < Active:
                Summary = Summaries[Peer]
                Need(Accepted == Number(Summary, 'accepted') and Completed == Number(Summary, 'completed') and
                     Errors == Number(Summary, 'errors'), 'RPC counts disagree with aggregate summary')
                Need(Count > 0, 'active peer has no RPC chronology')
            RecordCount += Count
        ValidateAggregateOperations(Data, Stable, Begin, End, Metadata['ChildMainTid'], Number, Need)
    return {'CompleteAggregatePhases': len(Arms), 'CompleteRpcRecords': RecordCount,
            'OperationCoverage': 'SAMPLED_NOT_COMPLETE_CAUSAL_PROOF', 'RpcCoverage': 'COMPLETE_SUBMITTED_RECORDS'}


def ValidateAggregateResource(Row, Number, Need):
    for Kind in ('thread', 'process'):
        Values = []
        for Boundary in ('start_before', 'start_after', 'end_before', 'end_after'):
            Need(Row[Boundary + '_' + Kind + '_valid'] == '1', 'RPC endpoint CPU counter unavailable/invalid')
            Values.append(Number(Row, Boundary + '_' + Kind + '_100ns'))
        Need(Values == sorted(Values) and Number(Row, Kind + '_cpu_lower_100ns') == Values[2] - Values[1] and
             Number(Row, Kind + '_cpu_upper_100ns') == Values[3] - Values[0], 'RPC endpoint CPU brackets invalid')
    Need(Row['sleep_delta_valid'] == Row['start_sleep_valid'] == Row['end_sleep_valid'] == '1',
         'RPC sleep ledger invalid')
    for Name in ('count', 'requested_ns', 'actual_ns'):
        First, Last = Number(Row, 'start_sleep_' + Name), Number(Row, 'end_sleep_' + Name)
        Need(Last >= First and Number(Row, 'measured_sleep_' + Name) == Last - First, 'RPC sleep delta invalid')
    Start, End = Number(Row, 'start_ns'), Number(Row, 'end_ns')
    First, Last = Number(Row, 'start_sleep_last_completed_ns'), Number(Row, 'end_sleep_last_completed_ns')
    Need(First <= Start and First <= Last <= End and Number(Row, 'measured_sleep_actual_ns') <= End - Start,
         'RPC sleep endpoints invalid')
    if Number(Row, 'measured_sleep_count') == 0:
        Need(First == Last and Number(Row, 'measured_sleep_requested_ns') == Number(Row, 'measured_sleep_actual_ns') == 0,
             'RPC sleep ledger changed without sleep')
    else: Need(Last >= Start, 'RPC measured sleep predates request')


def ValidateAggregateOperations(Data, Stable, Begin, End, MainTid, Number, Need):
    Coverage = Data.get('AggregateOperationCoverage', [])
    Need(len(Coverage) == 1, 'operation coverage header missing/duplicate')
    Header = Coverage[0]
    Need(Header['invalid'] == Header['overflow'] == '0' and Number(Header, 'capacity') == 64 and
         Number(Header, 'observed_steps') > 0 and Number(Header, 'snapshots') <= 64 and
         Header['selection'] == 'existing_p95_gap_ge_150ms' and
         Header['coverage'] == 'SAMPLED_NOT_COMPLETE_CAUSAL_PROOF', 'operation coverage invalid/overflow/scope')
    Snapshots = {}
    for Row in Data.get('AggregateStepSnapshot', []):
        Tick = Number(Row, 'tick')
        Need(Tick not in Snapshots and Begin <= Number(Row, 'gap_start_ns') <= Number(Row, 'work_start_ns') <=
             Number(Row, 'work_end_ns') == Number(Row, 'gap_end_ns') <= End, 'operation snapshot timestamp/identity invalid')
        Snapshots[Tick] = Row
    Need(len(Snapshots) == Number(Header, 'snapshots'), 'operation snapshot count mismatch')
    Keys, Maxima = set(), set()
    for Row in Data.get('AggregateOperationSpan', []):
        Peer, Tick = Number(Row, 'peer'), Number(Row, 'tick')
        Need(Peer <= 32 and Row['subphase'] in ('poll', 'engine', 'session') and
             Row['coverage'] in ('phase_maximum', 'slow_step'), 'operation matrix scope invalid')
        Key = (Row['coverage'], Tick, Peer, Row['subphase'])
        Need(Key not in Keys and Row['timestamps_valid'] == '1' and Number(Row, 'native_tid') == MainTid and
             Begin <= Number(Row, 'start_ns') <= Number(Row, 'end_ns') <= End, 'operation matrix invalid/duplicate')
        Keys.add(Key)
        Need(Row['side'] == ('server' if Peer == 32 else 'client') and
             (Number(Row, 'slot'), Number(Row, 'generation')) == ((0, 0) if Peer == 32 else Stable[Peer]),
             'operation generation/side mismatch')
        if Row['coverage'] == 'phase_maximum':
            Need((Peer, Row['subphase']) not in Maxima, 'operation maxima duplicate')
            Maxima.add((Peer, Row['subphase']))
        else:
            Need(Tick in Snapshots and Number(Snapshots[Tick], 'work_start_ns') <= Number(Row, 'start_ns') <=
                 Number(Row, 'end_ns') <= Number(Snapshots[Tick], 'work_end_ns'), 'operation snapshot/span boundary mismatch')
        for Kind in ('thread', 'process'):
            Need(Row['before_' + Kind + '_valid'] in ('0', '1') and Row['after_' + Kind + '_valid'] in ('0', '1'),
                 'operation CPU validity flag invalid')
            if Row['before_' + Kind + '_valid'] == Row['after_' + Kind + '_valid'] == '1':
                Need(Number(Row, 'after_' + Kind + '_100ns') >= Number(Row, 'before_' + Kind + '_100ns'),
                     'operation CPU counter reversed')
                Need(math.isfinite(float(Row[Kind + '_cpu_ms'])) and float(Row[Kind + '_cpu_ms']) >= 0,
                     'operation CPU duration invalid')
            else: Need(Row[Kind + '_cpu_ms'] == 'NOT_MEASURED', 'operation CPU measurement fabricated')
    Need(Maxima == {(Peer, Kind) for Peer in range(33) for Kind in ('poll', 'engine', 'session')},
         'operation phase maxima incomplete')
    for Tick in Snapshots:
        Need({(Peer, Kind) for Scope, Step, Peer, Kind in Keys if Scope == 'slow_step' and Step == Tick} ==
             {(Peer, Kind) for Peer in range(33) for Kind in ('poll', 'engine', 'session')},
             'selected operation snapshot matrix incomplete')


def ValidateStats(Metadata, Stdout, CsvPath, Arms):
    def Need(Value, Message):
        if not Value:
            raise ValueError(Message)
    def Fields(Line):
        Parts = [Part.split("=", 1) for Part in Line.split()[1:]]
        Need(all(len(Part) == 2 for Part in Parts), "malformed stats marker")
        Value = dict(Parts)
        Need(len(Value) == len(Parts), "duplicate stats marker field")
        return Value
    Services, Snapshots, Summaries = [], [], []
    for Line in Stdout.splitlines():
        Need(len(Line) <= 8192, "stats log line cap")
        if Line.startswith("[Qualification:AckStatsServiceThread] "):
            Services.append(Fields(Line))
        elif Line.startswith("[Qualification:AckStatsNativeSnapshot] "):
            Snapshots.append(Fields(Line))
        elif Line.startswith("[Qualification:AckStatsDiagnostic] "):
            Summaries.append(Fields(Line))
        Need(len(Services) <= 16 and len(Snapshots) <= 16 and len(Summaries) <= 1, "stats evidence cap")
    Need(Services and len(Summaries) == 1 and int(Summaries[0]["valid"]) == 1 and
         int(Summaries[0]["thread_count"]) == len(Services), "service callback missing/overflow")
    Need([int(Row['sequence']) for Row in Services] == list(range(len(Services))), "service callback sequence")
    Need(set(Row['case'] for Row in Services) == set(Arms), "service callback/arm mismatch")
    if 'ackstats-prompt' in Arms:
        Need(Arms['ackstats-control'][-1]['qpc_after'] <= Arms['ackstats-prompt'][0]['qpc_before'], "stats arm order")
    for Row in Services + Snapshots:
        Need(Row['case'] in Arms and len(Arms[Row['case']]) == 2, "unbracketed stats arm")
        Begin, End = Arms[Row['case']]
        Need(int(Row['native_valid']) == 1 and int(Row['pid']) == Metadata['ChildPid'] and
             int(Row['qpc_frequency']) == Metadata['QpcFrequency'] and
             Begin['qpc_before'] <= int(Row['qpc_before']) <= int(Row['qpc_after']) <= End['qpc_after'],
             "stats identity/clock outside arm")
    for Row in Snapshots:
        Need(int(Row['native_tid']) == Metadata['ChildMainTid'] and int(Row['available']) == 1 and
             int(Row['native_us']) > 0 and int(Row['token']) >= 0 and Row['side'] in ('sender', 'receiver') and
             Row['stage'] in ('after-quiet', 'terminal', 'grant-failure'), "native snapshot identity/value")
    Need(Snapshots, "native snapshot brackets absent")
    Need(CsvPath is not None and CsvPath.is_file() and CsvPath.stat().st_size <= 512 * 1024 * 1024, "bounded scheduler CSV absent")
    Identities = {int(Row['native_tid']): [] for Row in Services}
    Count, Last = 0, 0
    with CsvPath.open('r', encoding='ascii', newline='') as Stream:
        csv.field_size_limit(1024)
        Reader = csv.DictReader(Stream)
        Need(Reader.fieldnames == CSV_FIELDS, "unexpected scheduler CSV schema")
        for Row in Reader:
            Count += 1
            Need(Count <= 5_000_000 and None not in Row and all(Value is not None for Value in Row.values()), "CSV cap/schema")
            Tick = int(Row['Qpc'])
            Need(Tick > 0 and Tick >= Last, "unordered scheduler CSV")
            Last = Tick
            if int(Row['Opcode']) not in (1, 2, 3, 4) or not Row['TargetTid'] or int(Row['TargetTid']) not in Identities:
                continue
            Need(int(Row['Version']) in (2, 3), "unsupported service lifecycle schema")
            Item = Identities[int(Row['TargetTid'])]
            Item.append((Tick, int(Row['Opcode']), int(Row['TargetPid'])))
            Need(len(Item) <= 64, "service lifecycle cap")
    Need(Count == Metadata['DecodedRows'], "CSV count mismatch")
    Used = set()
    for Service in Services:
        Events = Identities[int(Service['native_tid'])]
        Before = [Row for Row in Events if Row[0] <= int(Service['qpc_before'])]
        After = [Row for Row in Events if Row[0] >= int(Service['qpc_after'])]
        Need(Before and After, "service lifecycle not retained")
        Start, End = Before[-1], After[0]
        Identity = (int(Service['native_tid']), Start[0], End[0])
        Need(Identity not in Used and Start[1] in (1, 3) and End[1] in (2, 4) and
             Start[2] == End[2] == Metadata['ChildPid'] and
             Metadata['BeforeChildResumeQpc'] <= Start[0] < End[0] <= Metadata['AfterChildExitQpc'],
             "service lifecycle conflicting/reused")
        Used.add(Identity)
    return {'ServiceThreadCount': len(Services), 'NativeSnapshotCount': len(Snapshots),
            'NativeClockMapping': 'SNAPSHOT_BRACKETS_ONLY_NO_GLOBAL_OFFSET',
            'FiniteGrantTimestampResolutionNs': 1000}


def Main():
    Parser = argparse.ArgumentParser()
    Parser.add_argument("--root", type=Path, required=True)
    Parser.add_argument("--case", choices=("Full", "AckStats", "Aggregate32Structural"), default="Full")
    Args = Parser.parse_args()
    Result = {"State": "INCOMPLETE", "CausalVerdict": "NOT_CLAIMED"}
    try:
        MetadataPath = Args.root / "metadata.json"
        LogPath = Args.root / "workload.stdout.txt"
        if MetadataPath.stat().st_size > 65536 or LogPath.stat().st_size > 32 * 1024 * 1024:
            raise ValueError("raw metadata/log size exceeds fixed bound")
        Result = Validate(json.loads(MetadataPath.read_text(encoding="utf-8")), LogPath.read_text(encoding="utf-8", errors="strict"),
                          Args.case, Args.root / "scheduler.csv")
    except (OSError, ValueError, TypeError) as Error:
        Result["Errors"] = [str(Error)]
    with (Args.root / "coverage.json").open("x", encoding="utf-8") as Output:
        json.dump(Result, Output, indent=2)
    return 0 if Result["State"] == "LOSS_FREE_ANCHOR_WINDOW_RETAINED" else 125


if __name__ == "__main__":
    raise SystemExit(Main())
