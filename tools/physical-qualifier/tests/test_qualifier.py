import importlib.util
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from types import SimpleNamespace
from unittest import mock

Spec = importlib.util.spec_from_file_location("qualifier", Path(__file__).parents[1] / "qualifier.py")
Q = importlib.util.module_from_spec(Spec)
Spec.loader.exec_module(Q)


def PcapNgBlock(BlockType, Payload):
    Padding = b"\0" * ((-len(Payload)) % 4)
    Length = 12 + len(Payload) + len(Padding)
    return struct.pack("<II", BlockType, Length) + Payload + Padding + struct.pack("<I", Length)


def EthernetUdp(SourceAddress, SourcePort, DestinationAddress, DestinationPort):
    import ipaddress
    Payload = b"qualification-direction-test"
    Udp = struct.pack("!HHHH", SourcePort, DestinationPort, 8 + len(Payload), 0) + Payload
    Ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(Udp), 1, 0, 64, 17, 0,
                     ipaddress.IPv4Address(SourceAddress).packed,
                     ipaddress.IPv4Address(DestinationAddress).packed)
    return bytes.fromhex("00112233445566778899aabb0800") + Ip + Udp


def WritePcapNg(File, Packets):
    Section = struct.pack("<IHHq", 0x1A2B3C4D, 1, 0, -1)
    Interface = struct.pack("<HHI", 1, 0, 65535)
    Content = PcapNgBlock(0x0A0D0D0A, Section) + PcapNgBlock(1, Interface)
    for Sequence, Packet in enumerate(Packets, 1):
        Event = struct.pack("<IIIII", 0, 0, Sequence, len(Packet), len(Packet)) + Packet
        Content += PcapNgBlock(6, Event)
    File.write_bytes(Content)


class QualificationTests(unittest.TestCase):
    def setUp(self):
        self.Temp = tempfile.TemporaryDirectory()
        self.Root = Path(self.Temp.name)
        with socket.socket() as Sock:
            Sock.bind(("127.0.0.1", 0))
            Port = Sock.getsockname()[1]
        self.Config = {"RunId": "870c114b-44e5-4e43-a677-59b010f388d9", "Token": "a" * 64,
                       "CoordinatorHost": "127.0.0.1", "PeerIps": {"CLIENT": "127.0.0.1", "SERVER": "127.0.0.1"},
                       "Port": Port, "Endpoint": "10.253.3.2:39450", "ArtifactSHA256": Q.PROBE_SHA,
                       "StageTimeout": 2, "RunTimeout": 2, "EvidenceDir": str(self.Root / "coordinator")}
        self.Thread = None
        self.Links = []

    def test_stage_uses_only_opted_in_privileged_capture_client(self):
        StageRoot = self.Root / "stage"
        Args = SimpleNamespace(
            output=str(StageRoot), client_lan="192.168.0.68", server_lan="192.168.0.108",
            port=39451, nonce=8123, client_bundle=r"C:\client", server_bundle=r"C:\server",
            server_evidence=r"C:\Sandbox\Codex\Evidence\physical-qualifier\run-test",
            capture_device="8", server_capture_client=r"C:\Program Files\Gargantuan\PhysicalQualifierCapture\PhysicalQualifier.CaptureService.exe",
            server_capture_start=None, server_capture_stop=None)
        Q.Stage(Args)
        Config = json.loads((StageRoot / "server.json").read_text())
        self.assertEqual(Config["CaptureStart"], [Args.server_capture_client, "start", "{EvidenceDir}", "{RunId}", "{EndpointPid}"])
        self.assertEqual(Config["CaptureStop"], [Args.server_capture_client, "stop", "{EvidenceDir}", "{RunId}", "{EndpointPid}"])

    def test_stage_requires_both_legacy_hooks_when_service_client_is_absent(self):
        Args = SimpleNamespace(
            output=str(self.Root / "stage-invalid"), client_lan="192.168.0.68", server_lan="192.168.0.108",
            port=39451, nonce=8123, client_bundle=r"C:\client", server_bundle=r"C:\server",
            server_evidence=r"C:\Sandbox\Codex\Evidence\physical-qualifier\run-test", capture_device="8",
            server_capture_client=None, server_capture_start=r"C:\capture.ps1", server_capture_stop=None)
        with self.assertRaisesRegex(ValueError, "both existing capture hooks"):
            Q.Stage(Args)
        self.assertFalse(Path(Args.output).exists())

    def tearDown(self):
        for Link in self.Links:
            Link.Socket.close()
            Link.Journal.Stream.close()
        if self.Thread:
            self.Thread.join(5)
            self.assertFalse(self.Thread.is_alive())
        self.Temp.cleanup()

    def Start(self):
        self.Code = None
        def Run():
            self.Code = Q.Coordinator(self.Config)
        self.Thread = threading.Thread(target=Run)
        self.Thread.start()

    def Peer(self, Role, **Overrides):
        Sock = socket.socket()
        Deadline = time.monotonic() + 2
        while True:
            try:
                Sock.connect(("127.0.0.1", self.Config["Port"]))
                break
            except ConnectionRefusedError:
                if time.monotonic() >= Deadline:
                    raise
                time.sleep(0.01)
        Log = Q.Journal(self.Root / (Role + str(len(self.Links))))
        Link = Q.Channel(Sock, {**self.Config, **Overrides}, Log)
        self.Links.append(Link)
        Link.Send("STAGE_READY", Role=Role, ArtifactSHA256=self.Config["ArtifactSHA256"], Endpoint=self.Config["Endpoint"])
        return Link

    def Read(self, Link, Expected):
        Deadline = time.monotonic() + 3
        while time.monotonic() < Deadline:
            Row = Link.Read()
            if Row:
                self.assertEqual(Expected, Row["Type"])
                return Row
        self.fail("missing " + Expected)

    def Finish(self, ExpectedCode):
        self.Thread.join(4)
        self.assertEqual(self.Code, ExpectedCode)
        Manifest = json.loads((self.Root / "coordinator" / "evidence-manifest.json").read_text())
        for Entry in Manifest["Files"]:
            self.assertEqual(Q.Digest(self.Root / "coordinator" / Entry["Path"]), Entry["SHA256"])
        self.assertNotIn(self.Config["Token"], (self.Root / "coordinator" / "control.jsonl").read_text())

    def test_success_and_signal_latency(self):
        self.Start()
        Client = self.Peer("CLIENT")
        Server = self.Peer("SERVER")
        self.Read(Client, "ARM_CAPTURE")
        Client.Send("CAPTURE_LIVE")
        self.Read(Server, "START_SERVER")
        Sent = time.monotonic()
        Server.Send("SERVER_LIVE", Pid=123, Endpoint=self.Config["Endpoint"])
        self.Read(Client, "START_CLIENT")
        self.assertLess(time.monotonic() - Sent, 0.5)
        Client.Send("CLIENT_RUNNING", Pid=456)
        Client.Send("CLIENT_DONE", Success=True)
        self.Read(Server, "FINALIZE")
        Server.Send("SERVER_DONE", Success=True)
        self.Read(Client, "RUN_DONE")
        self.Read(Server, "RUN_DONE")
        self.Finish(0)

    def test_worker_accepts_delayed_finalize_after_reporting_success(self):
        Config = {**self.Config, "Role": "SERVER", "EvidenceDir": str(self.Root / "late-finalize-worker"),
                  "CaptureCommand": ["test-only"]}
        Listener = socket.socket()
        Listener.bind(("127.0.0.1", 0))
        Config["Port"] = Listener.getsockname()[1]
        Listener.listen(1)
        Listener.settimeout(3)
        EndpointResult = {}

        def Start(Run):
            Output = (Run.Log.Directory / "probe.stdout.log").open("wb")
            Error = (Run.Log.Directory / "probe.stderr.log").open("wb")
            Run.Files.extend((Output, Error))
            Run.Probe = subprocess.Popen(
                [sys.executable, "-c", "import time; time.sleep(.15); print('[Probe:Readiness] result=pass'); print('[Probe:Cleanup] good=1')"],
                stdout=Output, stderr=Error, **Q.Hidden())
            Run.Started = time.monotonic()

        def Helper():
            EndpointResult["Code"] = Q.Endpoint(Config)

        Worker = threading.Thread(target=Helper)
        with mock.patch.object(Q.LocalRun, "Check"), mock.patch.object(Q.LocalRun, "ArmCapture"), \
             mock.patch.object(Q.LocalRun, "Start", Start), mock.patch.object(Q.LocalRun, "ServerLive", return_value=True):
            Worker.start()
            Sock, _ = Listener.accept()
            Link = Q.Channel(Sock, Config, Q.Journal(self.Root / "late-finalize-coordinator"))
            self.Links.append(Link)
            self.Read(Link, "STAGE_READY")
            Link.Send("START_SERVER")
            self.Read(Link, "SERVER_LIVE")
            ServerResult = self.Read(Link, "SERVER_DONE")
            self.assertTrue(ServerResult["Success"])
            # FINALIZE was in flight while the coordinator had not yet received
            # SERVER_DONE; deliver it after the result to force the observed race.
            Link.Send("FINALIZE")
            Link.Send("RUN_DONE", Success=True)
            Worker.join(4)
        Listener.close()
        self.assertFalse(Worker.is_alive())
        self.assertEqual(0, EndpointResult["Code"])
        Result = json.loads((Path(Config["EvidenceDir"]) / "result.json").read_text())
        self.assertTrue(Result["Success"])
        Control = (Path(Config["EvidenceDir"]) / "control.jsonl").read_text()
        self.assertIn('"Detail": "late FINALIZE after SERVER_DONE"', Control)
        self.assertNotIn('"Type": "FAILED"', Control)

    def test_worker_abort_during_live_probe_still_fails_closed(self):
        Config = {**self.Config, "Role": "SERVER", "EvidenceDir": str(self.Root / "abort-live-worker"),
                  "CaptureCommand": ["test-only"]}
        Listener = socket.socket()
        Listener.bind(("127.0.0.1", 0))
        Config["Port"] = Listener.getsockname()[1]
        Listener.listen(1)
        Listener.settimeout(3)
        EndpointResult = {}
        ProbeProcesses = []

        def Start(Run):
            Output = (Run.Log.Directory / "probe.stdout.log").open("wb")
            Error = (Run.Log.Directory / "probe.stderr.log").open("wb")
            Run.Files.extend((Output, Error))
            Run.Probe = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"],
                                         stdout=Output, stderr=Error, **Q.Hidden())
            ProbeProcesses.append(Run.Probe)
            Run.Started = time.monotonic()

        def Helper():
            EndpointResult["Code"] = Q.Endpoint(Config)

        Worker = threading.Thread(target=Helper)
        with mock.patch.object(Q.LocalRun, "Check"), mock.patch.object(Q.LocalRun, "ArmCapture"), \
             mock.patch.object(Q.LocalRun, "Start", Start), mock.patch.object(Q.LocalRun, "ServerLive", return_value=True):
            Worker.start()
            Sock, _ = Listener.accept()
            Link = Q.Channel(Sock, Config, Q.Journal(self.Root / "abort-live-coordinator"))
            self.Links.append(Link)
            self.Read(Link, "STAGE_READY")
            Link.Send("START_SERVER")
            self.Read(Link, "SERVER_LIVE")
            Link.Send("ABORT", Detail="test abort")
            Worker.join(4)
        Listener.close()
        self.assertFalse(Worker.is_alive())
        self.assertEqual(1, EndpointResult["Code"])
        self.assertIsNotNone(ProbeProcesses[0].poll())
        Result = json.loads((Path(Config["EvidenceDir"]) / "result.json").read_text())
        self.assertFalse(Result["Success"])
        self.assertEqual("ABORT", Result["Classification"])

    def test_worker_rejects_finalize_before_probe_result(self):
        Config = {**self.Config, "Role": "SERVER", "EvidenceDir": str(self.Root / "early-finalize-worker"),
                  "CaptureCommand": ["test-only"]}
        Listener = socket.socket()
        Listener.bind(("127.0.0.1", 0))
        Config["Port"] = Listener.getsockname()[1]
        Listener.listen(1)
        Listener.settimeout(3)
        EndpointResult = {}

        def Helper():
            EndpointResult["Code"] = Q.Endpoint(Config)

        Worker = threading.Thread(target=Helper)
        with mock.patch.object(Q.LocalRun, "Check"):
            Worker.start()
            Sock, _ = Listener.accept()
            Link = Q.Channel(Sock, Config, Q.Journal(self.Root / "early-finalize-coordinator"))
            self.Links.append(Link)
            self.Read(Link, "STAGE_READY")
            Link.Send("FINALIZE")
            Worker.join(4)
        Listener.close()
        self.assertFalse(Worker.is_alive())
        self.assertEqual(1, EndpointResult["Code"])
        Result = json.loads((Path(Config["EvidenceDir"]) / "result.json").read_text())
        self.assertFalse(Result["Success"])
        self.assertEqual("ABORT", Result["Classification"])
        Control = (Path(Config["EvidenceDir"]) / "control.jsonl").read_text()
        self.assertIn('"Type": "FAILED"', Control)
        self.assertIn('"Detail": "unexpected command FINALIZE in STAGED"', Control)

    def test_capture_direction_parser_sees_both_udp_directions(self):
        Capture = self.Root / "bidirectional.pcapng"
        WritePcapNg(Capture, [
            EthernetUdp("10.253.3.1", 49155, "10.253.3.2", 39450),
            EthernetUdp("10.253.3.2", 39450, "10.253.3.1", 49155),
            EthernetUdp("192.168.0.68", 49155, "10.253.3.2", 39450),
            EthernetUdp("10.253.3.2", 39450, "192.168.0.68", 49155),
        ])
        self.assertEqual({"Outbound": 1, "Inbound": 1}, Q.CaptureDirections(Capture, "CLIENT"))
        self.assertEqual({"Outbound": 1, "Inbound": 1}, Q.CaptureDirections(Capture, "SERVER"))

    def test_cleanup_fails_when_capture_lacks_a_direction(self):
        Evidence = self.Root / "one-direction"
        Log = Q.Journal(Evidence)
        Run = Q.LocalRun({"Role": "CLIENT", "CaptureCommand": ["test-only"]}, Log)
        Run.CaptureArmed = True
        WritePcapNg(Evidence / "client.pcapng", [EthernetUdp("10.253.3.1", 49155, "10.253.3.2", 39450)])
        Errors = Run.Cleanup()
        self.assertTrue(any("missed a qualified direction" in Error for Error in Errors))
        self.assertEqual(Errors, Run.Cleanup())
        Log.Close({"Success": False})

    def test_cleanup_is_idempotent_including_capture_stop(self):
        Evidence = self.Root / "idempotent-cleanup"
        Log = Q.Journal(Evidence)
        Run = Q.LocalRun({"Role": "SERVER", "CaptureStart": ["start"], "CaptureStop": ["stop"]}, Log)
        Run.CaptureArmed = True
        Run.Hook = mock.Mock()
        WritePcapNg(Evidence / "worker-capture.pcapng", [
            EthernetUdp("10.253.3.2", 39450, "10.253.3.1", 49155),
            EthernetUdp("10.253.3.1", 49155, "10.253.3.2", 39450),
        ])
        First = Run.Cleanup()
        Second = Run.Cleanup()
        self.assertEqual([], First)
        self.assertEqual(First, Second)
        Run.Hook.assert_called_once_with("CaptureStop")
        self.assertEqual(1, (Evidence / "control.jsonl").read_text().count('"Event": "LOCAL_CLEANUP"'))
        Log.Close({"Success": True})

    def test_failed_probe_finalizes_both_endpoints(self):
        self.Start()
        Client, Server = self.Peer("CLIENT"), self.Peer("SERVER")
        self.Read(Client, "ARM_CAPTURE")
        Client.Send("CAPTURE_LIVE")
        self.Read(Server, "START_SERVER")
        Server.Send("SERVER_LIVE", Pid=123, Endpoint=self.Config["Endpoint"])
        self.Read(Client, "START_CLIENT")
        Client.Send("CLIENT_DONE", Success=False)
        self.Read(Server, "FINALIZE")
        Server.Send("SERVER_DONE", Success=False)
        self.Read(Client, "RUN_DONE")
        self.Read(Server, "RUN_DONE")
        self.Finish(1)

    def test_server_finishes_before_client(self):
        self.Start()
        Client, Server = self.Peer("CLIENT"), self.Peer("SERVER")
        self.Read(Client, "ARM_CAPTURE")
        Client.Send("CAPTURE_LIVE")
        self.Read(Server, "START_SERVER")
        Server.Send("SERVER_LIVE", Pid=123, Endpoint=self.Config["Endpoint"])
        self.Read(Client, "START_CLIENT")
        Server.Send("SERVER_DONE", Success=True)
        Client.Send("CLIENT_DONE", Success=True)
        self.Read(Client, "RUN_DONE")
        self.Read(Server, "RUN_DONE")
        self.Finish(0)

    def test_wrong_token_aborts(self):
        self.Start()
        Client = self.Peer("CLIENT", Token="b" * 64)
        with self.assertRaises(ValueError):
            self.Read(Client, "ABORT")  # Coordinator envelope uses the expected token.
        self.Finish(1)

    def test_prior_run_cannot_trigger_start(self):
        self.Start()
        Client = self.Peer("CLIENT", RunId="870c114b-44e5-4e43-a677-59b010f388d8")
        with self.assertRaises(ValueError):
            self.Read(Client, "ABORT")
        self.Finish(1)

    def test_duplicate_sequence_aborts(self):
        self.Start()
        Client, Server = self.Peer("CLIENT"), self.Peer("SERVER")
        self.Read(Client, "ARM_CAPTURE")
        Client.Tx = 0
        Client.Send("CAPTURE_LIVE")
        self.Read(Client, "ABORT")
        self.Read(Server, "ABORT")
        self.Finish(1)

    def test_early_server_live_cannot_launch_client(self):
        self.Start()
        Client, Server = self.Peer("CLIENT"), self.Peer("SERVER")
        self.Read(Client, "ARM_CAPTURE")
        Server.Send("SERVER_LIVE", Pid=123, Endpoint=self.Config["Endpoint"])
        self.Read(Client, "ABORT")
        self.Finish(1)

    def test_disconnect_aborts_surviving_peer(self):
        self.Start()
        Client, Server = self.Peer("CLIENT"), self.Peer("SERVER")
        self.Read(Client, "ARM_CAPTURE")
        Server.Socket.close()
        self.Read(Client, "ABORT")
        self.Finish(1)

    def test_barrier_timeout_does_not_launch(self):
        self.Config["StageTimeout"] = 1
        self.Start()
        Client = self.Peer("CLIENT")
        self.Read(Client, "ABORT")
        self.Finish(1)

    def test_oversized_frame_aborts(self):
        self.Start()
        Client = self.Peer("CLIENT")
        Client.Socket.sendall(b"x" * Q.MAX_FRAME)
        self.Read(Client, "ABORT")
        self.Finish(1)

    def test_fiber_control_address_rejected(self):
        self.Config["CoordinatorHost"] = "10.253.3.1"
        with self.assertRaises(ValueError):
            Q.ValidateConfig(self.Config)

    def test_actual_endpoint_helpers_coordinate_owned_child_processes(self):
        self.Start()
        Codes = {}
        def Check(Run):
            Run.Log.Write("TEST_PROVENANCE")
        def Arm(Run):
            Run.CaptureArmed = True
            WritePcapNg(Run.Log.Directory / "client.pcapng", [
                EthernetUdp("10.253.3.1", 49155, "10.253.3.2", 39450),
                EthernetUdp("10.253.3.2", 39450, "10.253.3.1", 49155),
            ])
        def Start(Run):
            Output = (Run.Log.Directory / "probe.stdout.log").open("wb")
            Error = (Run.Log.Directory / "probe.stderr.log").open("wb")
            Run.Files.extend((Output, Error))
            Delay = 0.5 if Run.Config["Role"] == "SERVER" else 0.2
            Code = "import time; time.sleep(" + str(Delay) + "); print('[Probe:Readiness] result=pass'); print('[Probe:Cleanup] good=1')"
            Run.Probe = subprocess.Popen([sys.executable, "-c", Code], stdout=Output, stderr=Error, **Q.Hidden())
            Run.Started = time.monotonic()
        def Helper(Role):
            Config = {**self.Config, "Role": Role, "EvidenceDir": str(self.Root / (Role + "-helper")),
                      "CaptureCommand": ["test-only"]}
            Codes[Role] = Q.Endpoint(Config)
        with mock.patch.object(Q.LocalRun, "Check", Check), mock.patch.object(Q.LocalRun, "ArmCapture", Arm), mock.patch.object(Q.LocalRun, "Start", Start), mock.patch.object(Q.LocalRun, "ServerLive", return_value=True):
            Workers = [threading.Thread(target=Helper, args=(Role,)) for Role in ("CLIENT", "SERVER")]
            for Worker in Workers:
                Worker.start()
            for Worker in Workers:
                Worker.join(5)
                self.assertFalse(Worker.is_alive())
        self.assertEqual({"CLIENT": 0, "SERVER": 0}, Codes)
        self.Finish(0)

    def test_cleanup_terminates_only_owned_children(self):
        Log = Q.Journal(self.Root / "cleanup")
        Run = Q.LocalRun({"CaptureCommand": ["unused"]}, Log)
        Run.Probe = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"], **Q.Hidden())
        Unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"], **Q.Hidden())
        try:
            self.assertEqual([], Run.Cleanup())
            self.assertIsNotNone(Run.Probe.poll())
            self.assertIsNone(Unrelated.poll())
        finally:
            Unrelated.terminate()
            Unrelated.wait()
            Log.Stream.close()

    @unittest.skipUnless(os.name == "nt", "Windows inline capture hook")
    def test_inline_hook_arguments_and_staged_hash(self):
        Log = Q.Journal(self.Root / "hook evidence")
        Script = self.Root / "hook ' quoted.ps1"
        Script.write_text("param([string]$EvidenceDir,[string]$Action)\nSet-Content -LiteralPath (Join-Path $EvidenceDir 'hook.txt') -Value $Action", encoding="utf-8")
        Run = Q.LocalRun({"CaptureStart": ["powershell.exe", "-NoProfile", "-NonInteractive", "-File", str(Script), "{EvidenceDir}", "Start"]}, Log)
        Run.HookHashes[str(Script)] = Q.Digest(Script)
        try:
            Run.Hook("CaptureStart")
            self.assertEqual("Start", (Log.Directory / "hook.txt").read_text().strip())
            Script.write_text("throw 'modified'", encoding="utf-8")
            with self.assertRaises(ValueError):
                Run.Hook("CaptureStart")
        finally:
            Log.Stream.close()


if __name__ == "__main__":
    unittest.main()
