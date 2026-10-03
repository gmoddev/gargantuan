"""Offline F1 replay. CSV rows are consumed native certificates, not packet events.

The native running-min verifier observes every native first-send. This replay
checks its latched failure/high-water certificate and finite-grant identities,
then derives the common four-grant bound by summing those four contracts. It
never invents a measured pool high-water or interpolates packet timestamps.
"""
import csv
from datetime import datetime, timezone
from decimal import Decimal, InvalidOperation
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import uuid

RATE = 16_777_216
GROUP = 524_288
RUN_BOUND = 18_025_216_000
FINITE_INTERCEPT = 101_911_296_000
MAX_ROWS = 65_536
TOOLS = Path(__file__).resolve().parents[1] / "tools" / "physical-qualifier"
FIELDS = ("simulation_step,slot,generation,consumed_us,feedback_us,present,valid,available,qualified,"
          "debt_token,debt_bytes,journal_lag,accepted_structural,structural_first,structural_ack,running_us,"
          "current_run_deficit_byte_us,max_run_deficit_byte_us,service_failed,active_grant_started_us,"
          "active_grant_bytes,first_sent_in_grant,grant_first_us,grant_complete_us,last_completed_token,"
          "last_completed_bytes,last_completed_activated_us,last_completed_first_us,last_completed_finish_us,"
          "last_completed_max_run_deficit_byte_us,last_completed_failed,retry,pending,unacked,retirement_sequence,"
          "retired_token,result_retired_bytes,grants,total_debt,created,retired,terminal,delta_first,delta_ack,"
          "delta_retry,backend_ns,queue_us,rate").split(",")


def Require(Condition, Message):
    if not Condition:
        raise ValueError("[Foundation3L:FourClient] " + Message)


def Digest(File):
    Hash = hashlib.sha256()
    with Path(File).open("rb") as Stream:
        for Chunk in iter(lambda: Stream.read(1024 * 1024), b""):
            Hash.update(Chunk)
    return Hash.hexdigest().upper()


def Read(File, Limit=16 * 1024 * 1024):
    Require(Path(File).stat().st_size <= Limit, "oversized text evidence")
    return Path(File).read_text(encoding="utf-8-sig")


def Object(File):
    def Unique(Pairs):
        Result = {}
        for Key, Value in Pairs:
            Require(Key not in Result, "duplicate JSON key")
            Result[Key] = Value
        return Result
    return json.loads(Read(File), object_pairs_hook=Unique)


def UInt(Value):
    Require(isinstance(Value, str) and re.fullmatch(r"0|[1-9][0-9]*", Value), "invalid unsigned integer")
    Number = int(Value)
    Require(Number <= 2**64 - 1, "integer exceeds native width")
    return Number


def Records(Text, Kind):
    Result = []
    for Line in Text.splitlines():
        if Line.startswith("[Probe:" + Kind + "] "):
            Fields = {}
            for Pair in Line.split()[1:]:
                Require("=" in Pair, "malformed native summary")
                Key, Value = Pair.split("=", 1)
                Require(Key not in Fields, "duplicate native summary field")
                Fields[Key] = Value
            Result.append(Fields)
    return Result


def One(Text, Kind):
    Values = Records(Text, Kind)
    Require(len(Values) == 1, "missing/duplicate " + Kind)
    return Values[0]


def NativePass(Text, Role):
    Require(One(Text, "Result") == {"pass": "1", "scope": "Phase1-only"}, "native result failed")
    Summary = One(Text, "Summary")
    Require(Summary.get("role") == Role and all(Summary.get(Key) == "0" for Key in
            ("errors", "overflow", "invalid", "floor_failure")), "native trace/role failure")
    Require(Summary.get("waves") == "32", "missing workload waves")
    Cleanup = One(Text, "Cleanup")
    Require(Cleanup.get("good") == "1" and all(Cleanup.get(Key) == "0" for Key in
            ("terminal", "outstanding", "grants")) and Cleanup.get("created") == Cleanup.get("retired"),
            "native terminal convergence failed")
    return Summary


def ReplayNative(ServerCsv, ServerLog, AdmissionsCsv=None):
    """Replay the fixed probe's bounded snapshot schema and native certificate."""
    Text = Read(ServerLog)
    Summary = NativePass(Text, "server")
    Curve = One(Text, "ServiceCurve")
    Expected = {"contract": "F1", "verdict": "PASS", "pool_curve": "derived-from-four-native-grant-curves",
                "peer_rate_Bps": str(RATE), "pool_rate_Bps": str(4 * RATE), "quantum_B": "1248",
                "startup_us": "5000", "run_us": "1000", "peer_finite_intercept_byte_us": str(FINITE_INTERCEPT),
                "peer_running_bound_byte_us": str(RUN_BOUND), "pool_running_bound_byte_us": str(4 * RUN_BOUND)}
    Require(all(Curve.get(Key) == Value for Key, Value in Expected.items()), "F1 constants/certificate mismatch")
    Admission = One(Text, "Admission")
    Require(all(Admission.get(Key) == "0" for Key in ("terminal", "outstanding", "grants")) and
            Admission.get("accepted") == Admission.get("retired") and Admission.get("grants_high_water") == "4",
            "admission conservation failed")
    PeerLogs = Records(Text, "PeerService")
    Require(len(PeerLogs) == 4 and len({P.get("slot") for P in PeerLogs}) == 4, "four unique service peers required")
    Identities = Records(Text, "Peer")
    Require(len(Identities) == 4 and len({P.get("slot") for P in Identities}) == 4 and
            len({P.get("nonce") for P in Identities}) == 4 and
            sorted(P.get("producer") for P in Identities) == ["0", "0", "0", "1"], "peer identity/producer mismatch")
    Identity = {UInt(P["slot"]): P for P in Identities}
    Admitted = {}
    if AdmissionsCsv is not None:
        LastToken, Qualified = {}, set()
        with Path(AdmissionsCsv).open(encoding="utf-8-sig", newline="") as Stream:
            Reader = csv.DictReader(Stream)
            Require(Reader.fieldnames == ["slot", "generation", "token", "bytes", "activated_us"], "admission CSV schema mismatch")
            for Raw in Reader:
                Require(None not in Raw and all(V is not None for V in Raw.values()) and len(Admitted) < 256,
                        "truncated/oversized admission evidence")
                Row = {K: UInt(V) for K, V in Raw.items()}
                Slot, Token = Row["slot"], Row["token"]
                Require(Slot in Identity and Row["generation"] == UInt(Identity[Slot]["generation"]) and
                        Token > LastToken.get(Slot, 0) and 0 < Row["bytes"] <= GROUP and Row["activated_us"] > 0,
                        "invalid admission identity/activation")
                if Row["activated_us"] == 2**64 - 1:
                    Require(Slot not in Qualified, "bootstrap resumed after a Ready-qualified grant")
                else:
                    Qualified.add(Slot)
                LastToken[Slot] = Token
                Admitted[Slot, Token] = Row
        Boundary = One(Text, "AdmissionBoundary")
        Require(Boundary == {"format": "F1_ACCEPTED_ACTIVATION_V1", "records": str(len(Admitted))},
                "admission record count differs from native summary")
    Peers = {}
    Completed = {}
    Grants = {}
    Retired = set()
    Bootstrap = []
    Rows = 0
    SeenSamples = set()
    PreviousGlobal = None
    with Path(ServerCsv).open(encoding="utf-8-sig", newline="") as Stream:
        Reader = csv.DictReader(Stream)
        Require(Reader.fieldnames == FIELDS, "native CSV schema mismatch")
        for Raw in Reader:
            Rows += 1
            Require(Rows <= MAX_ROWS and None not in Raw and all(V is not None for V in Raw.values()), "truncated/oversized CSV")
            Row = {Key: UInt(Value) for Key, Value in Raw.items()}
            Slot = Row["slot"]
            Sample = (Row["simulation_step"], Slot)
            Require(Sample not in SeenSamples, "duplicate native snapshot")
            SeenSamples.add(Sample)
            Require(Slot in Identity and Row["generation"] == UInt(Identity[Slot]["generation"]), "generation/slot mismatch")
            Require(Row["grants"] <= 4 and Row["terminal"] == 0 and Row["retired"] <= Row["created"] and
                    Row["created"] - Row["retired"] == Row["total_debt"], "row debt conservation failed")
            if PreviousGlobal:
                Require(all(Row[K] >= PreviousGlobal[K] for K in ("simulation_step", "consumed_us", "created", "retired")),
                        "global counters regressed")
            PreviousGlobal = Row
            Previous = Peers.get(Slot)
            if Previous:
                Require(all(Row[K] >= Previous[K] for K in ("feedback_us", "structural_first", "structural_ack",
                        "running_us", "max_run_deficit_byte_us", "retry", "accepted_structural", "retirement_sequence")),
                        "native cumulative counters regressed")
            Require(Row["structural_ack"] <= Row["structural_first"] <= Row["accepted_structural"] and
                    Row["structural_first"] - Row["structural_ack"] <= GROUP, "native first-send/ACK conservation failed")
            Require(Row["service_failed"] == Row["last_completed_failed"] == 0 and
                    max(Row["current_run_deficit_byte_us"], Row["max_run_deficit_byte_us"],
                        Row["last_completed_max_run_deficit_byte_us"]) <= RUN_BOUND, "latched native running service failure")
            # The maximum-size campaign, unlike preliminary connection setup,
            # must have current valid native feedback on every sampled row.
            Campaign = any(Key[0] == Slot and Value == GROUP for Key, Value in Grants.items())
            if Row["debt_bytes"] == GROUP or Campaign:
                Require(Row["present"] == Row["valid"] == Row["available"] == 1 and
                        0 <= Row["consumed_us"] - Row["feedback_us"] <= 50_000, "campaign feedback missing/stale")
            Token = Row["debt_token"]
            if Token:
                Key = (Slot, Token)
                Require(0 < Row["debt_bytes"] <= GROUP and Key not in Retired, "invalid/reused accepted token")
                Require(Key not in Grants or Grants[Key] == Row["debt_bytes"], "grant changed byte basis")
                Grants[Key] = Row["debt_bytes"]
                if AdmissionsCsv is not None:
                    Require(Key in Admitted and Admitted[Key]["bytes"] == Row["debt_bytes"], "unrecorded/changed accepted obligation")
                if Row["active_grant_bytes"]:
                    Require(Row["active_grant_bytes"] == Row["debt_bytes"] and
                            Row["first_sent_in_grant"] <= Row["active_grant_bytes"], "active grant attribution mismatch")
                    Demand = min(Row["active_grant_bytes"] * 1_000_000,
                                 max(0, RATE * (Row["feedback_us"] - Row["active_grant_started_us"]) - FINITE_INTERCEPT))
                    Require(Row["active_grant_started_us"] > 0 and
                            Demand <= Row["first_sent_in_grant"] * 1_000_000, "sampled finite envelope failed")
            Token = Row["last_completed_token"]
            if Token:
                Key = (Slot, Token)
                Certificate = tuple(Row[K] for K in ("last_completed_bytes", "last_completed_activated_us",
                    "last_completed_first_us", "last_completed_finish_us", "last_completed_max_run_deficit_byte_us"))
                Require(Key in Grants and Grants[Key] == Certificate[0], "completion has no exact accepted grant")
                Require(Key not in Completed or Completed[Key] == Certificate, "completed certificate changed")
                Size, Start, First, Finish, _ = Certificate
                if AdmissionsCsv is not None:
                    Require(Key in Admitted and Admitted[Key]["activated_us"] == Start, "completion activation differs from admission boundary")
                Require(0 < Start <= First <= Finish <= Row["feedback_us"] and
                        RATE * (Finish - Start) <= FINITE_INTERCEPT + Size * 1_000_000, "finite completion envelope failed")
                Completed[Key] = Certificate
            if Row["result_retired_bytes"]:
                Key = (Slot, Row["retired_token"])
                # Connection bootstrap can retire unqualified small grants;
                # the demanded campaign starts after this setup. Every maximum
                # campaign grant must carry an exact native completion certificate.
                UncertifiedBootstrap = (Key in Admitted and Admitted[Key]["activated_us"] == 2**64 - 1) if AdmissionsCsv is not None else (
                    not Campaign and Grants.get(Key, GROUP) < GROUP and Row["active_grant_started_us"] == 0 and
                    not any(B["Slot"] == Slot for B in Bootstrap))
                Require(Key in Grants and Key not in Retired and Grants[Key] == Row["result_retired_bytes"] and
                        (Key in Completed or UncertifiedBootstrap),
                        "uncompleted/duplicate/mis-sized retirement")
                if Key not in Completed:
                    Bootstrap.append({"Slot": Slot, "Token": Key[1], "Bytes": Grants[Key],
                                      "Classification": "PRE_READY_SENTINEL" if AdmissionsCsv is not None else "NOT_MEASURED",
                                      "FiniteCertificate": "NOT_APPLICABLE" if AdmissionsCsv is not None else "NOT_MEASURED"})
                Retired.add(Key)
            Peers[Slot] = Row
    Require(Rows == UInt(Summary["samples"]) and Rows > 0, "CSV row count differs from sealed native summary")
    Require(len(Peers) == 4 and set(Grants) == Retired and set(Completed) <= Retired, "missing/unretired grant evidence")
    if AdmissionsCsv is not None:
        Require(set(Admitted) == set(Grants) and all((Key in Completed) == (A["activated_us"] != 2**64 - 1)
                for Key, A in Admitted.items()), "accepted grant coverage/classification incomplete")
    Require(PreviousGlobal["created"] == UInt(Admission["accepted"]) and
            PreviousGlobal["retired"] == UInt(Admission["retired"]) and PreviousGlobal["total_debt"] == 0 and
            sum(Grants.values()) == PreviousGlobal["created"],
            "final CSV/admission mismatch")
    Events = []
    PerPeer = []
    for Log in PeerLogs:
        Slot = UInt(Log["slot"])
        Last = Peers[Slot]
        Selected = [(Key, Value) for Key, Value in Completed.items() if Key[0] == Slot and Value[0] == GROUP]
        Require(len(Selected) == 32 and all(Log.get(K) == "32" for K in
                ("grants", "qualified_grants", "completed_grants")), "expected 32 complete maximum grants per peer")
        Require(Log.get("structural_first") == Log.get("structural_ack") == str(32 * GROUP) and
                Last["structural_first"] == Last["structural_ack"] == Last["accepted_structural"] and
                Last["accepted_structural"] == sum(V for K, V in Grants.items() if K[0] == Slot) and
                Last["pending"] == Last["active_grant_bytes"] == Last["debt_bytes"] == 0 and
                UInt(Log["running_us"]) > 0 and UInt(Log["max_run_deficit_byte_us"]) == Last["max_run_deficit_byte_us"],
                "peer service summary/convergence mismatch")
        for (Peer, Token), (_, Start, First, Finish, Maximum) in Selected:
            Require(First < Finish, "maximum grant has no intra-grant running interval")
            Events.extend([(First, 1, Peer), (Finish, 0, Peer)])
        PerPeer.append({"Slot": Slot, "Generation": Last["generation"], "Nonce": UInt(Identity[Slot]["nonce"]),
                        "MaximumGrants": len(Selected), "MaximumRunningDeficitByteUs": Last["max_run_deficit_byte_us"]})
    Active, At, CommonUs, Episodes = set(), 0, 0, 0
    for Time, Start, Slot in sorted(Events):
        if len(Active) == 4:
            CommonUs += Time - At
        Before = len(Active)
        Require((Slot not in Active) if Start else (Slot in Active), "overlapping grants within one ACK-gated peer")
        Active.add(Slot) if Start else Active.remove(Slot)
        if len(Active) == 4 and Before != 4:
            Episodes += 1
        At = Time
    Require(CommonUs > 0 and Episodes >= 3 and Curve.get("pool_common_run_us") == str(CommonUs) and
            Curve.get("pool_episodes") == str(Episodes), "common first-send overlap differs from native summary")
    return {"Rows": Rows, "MaximumGrants": 128, "Peers": sorted(PerPeer, key=lambda P: P["Slot"]),
            "Accepted": UInt(Admission["accepted"]), "Retired": UInt(Admission["retired"]),
            "CommonRunningUs": CommonUs, "CommonEpisodes": Episodes, "PoolBoundByteUs": 4 * RUN_BOUND,
            "PoolProof": "sum-of-four-native-all-subinterval-certificates", "MeasuredPoolMaximum": "NOT_MEASURED",
            "NativeEvidenceKind": "consumed-feedback-snapshots-with-latched-native-certificates",
            "PreCampaignUncertifiedBootstrap": Bootstrap,
            "AdmissionBoundaryState": "MEASURED" if AdmissionsCsv is not None else "NOT_MEASURED",
            "ProducerNonce": UInt(next(P["nonce"] for P in Identities if P["producer"] == "1"))}


def ReplayGameplay(File, Text, Nonce, Producer):
    Summary = NativePass(Text, "client")
    Require(Summary.get("nonce") == str(Nonce) and Summary.get("producer") == str(int(Producer)) and
            Summary.get("applied") == "32" and Summary.get("moved") == "1", "client readiness/workload mismatch")
    Values = {"rpc": [], "event": []}
    with Path(File).open(encoding="utf-8-sig", newline="") as Stream:
        Reader = csv.DictReader(Stream)
        Require(Reader.fieldnames == ["kind", "sequence", "rtt_ms"], "gameplay CSV schema mismatch")
        for Row in Reader:
            Require(Row.get("kind") in Values and None not in Row, "unexpected gameplay row")
            Kind = Row["kind"]
            Require(UInt(Row["sequence"]) == len(Values[Kind]) + 1 and len(Values[Kind]) < 80, "gameplay sequence hole/overflow")
            try:
                Value = Decimal(Row["rtt_ms"])
            except InvalidOperation as Error:
                raise ValueError("invalid gameplay latency") from Error
            Require(Value.is_finite() and Value >= 0, "invalid gameplay latency")
            Values[Kind].append(Value)
    Require(all(Summary.get(K + "_n") == str(len(V)) for K, V in Values.items()), "gameplay count differs from native summary")
    if Producer:
        Require(all(len(V) >= 60 for V in Values.values()), "insufficient protected gameplay workload")
        Rpc = sorted(Values["rpc"])
        Require(Rpc[(len(Rpc) * 95 + 99) // 100 - 1] <= 150 and
                Rpc[(len(Rpc) * 99 + 99) // 100 - 1] <= 250 and max(Rpc) <= 500 and
                max(Values["event"]) <= 250, "protected gameplay latency failed")
    else:
        Require(not any(Values.values()), "unexpected additional gameplay producer")
    return {Key: len(Value) for Key, Value in Values.items()}


def Manifest(Root, Hashes):
    Root = Path(Root).resolve()
    File = Root / "evidence-manifest.json"
    Value = Object(File)
    Require(isinstance(Value.get("Files"), list) and 0 < len(Value["Files"]) <= 256, "invalid evidence manifest")
    Hashes[str(File)] = Digest(File)
    Files = {}
    TotalBytes = 0
    for Entry in Value["Files"]:
        Name = Entry["Path"]
        Require(isinstance(Name, str) and re.fullmatch(r"[A-Za-z0-9_.-]+", Name) and Name.casefold() not in Files,
                "unsafe/duplicate manifest path")
        Target = Root / Name
        Require(Target.is_file() and Target.resolve().parent == Root and not Target.is_symlink(), "manifest path escaped/missing")
        Require(type(Entry["Bytes"]) is int and 0 <= Entry["Bytes"] <= 1024**3, "manifest file exceeds bounded replay input")
        TotalBytes += Entry["Bytes"]
        Require(TotalBytes <= 2 * 1024**3, "endpoint evidence exceeds bounded replay input")
        Require(Target.stat().st_size == Entry["Bytes"] and
                Digest(Target) == Entry["SHA256"].upper(), "evidence manifest hash/size mismatch: " + Name)
        Files[Name.casefold()] = Target
        Hashes[str(Target)] = Entry["SHA256"].upper()
    return Files


def Needed(Files, Name):
    Require(Name.casefold() in Files, "unsealed/missing evidence: " + Name)
    return Files[Name.casefold()]


def Journal(Files, Result, RunId, Lifecycle=False):
    Rows = [json.loads(Line) for Line in Read(Needed(Files, "control.jsonl")).splitlines()]
    Require(0 < len(Rows) <= 4096 and all(isinstance(R, dict) for R in Rows), "invalid control journal")
    Require(all(R.get("RunId", RunId) == RunId and R.get("Success", True) is True and
                R.get("Event") not in ("ERROR", "ABORT") and R.get("Type") not in ("ABORT", "ERROR") for R in Rows),
            "control journal records failure/wrong run")
    Results = [R for R in Rows if R.get("Event") == "RESULT"]
    Require(len(Results) == 1 and all(Results[0].get(K) == V for K, V in Result.items()), "control journal result mismatch")
    if Lifecycle:
        Cleaned = [R for R in Rows if R.get("Event") == "RECEIVE" and R.get("Type") == "CLEANED"]
        Require(len(Cleaned) == 2 and {R.get("Role") for R in Cleaned} == {"CLIENT", "SERVER"}, "missing lifecycle CLEANED evidence")
    else:
        for Role, Value in Result["Endpoints"].items():
            Received = [R for R in Rows if R.get("Event") == "RECEIVE" and R.get("Type") == Role + "_DONE"]
            Require(len(Received) == 1 and all(Received[0].get(K) == V for K, V in Value.items()), "coordinator received result mismatch")
        Require(len([R for R in Rows if R.get("Event") == "SEND" and R.get("Type") == "RUN_DONE"]) == 2,
                "coordinator finalization missing")


def EndpointJournal(Files, Result, Source, RunId, Tuples):
    Rows = [json.loads(Line) for Line in Read(Needed(Files, "control.jsonl")).splitlines()]
    Require(0 < len(Rows) <= 4096 and all(R.get("RunId", RunId) == RunId and R.get("Success", True) is True and
                R.get("Event") not in ("ERROR", "ABORT") and R.get("Type") not in ("ABORT", "ERROR") for R in Rows),
            "endpoint control journal failed")
    for Event, Expected in (("RESULT", Result), ("LOCAL_CLEANUP", {"Errors": []}), ("CAPTURE_TUPLES", {"Tuples": Tuples}),
                            ("PROVENANCE", {"ArtifactSHA256": Source["ProbeSHA256"], "BaseHead": Source["BaseHead"],
                             "OverlaySHA256": Source["OverlayArchiveSha256"], "GnsPin": Source["GnsPin"]})):
        Matched = [R for R in Rows if R.get("Event") == Event]
        Require(len(Matched) == 1 and all(Matched[0].get(K) == V for K, V in Expected.items()),
                "endpoint journal binding failed: " + Event)
    Require(any(R.get("Event") == "RECEIVE" and R.get("Type") == "RUN_DONE" for R in Rows) and
            any(R.get("Event") == "STATE" and R.get("Value") == "COMPLETE" for R in Rows), "endpoint finalization missing")


def FullPackets(File):
    """Additional full-frame check before the existing tuple parser is reused."""
    Require(Path(File).stat().st_size <= 1024**3, "four-client capture exceeds bounded replay input")
    Endian, Count = None, 0
    with Path(File).open("rb") as Stream:
        while Header := Stream.read(8):
            Require(len(Header) == 8, "truncated pcap block")
            if Header[:4] == b"\x0a\x0d\x0d\x0a":
                Magic = Stream.read(4)
                Require(Magic in (b"\x4d\x3c\x2b\x1a", b"\x1a\x2b\x3c\x4d"), "invalid pcap section")
                Endian = "<" if Magic[0] == 0x4d else ">"
            else:
                Require(Endian is not None, "pcap block before section")
                Magic = b""
            Kind, Length = struct.unpack(Endian + "II", Header)
            Require(12 <= Length <= 1024 * 1024 and Length % 4 == 0, "invalid pcap block size")
            Body = Magic + Stream.read(Length - 8 - len(Magic))
            Require(len(Body) == Length - 8 and struct.unpack(Endian + "I", Body[-4:])[0] == Length,
                    "incomplete pcap block")
            if Kind == 6:
                Require(len(Body) >= 24, "truncated enhanced packet")
                Captured, Original = struct.unpack_from(Endian + "II", Body, 12)
                Require(Captured == Original and Captured > 0 and 20 + Captured <= len(Body) - 4, "capture truncation")
                Count += 1
    Require(Count > 0, "empty capture")
    return Count


def Replay(Inputs, ExpectedCommit):
    try:
        return ReplayInputs(Inputs, ExpectedCommit)
    except (KeyError, TypeError, IndexError, OSError, UnicodeError, OverflowError) as Error:
        raise ValueError("[Foundation3L:FourClient] malformed/inaccessible retained evidence: " + str(Error)) from Error


def ReplayInputs(Inputs, ExpectedCommit):
    """Fixed, offline entry point. Inputs map paths to retained raw evidence.

    Missing inputs are NOT_MEASURED. Present malformed/failed evidence raises.
    RawCsvSha256 binds Server, Admissions and nonce-named Client CSVs collected
    from probe working directories; endpoint files match original manifests.
    """
    Require(isinstance(Inputs, dict) and re.fullmatch(r"[0-9a-f]{40}", ExpectedCommit), "invalid replay request")
    Required = ("ClientRoot", "ServerRoot", "CoordinatorRoot", "LifecycleHostRoot", "LifecycleQualification",
                "Cleanup", "ServerCsv", "AdmissionsCsv", "ClientCsv", "RawCsvSha256", "CandidateRoot")
    if any(Key not in Inputs for Key in Required):
        return {"State": "NOT_MEASURED", "Reason": "missing raw four-client inputs"}
    for Key in Required:
        if Key not in ("ClientCsv", "RawCsvSha256") and not Path(Inputs[Key]).exists():
            return {"State": "NOT_MEASURED", "Reason": "missing " + Key}
    Require(isinstance(Inputs["ClientCsv"], dict) and isinstance(Inputs["RawCsvSha256"], dict), "invalid raw CSV map")
    Hashes = {}
    Client, Server, Coordinator, Host = [Manifest(Inputs[Key], Hashes) for Key in
                                        ("ClientRoot", "ServerRoot", "CoordinatorRoot", "LifecycleHostRoot")]
    Source = Object(Needed(Client, "source-manifest.json"))
    Require(Source == Object(Needed(Server, "source-manifest.json")), "endpoints ran different candidates")
    Require(Source.get("BaseHead") == ExpectedCommit and Source.get("Contract") == "F1" and
            Source.get("GnsPin") == "2cb93a06350bb065db53abdb0d87cf297e0bfd34" and
            Source.get("SourceArchiveFormat") == "F1_OWNED_TREE_V2", "candidate source provenance mismatch")
    sys.path.insert(0, str(TOOLS))
    from f1_candidate_source import VerifyArchive
    Candidate = Path(Inputs["CandidateRoot"])
    Require(Object(Candidate / "source-manifest.json") == Source, "candidate manifest differs from run")
    Pins = {"gargantuan_physical_gns_funding_probe.exe": Source["ProbeSHA256"],
            "f1-native-source.zip": Source["OverlayArchiveSha256"]}
    Pins.update(Source["NativeDependenciesSha256"])
    Pins.update({"runtime/" + Name: Value for Name, Value in Source["RuntimeSha256"].items()})
    for Name, Expected in Pins.items():
        Require(re.fullmatch(r"(?:runtime/)?[A-Za-z0-9_.-]+", Name) and
                Digest(Candidate / Name) == Expected.upper(), "candidate artifact changed: " + Name)
        Hashes[str(Candidate / Name)] = Expected.upper()
    VerifyArchive(Candidate / "f1-native-source.zip", ExpectedCommit)
    RawFiles = {"Server": Inputs["ServerCsv"], "Admissions": Inputs["AdmissionsCsv"], **Inputs["ClientCsv"]}
    Require(set(RawFiles) == set(Inputs["RawCsvSha256"]), "raw CSV pin map mismatch")
    for Key, File in RawFiles.items():
        Require(Path(File).is_file() and Path(File).stat().st_size <= 64 * 1024 * 1024 and
                Digest(File) == Inputs["RawCsvSha256"][Key].upper(), "raw CSV pin mismatch: " + Key)
        Hashes[str(Path(File))] = Digest(File)
        Sealed = Needed(Server, "physical-gns-server.csv" if Key == "Server" else "physical-gns-server-admissions.csv") if Key in (
            "Server", "Admissions") else Needed(Client, "physical-gns-client-" + Key + ".csv")
        Require(Digest(Sealed) == Hashes[str(Path(File))] and Sealed.stat().st_size == Path(File).stat().st_size,
                "raw CSV differs from endpoint sealed evidence: " + Key)
    ServerLogs = [File for Name, File in Server.items() if re.fullmatch(r"probe-server(?:-[0-9]+)?\.stdout\.log", Name)]
    Require(len(ServerLogs) == 1, "missing/ambiguous server log")
    Native = ReplayNative(Inputs["ServerCsv"], ServerLogs[0], Inputs["AdmissionsCsv"])
    Nonces = [P["Nonce"] for P in Native["Peers"]]
    Require(set(Inputs["ClientCsv"]) == {str(N) for N in Nonces}, "raw client identity mismatch")
    Gameplay = {}
    for Nonce in Nonces:
        Gameplay[str(Nonce)] = ReplayGameplay(Inputs["ClientCsv"][str(Nonce)],
            Read(Needed(Client, f"probe-client-{Nonce}.stdout.log")), Nonce, Nonce == Native["ProducerNonce"])
    Results = {Role: Object(Needed(Files, "result.json")) for Role, Files in
               (("CLIENT", Client), ("SERVER", Server), ("COORDINATOR", Coordinator))}
    for Role, Result in Results.items():
        Require(Result.get("Success") is True and Result.get("Classification") == "FOUR_CLIENT_PHASE1_ONLY",
                "physical endpoint/coordinator failed: " + Role)
        if Role != "COORDINATOR":
            Require(Result.get("ExitCode") == 0, "physical child failed")
    Require(Results["CLIENT"].get("ClientNonces") == Nonces, "client result nonces mismatch")
    Endpoint = Results["COORDINATOR"]["Endpoints"]
    Require(set(Endpoint) == {"CLIENT", "SERVER"}, "coordinator endpoint set mismatch")
    RunId = Endpoint["CLIENT"]["RunId"]
    uuid.UUID(RunId)
    for Role, Result in Endpoint.items():
        Require(Result.get("RunId") == RunId and Result.get("Type") == Role + "_DONE" and
                all(Result.get(Key) == Value for Key, Value in Results[Role].items()), "coordinator result binding mismatch")
    Journal(Coordinator, Results["COORDINATOR"], RunId)
    Outer = Object(Needed(Host, "result.json"))
    LifecycleRunId = Outer["RunId"]
    Require(str(uuid.UUID(LifecycleRunId)) == LifecycleRunId and RunId != LifecycleRunId and Outer.get("Success") is True,
            "outer lifecycle failed/identity reused")
    Require(all(R.get("RunId") == LifecycleRunId and R.get("Success") is True and
                R.get("Type") in ("LIVE", "RESULT") for R in Outer["Results"]), "outer lifecycle contains failed result")
    Journal(Host, Outer, LifecycleRunId, True)
    FinalResults = [R for R in Outer["Results"] if R.get("Type") == "RESULT"]
    Require(len(FinalResults) == 2 and {R.get("Role") for R in FinalResults} == {"CLIENT", "SERVER"}, "outer results incomplete")
    for Result in FinalResults:
        Require(Result.get("RunId") == LifecycleRunId and Result.get("Success") is True, "outer endpoint result failed")
        RequiredFiles = [Needed(Client if Result["Role"] == "CLIENT" else Server, "result.json")]
        if Result["Role"] == "CLIENT":
            RequiredFiles.append(Needed(Coordinator, "result.json"))
        Require(sorted((E["SHA256"].upper(), E["Bytes"]) for E in Result["Evidence"]) ==
                sorted((Digest(F), F.stat().st_size) for F in RequiredFiles), "outer evidence hash binding mismatch")
    Qualification = Object(Inputs["LifecycleQualification"])
    Require(Qualification.get("Success") is True and Qualification.get("LifecycleRunId") == LifecycleRunId and
            Qualification.get("Codes") == {"Host": 0} and set(Qualification["Final"]) == {"CLIENT", "SERVER"}, "lifecycle reconciliation failed")
    Require(all(F.get("RunId") == LifecycleRunId and F.get("Status") == "IDLE" and F.get("Reason") == "NONE"
                for F in Qualification["Final"].values()), "endpoint lifecycle not cleanly idle")
    Cleanup = Object(Inputs["Cleanup"])
    Require(Cleanup.get("PhysicalRunId") == RunId and Cleanup.get("SshExited") is True and
            Cleanup.get("BothListenersClear") is True and type(Cleanup.get("StoppedUnixMs")) is int, "tunnel cleanup failed")
    # The cleanup receipt is written by the controlling-host orchestration,
    # unlike worker result-message clocks. Use its later completion timestamp.
    CompletedUtc = datetime.fromtimestamp(Cleanup["StoppedUnixMs"] / 1000, timezone.utc).isoformat()
    for Key in ("LifecycleQualification", "Cleanup"):
        Hashes[str(Path(Inputs[Key]))] = Digest(Inputs[Key])
    CaptureLog = Read(Needed(Client, "capture.log"))
    Drops = re.findall(r"Packets received/dropped on interface .*?:\s*([0-9]+)/([0-9]+)\s*\(pcap:([0-9]+)/dumpcap:([0-9]+)/flushed:([0-9]+)/ps_ifdrop:([0-9]+)\)", CaptureLog)
    Require(len(Drops) == 1 and all(int(V) == 0 for V in Drops[0][1:]), "client capture loss/unfinalized capture")
    Lost = re.findall(r"(?m)^Total Events\s+Lost\s+([0-9]+)\s*$", Read(Needed(Server, "capture-summary.txt")))
    Require(Lost == ["0"], "worker ETW loss evidence missing/failed")
    Stop = Object(Needed(Server, "CaptureStop.log"))
    Require(Stop.get("Success") is True and Stop.get("Operation") == "stop" and
            Stop.get("RunId") == RunId and Stop.get("State") == "stopped", "worker capture finalization failed")
    Owner = Object(Needed(Server, "netsh-owner.json"))
    Require(Owner.get("TraceMaximumMiB") == 256 and Owner.get("NoWrapThresholdMiB") == 240 and
            Owner.get("MiniportIfIndex") == 19 and Owner.get("CapturePort") == 39450 and
            Owner.get("CaptureLayers") == ["NDIS physical miniport"], "worker capture profile mismatch")
    Etl = Needed(Server, "worker-capture.etl")
    Require(0 < Etl.stat().st_size < 240 * 1024 * 1024, "worker non-wrap capacity gate failed")
    from qualifier import CaptureDirections
    Captures = {}
    for Role, Files, Name in (("CLIENT", Client, "client.pcapng"), ("SERVER", Server, "worker-capture.pcapng")):
        File = Needed(Files, Name)
        Count = FullPackets(File)
        if Role == "CLIENT":
            Require(Count == int(Drops[0][0]), "dumpcap/export packet count mismatch")
        _, Tuples = CaptureDirections(File, Role, True)
        Require(len(Tuples) == 4 and all(T["Inbound"] > 0 and T["Outbound"] > 0 for T in Tuples.values()),
                "four bidirectional captured tuples required")
        Captures[Role] = {"Packets": Count, "Tuples": Tuples}
        EndpointJournal(Files, Results[Role], Source, RunId, Tuples)
    Require(set(Captures["CLIENT"]["Tuples"]) == set(Captures["SERVER"]["Tuples"]), "capture endpoint tuple mismatch")
    return {"State": "MEASURED_PASS", "SourceCommit": ExpectedCommit, "RunId": RunId, "LifecycleRunId": LifecycleRunId,
            "CompletedUtc": CompletedUtc, "CandidatePins": Pins, "InputHashes": Hashes, "Native": Native,
            "Gameplay": Gameplay, "Capture": Captures}
