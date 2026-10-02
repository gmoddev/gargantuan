"""Offline, bounded Character publication conservation from farm native traces.

This joins exact generation-safe state identities across role-local traces.
Server and client monotonic clocks are never subtracted from one another.
The accepted-GRPL RecipientRetired stage closes only the pending obligation
that precedes it. A later publication may reenter with the same ObjectId.
"""

import argparse
import hashlib
import json
import re
import shutil
import sqlite3
import sys
import tempfile
from pathlib import Path

from farm_publication_trace import IterRecords, Require


MAX_DATABASE_PAGES = 1_048_576  # At 4 KiB/page, at most 4 GiB of scratch data.
MAX_DATABASE_BYTES = MAX_DATABASE_PAGES * 4096
MAX_READY_LOG_BYTES = 16 * 1024 * 1024
MAX_SERVER_TRACE_BYTES = 4_194_304 * 80 + 512
MAX_CLIENT_TRACE_BYTES = 131_072 * 80 + 512
READY_FIELD = re.compile(r"([a-z_]+)=([^\s]+)")
SHA256 = re.compile(r"[0-9a-f]{64}")
RELATIONSHIP = ("ConnectionSlot", "ConnectionGeneration", "ObjectSlot", "ObjectGeneration")
STATE = RELATIONSHIP + ("Tick", "Sequence", "MaterializationEpoch")


def Identity(Value):
    return tuple(getattr(Value, Field) for Field in RELATIONSHIP)


def StateIdentity(Value):
    return tuple(getattr(Value, Field) for Field in STATE)


def ReadyRows(PathValue, Prefix):
    PathValue = Path(PathValue)
    Require(PathValue.is_file() and not PathValue.is_symlink() and
            0 < PathValue.stat().st_size <= MAX_READY_LOG_BYTES,
            "ready log missing, redirected, or unbounded")
    with PathValue.open(encoding="utf-8", errors="strict") as Stream:
        for Line in Stream:
            if Line.startswith(Prefix):
                Pairs = READY_FIELD.findall(Line[len(Prefix):])
                Require(len(Pairs) == len(set(Key for Key, _ in Pairs)),
                        "duplicate ready log field")
                yield dict(Pairs)


def ReadyNumber(Row, Field):
    Value = Row.get(Field)
    Require(isinstance(Value, str) and re.fullmatch(r"(0|[1-9][0-9]*)", Value),
            "ready log invalid " + Field)
    return int(Value)


def Digest(PathValue):
    Hash = hashlib.sha256()
    with Path(PathValue).open("rb") as Stream:
        while Bytes := Stream.read(1 << 20):
            Hash.update(Bytes)
    return Hash.hexdigest()


def Indexed(IndexPath, RunId, Role, Name, MaximumBytes):
    """Resolve a sealed role member without trusting a user-supplied data path."""
    IndexPath = Path(IndexPath)
    Require(IndexPath.name == "evidence-sha256.json" and IndexPath.is_file() and
            not IndexPath.is_symlink() and 0 < IndexPath.stat().st_size <= 65536,
            "bounded role evidence index is missing")
    Row = json.loads(IndexPath.read_text(encoding="utf-8"))
    Require(isinstance(Row, dict) and Row.get("RunId") == RunId and
            Row.get("Role") == Role and isinstance(Row.get("Files"), list) and
            len(Row["Files"]) <= 128, "role evidence index identity invalid")
    Members = [Entry for Entry in Row["Files"] if isinstance(Entry, dict) and
               Entry.get("Name") == Name]
    Require(len(Members) == 1, "indexed publication member missing or duplicated")
    Entry = Members[0]
    Require(set(Entry) == {"Name", "Bytes", "Sha256"} and
            type(Entry["Bytes"]) is int and 0 < Entry["Bytes"] <= MaximumBytes and
            isinstance(Entry["Sha256"], str) and SHA256.fullmatch(Entry["Sha256"]),
            "indexed publication member bound invalid")
    File = IndexPath.parent / Name
    Require(File.is_file() and not File.is_symlink() and
            File.stat().st_size == Entry["Bytes"] and
            Digest(File) == Entry["Sha256"],
            "indexed publication member hash or size mismatch")
    return File, Entry["Sha256"]


def ReadyMappings(ServerReadyPath, ClientSources, RunId, ExpectedClients):
    """Bind different host-local ConnectionId namespaces through ready nonces."""
    Server = {}
    Connections = set()
    for Row in ReadyRows(ServerReadyPath, "[Qualification:Server] event=ready "):
        Require(Row.get("run") == RunId, "server ready run identity differs")
        Nonce = ReadyNumber(Row, "nonce")
        Connection = (ReadyNumber(Row, "connection_slot"),
                      ReadyNumber(Row, "connection_generation"))
        Require(Nonce > 0 and all(Connection) and Nonce not in Server and
                Connection not in Connections, "duplicate server ready identity")
        Server[Nonce] = Connection
        Connections.add(Connection)
        Require(len(Server) <= ExpectedClients, "server ready connection bound exceeded")
    Require(len(Server) == ExpectedClients, "server ready connections incomplete")
    BySlot = {}
    for Slot in range(ExpectedClients):
        _, Nonce, ClientReadyPath = ClientSources[Slot]
        Ready = None
        for Row in ReadyRows(ClientReadyPath, "[Qualification:Client] event=ready "):
            Require(Row.get("run_id") == RunId and Ready is None,
                    "duplicate or wrong-run client ready identity")
            Ready = Row
        Require(Ready is not None and ReadyNumber(Ready, "slot") == Slot and
                ReadyNumber(Ready, "nonce") == Nonce and Nonce in Server,
                "client ready run/slot/nonce differs from server")
        ClientConnection = (ReadyNumber(Ready, "connection_slot"),
                            ReadyNumber(Ready, "connection_generation"))
        Require(all(ClientConnection), "client ready connection invalid")
        BySlot[Slot] = (Server[Nonce], ClientConnection)
    Require(len({Nonce for _, Nonce, _ in ClientSources.values()}) == ExpectedClients,
            "client nonces duplicated")
    return BySlot


def FullRelationship(Value):
    Require(all(Identity(Value)) and Value.Tick > 0 and Value.Nanoseconds > 0,
            "event lacks full recipient/ObjectId generation or tick")


def CreateDatabase(PathValue):
    with Path(PathValue).open("xb"):
        pass
    Database = sqlite3.connect(PathValue)
    Database.execute("PRAGMA page_size=4096")
    Database.execute(f"PRAGMA max_page_count={MAX_DATABASE_PAGES}")
    Database.execute("PRAGMA journal_mode=OFF")
    Database.execute("PRAGMA synchronous=OFF")
    Database.execute("PRAGMA temp_store=FILE")
    Database.execute("PRAGMA cache_size=-8192")
    Database.executescript("""
        CREATE TABLE Frame (Tick INTEGER PRIMARY KEY, Ns INTEGER NOT NULL);
        CREATE TABLE Relationship (
            CS INTEGER, CG INTEGER, OS INTEGER, OG INTEGER,
            ForecastDue INTEGER, PendingDue INTEGER, Confirmed INTEGER NOT NULL DEFAULT 0,
            Lifetime INTEGER NOT NULL DEFAULT 0, Retired INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (CS, CG, OS, OG));
        CREATE INDEX RelationshipForecast ON Relationship (ForecastDue)
            WHERE PendingDue IS NULL AND Retired=0;
        CREATE TABLE DueEvent (
            CS INTEGER, CG INTEGER, OS INTEGER, OG INTEGER,
            Lifetime INTEGER, Due INTEGER, Forced INTEGER,
            PRIMARY KEY (CS, CG, OS, OG, Lifetime, Due, Forced));
        CREATE TABLE DueWork (
            CS INTEGER, CG INTEGER, OS INTEGER, OG INTEGER,
            Lifetime INTEGER, Due INTEGER, Resolution TEXT,
            PRIMARY KEY (CS, CG, OS, OG, Lifetime, Due));
        CREATE TABLE Built (
            OS INTEGER, OG INTEGER, Tick INTEGER, Seq INTEGER,
            Control INTEGER NOT NULL, Ns INTEGER NOT NULL,
            PRIMARY KEY (OS, OG, Tick, Seq));
        CREATE TABLE Snapshot (
            OS INTEGER, OG INTEGER, Tick INTEGER, Seq INTEGER, Ns INTEGER NOT NULL,
            PRIMARY KEY (OS, OG, Tick, Seq));
        CREATE TABLE Produced (
            CS INTEGER, CG INTEGER, OS INTEGER, OG INTEGER,
            Tick INTEGER, Seq INTEGER, Material INTEGER,
            Control INTEGER NOT NULL, Due INTEGER NOT NULL, Forced INTEGER NOT NULL,
            BuiltNs INTEGER NOT NULL, ProducedNs INTEGER NOT NULL,
            AcceptedNs INTEGER, FrameSeq INTEGER, ServiceBytes INTEGER,
            ReceiveNs INTEGER, HandledNs INTEGER, ClientSlot INTEGER,
            PRIMARY KEY (CS, CG, OS, OG, Tick, Seq, Material));
    """)
    return Database


def InsertUnique(Database, Sql, Values, Message):
    try:
        Database.execute(Sql, Values)
    except sqlite3.IntegrityError as Error:
        raise ValueError("[Qualification:Publication] " + Message) from Error


def ReadServer(Database, ServerPath, RunId, ReadyConnections):
    Counts = {"Due": 0, "DueClasses": 0, "DueRediscoveries": 0,
              "DueAccepted": 0, "Retired": 0, "RetiredPending": 0,
              "RetiredDue": 0, "ForecastRescheduled": 0,
              "UnchangedSuppressed": 0, "Produced": 0, "ForcedProduced": 0,
              "Accepted": 0, "ForcedAccepted": 0}
    LastFrameTick = 0
    for Value in IterRecords(ServerPath, RunId, "SERVER"):
        Stage = Value.Stage
        if Stage == 1:
            Require(Value.Tick > LastFrameTick and Value.Nanoseconds > 0,
                    "duplicate or nonmonotonic FrameBegin tick")
            InsertUnique(Database, "INSERT INTO Frame VALUES (?,?)",
                         (Value.Tick, Value.Nanoseconds), "duplicate FrameBegin")
            LastFrameTick = Value.Tick
            Database.execute("""UPDATE Relationship SET PendingDue=ForecastDue
                WHERE PendingDue IS NULL AND Retired=0 AND ForecastDue<=?""", (Value.Tick,))
        elif Stage in (2, 3, 6, 7, 8, 11):
            FullRelationship(Value)
            Require(Value.Tick <= LastFrameTick, "Character event lacks preceding FrameBegin")
            if Stage in (2, 3, 6, 7):
                Require(Value.DueTick > 0, "Character event lacks desired due tick")
            if Stage == 2:
                Previous = Database.execute("""SELECT PendingDue, Confirmed FROM Relationship
                    WHERE CS=? AND CG=? AND OS=? AND OG=?""", Identity(Value)).fetchone()
                if Previous and Previous[0] is not None and not Previous[1] and Value.DueTick > Value.Tick:
                    Counts["ForecastRescheduled"] += 1
                    Database.execute("""UPDATE Relationship SET PendingDue=NULL
                        WHERE CS=? AND CG=? AND OS=? AND OG=?""", Identity(Value))
                Database.execute("""INSERT INTO Relationship (CS,CG,OS,OG,ForecastDue)
                    VALUES (?,?,?,?,?) ON CONFLICT (CS,CG,OS,OG)
                    DO UPDATE SET ForecastDue=excluded.ForecastDue, Retired=0""",
                    (*Identity(Value), Value.DueTick))
            elif Stage == 3:
                Require(Value.Flags in (0, 1) and Value.DueTick <= Value.Tick,
                        "invalid confirmed due event")
                DueOrigin = Database.execute("SELECT Ns FROM Frame WHERE Tick=?",
                                             (Value.DueTick,)).fetchone()
                Require(DueOrigin is not None and DueOrigin[0] <= Value.Nanoseconds,
                        "confirmed due lacks authoritative FrameBegin origin")
                Previous = Database.execute("""SELECT PendingDue, Confirmed, Lifetime FROM Relationship
                    WHERE CS=? AND CG=? AND OS=? AND OG=?""", Identity(Value)).fetchone()
                Lifetime = Previous[2] if Previous else 0
                Seen = Database.execute("""SELECT 1 FROM DueEvent WHERE CS=? AND CG=? AND
                    OS=? AND OG=? AND Lifetime=? AND Due=? AND Forced=?""",
                    (*Identity(Value), Lifetime, Value.DueTick, Value.Flags)).fetchone()
                if Seen:
                    Require(Previous is not None and Previous[1] == 1 and
                            Previous[0] == Value.DueTick,
                            "confirmed due obligation reused after completion")
                    Counts["DueRediscoveries"] += 1
                else:
                    InsertUnique(Database, "INSERT INTO DueEvent VALUES (?,?,?,?,?,?,?)",
                                 (*Identity(Value), Lifetime, Value.DueTick, Value.Flags),
                                 "duplicate due obligation identity")
                    Counts["DueClasses"] += 1
                Work = Database.execute("""SELECT Resolution FROM DueWork WHERE CS=? AND CG=?
                    AND OS=? AND OG=? AND Lifetime=? AND Due=?""",
                    (*Identity(Value), Lifetime, Value.DueTick)).fetchone()
                if Work is None:
                    InsertUnique(Database, "INSERT INTO DueWork VALUES (?,?,?,?,?,?,NULL)",
                                 (*Identity(Value), Lifetime, Value.DueTick),
                                 "duplicate due work identity")
                    Counts["Due"] += 1
                else:
                    Require(Work[0] is None and Previous is not None and Previous[1] == 1 and
                            Previous[0] == Value.DueTick,
                            "due work reused after disposition")
                if Previous and Previous[0] is not None:
                    Require(Previous[0] == Value.DueTick,
                            "confirmed due changed without service or suppression")
                Database.execute("""INSERT INTO Relationship
                    (CS,CG,OS,OG,ForecastDue,PendingDue,Confirmed)
                    VALUES (?,?,?,?,?,?,1) ON CONFLICT (CS,CG,OS,OG)
                    DO UPDATE SET PendingDue=excluded.PendingDue, Confirmed=1, Retired=0""",
                    (*Identity(Value), Value.DueTick, Value.DueTick))
            elif Stage == 7:
                Previous = Database.execute("""SELECT PendingDue, Confirmed, Lifetime FROM Relationship
                    WHERE CS=? AND CG=? AND OS=? AND OG=?""", Identity(Value)).fetchone()
                Require(Previous is not None and Previous[0] == Value.DueTick and Previous[1] == 1,
                        "unchanged suppression lacks matching due work")
                Changed = Database.execute("""UPDATE DueWork SET Resolution='UNCHANGED'
                    WHERE CS=? AND CG=? AND OS=? AND OG=? AND Lifetime=? AND Due=?
                    AND Resolution IS NULL""",
                    (*Identity(Value), Previous[2], Value.DueTick)).rowcount
                Require(Changed == 1, "unchanged suppression duplicated due disposition")
                Database.execute("""UPDATE Relationship SET ForecastDue=NULL,
                    PendingDue=NULL, Confirmed=0
                    WHERE CS=? AND CG=? AND OS=? AND OG=?""", Identity(Value))
                Counts["UnchangedSuppressed"] += 1
            elif Stage == 6:
                Require(Value.Sequence > 0 and Value.MaterializationEpoch > 0 and
                        Value.Flags in (0, 1), "produced state lacks sequence, epoch, or mode")
                Built = Database.execute("""SELECT B.Control, B.Ns, S.Ns FROM Built B
                    JOIN Snapshot S USING (OS,OG,Tick,Seq)
                    WHERE B.OS=? AND B.OG=? AND B.Tick=? AND B.Seq=?""",
                    (Value.ObjectSlot, Value.ObjectGeneration, Value.Tick, Value.Sequence)).fetchone()
                Require(Built is not None and Built[0] > 0 and
                        Built[1] <= Built[2] <= Value.Nanoseconds,
                        "produced state lacks matching built/snapshot identity")
                InsertUnique(Database, """INSERT INTO Produced
                    (CS,CG,OS,OG,Tick,Seq,Material,Control,Due,Forced,BuiltNs,ProducedNs)
                    VALUES (?,?,?,?,?,?,?,?,?,?,?,?)""",
                    (*StateIdentity(Value), Built[0], Value.DueTick, Value.Flags,
                     Built[1], Value.Nanoseconds), "duplicate produced state")
                Counts["Produced"] += 1
                Counts["ForcedProduced"] += Value.Flags
            elif Stage == 8:
                Require((Value.ConnectionSlot, Value.ConnectionGeneration) in ReadyConnections,
                        "accepted recipient lacks server ready identity")
                Require(all((Value.Sequence, Value.ControlEpoch, Value.MaterializationEpoch,
                             Value.FrameSequence, Value.ServiceBytes)),
                        "accepted packet lacks full state/frame byte identity")
                Produced = Database.execute("""SELECT Control, Due, Forced, ProducedNs, AcceptedNs
                    FROM Produced WHERE CS=? AND CG=? AND OS=? AND OG=? AND Tick=?
                    AND Seq=? AND Material=?""", StateIdentity(Value)).fetchone()
                Require(Produced is not None and Produced[0] == Value.ControlEpoch and
                        Produced[4] is None and Value.Nanoseconds >= Produced[3],
                        "accepted packet is unmatched, duplicated, or precedes production")
                Pending = Database.execute("""SELECT PendingDue, Confirmed, Lifetime FROM Relationship
                    WHERE CS=? AND CG=? AND OS=? AND OG=?""", Identity(Value)).fetchone()
                Require(Pending is not None and Pending[0] == Produced[1] and
                        Pending[1] == 1 and Value.Tick >= Produced[1],
                        "accepted state lacks its full recipient due obligation")
                Origin = Database.execute("SELECT Ns FROM Frame WHERE Tick=?", (Produced[1],)).fetchone()
                Require(Origin is not None and Origin[0] <= Value.Nanoseconds,
                        "accepted state lacks authoritative due-tick FrameBegin")
                Changed = Database.execute("""UPDATE DueWork SET Resolution='ACCEPTED'
                    WHERE CS=? AND CG=? AND OS=? AND OG=? AND Lifetime=? AND Due=?
                    AND Resolution IS NULL""",
                    (*Identity(Value), Pending[2], Produced[1])).rowcount
                Require(Changed == 1, "accepted state duplicates a due disposition")
                Database.execute("""UPDATE Produced SET AcceptedNs=?, FrameSeq=?, ServiceBytes=?
                    WHERE CS=? AND CG=? AND OS=? AND OG=? AND Tick=? AND Seq=? AND Material=?""",
                    (Value.Nanoseconds, Value.FrameSequence, Value.ServiceBytes,
                     *StateIdentity(Value)))
                Database.execute("""UPDATE Relationship SET ForecastDue=NULL,
                    PendingDue=NULL, Confirmed=0
                    WHERE CS=? AND CG=? AND OS=? AND OG=?""", Identity(Value))
                Counts["Accepted"] += 1
                Counts["DueAccepted"] += 1
                Counts["ForcedAccepted"] += Produced[2]
            elif Stage == 11:
                Previous = Database.execute("""SELECT PendingDue, Confirmed, Lifetime, Retired
                    FROM Relationship WHERE CS=? AND CG=? AND OS=? AND OG=?""",
                    Identity(Value)).fetchone()
                Require(Previous is None or Previous[3] == 0,
                        "duplicate retirement without intervening reentry")
                Counts["Retired"] += 1
                if Previous and Previous[0] is not None:
                    Require(Value.Tick >= Previous[0],
                            "retirement precedes the pending due tick")
                    Counts["RetiredPending"] += 1
                    if Previous[1]:
                        Changed = Database.execute("""UPDATE DueWork SET Resolution='RETIRED'
                            WHERE CS=? AND CG=? AND OS=? AND OG=? AND Lifetime=? AND Due=?
                            AND Resolution IS NULL""",
                            (*Identity(Value), Previous[2], Previous[0])).rowcount
                        Require(Changed == 1, "retirement lacks its confirmed due work")
                        Counts["RetiredDue"] += 1
                if Previous:
                    Database.execute("""UPDATE Relationship SET ForecastDue=NULL,
                        PendingDue=NULL, Confirmed=0, Lifetime=Lifetime+1, Retired=1
                        WHERE CS=? AND CG=? AND OS=? AND OG=?""", Identity(Value))
                else:
                    Database.execute("""INSERT INTO Relationship
                        (CS,CG,OS,OG,Lifetime,Retired) VALUES (?,?,?,?,1,1)""",
                        Identity(Value))
        elif Stage == 4:
            Require(all((Value.ObjectSlot, Value.ObjectGeneration, Value.Tick,
                         Value.Sequence, Value.ControlEpoch, Value.Nanoseconds)),
                    "StateBuilt lacks full ObjectId/control epoch")
            InsertUnique(Database, "INSERT INTO Built VALUES (?,?,?,?,?,?)",
                         (Value.ObjectSlot, Value.ObjectGeneration, Value.Tick,
                          Value.Sequence, Value.ControlEpoch, Value.Nanoseconds),
                         "duplicate StateBuilt")
        elif Stage == 5:
            Require(all((Value.ObjectSlot, Value.ObjectGeneration, Value.Tick,
                         Value.Sequence, Value.Nanoseconds)),
                    "CharacterSnapshot lacks full ObjectId/state sequence")
            InsertUnique(Database, "INSERT INTO Snapshot VALUES (?,?,?,?,?)",
                         (Value.ObjectSlot, Value.ObjectGeneration, Value.Tick,
                          Value.Sequence, Value.Nanoseconds), "duplicate CharacterSnapshot")
    Require(LastFrameTick > 0, "server trace has no FrameBegin")
    Counts["UnresolvedDue"] = Database.execute(
        "SELECT count(*) FROM Relationship WHERE PendingDue IS NOT NULL").fetchone()[0]
    Counts["FutureForecasts"] = Database.execute(
        "SELECT count(*) FROM Relationship WHERE Retired=0 AND ForecastDue>?",
        (LastFrameTick,)).fetchone()[0]
    Counts["OverdueForecasts"] = Database.execute(
        "SELECT count(*) FROM Relationship WHERE Retired=0 AND ForecastDue<=?",
        (LastFrameTick,)).fetchone()[0]
    Counts["UnacceptedProduced"] = Database.execute(
        "SELECT count(*) FROM Produced WHERE AcceptedNs IS NULL").fetchone()[0]
    Counts["UnresolvedDueWork"] = Database.execute(
        "SELECT count(*) FROM DueWork WHERE Resolution IS NULL").fetchone()[0]
    Require(Counts["UnresolvedDue"] == 0 and Counts["UnresolvedDueWork"] == 0 and
            Counts["OverdueForecasts"] == 0 and
            Counts["UnacceptedProduced"] == 0 and
            Counts["Due"] == Counts["DueAccepted"] + Counts["UnchangedSuppressed"] +
            Counts["RetiredDue"],
            "server due/production/acceptance conservation incomplete")
    Require(Counts["Accepted"] > 0 and Counts["ForcedAccepted"] == Counts["ForcedProduced"],
            "accepted Character state or forced-state conservation missing")
    return Counts


def ReadClients(Database, ClientSources, ReadyBySlot, RunId, ExpectedClients):
    Require(set(ClientSources) == set(range(ExpectedClients)) and
            1 <= ExpectedClients <= 32, "client slot set is incomplete")
    Nonces = [Value[1] for Value in ClientSources.values()]
    Require(all(type(Nonce) is int and Nonce > 0 for Nonce in Nonces) and
            len(set(Nonces)) == ExpectedClients, "client nonces are missing or duplicated")
    ConnectionBySlot = {}
    Counts = {"NativeReceive": 0, "ClientHandled": 0,
              "ForcedNativeReceive": 0, "ForcedClientHandled": 0}
    for Slot in range(ExpectedClients):
        PathValue, Nonce, _ = ClientSources[Slot]
        ServerConnection, ClientConnection = ReadyBySlot[Slot]
        for Value in IterRecords(PathValue, RunId, "CLIENT", Slot, Nonce):
            Connection = Value.ConnectionSlot, Value.ConnectionGeneration
            Require(Connection == ClientConnection and
                    ConnectionBySlot.setdefault(Slot, Connection) == Connection,
                    "client slot maps to a different connection generation")
            ServerState = (*ServerConnection, Value.ObjectSlot, Value.ObjectGeneration,
                           Value.Tick, Value.Sequence, Value.MaterializationEpoch)
            Row = Database.execute("""SELECT Control, FrameSeq, ServiceBytes, Forced,
                AcceptedNs, ReceiveNs, HandledNs, ClientSlot
                FROM Produced WHERE CS=? AND CG=? AND OS=? AND OG=? AND Tick=?
                AND Seq=? AND Material=?""", ServerState).fetchone()
            Require(Row is not None and Row[0] == Value.ControlEpoch and
                    Row[1] == Value.FrameSequence and Row[2] == Value.ServiceBytes and
                    Row[4] is not None,
                    "client state is not the accepted full-generation server state")
            if Value.Stage == 9:
                Require(Row[5] is None and Row[6] is None and Row[7] is None,
                        "duplicate or reordered client native receive")
                Database.execute("""UPDATE Produced SET ReceiveNs=?, ClientSlot=?
                    WHERE CS=? AND CG=? AND OS=? AND OG=? AND Tick=? AND Seq=? AND Material=?""",
                    (Value.Nanoseconds, Slot, *ServerState))
                Counts["NativeReceive"] += 1
                Counts["ForcedNativeReceive"] += Row[3]
            else:
                Require(Value.Stage == 10 and Row[5] is not None and Row[6] is None and
                        Row[7] == Slot and Value.Nanoseconds >= Row[5],
                        "client handled event lacks matching earlier native receive")
                Database.execute("""UPDATE Produced SET HandledNs=?
                    WHERE CS=? AND CG=? AND OS=? AND OG=? AND Tick=? AND Seq=? AND Material=?""",
                    (Value.Nanoseconds, *ServerState))
                Counts["ClientHandled"] += 1
                Counts["ForcedClientHandled"] += Row[3]
    Missing = Database.execute("""SELECT count(*) FROM Produced
        WHERE AcceptedNs IS NOT NULL AND (ReceiveNs IS NULL OR HandledNs IS NULL)""").fetchone()[0]
    Require(Missing == 0 and Counts["NativeReceive"] == Counts["ClientHandled"],
            "accepted Character states lack complete client observation")
    Require(len(ConnectionBySlot) == ExpectedClients,
            "a client lacks a unique received connection generation")
    return Counts


def Duration(Database, Predicate, Expression, ExtraJoin=""):
    Row = Database.execute(f"""SELECT count(*), min({Expression}), max({Expression}),
        sum({Expression}) FROM Produced P {ExtraJoin} WHERE {Predicate}""").fetchone()
    Count, Minimum, Maximum, Total = Row
    return {"Count": Count, "MinimumNs": Minimum, "MaximumNs": Maximum,
            "MeanNs": None if Count == 0 else Total // Count}


def Join(ServerPath, ServerReadyPath, ClientSources, RunId, ScratchParent,
         ExpectedClients=32):
    """Require complete trace conservation; return only role-local durations."""
    Require(isinstance(RunId, str) and RunId, "run identity is missing")
    Require(set(ClientSources) == set(range(ExpectedClients)) and
            all(isinstance(Source, (tuple, list)) and len(Source) == 3
                for Source in ClientSources.values()), "client sources incomplete")
    ScratchParent = Path(ScratchParent)
    Require(ScratchParent.is_dir() and not ScratchParent.is_symlink(),
            "explicit scratch parent missing or redirected")
    ScratchParent = ScratchParent.resolve(strict=True)
    Sources = [ServerPath, ServerReadyPath] + [PathText for Trace, _, Log in
        ClientSources.values() for PathText in (Trace, Log)]
    for Source in Sources:
        Source = Path(Source).resolve(strict=True)
        Require(ScratchParent != Source.parent and
                not ScratchParent.is_relative_to(Source.parent) and
                not Source.parent.is_relative_to(ScratchParent),
                "scratch parent overlaps immutable role evidence")
    Require(shutil.disk_usage(ScratchParent).free >= MAX_DATABASE_BYTES,
            "scratch volume has less than the bounded 4 GiB database cap free")
    ReadyBySlot = ReadyMappings(ServerReadyPath, ClientSources, RunId, ExpectedClients)
    with tempfile.TemporaryDirectory(prefix="gargantuan-publication-join-",
                                     dir=ScratchParent) as Root:
        Root = Path(Root).resolve(strict=True)
        Require(Root.parent == ScratchParent and Root.name.startswith("gargantuan-publication-join-"),
                "scratch directory escaped the explicit task root")
        DatabasePath = Root / "join.sqlite3"
        Database = CreateDatabase(DatabasePath)
        try:
            with Database:
                Server = ReadServer(Database, ServerPath, RunId,
                                    {Pair[0] for Pair in ReadyBySlot.values()})
                Clients = ReadClients(Database, ClientSources, ReadyBySlot, RunId,
                                      ExpectedClients)
                Require(Server["Accepted"] == Clients["NativeReceive"] ==
                        Clients["ClientHandled"] and
                        Server["ForcedAccepted"] == Clients["ForcedNativeReceive"] ==
                        Clients["ForcedClientHandled"],
                        "server/client Character conservation differs")
                Ordinary = Duration(Database, "P.Forced=0 AND P.HandledNs IS NOT NULL",
                                    "P.AcceptedNs-F.Ns", "JOIN Frame F ON F.Tick=P.Due")
                Forced = Duration(Database, "P.Forced=1 AND P.HandledNs IS NOT NULL",
                                  "P.AcceptedNs-P.BuiltNs")
                Handled = Duration(Database, "P.HandledNs IS NOT NULL",
                                   "P.HandledNs-P.ReceiveNs")
                Require(Ordinary["Count"] + Forced["Count"] == Server["Accepted"] and
                        all(Value["MinimumNs"] is None or Value["MinimumNs"] >= 0
                            for Value in (Ordinary, Forced, Handled)),
                        "role-local publication timing is incomplete or negative")
                Pages = Database.execute("PRAGMA page_count").fetchone()[0]
                Require(Pages <= MAX_DATABASE_PAGES, "offline join scratch bound exceeded")
                DatabaseBytes = DatabasePath.stat().st_size
                Require(DatabaseBytes <= MAX_DATABASE_BYTES,
                        "offline join database byte cap exceeded")
                return {"Format": "GargantuanFarmPublicationJoin", "Version": 1,
                        "RunId": RunId, "Status": "ACCEPTED_STATE_CHAIN_OBSERVED",
                        "Clients": ExpectedClients, "Server": Server, "Client": Clients,
                        "DueCompleteness": "OBSERVED",
                        "ServerDueToAccepted": Ordinary,
                        "ServerForcedBuiltToAccepted": Forced,
                        "ClientReceiveToHandled": Handled,
                        "Retirement": "OBSERVED" if Server["Retired"] else "NONE_OBSERVED",
                        "CrossHostDueToHandled": "NOT_MEASURED",
                        "ScratchPeakDatabaseBytes": DatabaseBytes,
                        "ScratchMaximumDatabaseBytes": MAX_DATABASE_BYTES,
                        "ScratchPages": Pages, "ScratchMaximumPages": MAX_DATABASE_PAGES}
        except sqlite3.Error as Error:
            raise ValueError("[Qualification:Publication] bounded offline join failed") from Error
        finally:
            Database.close()


def JoinIndexed(ServerIndexPath, ClientIndexPath, RunManifestPath, ScratchParent,
                ExpectedClients=32):
    """Read only hash-sealed role traces and ready logs for a pinned run."""
    ManifestPath = Path(RunManifestPath)
    Require(ManifestPath.is_file() and not ManifestPath.is_symlink() and
            0 < ManifestPath.stat().st_size <= 65536,
            "bounded run manifest missing")
    Manifest = json.loads(ManifestPath.read_text(encoding="utf-8"))
    RunId = Manifest.get("RunId")
    NonceTexts = Manifest.get("Nonces")
    Require(isinstance(RunId, str) and
            re.fullmatch(r"[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}", RunId) and
            isinstance(NonceTexts, list) and len(NonceTexts) == ExpectedClients and
            all(isinstance(Nonce, str) and re.fullmatch(r"[1-9][0-9]*", Nonce) and
                int(Nonce) <= 0xffffffffffffffff for Nonce in NonceTexts) and
            len(set(NonceTexts)) == ExpectedClients,
            "run manifest nonce identity invalid")
    Nonces = [int(Nonce) for Nonce in NonceTexts]
    Members = []
    Server, Hash = Indexed(ServerIndexPath, RunId, "Server",
                           "publication-service.bin", MAX_SERVER_TRACE_BYTES)
    Members.append((Server, Hash))
    ServerReady, Hash = Indexed(ServerIndexPath, RunId, "Server",
                                "server.stdout.log", MAX_READY_LOG_BYTES)
    Members.append((ServerReady, Hash))
    Clients = {}
    for Slot, Nonce in enumerate(Nonces):
        Trace, Hash = Indexed(ClientIndexPath, RunId, "Clients",
                              f"publication-service-{Slot}.bin", MAX_CLIENT_TRACE_BYTES)
        Members.append((Trace, Hash))
        Log, Hash = Indexed(ClientIndexPath, RunId, "Clients",
                            f"client-{Slot:02d}.stdout.log", MAX_READY_LOG_BYTES)
        Members.append((Log, Hash))
        Clients[Slot] = Trace, Nonce, Log
    Result = Join(Server, ServerReady, Clients, RunId, ScratchParent,
                  ExpectedClients=ExpectedClients)
    Require(all(Digest(File) == Hash for File, Hash in Members),
            "sealed publication member changed during offline join")
    Result["ServerIndexSha256"] = Digest(ServerIndexPath)
    Result["ClientIndexSha256"] = Digest(ClientIndexPath)
    Result["RunManifestSha256"] = Digest(ManifestPath)
    return Result


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("server_index")
    Parser.add_argument("client_index")
    Parser.add_argument("run_manifest")
    Parser.add_argument("scratch_parent", help="existing task-owned analysis directory outside sealed evidence")
    Parser.add_argument("--expected-clients", type=int, default=32)
    Arguments = Parser.parse_args()
    print(json.dumps(JoinIndexed(Arguments.server_index, Arguments.client_index,
                                 Arguments.run_manifest, Arguments.scratch_parent,
                                 Arguments.expected_clients), sort_keys=True))


if __name__ == "__main__":
    try:
        Main()
    except (ValueError, OSError, sqlite3.Error, json.JSONDecodeError) as Error:
        print(str(Error), file=sys.stderr)
        raise SystemExit(1)
