import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest import mock

Tool = Path(__file__).parents[1]
sys.path.insert(0, str(Tool))
import f1_candidate_source as S
import qualifier as Q


class CandidateSourceTests(unittest.TestCase):
    def setUp(self):
        self.Temp = tempfile.TemporaryDirectory()
        self.Root = Path(self.Temp.name) / "repo"
        self.Root.mkdir()
        S.Git(self.Root, "init", "-q")
        Email = subprocess.run(["git", "config", "--get", "user.email"], cwd=Tool,
                               capture_output=True, text=True).stdout.strip() or "fixture@example.invalid"
        S.Git(self.Root, "config", "user.email", Email)
        S.Git(self.Root, "config", "user.name", "Source Fixture")
        S.Git(self.Root, "config", "core.autocrlf", "false")
        S.Git(self.Root, "config", "commit.gpgsign", "false")
        for Name in ("CMakeLists.txt", "cmake/gns/AckDiagnostics.hpp", "cmake/gns/PromptAckWireBudget.hpp",
                     "src/network/GameNetworkingSocketsTransport.cpp", "assets/runtime/DefaultActionMap.luau"):
            File = self.Root / Name
            File.parent.mkdir(parents=True, exist_ok=True)
            File.write_bytes((Name + "\n").encode())
        S.Git(self.Root, "add", ".")
        S.Git(self.Root, "commit", "-qm", "fixture")
        self.Head = S.Git(self.Root, "rev-parse", "HEAD").decode().strip()
        self.Archive = Path(self.Temp.name) / "candidate.zip"
        S.Create(self.Root, self.Head, self.Archive)

    def tearDown(self):
        self.Temp.cleanup()

    def Rewrite(self, Transform):
        with zipfile.ZipFile(self.Archive) as Archive:
            Entries = {Name: Archive.read(Name) for Name in Archive.namelist()}
        Transform(Entries)
        with zipfile.ZipFile(self.Archive, "w") as Archive:
            for Name in sorted(Entries):
                Archive.writestr(Name, Entries[Name])

    def test_deterministic_full_owned_tree_and_exclusive_output(self):
        Second = Path(self.Temp.name) / "again.zip"
        S.Create(self.Root, self.Head, Second)
        self.assertEqual(self.Archive.read_bytes(), Second.read_bytes())
        with self.assertRaises(FileExistsError):
            S.Create(self.Root, self.Head, Second)
        Manifest = S.VerifyPinned(self.Root, self.Archive, self.Head)
        self.assertIn("cmake/gns/PromptAckWireBudget.hpp", [F["Path"] for F in Manifest["Files"]])
        S.VerifyTree(self.Root, self.Archive, self.Head)

    def test_expected_commit_is_external_not_archive_claim(self):
        with self.assertRaisesRegex(ValueError, "revision"):
            S.VerifyArchive(self.Archive, "1" * 40)
        for Value in ("HEAD", "--all", "a" * 39):
            with self.assertRaises(ValueError):
                S.ReadRevision(self.Root, Value)

    def test_extra_omitted_and_changed_archive_files_reject(self):
        for Transform in (lambda E: E.update({"extra.txt": b"extra"}),
                          lambda E: E.pop("CMakeLists.txt"),
                          lambda E: E.update({"CMakeLists.txt": b"changed"})):
            Original = self.Archive.read_bytes()
            self.Rewrite(Transform)
            with self.assertRaises((ValueError, KeyError)):
                S.VerifyArchive(self.Archive, self.Head)
            self.Archive.write_bytes(Original)

    def test_consistent_omission_still_fails_git_inventory_join(self):
        def Omit(Entries):
            Manifest = json.loads(Entries[S.INVENTORY])
            Manifest["Files"] = [F for F in Manifest["Files"] if F["Path"] != "CMakeLists.txt"]
            Entries[S.INVENTORY] = S.Canonical(Manifest)
            del Entries["CMakeLists.txt"]
        self.Rewrite(Omit)
        with self.assertRaisesRegex(ValueError, "inventory"):
            S.VerifyPinned(self.Root, self.Archive, self.Head)

    def test_path_redirects_and_unreviewed_roots_reject(self):
        for Name in ("../escape", "src/../escape", "C:/escape", "src\\escape", "src//x", "src/x.", "src/NUL.cpp", "new-root/x"):
            with self.assertRaises(ValueError):
                S.SafePath(Name)
        Blob = S.Git(self.Root, "hash-object", "-w", "--stdin", Input=b"../redirect").decode().strip()
        S.Git(self.Root, "update-index", "--add", "--cacheinfo", "120000," + Blob + ",src/redirect")
        S.Git(self.Root, "commit", "-qm", "unsafe redirect")
        Head = S.Git(self.Root, "rev-parse", "HEAD").decode().strip()
        with self.assertRaisesRegex(ValueError, "redirect"):
            S.ReadRevision(self.Root, Head)

    def test_tree_changed_bytes_and_stale_head_reject(self):
        File = self.Root / "CMakeLists.txt"
        File.write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "source mismatch"):
            S.VerifyTree(self.Root, self.Archive, self.Head)
        S.Git(self.Root, "add", ".")
        S.Git(self.Root, "commit", "-qm", "new head")
        with self.assertRaisesRegex(ValueError, "HEAD"):
            S.VerifyTree(self.Root, self.Archive, self.Head)

    def test_gitlink_pin_is_recorded_and_stale_pin_rejected(self):
        S.Git(self.Root, "update-index", "--add", "--cacheinfo", "160000," + self.Head + ",vendor/example")
        S.Git(self.Root, "commit", "-qm", "pin dependency")
        Head = S.Git(self.Root, "rev-parse", "HEAD").decode().strip()
        Pinned = Path(self.Temp.name) / "submodule.zip"
        S.Create(self.Root, Head, Pinned)
        self.assertEqual(self.Head, S.VerifyPinned(self.Root, Pinned, Head)["Submodules"]["vendor/example"])
        self.Archive = Pinned
        def Mutate(Entries):
            Manifest = json.loads(Entries[S.INVENTORY])
            Manifest["Submodules"]["vendor/example"] = "2" * 40
            Entries[S.INVENTORY] = S.Canonical(Manifest)
        self.Rewrite(Mutate)
        with self.assertRaisesRegex(ValueError, "inventory"):
            S.VerifyPinned(self.Root, Pinned, Head)

    def test_format_dispatch_requires_intentional_new_pins(self):
        Manifest = {"SourceArchiveFormat": S.FORMAT, "BaseHead": self.Head}
        with self.assertRaisesRegex(ValueError, "pin mismatch"):
            Q.CheckPhase1SourceArchive(Manifest, self.Archive)
        with mock.patch.object(Q, "PHASE1_BASE_HEAD", self.Head), mock.patch.object(Q, "PHASE1_SOURCE_FORMAT", S.FORMAT):
            Q.CheckPhase1SourceArchive(Manifest, self.Archive)
            for Format in ("unknown", "F1_LEGACY_31_V1"):
                with self.assertRaises(ValueError):
                    Q.CheckPhase1SourceArchive({**Manifest, "SourceArchiveFormat": Format}, self.Archive)

    def test_size_bound_is_fail_closed(self):
        with mock.patch.object(S, "MAX_BYTES", 1):
            with self.assertRaises(ValueError):
                S.Create(self.Root, self.Head, Path(self.Temp.name) / "oversize.zip")
            with self.assertRaises(ValueError):
                S.VerifyArchive(self.Archive, self.Head)

    def test_malformed_inventory_and_duplicate_entries_reject(self):
        Original = self.Archive.read_bytes()
        for Data in (b"[]", b"{}", b"{not json}"):
            self.Rewrite(lambda E: E.update({S.INVENTORY: Data}))
            with self.assertRaises(ValueError):
                S.VerifyArchive(self.Archive, self.Head)
            self.Archive.write_bytes(Original)
        with zipfile.ZipFile(self.Archive, "a") as Archive:
            Archive.writestr("CMakeLists.txt", b"duplicate")
        with self.assertRaisesRegex(ValueError, "path list"):
            S.VerifyArchive(self.Archive, self.Head)

    def test_reviewed_documentation_alias_is_data_and_target_is_pinned(self):
        Target = S.ALIASES["AGENTS.md"]
        Blob = S.Git(self.Root, "hash-object", "-w", "--stdin", Input=Target.encode()).decode().strip()
        S.Git(self.Root, "update-index", "--add", "--cacheinfo", "120000," + Blob + ",AGENTS.md")
        S.Git(self.Root, "commit", "-qm", "reviewed alias")
        Head = S.Git(self.Root, "rev-parse", "HEAD").decode().strip()
        Output = Path(self.Temp.name) / "alias.zip"
        S.Create(self.Root, Head, Output)
        S.VerifyPinned(self.Root, Output, Head)
        with zipfile.ZipFile(Output) as Archive:
            self.assertEqual(Target.encode(), Archive.read("AGENTS.md"))
            self.assertNotEqual(0o120000, (Archive.getinfo("AGENTS.md").external_attr >> 16) & 0o170000)
        Bad = S.Git(self.Root, "hash-object", "-w", "--stdin", Input=b"../escape").decode().strip()
        S.Git(self.Root, "update-index", "--add", "--cacheinfo", "120000," + Bad + ",AGENTS.md")
        S.Git(self.Root, "commit", "-qm", "changed alias")
        with self.assertRaisesRegex(ValueError, "alias target"):
            S.ReadRevision(self.Root, S.Git(self.Root, "rev-parse", "HEAD").decode().strip())


if __name__ == "__main__":
    unittest.main()
