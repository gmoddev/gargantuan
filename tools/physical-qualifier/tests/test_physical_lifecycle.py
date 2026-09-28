"""Exercise the installed physical catalog with harmless local stand-ins."""

import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / ".agent-coordinator"))
sys.path.insert(0, str(ROOT))
import physical_qualifier_lifecycle as Physical
from agent_coordinator.workflow import Operation, Workflow


FAKE = '''import json, pathlib, sys, time
mode, config = sys.argv[1:]
item = json.loads(pathlib.Path(config).read_text())
root = pathlib.Path(item["EvidenceDir"])
root.mkdir(parents=True, exist_ok=False)
with (root / "control.jsonl").open("w") as out:
    out.write(json.dumps({"Event": "LISTENING" if mode == "coordinator" else "SEND",
                          "Type": "STAGE_READY"}) + "\\n")
    out.flush()
    time.sleep(0.1)
(root / "result.json").write_text(json.dumps({"Success": True}))
'''


class PhysicalLifecycleTests(unittest.TestCase):
    def test_worker_broker_request_waits_for_pinned_result(self):
        with tempfile.TemporaryDirectory() as Temporary:
            Root = Path(Temporary)
            (Root / ".lifecycle").mkdir()
            Label = "0123456789abcdef"
            RunId = "12345678-1234-1234-1234-123456789abc"
            Evidence = Root / "worker-evidence"
            Evidence.mkdir()
            ResultFile = Evidence / "result.json"
            ConfigFile = Root / "server.json"
            ConfigFile.write_text(json.dumps({"EvidenceDir": str(Evidence)}), encoding="utf-8")
            Config = {"Role": "SERVER", "BrokerLabel": Label,
                      "RunId": RunId, "Endpoint": str(ConfigFile)}
            Response = Root / ".lifecycle" / ("physical-broker-" + Label + ".start-result.json")
            Request = Root / ".lifecycle" / ("physical-broker-" + Label + ".start.json")

            def Finish():
                while not Request.exists():
                    time.sleep(0.01)
                self.assertEqual(json.loads(Request.read_text()),
                                 {"RunId": RunId, "Action": "START"})
                ResultFile.write_text(json.dumps({"Success": True}), encoding="utf-8")
                Response.write_text(json.dumps({"RunId": RunId, "ReturnCode": 0}),
                                    encoding="utf-8")

            Previous = Path.cwd()
            os.chdir(Root)
            try:
                Thread = threading.Thread(target=Finish)
                Thread.start()
                with patch.object(Physical, "Settings", return_value=Config):
                    Result = Physical.ServerRun({}, Operation(time.monotonic() + 2))
                Thread.join(2)
                self.assertFalse(Thread.is_alive())
                self.assertTrue(Result["Success"])
                self.assertEqual(Result["Evidence"][0]["Path"], str(ResultFile))
            finally:
                os.chdir(Previous)

    def test_fixed_catalog_process_lifecycle(self):
        with tempfile.TemporaryDirectory() as Temporary:
            Root = Path(Temporary)
            (Root / ".lifecycle").mkdir()
            Tool = Root / "qualifier.py"
            Tool.write_text(FAKE, encoding="utf-8")
            Configs = {}
            for Name in ("Coordinator", "Client", "Server"):
                File = Root / (Name + ".json")
                File.write_text(json.dumps({"EvidenceDir": str(Root / (Name + "-evidence"))}), encoding="utf-8")
                Configs[Name] = File
            Shared = {"Python": sys.executable, "Tool": str(Tool), "RunId": "test"}
            Client = {**Shared, "Role": "CLIENT", "Coordinator": str(Configs["Coordinator"]),
                      "Endpoint": str(Configs["Client"])}
            Server = {**Shared, "Role": "SERVER", "Coordinator": "", "Endpoint": str(Configs["Server"])}
            Previous = Path.cwd()
            os.chdir(Root)
            try:
                with patch.object(Physical, "Settings", return_value=Client):
                    self.assertTrue(Physical.ClientStart({}, Operation(time.monotonic() + 5))["Success"])
                ClientProcesses, ClientStreams = Physical.Processes, Physical.Streams
                Physical.Processes, Physical.Streams = {}, {}
                try:
                    with patch.object(Physical, "Settings", return_value=Server):
                        Result = Physical.ServerRun({}, Operation(time.monotonic() + 5))
                        self.assertTrue(Result["Success"])
                        self.assertEqual(len(Result["Evidence"]), 1)
                    Physical.Cleanup()
                finally:
                    Physical.Processes, Physical.Streams = ClientProcesses, ClientStreams
                with patch.object(Physical, "Settings", return_value=Client):
                    Result = Physical.ClientResult({}, Operation(time.monotonic() + 5))
                    self.assertTrue(Result["Success"])
                    self.assertEqual(len(Result["Evidence"]), 2)
                Workflow(json.loads((ROOT / "workflows" / "one-client-lifecycle.json").read_text()))
            finally:
                Physical.Cleanup()
                Physical.Processes.clear()
                Physical.Streams.clear()
                os.chdir(Previous)


if __name__ == "__main__":
    unittest.main()
