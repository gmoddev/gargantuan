import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from farm_publication_trace import RECORD, Validate


class FarmPublicationTraceTests(unittest.TestCase):
    def setUp(self):
        self.Temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.Temp.cleanup)
        self.Path = Path(self.Temp.name) / "publication-service-0.bin"

    def Write(self, Records, Dropped=0, DecodeFailures=0, Role="CLIENT", Slot=0, Nonce=17):
        Header = (f"format=GargantuanFarmPublicationV1\trun=run-a\trole={Role}"
                  f"\tslot={Slot}\tnonce={Nonce}\tcount={len(Records)}"
                  f"\tdropped={Dropped}\tdecode_failures={DecodeFailures}\n").encode()
        self.Path.write_bytes(Header + b"".join(Records))

    @staticmethod
    def Record(Stage=9, Nanoseconds=100, ObjectGeneration=2):
        return RECORD.pack(Stage, 0, 1, 1, 8, ObjectGeneration, 74,
                           Nanoseconds, 200, 19, 0, 3, 7, 11)

    def test_client_first_receive_and_handled_are_complete(self):
        self.Write([self.Record(9, 100), self.Record(10, 150)])
        Value = Validate(self.Path, "run-a", "CLIENT", 0, 17)
        self.assertEqual(Value["Stages"], {9: 1, 10: 1})
        self.assertEqual(Value["CrossHostLatency"], "NOT_MEASURED")

    def test_server_due_and_scheduler_acceptance_are_retained(self):
        self.Path = Path(self.Temp.name) / "publication-service.bin"
        Retired = RECORD.pack(11, 0, 1, 1, 8, 2, 0,
                              110, 200, 0, 0, 0, 0, 0)
        self.Write([self.Record(1, 50), self.Record(3, 75), self.Record(8, 100), Retired],
                   Role="SERVER", Slot=-1, Nonce=0)
        Value = Validate(self.Path, "run-a", "SERVER")
        self.assertEqual(Value["Stages"], {1: 1, 3: 1, 8: 1, 11: 1})
        self.assertEqual(Value["CrossHostLatency"], "NOT_MEASURED")

    @staticmethod
    def Rpc(Stage, Nanoseconds, Bytes=0, Request=7):
        return RECORD.pack(Stage, 0, 1, 1, 18, 2, Bytes,
                           Nanoseconds, 0, Request, 0, 0, 0, 0)

    def test_rpc_causal_stages_require_request_identity_and_byte_shape(self):
        self.Write([self.Record(9, 100), self.Record(10, 150)] +
                   [self.Rpc(Stage, 160 + Stage, 64 if Stage in (16, 22) else 0)
                    for Stage in (14, 15, 16, 22, 23)])
        Value = Validate(self.Path, "run-a", "CLIENT", 0, 17)
        self.assertEqual({Stage: Value["Stages"][Stage] for Stage in (14, 15, 16, 22, 23)},
                         {Stage: 1 for Stage in (14, 15, 16, 22, 23)})
        self.Path = Path(self.Temp.name) / "publication-service.bin"
        self.Write([self.Record(8, 100)] +
                   [self.Rpc(Stage, 160 + Stage, 64 if Stage in (17, 21) else 0)
                    for Stage in (17, 18, 19, 20, 21)], Role="SERVER", Slot=-1, Nonce=0)
        Validate(self.Path, "run-a", "SERVER")
        self.Write([self.Record(8, 100), self.Rpc(17, 177, 64, Request=0)],
                   Role="SERVER", Slot=-1, Nonce=0)
        with self.assertRaises(ValueError):
            Validate(self.Path, "run-a", "SERVER")
        self.Write([self.Record(8, 100), self.Rpc(16, 177, 0)],
                   Role="SERVER", Slot=-1, Nonce=0)
        with self.assertRaises(ValueError):
            Validate(self.Path, "run-a", "SERVER")

    def test_rejects_retirement_with_packet_fields_or_client_role(self):
        Retired = RECORD.pack(11, 0, 1, 1, 8, 2, 0,
                              110, 200, 0, 0, 0, 0, 0)
        self.Write([self.Record(9, 100), Retired, self.Record(10, 150)])
        with self.assertRaises(ValueError):
            Validate(self.Path, "run-a", "CLIENT", 0, 17)
        self.Path = Path(self.Temp.name) / "publication-service.bin"
        Bad = RECORD.pack(11, 0, 1, 1, 8, 2, 74,
                          110, 200, 19, 0, 3, 7, 11)
        self.Write([self.Record(8, 100), Bad], Role="SERVER", Slot=-1, Nonce=0)
        with self.assertRaises(ValueError):
            Validate(self.Path, "run-a", "SERVER")

    def test_direct_offer_requires_full_generation_and_state_identity(self):
        self.Path = Path(self.Temp.name) / "publication-service.bin"
        Direct = RECORD.pack(12, 1, 1, 1, 8, 2, 0,
                             90, 200, 19, 0, 0, 7, 0)
        self.Write([Direct, self.Record(8, 100)], Role="SERVER", Slot=-1, Nonce=0)
        self.assertEqual(Validate(self.Path, "run-a", "SERVER")["Stages"], {12: 1, 8: 1})
        Bad = RECORD.pack(12, 1, 1, 1, 8, 2, 0,
                          90, 200, 19, 201, 0, 7, 0)
        self.Write([Bad, self.Record(8, 100)], Role="SERVER", Slot=-1, Nonce=0)
        with self.assertRaises(ValueError):
            Validate(self.Path, "run-a", "SERVER")

    def test_direct_rejection_is_server_only_and_requires_matching_shape(self):
        self.Path = Path(self.Temp.name) / "publication-service.bin"
        Rejected = RECORD.pack(13, 1, 1, 1, 8, 2, 0,
                               90, 200, 19, 0, 0, 7, 0)
        self.Write([Rejected, self.Record(8, 100)], Role="SERVER", Slot=-1, Nonce=0)
        self.assertEqual(Validate(self.Path, "run-a", "SERVER")["Stages"], {13: 1, 8: 1})
        Bad = RECORD.pack(13, 1, 1, 1, 8, 2, 74,
                          90, 200, 19, 0, 0, 7, 0)
        self.Write([Bad, self.Record(8, 100)], Role="SERVER", Slot=-1, Nonce=0)
        with self.assertRaises(ValueError):
            Validate(self.Path, "run-a", "SERVER")
        self.Write([Rejected, self.Record(9, 100), self.Record(10, 150)])
        with self.assertRaises(ValueError):
            Validate(self.Path, "run-a", "CLIENT", 0, 17)

    def test_rejects_overflow_decode_failure_and_truncation(self):
        for Dropped, DecodeFailures in ((1, 0), (0, 1)):
            self.Write([self.Record(9), self.Record(10, 150)], Dropped, DecodeFailures)
            with self.assertRaises(ValueError):
                Validate(self.Path, "run-a", "CLIENT", 0, 17)
        self.Write([self.Record(9), self.Record(10, 150)])
        self.Path.write_bytes(self.Path.read_bytes()[:-1])
        with self.assertRaises(ValueError):
            Validate(self.Path, "run-a", "CLIENT", 0, 17)

    def test_rejects_wrong_identity_generation_and_order(self):
        self.Write([self.Record(9), self.Record(10, 150)])
        with self.assertRaises(ValueError):
            Validate(self.Path, "run-a", "CLIENT", 1, 17)
        self.Write([self.Record(9), self.Record(10, 150, ObjectGeneration=0)])
        with self.assertRaises(ValueError):
            Validate(self.Path, "run-a", "CLIENT", 0, 17)
        self.Write([self.Record(9, 150), self.Record(10, 100)])
        with self.assertRaises(ValueError):
            Validate(self.Path, "run-a", "CLIENT", 0, 17)

    def test_rejects_observer_or_missing_actual_client_stage(self):
        self.Write([self.Record(9), self.Record(10, 150), self.Record(3, 160)])
        with self.assertRaises(ValueError):
            Validate(self.Path, "run-a", "CLIENT", 0, 17)
        self.Write([self.Record(9)])
        with self.assertRaises(ValueError):
            Validate(self.Path, "run-a", "CLIENT", 0, 17)


if __name__ == "__main__":
    unittest.main()
