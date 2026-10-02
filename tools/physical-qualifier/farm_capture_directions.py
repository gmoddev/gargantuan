"""Stream and reconcile both immutable Farm32 Ethernet pcapng captures.

The result proves 32 bidirectional UDP tuples at each observation point. It
does not by itself prove application identity, packet loss, or provider gates.
"""

import argparse
import hashlib
import ipaddress
import json
from pathlib import Path
import struct


CAPTURE_PROFILE = "Farm32Capture16GiB-v2"
MAX_CAPTURE_BYTES = 15 * 1024 * 1024 * 1024
MAX_BLOCK_BYTES = 1024 * 1024
SERVER_ADDRESS = "10.253.3.2"
CLIENT_ADDRESS = "10.253.3.1"
SERVER_PORT = 39450


def Digest(File):
    with Path(File).open("rb") as Stream:
        return hashlib.file_digest(Stream, "sha256").hexdigest()


def ReadIndex(File, Role):
    File = Path(File).resolve(strict=True)
    if File.name != "capture-sha256.json" or File.stat().st_size > 65536:
        raise ValueError("invalid Farm32 capture index")
    Index = json.loads(File.read_text(encoding="utf-8"))
    if (Index.get("Format") != "GargantuanFarm32CaptureEvidence" or
            Index.get("Version") != 1 or Index.get("Role") != Role or
            Index.get("Profile") != CAPTURE_PROFILE or
            Index.get("State") != "SEALED_UNQUALIFIED" or
            not isinstance(Index.get("Files"), list) or
            not 1 <= len(Index["Files"]) <= 20):
        raise ValueError("Farm32 capture index identity or state is invalid")
    Expected = {}
    for Entry in Index["Files"]:
        Name = Entry.get("Name")
        if (not isinstance(Name, str) or Name in (".", "..") or
                "/" in Name or "\\" in Name or Name in Expected or
                not isinstance(Entry.get("Bytes"), int) or
                not 0 <= Entry["Bytes"] < MAX_CAPTURE_BYTES):
            raise ValueError("Farm32 capture index member is invalid")
        Expected[Name] = Entry
    Actual = {Member.name: Member for Member in File.parent.iterdir() if Member.name != File.name}
    if set(Actual) != set(Expected):
        raise ValueError("Farm32 capture index member set changed")
    for Name, Member in Actual.items():
        if (Member.is_symlink() or not Member.is_file() or
                Member.stat().st_size != Expected[Name]["Bytes"] or
                Digest(Member) != Expected[Name].get("Sha256")):
            raise ValueError("Farm32 capture index member hash changed: " + Name)
    return Index, File.parent


def UdpTuple(Frame, Role):
    if len(Frame) < 14:
        return None
    EtherType = struct.unpack_from(">H", Frame, 12)[0]
    Offset = 14
    while EtherType in (0x8100, 0x88A8, 0x9100):
        if len(Frame) < Offset + 4:
            return None
        EtherType = struct.unpack_from(">H", Frame, Offset + 2)[0]
        Offset += 4
    if EtherType != 0x0800 or len(Frame) < Offset + 20:
        return None
    HeaderBytes = (Frame[Offset] & 0x0F) * 4
    if (Frame[Offset] >> 4 != 4 or HeaderBytes < 20 or
            len(Frame) < Offset + HeaderBytes + 8 or Frame[Offset + 9] != 17 or
            struct.unpack_from(">H", Frame, Offset + 6)[0] & 0x1FFF):
        return None
    Source = str(ipaddress.IPv4Address(Frame[Offset + 12:Offset + 16]))
    Destination = str(ipaddress.IPv4Address(Frame[Offset + 16:Offset + 20]))
    SourcePort, DestinationPort = struct.unpack_from(">HH", Frame, Offset + HeaderBytes)
    if Source == CLIENT_ADDRESS and Destination == SERVER_ADDRESS and DestinationPort == SERVER_PORT:
        return SourcePort, "Outbound" if Role == "CLIENT" else "Inbound"
    if Source == SERVER_ADDRESS and Destination == CLIENT_ADDRESS and SourcePort == SERVER_PORT:
        return DestinationPort, "Inbound" if Role == "CLIENT" else "Outbound"
    return None


def CountDirections(File, Role):
    File = Path(File)
    if not 0 < File.stat().st_size < MAX_CAPTURE_BYTES:
        raise ValueError("Farm32 pcapng size is invalid")
    Tuples = {}
    Interfaces = []
    Endian = None
    with File.open("rb") as Stream:
        while True:
            Header = Stream.read(8)
            if not Header:
                break
            if len(Header) != 8:
                raise ValueError("partial pcapng block header")
            if Header[:4] == b"\x0a\x0d\x0d\x0a":
                Magic = Stream.read(4)
                if Magic == b"\x4d\x3c\x2b\x1a":
                    Endian = "<"
                elif Magic == b"\x1a\x2b\x3c\x4d":
                    Endian = ">"
                else:
                    raise ValueError("invalid pcapng section byte order")
                Length = struct.unpack(Endian + "I", Header[4:])[0]
                if Length < 28 or Length > MAX_BLOCK_BYTES or Length % 4:
                    raise ValueError("invalid pcapng section size")
                Body = Magic + Stream.read(Length - 12)
                Interfaces = []
                Type = 0x0A0D0D0A
            else:
                if Endian is None:
                    raise ValueError("pcapng packet precedes section header")
                Type, Length = struct.unpack(Endian + "II", Header)
                if Length < 12 or Length > MAX_BLOCK_BYTES or Length % 4:
                    raise ValueError("invalid pcapng block size")
                Body = Stream.read(Length - 8)
            if len(Body) != Length - 8 or struct.unpack_from(Endian + "I", Body, len(Body) - 4)[0] != Length:
                raise ValueError("partial or malformed pcapng block")
            if Type == 1:
                Interfaces.append(struct.unpack_from(Endian + "H", Body)[0])
            elif Type == 6:
                if len(Body) < 24:
                    raise ValueError("short pcapng packet block")
                Interface, _, _, Captured, Original = struct.unpack_from(Endian + "IIIII", Body)
                if Interface >= len(Interfaces) or Interfaces[Interface] != 1:
                    raise ValueError("Farm32 capture is not Ethernet")
                if (Captured != Original or Captured > 65535 or
                        20 + Captured > len(Body) - 4):
                    raise ValueError("Farm32 captured packet is truncated")
                Match = UdpTuple(Body[20:20 + Captured], Role)
                if Match is not None:
                    Port, Direction = Match
                    Row = Tuples.setdefault(str(Port), {"Outbound": 0, "Inbound": 0})
                    Row[Direction] += 1
    if Endian is None or not Interfaces:
        raise ValueError("Farm32 pcapng has no Ethernet section")
    return Tuples


def Analyze(ServerIndex, ClientIndex):
    Server, ServerRoot = ReadIndex(ServerIndex, "SERVER")
    Client, ClientRoot = ReadIndex(ClientIndex, "CLIENT")
    if (Server.get("RunId") != Client.get("RunId") or
            Server.get("CoordinatorRunId") != Client.get("CoordinatorRunId") or
            Server.get("RoleIndexSha256") is None or Client.get("RoleIndexSha256") is None):
        raise ValueError("Farm32 capture identities differ")
    ServerTuples = CountDirections(ServerRoot / "farm32-worker-capture.pcapng", "SERVER")
    ClientTuples = CountDirections(ClientRoot / "farm32-client-capture.pcapng", "CLIENT")
    if set(ServerTuples) != set(ClientTuples) or len(ServerTuples) != 32:
        raise ValueError("Farm32 captures do not share exactly 32 client UDP tuples")
    for Port in ServerTuples:
        if any(ServerTuples[Port][Direction] < 1 or ClientTuples[Port][Direction] < 1
               for Direction in ("Outbound", "Inbound")):
            raise ValueError("Farm32 capture lacks a direction for UDP client port " + Port)
    return {"Format": "GargantuanFarm32CaptureDirections", "Version": 1,
            "RunId": Server["RunId"], "CoordinatorRunId": Server["CoordinatorRunId"],
            "Status": "BIDIRECTIONAL_32_TUPLES", "Ports": sorted(ServerTuples, key=int),
            "Server": ServerTuples, "Client": ClientTuples,
            "ServerIndexSha256": Digest(ServerIndex), "ClientIndexSha256": Digest(ClientIndex)}


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("ServerIndex")
    Parser.add_argument("ClientIndex")
    Parser.add_argument("Report")
    Args = Parser.parse_args()
    Report = Analyze(Args.ServerIndex, Args.ClientIndex)
    with Path(Args.Report).open("x", encoding="utf-8") as Stream:
        json.dump(Report, Stream, indent=2)
        Stream.write("\n")


if __name__ == "__main__":
    Main()
