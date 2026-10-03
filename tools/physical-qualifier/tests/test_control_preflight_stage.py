"""Keep the no-probe lifecycle stage distinct from physical Phase 1."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / ".agent-coordinator"))
from agent_coordinator.workflow import Workflow


class ControlPreflightStageTests(unittest.TestCase):
    def test_interactive_control_stage_requires_control_workflow_and_both_adapters(self):
        with tempfile.TemporaryDirectory() as Temporary:
            Artifact = Path(Temporary)
            RunId = "12345678-1234-1234-1234-123456789abc"
            Label = "0123456789abcdef"
            WorkflowFile = ROOT / "workflows" / "four-client-control-preflight-lifecycle.json"
            WorkflowItem = Workflow(json.loads(WorkflowFile.read_text(encoding="utf-8")))
            ClientConfig = Artifact / "client-daemon.json"
            ClientConfig.write_text(json.dumps({"Profile": "physical-qualification-interactive",
                                                "EndpointId": "CLIENT"}), encoding="utf-8")
            (Artifact / "server-daemon.json").write_text(json.dumps({
                "Profile": "physical-qualification-interactive", "EndpointId": "SERVER"}), encoding="utf-8")
            (Artifact / "client-physical.json").write_text(json.dumps({
                "Role": "CLIENT", "ControlOnly": True}), encoding="utf-8")
            WorkerPhysical = Artifact / "server-physical.json"
            WorkerPhysical.write_text(json.dumps({
                "Role": "SERVER", "ControlOnly": True, "RunId": RunId}), encoding="utf-8")
            (Artifact / "setup.json").write_text(json.dumps({
                "WorkflowFile": str(WorkflowFile)}), encoding="utf-8")
            Stage = {"QualificationProfile": "PHYSICAL_QUALIFICATION_INTERACTIVE",
                     "ControlPreflight": True, "RunId": RunId, "Label": Label,
                     "Physical": (r"C:\Sandbox\Codex\Evidence\physical-qualifier\lifecycle-" + Label),
                     "ClientConfig": str(ClientConfig), "WorkflowHash": WorkflowItem.Hash}
            StageFile = Artifact / "stage.json"
            StageFile.write_text(json.dumps(Stage), encoding="utf-8")
            Command = [sys.executable, "-c", "import sys; from pathlib import Path; "
                       "import run_one_client_lifecycle as L; L.ReadStage(Path(sys.argv[1]))",
                       str(Artifact)]
            def ReadStage():
                return subprocess.run(Command, cwd=ROOT, capture_output=True,
                                      text=True, timeout=10)
            self.assertEqual(0, ReadStage().returncode)
            Stage["QualificationProfile"] = "RESTRICTED"
            StageFile.write_text(json.dumps(Stage), encoding="utf-8")
            self.assertIn("interactive child profile", ReadStage().stderr)
            Stage["QualificationProfile"] = "PHYSICAL_QUALIFICATION_INTERACTIVE"
            StageFile.write_text(json.dumps(Stage), encoding="utf-8")
            WorkerPhysical.write_text(json.dumps({"Role": "SERVER", "RunId": RunId}), encoding="utf-8")
            self.assertIn("restricted endpoint", ReadStage().stderr)
            WorkerPhysical.write_text(json.dumps({
                "Role": "SERVER", "ControlOnly": True, "RunId": RunId}), encoding="utf-8")
            Stage["WorkflowHash"] = "0" * 64
            StageFile.write_text(json.dumps(Stage), encoding="utf-8")
            self.assertIn("control preflight mode", ReadStage().stderr)


if __name__ == "__main__":
    unittest.main()
