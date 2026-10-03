"""Offline Farm32 capture qualification against sealed nonce and UDP-port evidence.

Bidirectionality is an observation at both fiber endpoints, not a packet-reserve
or application-service measurement. No capture process is started here.
"""

import argparse
from datetime import datetime
import json
from pathlib import Path
import re
import uuid

import farm_capture_directions as directions


SHA256 = re.compile(r"[0-9a-f]{64}\Z")
READY = re.compile(
    r"^\[Qualification:Server\] event=ready run=(?P<RunId>[0-9a-f-]+) "
    r"nonce=(?P<Nonce>[0-9]+) connection_slot=(?P<Slot>[0-9]+) "
    r"connection_generation=(?P<Generation>[0-9]+) .*?client_port=(?P<Port>[0-9]+)(?: |$)")
RECEIVED_DROPPED = re.compile(
    r"^Packets received/dropped on interface .+?:\s*([0-9]+)/([0-9]+)(?:\s|$)", re.MULTILINE)
CAPTURED = re.compile(r"^Packets captured:\s*([0-9]+)\s*$", re.MULTILINE)
WORKER_LOSS = re.compile(r"^Total Events\s+Lost\s+([0-9]+)\s*$", re.MULTILINE)


def ReadJson(File, Maximum=1024 * 1024):
    File = Path(File).resolve(strict=True)
    if not File.is_file() or File.stat().st_size > Maximum:
        raise ValueError("missing or oversized Farm32 JSON")
    return json.loads(File.read_text(encoding="utf-8"))


def ReadText(File, Maximum=16 * 1024 * 1024):
    File = Path(File).resolve(strict=True)
    if not File.is_file() or File.stat().st_size > Maximum:
        raise ValueError("missing or oversized Farm32 text evidence")
    return File.read_text(encoding="utf-8", errors="strict")


def IndexedFile(IndexPath, Name, Maximum):
    IndexPath = Path(IndexPath).resolve(strict=True)
    Index = ReadJson(IndexPath)
    Entries = [Row for Row in Index.get("Files", []) if Row.get("Name") == Name]
    if len(Entries) != 1 or not isinstance(Entries[0].get("Bytes"), int) or not 0 <= Entries[0]["Bytes"] <= Maximum:
        raise ValueError("missing or duplicated indexed Farm32 member: " + Name)
    File = IndexPath.parent / Name
    if (File.is_symlink() or not File.is_file() or File.stat().st_size != Entries[0]["Bytes"] or
            directions.Digest(File) != Entries[0].get("Sha256")):
        raise ValueError("changed indexed Farm32 member: " + Name)
    return File


def Utc(Value):
    if not isinstance(Value, str):
        raise ValueError("Farm32 capture timestamp is missing")
    Result = datetime.fromisoformat(Value.replace("Z", "+00:00"))
    if Result.tzinfo is None:
        raise ValueError("Farm32 capture timestamp lacks UTC offset")
    return Result


def ReadyPorts(Log, RunId, Nonces):
    Found = {}
    Handles = set()
    Ports = set()
    for Line in Log.splitlines():
        if not Line.startswith("[Qualification:Server] event=ready "):
            continue
        Match = READY.fullmatch(Line)
        if not Match or Match["RunId"] != RunId or Match["Nonce"] not in Nonces:
            raise ValueError("invalid Farm32 nonce-to-port ready marker")
        Nonce = Match["Nonce"]
        Slot, Generation, Port = (int(Match[Key]) for Key in ("Slot", "Generation", "Port"))
        if (Nonce in Found or Slot == 0 or Generation == 0 or not 1 <= Port <= 65535 or
                (Slot, Generation) in Handles or Port in Ports):
            raise ValueError("duplicate or invalid Farm32 nonce-to-port binding")
        Found[Nonce] = Port
        Handles.add((Slot, Generation))
        Ports.add(Port)
    if set(Found) != set(Nonces) or len(Found) != 32:
        raise ValueError("missing Farm32 nonce-to-port ready bindings")
    return Found


def ZeroLoss(ServerRoot, ClientRoot, RunId):
    WorkerMarker = ReadJson(ServerRoot / "farm32-netsh-owner.json", 65536)
    if (WorkerMarker.get("Profile") != directions.CAPTURE_PROFILE or
            WorkerMarker.get("CapturePort") != 39450 or
            WorkerMarker.get("MiniportIfIndex") != 19 or
            WorkerMarker.get("CaptureLayers") != ["NDIS physical miniport"] or
            WorkerMarker.get("TraceMaximumMiB") != 16384 or
            WorkerMarker.get("NoWrapThresholdMiB") != 15360 or
            WorkerMarker.get("PerformanceMetadataMerge") is not False or
            not (ServerRoot / "farm32-worker-capture.etl").is_file() or
            not 0 < (ServerRoot / "farm32-worker-capture.etl").stat().st_size < directions.MAX_CAPTURE_BYTES):
        raise ValueError("worker Farm32 capture ownership or completeness marker is invalid")
    Summary = ReadText(ServerRoot / "farm32-capture-summary.txt", 65536)
    Loss = WORKER_LOSS.findall(Summary)
    if len(Loss) != 1 or int(Loss[0]) != 0:
        raise ValueError("worker capture loss is missing or nonzero")
    Marker = ReadJson(ClientRoot / "farm32-client-capture.json", 65536)
    Capture = ClientRoot / "farm32-client-capture.pcapng"
    if (Marker.get("Format") != "GargantuanFarm32Dumpcap" or Marker.get("Version") != 1 or
            Marker.get("Profile") != directions.CAPTURE_PROFILE or Marker.get("RunId") != RunId or
            Marker.get("Filter") != "udp port 39450 and host 10.253.3.2" or
            Marker.get("DurationSeconds") != 600 or
            Marker.get("AutostopKilobytes") != 16777216 or
            Marker.get("CompletenessBytes") != directions.MAX_CAPTURE_BYTES or
            Marker.get("RequestedBufferMiB") != 64 or
            not Capture.stat().st_size < Marker["CompletenessBytes"] or
            not isinstance(Marker.get("DumpcapSha256"), str) or
            not SHA256.fullmatch(Marker["DumpcapSha256"])):
        raise ValueError("Farm32 client capture marker is invalid")
    Diagnostic = ReadText(ClientRoot / "farm32-dumpcap-error.txt", 65536)
    Counts = RECEIVED_DROPPED.findall(Diagnostic)
    Captured = CAPTURED.findall(Diagnostic)
    if len(Counts) != 1 or len(Captured) != 1 or int(Counts[0][0]) == 0 or int(Captured[0]) == 0:
        raise ValueError("client dumpcap loss counters are missing or ambiguous")
    if int(Counts[0][1]) != 0:
        raise ValueError("client dumpcap dropped packets")
    return {"WorkerLostEvents": 0, "ClientReceivedPackets": int(Counts[0][0]),
            "ClientDroppedPackets": 0, "ClientCapturedPackets": int(Captured[0])}


def Analyze(ServerCapture, ClientCapture, ServerRole, ClientRole, OuterReceipt, CoordinatorResult):
    ServerCapture = Path(ServerCapture).resolve(strict=True)
    ClientCapture = Path(ClientCapture).resolve(strict=True)
    ServerRole = Path(ServerRole).resolve(strict=True)
    ClientRole = Path(ClientRole).resolve(strict=True)
    Capture = directions.Analyze(ServerCapture, ClientCapture)
    RunId = Capture["RunId"]
    try:
        if str(uuid.UUID(RunId)) != RunId:
            raise ValueError()
    except (TypeError, ValueError):
        raise ValueError("noncanonical Farm32 run identity") from None
    ServerIndex, ServerRoot = directions.ReadIndex(ServerCapture, "SERVER")
    ClientIndex, ClientRoot = directions.ReadIndex(ClientCapture, "CLIENT")
    Outer = ReadJson(OuterReceipt, 65536)
    Coordinator = ReadJson(CoordinatorResult)
    if (Outer.get("Format") != "GargantuanFarm32OuterReceipt" or Outer.get("Version") != 1 or
            Outer.get("State") != "SEALED_UNQUALIFIED" or Outer.get("RunId") != RunId or
            Outer.get("CoordinatorRunId") != Capture["CoordinatorRunId"] or
            Outer.get("CoordinatorResultSha256") != directions.Digest(CoordinatorResult) or
            Coordinator.get("RunId") != Capture["CoordinatorRunId"] or Coordinator.get("Success") is not True):
        raise ValueError("Farm32 outer/coordinator receipt identity differs")
    for Role, CaptureIndex, RoleIndex in (("SERVER", ServerIndex, ServerRole),
                                          ("CLIENT", ClientIndex, ClientRole)):
        RoleDigest = directions.Digest(RoleIndex)
        CaptureDigest = directions.Digest(ServerCapture if Role == "SERVER" else ClientCapture)
        Bound = Outer.get("Roles", {}).get(Role, {})
        if (CaptureIndex.get("RoleIndexSha256") != RoleDigest or
                Bound.get("RoleIndexSha256") != RoleDigest or
                Bound.get("CaptureIndexSha256") != CaptureDigest or
                ReadJson(RoleIndex).get("RunId") != RunId):
            raise ValueError("Farm32 capture is not bound to sealed role evidence")
    Manifest = ReadJson(IndexedFile(ServerRole, "run-manifest.json", 65536), 65536)
    ClientManifest = ReadJson(IndexedFile(ClientRole, "run-manifest.json", 65536), 65536)
    Nonces = Manifest.get("Nonces")
    if (Manifest != ClientManifest or Manifest.get("RunId") != RunId or
            Manifest.get("Endpoint") != "10.253.3.2:39450" or
            Manifest.get("Provider") not in ("Local", "Node") or
            not isinstance(Nonces, list) or len(Nonces) != 32 or
            len(set(Nonces)) != 32 or any(not isinstance(Nonce, str) or
                                         not Nonce.isdecimal() for Nonce in Nonces)):
        raise ValueError("Farm32 physical run manifest identity is invalid")
    Log = ReadText(IndexedFile(ServerRole, "server.stdout.log", 16 * 1024 * 1024))
    Binding = ReadyPorts(Log, RunId, Nonces)
    if set(Binding.values()) != set(map(int, Capture["Ports"])):
        raise ValueError("captured UDP tuples differ from nonce-bound GNS peers")
    ServerRoleResult = ReadJson(IndexedFile(ServerRole, "result.json", 65536), 65536)
    ClientRoleResult = ReadJson(IndexedFile(ClientRole, "result.json", 65536), 65536)
    for RoleResult, Index in ((ServerRoleResult, ServerIndex), (ClientRoleResult, ClientIndex)):
        if (RoleResult.get("RunId") != RunId or RoleResult.get("Status") != "PASS" or
                Utc(Index.get("ReadyUtc")) > Utc(RoleResult.get("StartedUtc")) or
                Utc(Index.get("StoppedUtc")) < Utc(RoleResult.get("CompletedUtc"))):
            raise ValueError("Farm32 capture did not cover the completed role")
    Loss = ZeroLoss(ServerRoot, ClientRoot, RunId)
    return {"Format": "GargantuanFarm32CaptureAcceptance", "Version": 1,
            "Status": "MEASURED_PASS", "RunId": RunId,
            "CoordinatorRunId": Capture["CoordinatorRunId"],
            "Provider": Manifest["Provider"], "NonceBoundTuples": 32,
            "Directions": Capture, "Loss": Loss,
            "OuterReceiptSha256": directions.Digest(OuterReceipt),
            "ServerRoleIndexSha256": directions.Digest(ServerRole),
            "ClientRoleIndexSha256": directions.Digest(ClientRole),
            "Limitations": "No packet-reserve, transport-headroom, or application-service claim"}


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    for Name in ("ServerCapture", "ClientCapture", "ServerRole", "ClientRole",
                 "OuterReceipt", "CoordinatorResult"):
        Parser.add_argument(Name)
    Args = Parser.parse_args()
    Report = Analyze(Args.ServerCapture, Args.ClientCapture, Args.ServerRole,
                     Args.ClientRole, Args.OuterReceipt, Args.CoordinatorResult)
    print(json.dumps(Report, separators=(",", ":")))


if __name__ == "__main__":
    Main()
