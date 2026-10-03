import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest import mock

Spec = importlib.util.spec_from_file_location("qualifier_dependency", Path(__file__).parents[1] / "dependency.py")
Dependency = importlib.util.module_from_spec(Spec)
Spec.loader.exec_module(Dependency)


class DependencyTests(unittest.TestCase):
    def test_current_pin_verifies(self):
        self.assertTrue((Dependency.GetRoot() / "agent_coordinator/legacy.py").is_file())

    def test_missing_or_modified_library_fails_closed(self):
        with tempfile.TemporaryDirectory() as Directory:
            Root = Path(Directory)
            Source = Path(__file__).parents[1]
            shutil.copyfile(Source / "upstream.lock.json", Root / "upstream.lock.json")
            with mock.patch.object(Dependency, "__file__", str(Root / "dependency.py")):
                with self.assertRaises(ValueError):
                    Dependency.GetRoot()
                for Name in json.loads((Source / "upstream.lock.json").read_text())["Files"]:
                    Target = Root / ".agent-coordinator" / Name
                    Target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(Source / ".agent-coordinator" / Name, Target)
                self.assertEqual(Root / ".agent-coordinator", Dependency.GetRoot())
                (Root / ".agent-coordinator/agent_coordinator/legacy.py").write_text("# changed")
                with self.assertRaises(ValueError):
                    Dependency.GetRoot()
