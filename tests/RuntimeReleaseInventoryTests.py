import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

Script = Path(__file__).resolve().parents[1] / "tools/distribution/release_inventory.py"
Spec = importlib.util.spec_from_file_location("ReleaseInventory", Script)
Release = importlib.util.module_from_spec(Spec)
Spec.loader.exec_module(Release)


class InventoryChecks(unittest.TestCase):
    def test_reviewed_source_and_payload(self):
        with tempfile.TemporaryDirectory() as Folder:
            Root = Path(Folder) / "Game"
            (Root / "notices").mkdir(parents=True)
            Directions = "Read the accompanying fixture archive."
            (Root / "notices/Gargantuan.txt").write_text(
                "Matching covered source\n" + Directions + "\nMozilla Public License Version 2.0\nExhibit B")
            (Root / "notices/SDL3_ttf.txt").write_text(
                "FreeType Project LICENSE HarfBuzz Roboto Regular END OF TERMS AND CONDITIONS")
            (Root / "game.package.json").write_text(json.dumps({"Player": "GargantuanPlayer.exe"}))
            Source = Path(Folder) / "fixture-source.zip"
            Source.write_bytes(b"fixture source")
            Files = Release.Inventory(Root)
            Review = {"Role": "Player", "Files": Files,
                      "Components": [{"Name": "Fixture", "Version": "1", "Origin": "Local", "License": "Fixture",
                                      "DistributionBasis": "Fixture", "Files": list(Files), "Notices": []}],
                      "Source": {"Revision": "0" * 40, "RetrievalEvidence": "Fixture", "RecipientDirections": Directions,
                                 "ArchiveSha256": Release.Digest(Source)}}
            Release.Verify(Root, "Player", Review, Source)
            with self.assertRaises(ValueError):
                Release.Verify(Root, "Server", Review, Source)
            Source.write_bytes(b"other source")
            with self.assertRaises(ValueError):
                Release.Verify(Root, "Player", Review, Source)
            (Root / "undeclared").write_bytes(b"extra")
            with self.assertRaises(ValueError):
                Release.Verify(Root, "Player", Review, Source)


if __name__ == "__main__":
    unittest.main()
