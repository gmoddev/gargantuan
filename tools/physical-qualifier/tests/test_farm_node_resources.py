"""No traffic: exercise bounded owned Node resource custody at collection."""
import hashlib
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from farm_outer_campaign import FetchNodeResources


class NodeResourcesTests(unittest.TestCase):
    def test_bounded_transfer_and_fail_closed_metadata(self):
        with tempfile.TemporaryDirectory() as Directory:
            Root = Path(Directory)
            Payload = b"bounded-resource-fixture\n"
            Receipt = {"ResourceContract": "node_process_resources_v1",
                       "ResourcePath": str(Root / "node-resources.csv"),
                       "ResourceBytes": len(Payload), "ResourceSamples": 2,
                       "ResourceSha256": hashlib.sha256(Payload).hexdigest()}
            Transport = mock.Mock()
            Transport.Fetch.side_effect = lambda Source, Target, Timeout: Target.write_bytes(Payload)
            FetchNodeResources(Transport, Receipt, Root)
            self.assertEqual(Transport.Fetch.call_args.kwargs, {"Timeout": 30})
            for Name, Value in (("ResourcePath", str(Root / "outside.csv")),
                                ("ResourceBytes", 1048577), ("ResourceSamples", 1203),
                                ("ResourceSamples", True), ("ResourceContract", "wrong")):
                with self.subTest(Name=Name, Value=Value):
                    Transport.reset_mock()
                    with self.assertRaisesRegex(ValueError, "bound invalid"):
                        FetchNodeResources(Transport, dict(Receipt, **{Name: Value}), Root)
                    Transport.Fetch.assert_not_called()
            with self.assertRaisesRegex(ValueError, "transfer changed"):
                FetchNodeResources(Transport, dict(Receipt, ResourceSha256="0" * 64), Root)
            with self.assertRaisesRegex(ValueError, "bound invalid"):
                FetchNodeResources(Transport, {"ResourceContract": "node_process_resources_v1"}, Root)
            Transport.reset_mock()
            FetchNodeResources(Transport, {}, Root)
            Transport.Fetch.assert_not_called()


if __name__ == "__main__":
    unittest.main()
