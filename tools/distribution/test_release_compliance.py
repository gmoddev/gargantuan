"""Regression checks for recipient source recovery and public CI artifact boundaries."""
import base64
import hashlib
import io
from pathlib import Path
import tempfile
import unittest
import zipfile

from runtime_source import BuildNotice, CheckArchive, ReadArchive
from release_inventory import MicrosoftRuntime

Root = Path(__file__).resolve().parents[2]
Revision = "a" * 40


class ReleaseComplianceTests(unittest.TestCase):
    def test_windows_prerequisite_files(self):
        for Name in ("msvcp140_atomic_wait.dll", "VCRUNTIME140_1.dll", "vccorlib140.dll", "mfc140u.dll",
                     "api-ms-win-crt-string-l1-1-0.dll", "vc_redist.x64.exe", "vcredist_x64.exe"):
            self.assertIsNotNone(MicrosoftRuntime.fullmatch(Name), Name)
        for Name in ("msvcp140-notes.txt", "vc_redist.x64.exe.txt", "game.dll"):
            self.assertIsNone(MicrosoftRuntime.fullmatch(Name), Name)

    def test_recipient_archive_integrity(self):
        License = (Root / "docs/src/content/docs/meta/license.mdx").read_text(encoding="utf-8")
        License = License[License.index("Mozilla Public License Version 2.0"):]
        Buffer = io.BytesIO()
        with zipfile.ZipFile(Buffer, "w") as Archive:
            for Name, Data in {"Readme.txt": Revision, "LICENSE.txt": License, "DependencyRevisions.tsv": "Path\tGit revision\n",
                               "src/main.cpp": "int main() {}", "include/main.hpp": "#pragma once"}.items():
                Archive.writestr(Name, Data)
        Data = Buffer.getvalue()
        with tempfile.TemporaryDirectory() as Temp:
            File = Path(Temp) / "source.zip"
            File.write_bytes(Data)
            Notice = BuildNotice(License, File, Revision)
        self.assertEqual(ReadArchive(Notice, Revision), Data)
        with self.assertRaises(ValueError):
            ReadArchive(Notice.replace(hashlib.sha256(Data).hexdigest(), "0"*64), Revision)
        with self.assertRaises(ValueError):
            ReadArchive(Notice, "b"*40)
        with self.assertRaises(ValueError):
            ReadArchive(Notice.replace(base64.b64encode(Data).decode()[:12], "ABCD"*3), Revision)
        with self.assertRaises((ValueError, zipfile.BadZipFile)):
            CheckArchive(b"source will be provided", Revision)

    def test_actions_publish_named_evidence_files(self):
        # Public CI shares diagnostic evidence. Runtime payloads are delivered
        # by a separate, reviewed release channel. Pages has its own action.
        Reports = {".json", ".txt", ".log", ".xml", ".yaml", ".csv", ".sha256"}
        Count = 0
        for Workflow in (Root / ".github/workflows").glob("*.y*ml"):
            Lines = Workflow.read_text(encoding="utf-8").splitlines()
            for Index, Line in enumerate(Lines):
                if "uses: actions/upload-artifact@" not in Line:
                    continue
                Count += 1
                Step = []
                for Later in Lines[Index+1:]:
                    if Later.strip() and len(Later)-len(Later.lstrip()) < 8:
                        break
                    Step.append(Later)
                matches = [I for I, Entry in enumerate(Step) if Entry.lstrip().startswith("path:")]
                self.assertEqual(len(matches), 1, Workflow)
                Start = matches[0]
                Value = Step[Start].split("path:",1)[1].strip()
                Paths = [Value]
                if Value == "|":
                    Paths = []
                    for Entry in Step[Start+1:]:
                        if not Entry.startswith("            "):
                            break
                        Paths.append(Entry.strip())
                self.assertTrue(Paths, Workflow)
                for File in Paths:
                    self.assertRegex(File, r"^[A-Za-z0-9_./-]+$", Workflow)
                    self.assertNotIn("..", File.split("/"), Workflow)
                    self.assertIn(Path(File).suffix, Reports, f"{Workflow.name}: {File}")
        self.assertGreater(Count, 0)


if __name__ == "__main__":
    unittest.main()
