import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from farm_publication_join import Join, JoinIndexed, MAX_DATABASE_PAGES
from farm_publication_trace import RECORD


class FarmPublicationJoinTests(unittest.TestCase):
    def setUp(self):
        self.Temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.Temp.cleanup)
        self.Root = Path(self.Temp.name)
        self.ServerEvidence = self.Root / "server-evidence"
        self.ServerEvidence.mkdir()
        self.ClientEvidence = self.Root / "client-evidence"
        self.ClientEvidence.mkdir()
        self.Server = self.ServerEvidence / "publication-service.bin"
        self.Client = self.ClientEvidence / "publication-service-0.bin"
        self.ServerReady = self.ServerEvidence / "server.stdout.log"
        self.ClientReady = self.ClientEvidence / "client-00.stdout.log"
        self.Scratch = self.Root / "analysis"
        self.Scratch.mkdir()
        self.RunId = "run-a"
        self.ServerRecords = self.OrdinaryServer()
        self.ClientRecords = self.ClientPair()

    @staticmethod
    def Record(Stage, Ns, CS=17, CG=2, OS=8, OG=2, Bytes=0,
               Tick=101, Seq=0, Due=0, Control=0, Material=0, Frame=0, Flags=0):
        return RECORD.pack(Stage, Flags, CS, CG, OS, OG, Bytes, Ns,
                           Tick, Seq, Due, Control, Material, Frame)

    def ClientRecord(self, Stage, Ns, **Values):
        return self.Record(Stage, Ns, CS=1, CG=1, **Values)

    def OrdinaryServer(self):
        R = self.Record
        return [R(1, 100, CS=0, CG=0, OS=0, OG=0, Tick=100),
                R(2, 110, Tick=100, Due=101),
                R(1, 200, CS=0, CG=0, OS=0, OG=0),
                R(3, 210, Due=101),
                R(4, 220, CS=0, CG=0, Seq=19, Control=3),
                R(5, 230, CS=0, CG=0, Seq=19),
                R(6, 240, Seq=19, Due=101, Material=7),
                R(8, 250, Bytes=74, Seq=19, Control=3, Material=7, Frame=11),
                R(2, 260, Due=103)]

    def ClientPair(self):
        R = self.ClientRecord
        return [R(9, 1000, Bytes=74, Seq=19, Control=3, Material=7, Frame=11),
                R(10, 1100, Bytes=74, Seq=19, Control=3, Material=7, Frame=11)]

    def Write(self, ServerRecords=None, ClientRecords=None, ServerDropped=0,
              ClientDropped=0, ServerDecode=0, ClientDecode=0, RunId=None):
        if RunId is not None:
            self.RunId = RunId
        def Trace(PathValue, Role, Slot, Nonce, Records, Dropped, Decode):
            Header = (f"format=GargantuanFarmPublicationV1\trun={self.RunId}\trole={Role}"
                      f"\tslot={Slot}\tnonce={Nonce}\tcount={len(Records)}"
                      f"\tdropped={Dropped}\tdecode_failures={Decode}\n").encode()
            PathValue.write_bytes(Header + b"".join(Records))
        Trace(self.Server, "SERVER", -1, 0, self.ServerRecords if ServerRecords is None else ServerRecords,
              ServerDropped, ServerDecode)
        Trace(self.Client, "CLIENT", 0, 17, self.ClientRecords if ClientRecords is None else ClientRecords,
              ClientDropped, ClientDecode)
        self.ServerReady.write_text(
            f"[Qualification:Server] event=ready run={self.RunId} nonce=17 connection_slot=17 "
            "connection_generation=2 session_epoch=1 player_id=1 monotonic_us=1\n")
        self.ClientReady.write_text(
            f"[Qualification:Client] event=ready run_id={self.RunId} slot=0 nonce=17 "
            "connection_slot=1 connection_generation=1 steady_ns=1\n")

    def Analyze(self, Sources=None):
        return Join(self.Server, self.ServerReady, Sources if Sources is not None else
                    {0: (self.Client, 17, self.ClientReady)}, self.RunId, self.Scratch,
                    ExpectedClients=1)

    def test_complete_role_local_join_and_scratch_bound(self):
        self.Write()
        Result = self.Analyze()
        self.assertEqual(Result["Status"], "ACCEPTED_STATE_CHAIN_OBSERVED")
        self.assertEqual(Result["Server"]["Accepted"], 1)
        self.assertEqual(Result["ServerDueToAccepted"]["MaximumNs"], 50)
        self.assertEqual(Result["ClientReceiveToHandled"]["MaximumNs"], 100)
        self.assertEqual(Result["Retirement"], "NOT_MEASURED")
        self.assertEqual(Result["DueCompleteness"], "NOT_MEASURED")
        self.assertEqual(Result["CrossHostDueToHandled"], "NOT_MEASURED")
        self.assertLess(Result["ScratchPages"], MAX_DATABASE_PAGES)
        self.assertGreater(Result["ScratchPeakDatabaseBytes"], 0)
        self.assertEqual(list(self.Scratch.iterdir()), [])

    def test_forced_state_has_separate_built_origin(self):
        R = self.Record
        self.ServerRecords += [
            R(3, 270, OS=9, Due=101, Flags=1),
            R(4, 280, CS=0, CG=0, OS=9, Seq=20, Control=4),
            R(5, 290, CS=0, CG=0, OS=9, Seq=20),
            R(6, 300, OS=9, Seq=20, Due=101, Material=7, Flags=1),
            R(8, 310, OS=9, Bytes=76, Seq=20, Control=4, Material=7, Frame=12)]
        self.ClientRecords += [
            self.ClientRecord(9, 1200, OS=9, Bytes=76, Seq=20, Control=4, Material=7, Frame=12),
            self.ClientRecord(10, 1300, OS=9, Bytes=76, Seq=20, Control=4, Material=7, Frame=12)]
        self.Write()
        Result = self.Analyze()
        self.assertEqual(Result["Server"]["ForcedAccepted"], 1)
        self.assertEqual(Result["Client"]["ForcedClientHandled"], 1)
        self.assertEqual(Result["ServerDueToAccepted"]["Count"], 1)
        self.assertEqual(Result["ServerForcedBuiltToAccepted"]["MaximumNs"], 30)

    def test_forecast_rescheduled_and_unchanged_are_not_missing_service(self):
        R = self.ClientRecord
        self.ServerRecords[2:2] = [R(2, 120, OS=9, Tick=100, Due=101)]
        self.ServerRecords[4:4] = [R(2, 205, OS=9, Due=104)]
        self.ServerRecords += [R(3, 280, OS=10, Due=101),
                               R(7, 290, OS=10, Due=101)]
        self.Write()
        Result = self.Analyze()
        self.assertEqual(Result["Server"]["ForecastRescheduled"], 1)
        self.assertEqual(Result["Server"]["UnchangedSuppressed"], 1)
        self.assertEqual(Result["Server"]["FutureForecasts"], 2)

    def test_missing_or_duplicate_server_chain_fails(self):
        Cases = [
            self.ServerRecords[:4] + self.ServerRecords[5:],  # Built missing.
            self.ServerRecords[:5] + self.ServerRecords[6:],  # Snapshot missing.
            self.ServerRecords[:7] + self.ServerRecords[8:],  # Accept missing.
            self.ServerRecords[:7] + [self.ServerRecords[7]] * 2 + self.ServerRecords[8:],
            self.ServerRecords[:3] + self.ServerRecords[4:],  # Confirmed due missing.
        ]
        for Records in Cases:
            with self.subTest(Records=len(Records), Last=Records[-1]):
                self.Write(ServerRecords=Records)
                with self.assertRaises(ValueError):
                    self.Analyze()

    def test_repeated_discovery_is_one_obligation_but_reuse_after_service_fails(self):
        Repeated = self.ServerRecords[:4] + [self.Record(3, 215, Due=101)] + self.ServerRecords[4:]
        self.Write(ServerRecords=Repeated)
        Result = self.Analyze()
        self.assertEqual(Result["Server"]["Due"], 1)
        self.assertEqual(Result["Server"]["DueRediscoveries"], 1)
        Reused = self.ServerRecords + [self.Record(3, 270, Due=101)]
        self.Write(ServerRecords=Reused)
        with self.assertRaises(ValueError):
            self.Analyze()

    def test_client_missing_duplicate_wrong_generation_and_wrong_epoch_fail(self):
        R = self.Record
        Cases = [
            self.ClientRecords[:1],
            [self.ClientRecords[0], self.ClientRecords[0], self.ClientRecords[1]],
            [R(9, 1000, OG=3, Bytes=74, Seq=19, Control=3, Material=7, Frame=11),
             self.ClientRecords[1]],
            [R(9, 1000, Bytes=74, Seq=19, Control=4, Material=7, Frame=11),
             self.ClientRecords[1]],
            [self.ClientRecords[1], self.ClientRecords[0]],
        ]
        for Records in Cases:
            with self.subTest(Records=Records):
                self.Write(ClientRecords=Records)
                with self.assertRaises(ValueError):
                    self.Analyze()

    def test_unresolved_due_or_retired_without_native_evidence_fails(self):
        R = self.Record
        self.ServerRecords += [R(3, 270, OS=9, Due=101)]
        self.Write()
        with self.assertRaisesRegex(ValueError, "conservation incomplete"):
            self.Analyze()

    def test_wrong_run_slot_nonce_and_overflow_fail(self):
        self.Write()
        for Sources in ({0: (self.Client, 18, self.ClientReady)},
                        {1: (self.Client, 17, self.ClientReady)},
                        {0: (self.Client, 0, self.ClientReady)}):
            with self.subTest(Sources=Sources), self.assertRaises(ValueError):
                self.Analyze(Sources)
        for Options in ({"ServerDropped": 1}, {"ClientDropped": 1},
                        {"ServerDecode": 1}, {"ClientDecode": 1}):
            self.Write(**Options)
            with self.subTest(Options=Options), self.assertRaises(ValueError):
                self.Analyze()

    def test_ready_mapping_is_host_local_and_generation_safe(self):
        self.Write()
        self.assertEqual(self.Analyze()["Server"]["Accepted"], 1)
        self.ServerReady.write_text(self.ServerReady.read_text().replace(
            "connection_generation=2", "connection_generation=3"))
        with self.assertRaises(ValueError):
            self.Analyze()

    def test_two_clients_may_reuse_the_same_local_connection_id(self):
        R = self.Record
        self.ServerRecords += [R(3, 270, CS=18, OS=9, Due=101),
                               R(4, 280, CS=0, CG=0, OS=9, Seq=20, Control=4),
                               R(5, 290, CS=0, CG=0, OS=9, Seq=20),
                               R(6, 300, CS=18, OS=9, Seq=20, Due=101, Material=7),
                               R(8, 310, CS=18, OS=9, Bytes=76, Seq=20,
                                 Control=4, Material=7, Frame=12)]
        self.Write()
        ClientTwo = self.ClientEvidence / "publication-service-1.bin"
        ReadyTwo = self.ClientEvidence / "client-01.stdout.log"
        ReadyTwo.write_text(
            "[Qualification:Client] event=ready run_id=run-a slot=1 nonce=18 "
            "connection_slot=1 connection_generation=1 steady_ns=2\n")
        Records = [self.ClientRecord(9, 1200, OS=9, Bytes=76, Seq=20,
                                     Control=4, Material=7, Frame=12),
                   self.ClientRecord(10, 1300, OS=9, Bytes=76, Seq=20,
                                     Control=4, Material=7, Frame=12)]
        Header = ("format=GargantuanFarmPublicationV1\trun=run-a\trole=CLIENT"
                  "\tslot=1\tnonce=18\tcount=2\tdropped=0\tdecode_failures=0\n")
        ClientTwo.write_bytes(Header.encode() + b"".join(Records))
        self.ServerReady.write_text(self.ServerReady.read_text() +
            "[Qualification:Server] event=ready run=run-a nonce=18 connection_slot=18 "
            "connection_generation=2 session_epoch=1 player_id=2 monotonic_us=2\n")
        Result = Join(self.Server, self.ServerReady,
                      {0: (self.Client, 17, self.ClientReady),
                       1: (ClientTwo, 18, ReadyTwo)}, "run-a", self.Scratch,
                      ExpectedClients=2)
        self.assertEqual(Result["Server"]["Accepted"], 2)
        self.assertEqual(Result["Client"]["ClientHandled"], 2)
        self.Write()
        self.ClientReady.write_text(self.ClientReady.read_text().replace(
            "connection_generation=1", "connection_generation=2"))
        with self.assertRaises(ValueError):
            self.Analyze()

    def test_indexed_entrypoint_pins_all_members_and_manifest(self):
        RunId = "11111111-2222-3333-4444-555555555555"
        self.Write(RunId=RunId)
        Manifest = self.Root / "run-manifest.json"
        Manifest.write_text(json.dumps({"RunId": RunId, "Nonces": ["17"]}))
        def Index(Root, Role, Files):
            Rows = [{"Name": File.name, "Bytes": File.stat().st_size,
                     "Sha256": hashlib.sha256(File.read_bytes()).hexdigest()}
                    for File in Files]
            PathValue = Root / "evidence-sha256.json"
            PathValue.write_text(json.dumps({"RunId": RunId, "Role": Role, "Files": Rows}))
            return PathValue
        ServerIndex = Index(self.ServerEvidence, "Server", [self.Server, self.ServerReady])
        ClientIndex = Index(self.ClientEvidence, "Clients", [self.Client, self.ClientReady])
        Result = JoinIndexed(ServerIndex, ClientIndex, Manifest, self.Scratch,
                             ExpectedClients=1)
        self.assertEqual(Result["Server"]["Accepted"], 1)
        self.assertIn("ServerIndexSha256", Result)
        self.Client.write_bytes(self.Client.read_bytes() + b"x")
        with self.assertRaises(ValueError):
            JoinIndexed(ServerIndex, ClientIndex, Manifest, self.Scratch,
                        ExpectedClients=1)


if __name__ == "__main__":
    unittest.main()
