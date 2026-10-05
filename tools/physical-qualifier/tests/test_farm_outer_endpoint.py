"""Source-only tests of the fixed Farm32 endpoint staging boundary."""

import hashlib
import io
import json
import importlib.util
import os
from contextlib import redirect_stdout
from pathlib import Path
import sys
import subprocess
import tempfile
import threading
import time
import unittest
import uuid
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import farm_outer_endpoint as Endpoint  # noqa: E402


def Save(File, Value):
    File.write_text(json.dumps(Value), encoding="utf-8")
    return File


def Hash(File):
    return hashlib.sha256(File.read_bytes()).hexdigest()


class FarmOuterEndpointTests(unittest.TestCase):
    def setUp(self):
        if os.name == "nt":
            Source = os.environ.get("GARGANTUAN_TEST_OWNED_SOURCE")
            if Source:
                # Explicit author-test snapshot only; production still requires lock exports.
                Library = Path(Source) / "agent_coordinator"
                Spec = importlib.util.spec_from_file_location("_owned_test_snapshot", Library / "__init__.py",
                                                              submodule_search_locations=[str(Library)])
                Package = importlib.util.module_from_spec(Spec)
                sys.modules[Spec.name] = Package
                Spec.loader.exec_module(Package)
                Item = importlib.import_module("_owned_test_snapshot.owned_process")
                Patch = mock.patch.object(Endpoint, "StartOwned", side_effect=Item.OwnedProcess.Start)
                Patch.start()
                self.addCleanup(Patch.stop)
        self.Temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.Temporary.cleanup)
        self.Root = Path(self.Temporary.name) / "stage"
        Endpoint.Prepare(self.Root)
        self.RunId = str(uuid.uuid4())
        self.Runner = self.Root / "farm_campaign_runner.py"
        self.Runner.write_text("print('fixed mock')\n", encoding="utf-8")
        self.Ticket = Save(self.Root / "ticket.json", {
            "Format": "GargantuanFarm32Campaign", "Version": 1, "Role": "CLIENT",
            "RunId": self.RunId,
        })
        self.Index = Save(self.Root / "index.json", {"Format": "GargantuanFarm32StageIndex",
            "Version": 1, "Files": [{"Name": File.name, "Sha256": Hash(File)}
                                  for File in (self.Runner, self.Ticket)]})
        self.Launch = Save(self.Root / "launch.json", {
            "Format": "GargantuanFarm32FixedLaunch", "Version": 1, "Action": "role",
            "RunnerName": self.Runner.name, "RunnerSha256": Hash(self.Runner),
            "InputName": self.Ticket.name, "InputSha256": Hash(self.Ticket),
        })

    def test_private_index_hash_and_fixed_runner(self):
        Endpoint.Verify(self.Root, self.Index)
        with mock.patch.object(Endpoint, "MAX_SECONDS", 2):
            Endpoint.Run(self.Root, self.Index, self.Launch, "role")
        self.assertTrue((self.Root / "role.stdout.log").is_file())
        self.assertRaisesRegex(ValueError, "runner is not fixed", self.InvalidRunner)

    def InvalidRunner(self):
        Alternate = self.Root / "other.py"
        Alternate.write_text("print('other')\n", encoding="utf-8")
        Index = json.loads(self.Index.read_text())
        Index["Files"].append({"Name": Alternate.name, "Sha256": Hash(Alternate)})
        Save(self.Index, Index)
        Row = json.loads(self.Launch.read_text())
        Row["RunnerName"] = Alternate.name
        Row["RunnerSha256"] = Hash(Alternate)
        Save(self.Launch, Row)
        Endpoint.Run(self.Root, self.Index, self.Launch, "role")

    def test_mismatch_and_unlisted_path_fail_closed(self):
        self.Ticket.write_text("changed", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "stage member hash mismatch"):
            Endpoint.Verify(self.Root, self.Index)
        Row = json.loads(self.Index.read_text())
        Row["Files"][1]["Name"] = "../ticket.json"
        Save(self.Index, Row)
        with self.assertRaisesRegex(ValueError, "invalid stage member"):
            Endpoint.Verify(self.Root, self.Index)

    def test_fails_inherited_acl_and_reused_stage_root(self):
        with self.assertRaisesRegex(ValueError, "already exists"):
            Endpoint.Prepare(self.Root)
        if Endpoint.os.name != "nt":
            self.Root.chmod(0o755)
            with self.assertRaises(ValueError):
                Endpoint.Verify(self.Root, self.Index)

    def test_prepare_requires_existing_parent_and_rejects_reparse_ancestry(self):
        Base = Path(self.Temporary.name).resolve(strict=True)
        with self.assertRaisesRegex(ValueError, "already exists"):
            Endpoint.Prepare(Base / "absent-parent" / "child")
        self.assertFalse((Base / "absent-parent").exists())
        Target = Base / "target"
        Target.mkdir()
        Link = Base / "redirect"
        if Endpoint.os.name == "nt":
            Result = subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(Link), str(Target)],
                                    capture_output=True, text=True, timeout=10,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
            self.assertEqual(Result.returncode, 0, Result.stderr)
        else:
            Link.symlink_to(Target, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "reparse"):
            Endpoint.Prepare(Link / "child")
        self.assertFalse((Target / "child").exists())
        Link.rmdir() if Endpoint.os.name == "nt" else Link.unlink()

    def test_one_run_node_token_is_private_unique_and_retired(self):
        RunRoot = Path(self.Temporary.name) / self.RunId
        Endpoint.Prepare(RunRoot)
        Output = io.StringIO()
        with redirect_stdout(Output):
            Endpoint.NewNodeToken(RunRoot)
        TokenFile = RunRoot / "node-token.secret"
        Token = TokenFile.read_text(encoding="ascii")
        self.assertRegex(Token, r"^[0-9a-f]{64}$")
        self.assertNotIn(Token, Output.getvalue())
        with self.assertRaises(FileExistsError):
            Endpoint.NewNodeToken(RunRoot)
        Endpoint.RetireNodeToken(RunRoot)

        for Name in ("node-key.pem", "node-cert.pem", "node-root-ca.pem"):
            (RunRoot / Name).write_text("bounded mock", encoding="ascii")
        Endpoint.RetireNodeTls(RunRoot)
        self.assertFalse(any((RunRoot / Name).exists() for Name in
                             ("node-key.pem", "node-cert.pem", "node-root-ca.pem")))
        Endpoint.RetireNodeTls(RunRoot)
        self.assertFalse(TokenFile.exists())
        Endpoint.RetireNodeToken(RunRoot)

    def RunInThread(self):
        Errors = []
        def Target():
            try:
                Endpoint.Run(self.Root, self.Index, self.Launch, "role")
            except (ValueError, RuntimeError, TimeoutError, OSError) as Error:
                Errors.append(Error)
        Thread = threading.Thread(target=Target)
        Thread.start()
        return Thread, Errors

    def WaitForLog(self):
        Deadline = time.monotonic() + 5
        while not (self.Root / "role.stdout.log").is_file():
            if time.monotonic() >= Deadline:
                self.fail("bounded mock child did not start")
            time.sleep(0.01)

    def SetSleepingRunner(self):
        self.Runner.write_text("import time\nprint('started', flush=True)\ntime.sleep(30)\n",
                               encoding="utf-8")
        Index = json.loads(self.Index.read_text())
        Index["Files"][0]["Sha256"] = Hash(self.Runner)
        Save(self.Index, Index)
        Launch = json.loads(self.Launch.read_text())
        Launch["RunnerSha256"] = Hash(self.Runner)
        Save(self.Launch, Launch)

    def test_run_bound_abort_reaps_owned_child(self):
        self.SetSleepingRunner()
        Thread, Errors = self.RunInThread()
        self.WaitForLog()
        Endpoint.Abort(self.Root, self.Index, "role")
        Thread.join(timeout=12)
        self.assertFalse(Thread.is_alive())
        self.assertEqual(1, len(Errors))
        self.assertIn("abort requested", str(Errors[0]))
        Terminal = json.loads((self.Root / "role.terminal.json").read_text())
        self.assertEqual(self.RunId, Terminal["RunId"])
        self.assertEqual("ABORTED", Terminal["Outcome"])
        self.assertTrue(Terminal["ChildTreeReaped"])

    def test_run_timeout_reaps_owned_child(self):
        self.SetSleepingRunner()
        with mock.patch.object(Endpoint, "MAX_SECONDS", 0.05):
            Thread, Errors = self.RunInThread()
            Thread.join(timeout=12)
        self.assertFalse(Thread.is_alive())
        self.assertEqual(1, len(Errors))
        self.assertIn("time or log bound", str(Errors[0]))
        Terminal = json.loads((self.Root / "role.terminal.json").read_text())
        self.assertEqual("BOUND_EXCEEDED", Terminal["Outcome"])
        self.assertTrue(Terminal["ChildTreeReaped"])

    def test_failed_runner_keeps_failure_independently_of_checked_tree(self):
        self.Runner.write_text("raise RuntimeError('mock failure')\n", encoding="utf-8")
        Index = json.loads(self.Index.read_text())
        Index["Files"][0]["Sha256"] = Hash(self.Runner)
        Save(self.Index, Index)
        Launch = json.loads(self.Launch.read_text())
        Launch["RunnerSha256"] = Hash(self.Runner)
        Save(self.Launch, Launch)
        with self.assertRaisesRegex(RuntimeError, "pinned role failed" if os.name == "nt" else "child tree was not reaped"):
            Endpoint.Run(self.Root, self.Index, self.Launch, "role")
        Terminal = json.loads((self.Root / "role.terminal.json").read_text())
        self.assertEqual("FAILED", Terminal["Outcome"])
        self.assertEqual(Terminal["ChildTreeReaped"], os.name == "nt")
        self.assertEqual(Terminal["ChildExitCode"], 1)

    def test_unexported_owned_primitive_cannot_launch(self):
        with mock.patch("dependency.GetRoot", return_value=self.Root), \
                mock.patch.object(Endpoint, "ReadJson", return_value={"Files": {}}):
            # Call the real source function, even when other tests inject the fixture.
            Module = importlib.util.spec_from_file_location("_outer_unexported", ROOT / "farm_outer_endpoint.py")
            Instance = importlib.util.module_from_spec(Module)
            Module.loader.exec_module(Instance)
            with mock.patch.object(Instance, "ReadJson", return_value={"Files": {}}):
                with self.assertRaisesRegex(ValueError, "source export is not pinned"):
                    Instance.StartOwned(["unused"], self.Root)

    def test_owned_output_is_bounded_while_continuing_to_drain(self):
        Output = io.BytesIO()
        Errors = []
        with mock.patch.object(Endpoint, "MAX_LOG", 20):
            Endpoint.DrainOwned(io.BytesIO(b"x" * 200000), Output, Errors)
        self.assertEqual(len(Output.getvalue()), 21)
        self.assertEqual(Errors, ["owned process log bound"])

    @unittest.skipUnless(os.name == "nt", "Windows persistent Job")
    def test_failed_exited_runner_descendant_is_reaped(self):
        self.Runner.write_text("import subprocess,sys\n"
            "subprocess.Popen([sys.executable,'-I','-c','import time;time.sleep(120)'])\n"
            "raise SystemExit(7)\n", encoding="utf-8")
        Index = json.loads(self.Index.read_text())
        Index["Files"][0]["Sha256"] = Hash(self.Runner)
        Save(self.Index, Index)
        Launch = json.loads(self.Launch.read_text())
        Launch["RunnerSha256"] = Hash(self.Runner)
        Save(self.Launch, Launch)
        with self.assertRaisesRegex(RuntimeError, "pinned role failed"):
            Endpoint.Run(self.Root, self.Index, self.Launch, "role")
        Row = json.loads((self.Root / "role.terminal.json").read_text())
        self.assertEqual(Row["ChildExitCode"], 7)
        self.assertEqual(Row["Outcome"], "FAILED")
        self.assertTrue(Row["ChildTreeReaped"])

    @unittest.skipUnless(os.name == "nt", "Windows persistent Job")
    def test_fast_success_exit_cannot_hide_late_oversized_output(self):
        self.Runner.write_text("import sys\nsys.stdout.write('o'*1025)\nsys.stderr.write('e'*1025)\n", encoding="utf-8")
        Index = json.loads(self.Index.read_text())
        Index["Files"][0]["Sha256"] = Hash(self.Runner)
        Save(self.Index, Index)
        Launch = json.loads(self.Launch.read_text())
        Launch["RunnerSha256"] = Hash(self.Runner)
        Save(self.Launch, Launch)
        with mock.patch.object(Endpoint, "MAX_LOG", 1024):
            with self.assertRaisesRegex((RuntimeError, TimeoutError), "output failed|log bound"):
                Endpoint.Run(self.Root, self.Index, self.Launch, "role")
        Row = json.loads((self.Root / "role.terminal.json").read_text())
        self.assertEqual(Row["Outcome"], "BOUND_EXCEEDED")
        self.assertTrue(Row["ChildTreeReaped"])
        self.assertLessEqual((self.Root / "role.stdout.log").stat().st_size, 1025)
        self.assertLessEqual((self.Root / "role.stderr.log").stat().st_size, 1025)

    @unittest.skipUnless(os.name == "nt", "Windows persistent Job")
    def test_cleanup_query_failure_still_preserves_false_terminal(self):
        Start = Endpoint.StartOwned.side_effect if isinstance(Endpoint.StartOwned, mock.Mock) else Endpoint.StartOwned
        def Denied(Args, Directory):
            Process, Tree = Start(Args, Directory)
            Close = Tree.Close
            def Fail():
                Close()
                raise OSError("member query denied")
            Tree.Close = Fail
            return Process, Tree
        with mock.patch.object(Endpoint, "StartOwned", side_effect=Denied):
            with self.assertRaisesRegex(RuntimeError, "child tree was not reaped"):
                Endpoint.Run(self.Root, self.Index, self.Launch, "role")
        Row = json.loads((self.Root / "role.terminal.json").read_text())
        self.assertFalse(Row["ChildTreeReaped"])
        Diagnostic = json.loads((self.Root / "role.terminal-diagnostic.json").read_text())
        self.assertEqual(Diagnostic["CleanupError"], "member query denied")

    @unittest.skipUnless(os.name == "nt", "Windows persistent Job")
    def test_cleanup_wait_timeout_retains_failure_with_unknown_exit(self):
        self.Runner.write_text("raise SystemExit(7)\n", encoding="utf-8")
        Index = json.loads(self.Index.read_text())
        Index["Files"][0]["Sha256"] = Hash(self.Runner)
        Save(self.Index, Index)
        Launch = json.loads(self.Launch.read_text())
        Launch["RunnerSha256"] = Hash(self.Runner)
        Save(self.Launch, Launch)
        Start = Endpoint.StartOwned.side_effect if isinstance(Endpoint.StartOwned, mock.Mock) else Endpoint.StartOwned
        def Exhausted(Args, Directory):
            Process, Tree = Start(Args, Directory)
            Close = Tree.Close
            def Fail():
                Close()  # Actual harmless children are reaped before the supplied proof failure.
                Process.returncode = None  # The failed deadline did not establish root exit truth.
                raise subprocess.TimeoutExpired("owned root wait", 0)
            Tree.Close = Fail
            return Process, Tree
        with mock.patch.object(Endpoint, "StartOwned", side_effect=Exhausted):
            with self.assertRaisesRegex(RuntimeError, "child tree was not reaped"):
                Endpoint.Run(self.Root, self.Index, self.Launch, "role")
        Row = json.loads((self.Root / "role.terminal.json").read_text())
        self.assertFalse(Row["ChildTreeReaped"])
        self.assertEqual(Row["Outcome"], "FAILED")
        self.assertIsNone(Row["ChildExitCode"])
        Diagnostic = json.loads((self.Root / "role.terminal-diagnostic.json").read_text())
        self.assertIn("timed out", Diagnostic["CleanupError"])
        self.assertLessEqual(len(Diagnostic["CleanupError"]), 512)


if __name__ == "__main__":
    unittest.main()
