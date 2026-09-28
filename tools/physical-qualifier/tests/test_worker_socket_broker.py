"""Fixed worker socket capability and failure-boundary regressions."""

import json
from pathlib import Path
import socket
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import worker_socket_broker as Broker
from socket_gate import RequireWorkerControlTunnel


class WorkerSocketBrokerTests(unittest.TestCase):
    def test_broker_preflights_then_launches_only_pinned_endpoint(self):
        with tempfile.TemporaryDirectory() as Temporary:
            Root = Path(Temporary)
            Artifacts = Root / "artifacts"
            Label = "0123456789abcdef"
            Directory = Artifacts / Label
            Directory.mkdir(parents=True)
            Workspace = Root / "workspace"
            (Workspace / ".lifecycle").mkdir(parents=True)
            Tool = Directory / "broker-tool" / "physical-qualifier" / "qualifier.py"
            Tool.parent.mkdir(parents=True)
            Tool.write_text("from pathlib import Path\n"
                            "Path(__file__).with_name('mock-started.txt').write_text('started')\n",
                            encoding="utf-8")
            Package = Directory / "physical-catalog.zip"
            Package.write_bytes(b"test package")
            RunId = "12345678-1234-1234-1234-123456789abc"
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as Listener:
                Listener.bind(("127.0.0.1", 0))
                Listener.listen(1)
                Listener.settimeout(3)
                Port = Listener.getsockname()[1]
                Config = Directory / "server-broker.json"
                Config.write_text(json.dumps({"RunId": RunId, "Role": "SERVER",
                                              "CoordinatorHost": "127.0.0.1", "Port": Port,
                                              "PeerIps": {"SERVER": "127.0.0.1"},
                                              "EvidenceDir": str(Path(
                                                  r"C:\GargantuanQualification\physical-qualifier-service-evidence") /
                                                  ("lifecycle-" + Label))}), encoding="utf-8")
                Manifest = Directory / "broker-manifest.json"
                Manifest.write_text(json.dumps({"Label": Label, "RunId": RunId,
                                                "ToolSHA256": Broker.Digest(Tool),
                                                "ConfigSHA256": Broker.Digest(Config),
                                                "PackageSHA256": Broker.Digest(Package)}), encoding="utf-8")
                Pin = Broker.Digest(Manifest)
                Result = {}

                def Work():
                    try:
                        Result["Code"] = Broker.Serve(Manifest, Pin)
                    except Exception as Error:
                        Result["Error"] = str(Error)

                with patch.object(Broker, "ARTIFACTS", Artifacts), \
                        patch.object(Broker, "WORKSPACE", Workspace), \
                        patch.object(Broker, "PYTHON", Path(sys.executable)), \
                        patch.object(Broker, "CONTROL_HOST", "127.0.0.1"), \
                        patch.object(Broker, "WORKER_HOST", "127.0.0.1"), \
                        patch.object(Broker, "CONTROL_PORT", Port):
                    Paths = Broker.Paths(Label)
                    Thread = threading.Thread(target=Work)
                    Thread.start()
                    Paths["preflight"].write_text(json.dumps(
                        {"RunId": RunId, "Action": "PREFLIGHT"}), encoding="utf-8")
                    Peer, _ = Listener.accept()
                    Peer.close()
                    Deadline = time.monotonic() + 3
                    while not Paths["preflight-result"].exists() and time.monotonic() < Deadline:
                        time.sleep(0.01)
                    self.assertTrue(Paths["preflight-result"].exists())
                    self.assertTrue(json.loads(Paths["preflight-result"].read_text())["Success"])
                    Paths["start"].write_text(json.dumps(
                        {"RunId": RunId, "Action": "START"}), encoding="utf-8")
                    Thread.join(5)
                    self.assertFalse(Thread.is_alive())
                    self.assertEqual(Result, {"Code": 0})
                    self.assertEqual((Tool.parent / "mock-started.txt").read_text(), "started")
                    self.assertTrue(json.loads(Paths["start-result"].read_text())["Success"])

    def test_reverse_tunnel_gate_rejects_missing_listener_before_wake(self):
        Calls = []

        def Missing(Command, **Options):
            Calls.append(Command)
            return SimpleNamespace(returncode=1, stdout="", stderr="not listening")

        with self.assertRaisesRegex(RuntimeError, "reverse lifecycle tunnel missing"):
            RequireWorkerControlTunnel("0123456789abcdef", "C:/fixed", Missing)
        self.assertEqual(len(Calls), 1)
        self.assertEqual(Calls[0][-1],
                         "C:/fixed/0123456789abcdef/Check-WorkerControlTunnel.ps1")

        def Ready(Command, **Options):
            return SimpleNamespace(returncode=0, stdout="LISTENING", stderr="")

        RequireWorkerControlTunnel("0123456789abcdef", "C:/fixed", Ready)

    def test_fixed_manifest_rejects_arbitrary_control_endpoint_and_changed_pin(self):
        with tempfile.TemporaryDirectory() as Temporary:
            Root = Path(Temporary)
            Artifacts = Root / "artifacts"
            Label = "0123456789abcdef"
            Directory = Artifacts / Label
            Directory.mkdir(parents=True)
            Tool = Directory / "broker-tool" / "physical-qualifier" / "qualifier.py"
            Tool.parent.mkdir(parents=True)
            Tool.write_text("pass\n", encoding="utf-8")
            Package = Directory / "physical-catalog.zip"
            Package.write_bytes(b"test package")
            Config = Directory / "server-broker.json"
            RunId = "12345678-1234-1234-1234-123456789abc"
            Value = {"RunId": RunId, "Role": "SERVER",
                     "CoordinatorHost": Broker.CONTROL_HOST, "Port": Broker.CONTROL_PORT,
                     "PeerIps": {"SERVER": Broker.WORKER_HOST},
                     "EvidenceDir": str(Path(
                         r"C:\GargantuanQualification\physical-qualifier-service-evidence") /
                         ("lifecycle-" + Label))}
            Config.write_text(json.dumps(Value), encoding="utf-8")
            Manifest = Directory / "broker-manifest.json"

            def WriteManifest():
                Manifest.write_text(json.dumps({"Label": Label, "RunId": RunId,
                                                "ToolSHA256": Broker.Digest(Tool),
                                                "ConfigSHA256": Broker.Digest(Config),
                                                "PackageSHA256": Broker.Digest(Package)}),
                                    encoding="utf-8")
                return Broker.Digest(Manifest)

            with patch.object(Broker, "ARTIFACTS", Artifacts), \
                    patch.object(Broker, "PYTHON", Path(sys.executable)):
                Pin = WriteManifest()
                self.assertEqual(Broker.ValidateManifest(Manifest, Pin)[1], Config)
                for Key, Bad in (("CoordinatorHost", "192.168.0.99"), ("Port", 39452),
                                 ("Role", "CLIENT")):
                    Old = Value[Key]
                    Value[Key] = Bad
                    Config.write_text(json.dumps(Value), encoding="utf-8")
                    Pin = WriteManifest()
                    with self.assertRaisesRegex(ValueError, "endpoint/port/role"):
                        Broker.ValidateManifest(Manifest, Pin)
                    Value[Key] = Old
                    Config.write_text(json.dumps(Value), encoding="utf-8")
                Pin = WriteManifest()
                Config.write_text("{}", encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "input pin changed"):
                    Broker.ValidateManifest(Manifest, Pin)

    def test_request_has_no_command_or_path_fields(self):
        with tempfile.TemporaryDirectory() as Temporary:
            File = Path(Temporary) / "request.json"
            RunId = "12345678-1234-1234-1234-123456789abc"
            File.write_text(json.dumps({"RunId": RunId, "Action": "START",
                                        "Port": 39452}), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "unapproved broker request"):
                Broker.CheckRequest(File, RunId, "START")
            File.write_text(json.dumps({"RunId": RunId, "Action": "START"}), encoding="utf-8")
            Broker.CheckRequest(File, RunId, "START")
            with self.assertRaisesRegex(ValueError, "unapproved broker request"):
                Broker.CheckRequest(File, RunId, "PREFLIGHT")

    def test_socket_preflight_connects_and_closes(self):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as Listener:
            Listener.bind(("127.0.0.1", 0))
            Listener.listen(1)
            Port = Listener.getsockname()[1]
            Accepted = []

            def Accept():
                Peer, _ = Listener.accept()
                Accepted.append(Peer)
                Peer.close()

            Thread = threading.Thread(target=Accept)
            Thread.start()
            with patch.object(Broker, "WORKER_HOST", "127.0.0.1"), \
                    patch.object(Broker, "CONTROL_HOST", "127.0.0.1"), \
                    patch.object(Broker, "CONTROL_PORT", Port):
                Report = Broker.SocketPreflight()
            Thread.join(2)
            self.assertFalse(Thread.is_alive())
            self.assertTrue(Report["Success"])
            self.assertEqual(Report["Operation"], "close")
            self.assertEqual(len(Accepted), 1)

    def test_stale_listener_fails_cleanly(self):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as Listener:
            Listener.bind(("127.0.0.1", 0))
            Listener.listen(1)
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as Duplicate:
                with self.assertRaises(OSError):
                    Duplicate.bind(Listener.getsockname())


if __name__ == "__main__":
    unittest.main()
