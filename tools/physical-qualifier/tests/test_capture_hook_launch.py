"""Installed service launch size and embedded exporter source contract."""

import base64
import gzip
from pathlib import Path
import re
import unittest


Root = Path(__file__).resolve().parents[1]
HookPath = Root / "worker/PktMonCapture.ps1"
SourcePath = Root / "worker/FastNdisExport.cs"


class CaptureHookLaunchTests(unittest.TestCase):
    def test_embedded_exporter_matches_reviewable_source(self):
        Hook = HookPath.read_bytes().decode("utf-8-sig")
        Match = re.search(r"\$CompressedDefinition = '([A-Za-z0-9+/=]+)'", Hook)
        self.assertIsNotNone(Match)
        self.assertEqual(
            gzip.decompress(base64.b64decode(Match.group(1))),
            SourcePath.read_bytes(),
        )

    def test_installed_service_encoded_command_fits_windows_limit(self):
        Hook = HookPath.read_bytes().decode("utf-8-sig")
        # The installed service embeds the complete hook in -EncodedCommand.
        # Include a long legal evidence path, both action names, the executable
        # path, and PowerShell flags rather than measuring the hook file alone.
        Prefix = r"C:\GargantuanQualification\physical-qualifier-service-evidence"
        EvidenceDir = Prefix + "\\" + ("x" * 100 + "\\") * 4
        EvidenceDir += "y" * (512 - len(EvidenceDir))
        self.assertEqual(len(EvidenceDir), 512)
        for Newline in ("\n", "\r\n"):
            InstalledHook = Hook.replace("\r\n", "\n").replace("\n", Newline)
            for Action in ("Start", "Stop"):
                Code = "& {\n" + InstalledHook + "\n} '" + EvidenceDir + "' " + Action
                Encoded = base64.b64encode(Code.encode("utf-16le")).decode("ascii")
                CommandLine = (
                    r'"C:\WINDOWS\System32\WindowsPowerShell\v1.0\powershell.exe" '
                    "-NoProfile -NonInteractive -EncodedCommand " + Encoded
                )
                self.assertLess(len(CommandLine), 32767)


if __name__ == "__main__":
    unittest.main()
