"""Streaming Farm32 packet-direction acceptance without a physical capture."""

import hashlib
import importlib.util
import ipaddress
import json
from pathlib import Path
import struct
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location(
    "farm_capture_directions", Path(__file__).parents[1] / "farm_capture_directions.py")
DIRECTIONS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DIRECTIONS)
RUN_ID = "12345678-1234-4234-8234-123456789abc"
COORDINATOR_ID = "23456789-1234-4234-8234-123456789abc"


def Block(Type, Body):
    Pad = b"\0" * (-len(Body) % 4)
    Length = 12 + len(Body) + len(Pad)
    return struct.pack("<II", Type, Length) + Body + Pad + struct.pack("<I", Length)


def Packet(Source, SourcePort, Destination, DestinationPort):
    Payload = b"farm32"
    Udp = struct.pack("!HHHH", SourcePort, DestinationPort, 8 + len(Payload), 0) + Payload
    Ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(Udp), 1, 0, 64, 17, 0,
                     ipaddress.IPv4Address(Source).packed,
                     ipaddress.IPv4Address(Destination).packed)
    return bytes.fromhex("00112233445566778899aabb0800") + Ip + Udp


def Pcap(Ports, MissingPort=None, Truncated=False):
    Content = Block(0x0A0D0D0A, struct.pack("<IHHq", 0x1A2B3C4D, 1, 0, -1))
    Content += Block(1, struct.pack("<HHI", 1, 0, 65535))
    Sequence = 1
    for Port in Ports:
        for Source, SourcePort, Destination, DestinationPort in (
            ("10.253.3.1", Port, "10.253.3.2", 39450),
            ("10.253.3.2", 39450, "10.253.3.1", Port),
        ):
            if Port == MissingPort and Source == "10.253.3.2":
                continue
            Frame = Packet(Source, SourcePort, Destination, DestinationPort)
            Original = len(Frame) + (1 if Truncated and Sequence == 1 else 0)
            Content += Block(6, struct.pack("<IIIII", 0, 0, Sequence, len(Frame), Original) + Frame)
            Sequence += 1
    return Content


def Index(Root, Role, PcapBytes):
    Root.mkdir()
    Name = "farm32-worker-capture.pcapng" if Role == "SERVER" else "farm32-client-capture.pcapng"
    Capture = Root / Name
    Capture.write_bytes(PcapBytes)
    Row = {"Format": "GargantuanFarm32CaptureEvidence", "Version": 1,
           "RunId": RUN_ID, "CoordinatorRunId": COORDINATOR_ID,
           "Role": Role, "State": "SEALED_UNQUALIFIED", "RoleIndexSha256": "a" * 64,
           "Files": [{"Name": Name, "Bytes": len(PcapBytes),
                      "Sha256": hashlib.sha256(PcapBytes).hexdigest()}]}
    File = Root / "capture-sha256.json"
    File.write_text(json.dumps(Row), encoding="utf-8")
    return File


class FarmCaptureDirectionsTests(unittest.TestCase):
    def setUp(self):
        self.Temp = tempfile.TemporaryDirectory()
        self.Root = Path(self.Temp.name)
        self.Ports = list(range(49000, 49032))

    def tearDown(self):
        self.Temp.cleanup()

    def Pair(self, Server=None, Client=None):
        return (Index(self.Root / "server", "SERVER", Server or Pcap(self.Ports)),
                Index(self.Root / "client", "CLIENT", Client or Pcap(self.Ports)))

    def test_exact_32_bidirectional_tuples_on_both_hosts(self):
        Server, Client = self.Pair()
        Result = DIRECTIONS.Analyze(Server, Client)
        self.assertEqual(Result["Status"], "BIDIRECTIONAL_32_TUPLES")
        self.assertEqual(len(Result["Ports"]), 32)
        self.assertEqual(sum(Row["Inbound"] for Row in Result["Server"].values()), 32)

    def test_missing_direction_is_rejected(self):
        Server, Client = self.Pair(Client=Pcap(self.Ports, MissingPort=49007))
        with self.assertRaisesRegex(ValueError, "lacks a direction"):
            DIRECTIONS.Analyze(Server, Client)

    def test_mismatched_client_ports_are_rejected(self):
        Server, Client = self.Pair(Client=Pcap(list(range(49001, 49033))))
        with self.assertRaisesRegex(ValueError, "do not share exactly 32"):
            DIRECTIONS.Analyze(Server, Client)

    def test_truncated_packet_is_rejected(self):
        Server, Client = self.Pair(Server=Pcap(self.Ports, Truncated=True))
        with self.assertRaisesRegex(ValueError, "truncated"):
            DIRECTIONS.Analyze(Server, Client)

    def test_partial_final_block_is_rejected(self):
        Server, Client = self.Pair(Server=Pcap(self.Ports)[:-1])
        with self.assertRaisesRegex(ValueError, "partial or malformed"):
            DIRECTIONS.Analyze(Server, Client)

    def test_changed_capture_hash_is_rejected(self):
        Server, Client = self.Pair()
        (Server.parent / "farm32-worker-capture.pcapng").write_bytes(Pcap(self.Ports) + b"extra")
        with self.assertRaisesRegex(ValueError, "member hash changed"):
            DIRECTIONS.Analyze(Server, Client)

    def test_31_tuples_do_not_qualify(self):
        Server, Client = self.Pair(Server=Pcap(self.Ports[:31]), Client=Pcap(self.Ports[:31]))
        with self.assertRaisesRegex(ValueError, "exactly 32"):
            DIRECTIONS.Analyze(Server, Client)


if __name__ == "__main__":
    unittest.main()
