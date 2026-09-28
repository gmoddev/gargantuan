"""Fixed two-direction tunnel, handshake, replay and cleanup regressions."""

import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import lifecycle_tunnel as Tunnel


LABEL = "0123456789abcdef"
RUN_ID = "12345678-1234-1234-1234-123456789abc"
SETUP = {"ControlPort": 49961,
         "Endpoints": {"CLIENT": {"LifecyclePort": 49962, "EndpointId": "CLIENT"},
                       "SERVER": {"LifecyclePort": 49964, "EndpointId": "SERVER"}}}
STAGE = {"Label": LABEL, "RunId": RUN_ID}


class LifecycleTunnelTests(unittest.TestCase):
    def test_exact_host_owned_forward_and_reverse(self):
        Command = Tunnel.TunnelCommand()
        self.assertEqual(Command[Command.index("-L") + 1],
                         "127.0.0.1:49964:127.0.0.1:49963")
        self.assertEqual(Command[Command.index("-R") + 1],
                         "127.0.0.1:49961:127.0.0.1:49961")
        self.assertIn("ExitOnForwardFailure=yes", Command)
        Wrong = json.loads(json.dumps(SETUP))
        Wrong["Endpoints"]["SERVER"]["LifecyclePort"] = 49965
        with self.assertRaisesRegex(ValueError, "fixed SSH topology"):
            Tunnel.ValidateTopology(Wrong)

    def test_missing_or_stale_reverse_fails_before_assignment(self):
        with tempfile.TemporaryDirectory() as Temporary:
            Session = Tunnel.TunnelSession(SETUP, STAGE, Temporary)
            with patch.object(Tunnel, "WorkerReverseListener", return_value={"Present": True}):
                with self.assertRaisesRegex(RuntimeError, "stale worker reverse listener"):
                    Session.__enter__()
            self.assertIsNone(Session.Process)
            self.assertFalse((Path(Temporary) / "tunnel-preflight.json").exists())
            with self.assertRaises(FileExistsError):
                Session.__enter__()  # a consumed run cannot reuse its tunnel mapping
            Session.Process = SimpleNamespace(poll=lambda: None)
            with patch.object(Tunnel, "WorkerReverseListener", return_value={"Present": False}):
                with self.assertRaisesRegex(RuntimeError, "disappeared before wake"):
                    Session.RequireAlive()

    def test_reverse_handshake_from_fixed_worker_endpoint(self):
        with tempfile.TemporaryDirectory() as Temporary:
            Session = Tunnel.TunnelSession(SETUP, STAGE, Temporary)
            Session.Process = SimpleNamespace(pid=731, poll=lambda: None)
            Session.StartedUnixMs = time.time_ns() // 1000000 - 1000

            class WorkerClient:
                def __init__(self, Host, Port, EndpointId, Key):
                    self.asserted = (Host, Port, EndpointId, Key)

                def GetPresence(self):
                    return {"EndpointId": "SERVER", "Status": "OFFLINE"}

            def Probe(_Stage, _Artifact):
                WorkerNonce = "a" * 32
                Started = time.time_ns() // 1000000
                with socket.create_connection(("127.0.0.1", 49961), timeout=3) as Connection:
                    Connection.settimeout(3)
                    Connection.sendall((json.dumps({"Version": 1, "Label": LABEL,
                                                    "RunId": RUN_ID,
                                                    "WorkerNonce": WorkerNonce}) + "\n").encode())
                    Response = json.loads(Connection.makefile("rb").readline(512))
                    Connection.sendall((json.dumps({"HostNonce": Response["HostNonce"]}) +
                                        "\n").encode())
                return {"ExitCode": 0, "Success": True, "Label": LABEL,
                        "RunId": RUN_ID, "Sid": Tunnel.WORKER_SID, "IsAdmin": False,
                        "Address": "127.0.0.1", "Port": 49961,
                        "WorkerNonce": WorkerNonce, "HostNonce": Response["HostNonce"],
                        "StartedUnixMs": Started,
                        "CompletedUnixMs": time.time_ns() // 1000000}

            with patch.object(Tunnel, "LaunchRestrictedReverseProbe", side_effect=Probe), \
                    patch.object(Tunnel, "WorkerReverseListener", return_value={
                        "Present": True, "Address": "127.0.0.1", "Port": 49961,
                        "OwnerPid": 732, "ProcessName": "sshd"}):
                Proof = Session.Preflight(b"key", WorkerClient)
            self.assertEqual(Proof["Forward"]["ListenerOwnerPid"], 731)
            self.assertEqual(Proof["Reverse"]["Sid"], Tunnel.WORKER_SID)
            self.assertEqual(Proof["ReverseHost"]["WorkerNonce"], "a" * 32)

    def test_wrong_endpoint_and_stale_nonce_rejected(self):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as Listener:
            Listener.bind(("127.0.0.1", 0))
            Listener.listen(1)
            Listener.settimeout(3)
            with socket.create_connection(Listener.getsockname(), timeout=3) as Client:
                Client.sendall((json.dumps({"Version": 1, "Label": LABEL,
                                            "RunId": "00000000-0000-0000-0000-000000000000",
                                            "WorkerNonce": "a" * 32}) + "\n").encode())
                with self.assertRaisesRegex(ValueError, "wrong run or endpoint"):
                    Tunnel.ServeChallenge(Listener, LABEL, RUN_ID)
        Host = {"WorkerNonce": "a" * 32, "HostNonce": "b" * 32}
        Worker = {"ExitCode": 0, "Success": True, "Label": LABEL, "RunId": RUN_ID,
                  "Sid": Tunnel.WORKER_SID, "IsAdmin": False, "Address": "127.0.0.1",
                  "Port": 49961, "WorkerNonce": "a" * 32, "HostNonce": "c" * 32,
                  "StartedUnixMs": time.time_ns() // 1000000,
                  "CompletedUnixMs": time.time_ns() // 1000000}
        with self.assertRaisesRegex(ValueError, "stale or mismatched"):
            Tunnel.RequireReverseProof(Worker, Host, STAGE, Worker["StartedUnixMs"])
        Worker["HostNonce"] = "b" * 32
        Worker["Port"] = 49960
        with self.assertRaisesRegex(ValueError, "stale or mismatched"):
            Tunnel.RequireReverseProof(Worker, Host, STAGE, Worker["StartedUnixMs"])
        Worker["Port"] = 49961
        with self.assertRaisesRegex(ValueError, "stale or mismatched"):
            Tunnel.RequireReverseProof(Worker, Host, STAGE, Worker["StartedUnixMs"] + 1)

    def test_cleanup_terminates_owned_session_and_checks_both_listeners(self):
        with tempfile.TemporaryDirectory() as Temporary:
            Session = Tunnel.TunnelSession(SETUP, STAGE, Temporary)
            State = {"running": True, "terminated": False}

            class Process:
                pid = 731

                def poll(self):
                    return None if State["running"] else 0

                def terminate(self):
                    State["running"] = False
                    State["terminated"] = True

                def wait(self, timeout):
                    return 0

            Session.Process = Process()
            with patch.object(Tunnel, "FreeLocalPort") as Local, \
                    patch.object(Tunnel, "WorkerReverseListener", return_value={"Present": False}):
                Session.Stop()
            self.assertTrue(State["terminated"])
            Local.assert_called_with(49964)
            self.assertTrue(json.loads((Path(Temporary) / "tunnel-cleanup.json").read_text())
                            ["BothListenersClear"])

    def test_lifecycle_abort_and_timeout_both_close_tunnel(self):
        for Failure in (RuntimeError("lifecycle abort"), TimeoutError("registration timeout")):
            with self.subTest(Failure=type(Failure).__name__), tempfile.TemporaryDirectory() as Temporary:
                Session = Tunnel.TunnelSession(SETUP, STAGE, Temporary)
                State = {"Running": True}

                class Process:
                    pid = 901

                    def poll(self):
                        return None if State["Running"] else 0

                    def terminate(self):
                        State["Running"] = False

                    def wait(self, timeout):
                        return 0

                with patch.object(Tunnel, "FreeLocalPort"), \
                        patch.object(Tunnel, "WorkerReverseListener", side_effect=[
                            {"Present": False}, {"Present": True}, {"Present": False}]), \
                        patch.object(Tunnel.subprocess, "Popen", return_value=Process()):
                    with self.assertRaises(type(Failure)):
                        with Session:
                            raise Failure
                self.assertFalse(State["Running"])
                self.assertTrue(json.loads((Path(Temporary) / "tunnel-cleanup.json").read_text())
                                ["BothListenersClear"])

    def test_pre_assignment_failure_stops_fixed_worker_broker(self):
        Code = """
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import run_one_client_lifecycle as Lifecycle
Stage = {'Label': '0123456789abcdef', 'RunId': '12345678-1234-1234-1234-123456789abc'}
with patch.object(Lifecycle, 'ReadStage', return_value=({}, Stage)), \\
     patch.object(Lifecycle, 'VerifyReady', side_effect=ValueError('stale evidence')), \\
     patch.object(Lifecycle, 'StopWorkerBroker', return_value=SimpleNamespace(returncode=0)) as Stop, \\
     patch.object(Lifecycle, 'Assignments') as Assign:
    try:
        Lifecycle.Main(Path('unused'))
    except ValueError as Error:
        assert str(Error) == 'stale evidence'
    else:
        raise AssertionError('pre-assignment failure was accepted')
Stop.assert_called_once_with(Stage)
Assign.assert_not_called()
print('PASS')
"""
        Result = subprocess.run([sys.executable, "-B", "-c", Code], cwd=ROOT,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(Result.returncode, 0, Result.stderr)
        self.assertEqual(Result.stdout.strip(), "PASS")


if __name__ == "__main__":
    unittest.main()
