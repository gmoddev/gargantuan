# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Run fresh, package-relocated Yard servers against one bounded SQLite history."""
import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import re
import socket
import sqlite3
import struct
import subprocess
import sys
import time

from SpatialConstructionPackageSession import Build, ConsoleCapture, ManagedEnvironment, Run

Root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Root / "samples" / "TerrainQuarry"))
from BuildProject import EditorHost


def Digest(Data):
    return hashlib.sha256(Data).hexdigest()


def Package(Engine, Packager, ServerRuntime, Work):
    Project = Work / "Yard"
    Run([sys.executable, "-B", Root / "samples/ConstructionYard/BuildProject.py",
         "--engine", Engine, "--project", Project], Work)
    with EditorHost(Engine) as Host:
        Host.Call("Handshake")
        Host.Call("OpenProject", Root=Project.as_posix())
        Classes = Host.Call("GetSchema")["Definitions"]
        Script = next(Row for Row in Classes if Row["Kind"] == "Class" and Row["Namespace"] == "Engine" and Row["Name"] == "Script")
        World = next(Row["Id"] for Row in Host.Call("GetSnapshot")["Snapshot"]["Objects"] if Row["ClassName"] == "DataModel")
        Host.Call("CreateInstance", ClassSchemaId=Script["SchemaId"], DefinitionVersion=Script["DefinitionVersion"],
                  Parent=World, Name="YardRecoveryObserver", InitialSource=Path(__file__).with_suffix(".luau").read_text(encoding="utf-8"),
                  InitialProperties=[dict(DeclaringClassSchemaId=Script["SchemaId"], DeclaringDefinitionVersion=Script["DefinitionVersion"],
                                          Property="RunContext", Value=dict(Type="EnumItem", Enum="RunContext", Value="Server"))])
        Host.Call("SaveProject")
    return Build(Packager, Project, ServerRuntime, "DedicatedServer", Work, "Development")[0]


def Event(Index):
    Group, Slot = divmod(Index - 1, 4)
    Coordinates = [-7, -3, 3, 8]
    if Slot == 2:
        return {"Kind": "dig", "X": 16 * Coordinates[Group % 4], "Y": -12, "Z": 16 * Coordinates[Group // 4]}
    return {"Kind": "wall" if Slot == 0 or (Slot == 3 and Group % 2 == 0) else "floor"}


def PackedText(Value):
    Raw = Value.encode("utf-8")
    return struct.pack("<I", len(Raw)) + Raw


def Encoded(Value):
    # Independent encoder for the subset of the SQLite v1 document format exercised here.
    if isinstance(Value, str):
        return bytes([4]) + PackedText(Value)
    if isinstance(Value, int):
        return bytes([3]) + struct.pack("<d", Value)
    if isinstance(Value, list):
        return bytes([5]) + struct.pack("<I", len(Value)) + b"".join(Encoded(Entry) for Entry in Value)
    return bytes([6]) + struct.pack("<I", len(Value)) + b"".join(PackedText(Key) + Encoded(Value[Key]) for Key in sorted(Value))


def Inspect(Database, Count):
    Expected = Encoded({"Version": 1, "Events": [Event(Index) for Index in range(1, Count + 1)]})
    Sidecars = [Path(str(Database) + Suffix) for Suffix in ("-journal", "-wal", "-shm")]
    with sqlite3.connect(Database.as_uri() + "?mode=ro", uri=True) as Connection:
        Check = Connection.execute("PRAGMA integrity_check").fetchall()
        Profile = Connection.execute("SELECT revision,records,payload_bytes FROM profile").fetchall()
        Records = Connection.execute("SELECT kind,store,key,revision,payload FROM records").fetchall()
        Pages = Connection.execute("PRAGMA page_count").fetchone()[0]
        Free = Connection.execute("PRAGMA freelist_count").fetchone()[0]
        PageSize = Connection.execute("PRAGMA page_size").fetchone()[0]
    Batches = Count // 16
    ExpectedRevision = Count + 2 * (Batches - 1)
    if Check != [("ok",)] or Profile != [(Count + 2 * Batches, 1, len(Expected))] or len(Records) != 1:
        raise ValueError(f"SQLite record or quota mismatch at prefix {Count}: {Check}, {Profile}, {len(Records)}")
    if Records[0] != (1, "ConstructionYard", "YardV1", ExpectedRevision, Expected):
        raise ValueError(f"Canonical stored document differs from independently encoded {Count}-event history")
    if any(Sidecar.exists() for Sidecar in Sidecars):
        raise ValueError("Unexpected surviving SQLite journal file after acknowledged work")
    return dict(DocumentRevision=ExpectedRevision, ProviderRevision=Profile[0][0], Records=1, PayloadBytes=len(Expected),
                PayloadSha256=Digest(Expected), DatabaseBytes=Database.stat().st_size, Pages=Pages, FreePages=Free, PageSize=PageSize)


def Boot(Server, Work, Database, Number, Start, Graceful):
    Log = Work / f"server-{Number}.log"
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as Port:
        Port.bind(("127.0.0.1", 0))
        Endpoint = f"127.0.0.1:{Port.getsockname()[1]}"
    Began = time.monotonic()
    Args = [str(Server), "--bind", Endpoint, "--max-ticks", "360" if Graceful else "2400",
            "--persistence-provider", "sqlite", "--persistence-local-db", str(Database)]
    with Log.open("wb") as Output:
        Process = subprocess.Popen(Args, cwd=Work, env=ManagedEnvironment(), stdout=Output, stderr=subprocess.STDOUT, **ConsoleCapture())
        try:
            while True:
                Text = Log.read_text(encoding="utf-8", errors="replace")
                # ServerHost also mirrors the native Luau stream to its own delayed status logger.
                Text = "\n".join(Line for Line in Text.splitlines() if "[Runtime:Server] [Warning]" not in Line)
                Marker = re.search(r"\[Spatial:YardSoak\] complete start=(\d+) end=(\d+) revision=(\d+)", Text)
                if Marker:
                    break
                if "[Spatial:YardSoak] failure" in Text or "Could not restore edits" in Text or Process.poll() is not None or time.monotonic() - Began > 38:
                    raise RuntimeError(f"{Log}\n{Text[-3200:]}")
                time.sleep(.025)
            Elapsed = time.monotonic() - Began
            if Graceful:
                if Process.wait(timeout=15) != 0:
                    raise RuntimeError("Final read-only server did not exit cleanly")
            else:
                Process.terminate()
                Process.wait(timeout=5)
        finally:
            if Process.poll() is None:
                Process.kill()
                Process.wait(timeout=5)
    Last = min(Start + 16, 64)
    if tuple(map(int, Marker.groups())) != (Start, Last, Last + 2 * (Last // 16 - 1)):
        raise ValueError(f"Unexpected durable boundary in {Log}")
    Replay = re.search(r"\[Spatial:YardSoak\] replay start=(\d+) wall=(\d+) floor=(\d+) digs=(\d+) field=(\d+) surface=(\d+) hits=(\d+)", Text)
    if Replay is None or tuple(map(int, Replay.groups()[:4])) != (Start, Start * 3 // 8, Start * 3 // 8, Start // 4):
        raise ValueError("Authored replay returned incorrect construction counts")
    Rows = [dict(Index=int(I), Revision=int(Revision), Milliseconds=float(Ms), Raced=int(Race), Stale=int(Stale))
            for I, Revision, Ms, Race, Stale in
            re.findall(r"\[Spatial:YardSoak\] write index=(\d+) revision=(\d+) ms=([.\d]+) race=(\d+) stale=(\d+)", Text)]
    if [Row["Index"] for Row in Rows] != list(range(Start + 1, Last + 1)) or any(
            Row["Revision"] != Row["Index"] + 2 * (Start // 16) for Row in Rows):
        raise ValueError("Observed duplicate or missing mutation revisions")
    Captures = {}
    for Label in ("field", "rays"):
        Raw = bytearray()
        for Offset, Hex in re.findall(r"\[Spatial:YardBytes\] " + Label + r" (\d+) ([0-9a-f]+)", Text):
            if int(Offset) != len(Raw):
                raise ValueError(f"Noncontiguous {Label} export")
            Raw.extend(bytes.fromhex(Hex))
        if (Label == "field" and len(Raw) != 4000 * 6) or (Label == "rays" and len(Raw) != 500):
            raise ValueError(f"Incomplete {Label} export ({len(Raw)} bytes)")
        (Work / f"{Label}-{Number}.bin").write_bytes(Raw)
        Captures[Label.title() + "Sha256"] = Digest(Raw)
    Observation = dict(Boot=Number, Replayed=Start, Written=Last, Exit="graceful" if Graceful else "terminated_after_ack",
                       ProcessId=Process.pid, LaunchToCompleteMs=round(1000 * Elapsed, 3),
                       Field=Replay[5], Surface=Replay[6], SurfaceHits=int(Replay[7]), **Captures, **Inspect(Database, Last))
    return Observation, Rows


def Percentiles(Rows):
    Values = sorted(Row["Milliseconds"] for Row in Rows)
    return {Name: Values[math.ceil(Percent * len(Values)) - 1] for Name, Percent in (("P50", .5), ("P95", .95), ("P99", .99), ("Max", 1))}


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    for Name in ("engine", "packager", "server-runtime", "output"):
        Parser.add_argument("--" + Name, type=Path, required=True)
    Options = Parser.parse_args()
    Work = Options.output.resolve()
    Work.mkdir(parents=True, exist_ok=False)
    PackageDir = Package(Options.engine.resolve(), Options.packager.resolve(), Options.server_runtime.resolve(), Work)
    Server = PackageDir / ("GargantuanServer.exe" if os.name == "nt" else "GargantuanServer")
    BootRows, Writes = [], []
    for Number, Start in enumerate((0, 16, 32, 48, 64, 64), 1):
        Observation, Rows = Boot(Server, Work, Work / "yard.sqlite", Number, Start, Number == 6)
        BootRows.append(Observation)
        Writes.extend(Rows)
        print(f"[Spatial:YardSoak] boot {Number}: replayed {Start}, wrote through {Observation['Written']}, {Observation['PayloadBytes']} bytes", flush=True)
    if len(set(Row["Field"] for Row in BootRows)) != 5 or len(set(Row["Surface"] for Row in BootRows)) != 5:
        raise ValueError("Terrain samples or native raycasts failed to distinguish each excavated history")
    if any(BootRows[-1][Key] != BootRows[-2][Key] for Key in ("Field","Surface","SurfaceHits","PayloadSha256","DocumentRevision","ProviderRevision","DatabaseBytes")):
        raise ValueError("Two independent final read-only replays diverged")
    for Name in ("field", "rays"):
        if (Work / f"{Name}-5.bin").read_bytes() != (Work / f"{Name}-6.bin").read_bytes():
            raise ValueError(f"Final {Name} samples differ between fresh processes")
    if sum(Row["Raced"] for Row in Writes) != 8 or sum(Row["Stale"] for Row in Writes) != 16:
        raise ValueError("CAS contention controls were not exercised")
    for Name, Rows in (("boots", BootRows), ("writes", Writes)):
        with (Work / (Name + ".csv")).open("w", encoding="utf-8", newline="") as File:
            Writer = csv.DictWriter(File, fieldnames=Rows[0].keys())
            Writer.writeheader()
            Writer.writerows(Rows)
    Summary = dict(EngineSha256=Digest(Options.engine.read_bytes()), ServerSha256=Digest(Server.read_bytes()),
                   ScriptSha256=Digest(Path(__file__).with_suffix(".luau").read_bytes()),
                   PackageSha256=Digest((PackageDir / "game.package.json").read_bytes()),
                   PercentilesMs=Percentiles(Writes), SingleWriteMs=Percentiles([R for R in Writes if not R["Raced"]]),
                   SimultaneousPairMs=Percentiles([R for R in Writes if R["Raced"]]), Boots=BootRows)
    (Work / "summary.json").write_text(json.dumps(Summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({K:V for K,V in Summary.items() if K != "Boots"}, indent=2))


if __name__ == "__main__":
    Main()
