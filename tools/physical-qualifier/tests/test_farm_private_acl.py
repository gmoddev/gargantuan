"""Real filesystem ACL regression for one-run Farm32 ticket roots."""

import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


SOURCE = Path(__file__).resolve().parents[1] / "private_ticket_acl.py"
SPEC = importlib.util.spec_from_file_location("private_ticket_acl", SOURCE)
ACL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ACL)


class PrivateTicketAclTests(unittest.TestCase):
    def test_private_root_rejects_inherited_or_added_readers(self):
        with tempfile.TemporaryDirectory(prefix="farm32-acl-") as Temporary:
            Root = Path(Temporary) / "private"
            Root.mkdir(mode=0o755)
            with self.assertRaises(ValueError):
                ACL.AssertPrivate(Root)
            ACL.Harden(Root)
            ACL.AssertPrivate(Root)
            (Root / "identity.json").write_text("test-only", encoding="utf-8")
            if os.name == "nt":
                subprocess.run(["icacls.exe", str(Root), "/grant", "*S-1-5-11:(OI)(CI)R"],
                               check=True, capture_output=True, timeout=15,
                               creationflags=subprocess.CREATE_NO_WINDOW)
            else:
                Root.chmod(0o755)
            with self.assertRaises(ValueError):
                ACL.AssertPrivate(Root)

    @unittest.skipUnless(os.name == "nt", "Windows ACL owner")
    def test_harden_assigns_exact_current_user_owner(self):
        with tempfile.TemporaryDirectory(prefix="farm32-acl-owner-") as Temporary:
            Root = Path(Temporary) / "private"
            Root.mkdir()
            Sid = ACL.UserSid()
            with mock.patch.object(ACL, "Run", wraps=ACL.Run) as Calls:
                ACL.Harden(Root)
            self.assertTrue(any(Invocation.args[0] ==
                                ["icacls.exe", str(Root.resolve(strict=True)), "/setowner", "*" + Sid]
                                for Invocation in Calls.call_args_list))
            ACL.AssertPrivate(Root)

    @unittest.skipUnless(os.name == "nt", "Windows ACL owner")
    def test_owner_drift_remains_a_fail_closed_error(self):
        with tempfile.TemporaryDirectory(prefix="farm32-acl-owner-drift-") as Temporary:
            Root = Path(Temporary) / "private"
            Root.mkdir()
            ACL.Harden(Root)
            RealRun = ACL.Run

            def OwnerDrift(Arguments, Environment=None):
                if Arguments[0] == "pwsh.exe":
                    raise subprocess.CalledProcessError(1, Arguments,
                                                        stderr="private ticket owner changed")
                return RealRun(Arguments, Environment)

            with mock.patch.object(ACL, "Run", side_effect=OwnerDrift):
                with self.assertRaisesRegex(ValueError, "private ticket owner changed"):
                    ACL.AssertPrivate(Root)


if __name__ == "__main__":
    unittest.main()
