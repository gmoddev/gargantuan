import random
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from farm_ordinary_demand import Analyze, Bucket, Direction, Offers
from farm_publication_trace import RECORD


class OrdinaryDemandTests(unittest.TestCase):
    def setUp(self):
        self.Temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.Temp.cleanup)
        self.Root = Path(self.Temp.name)
        self.Server = self.Root / "server.bin"
        self.Log = self.Root / "server.stdout.log"
        self.Clients = {}
        self.Ready = {Slot: ((Slot + 1, 1), (1, 1)) for Slot in range(32)}
        self.Make()

    def Clock(self, Role, Slot, Nonce, Host):
        return (f"[Qualification:TrafficClock] contract=sender_qpc_v1 run=test role={Role} "
                f"slot={Slot} nonce={Nonce} host={Host} frequency=10000000\n")

    @staticmethod
    def Rec(Stage, At, Slot=1, Phase=0, Bytes=0, Flags=0, Kind=0, Sequence=0, Origin=0):
        return RECORD.pack(Stage, Flags, Slot, int(Slot != 0), 0, 0, Bytes,
                           At, 0, Phase if Stage == 26 else Sequence, Kind, Origin, 0, At)

    @staticmethod
    def Packet(Stage, At):
        return RECORD.pack(Stage, 0, 1, 1, 1, 1, 74, At, 1, 1, 0, 1, 1, 1)

    def Write(self, PathValue, Role, Slot, Nonce, Rows):
        PathValue.write_bytes((f"format=GargantuanFarmPublicationV1\trun=test\trole={Role}"
                              f"\tslot={Slot}\tnonce={Nonce}\tcount={len(Rows)}\tdropped=0"
                              "\tdecode_failures=0\n").encode() + b"".join(Rows))

    def Make(self, Repeat=1, Rejected=False, MissingEnd=False, Size=212):
        self.Log.write_text(self.Clock("SERVER", -1, 0, "WORKER"))
        Rows = [self.Packet(8, 1)]
        Sequence = 0
        for Phase in range(1, 6):
            At = Phase * 100000000
            Rows.append(self.Rec(26, At, 0, Phase, Flags=1))
            for Slot in range(1, 33):
                for _ in range(Repeat if Slot == 1 else 1):
                    Sequence += 1
                    Rows.append(self.Rec(27, At + 1, Slot, Bytes=Size, Flags=1, Kind=2, Sequence=Sequence, Origin=Phase))
                    Rows.append(self.Rec(25 if Rejected and Slot == 1 else 24, At + 1,
                                         Slot, Bytes=Size, Flags=1, Kind=2, Sequence=Sequence, Origin=Phase))
            if not MissingEnd or Phase != 5:
                Rows.append(self.Rec(26, At + 2, 0, Phase))
        self.Write(self.Server, "SERVER", -1, 0, Rows)
        for Slot in range(32):
            PathValue, Log = self.Root / f"client-{Slot}.bin", self.Root / f"client-{Slot}.log"
            Log.write_text(self.Clock("CLIENT", Slot, Slot + 100, "CLIENTHOST"))
            Rows = [self.Packet(9, 1), self.Packet(10, 2)]
            for Phase in range(1, 6):
                At = Phase * 100000000
                Rows.append(self.Rec(26, At, 1, Phase, Flags=1))
                if Slot == 0:
                    Rows.append(self.Rec(27, At + 1, Bytes=112, Kind=1, Sequence=Phase, Origin=Phase))
                    Rows.append(self.Rec(24, At + 1, Bytes=112, Kind=1, Sequence=Phase, Origin=Phase))
                Rows.append(self.Rec(26, At + 2, 1, Phase))
            self.Write(PathValue, "CLIENT", Slot, Slot + 100, Rows)
            self.Clients[Slot] = PathValue, Slot + 100, Log

    def Analyze(self):
        return Analyze(self.Server, self.Log, self.Clients, self.Ready, "test")

    def ServerRows(self):
        Raw = self.Server.read_bytes().split(b"\n", 1)[1]
        return [list(RECORD.unpack(Raw[I:I + RECORD.size])) for I in range(0, len(Raw), RECORD.size)]

    def ReplaceServer(self, Rows):
        self.Write(self.Server, "SERVER", -1, 0, [RECORD.pack(*Row) for Row in Rows])

    def test_late_normal_tail_after_last_phase_is_charged(self):
        Rows = self.ServerRows()
        Tail = next(Row for Row in Rows if Row[0] == 24 and Row[9] == 160)
        Rows.remove(Tail)
        Tail[7] = Tail[13] = Rows[-1][13] + 1
        Rows.append(Tail)
        self.ReplaceServer(Rows)
        Value = self.Analyze()
        self.assertEqual(Value["Egress"]["Phases"][-1]["Messages"], 32)
        self.assertEqual(Value["Egress"]["Global"]["Messages"], 160)

    def test_excessive_tail_after_phase_end_still_fails(self):
        Rows = self.ServerRows()
        End = Rows.pop()
        Tail = []
        for Sequence in range(161, 177):
            Rows.append(list(RECORD.unpack(self.Rec(27, End[13] - 1, Bytes=212, Flags=1, Kind=2, Sequence=Sequence, Origin=5))))
            Tail.append(list(RECORD.unpack(self.Rec(24, End[13] + 1, Bytes=212, Flags=1, Kind=2, Sequence=Sequence, Origin=5))))
        self.ReplaceServer(Rows + [End] + Tail)
        with self.assertRaisesRegex(ValueError, "all-interval"):
            self.Analyze()

    def test_pending_ordinary_tail_cannot_disappear(self):
        Rows = self.ServerRows()
        Rows.remove(next(Row for Row in Rows if Row[0] == 24 and Row[9] == 160))
        self.ReplaceServer(Rows)
        with self.assertRaisesRegex(ValueError, "dropped or never"):
            self.Analyze()

    def test_missing_enqueue_is_not_direct_gameplay(self):
        Rows = self.ServerRows()
        Rows.remove(next(Row for Row in Rows if Row[0] == 27 and Row[9] == 160))
        self.ReplaceServer(Rows)
        with self.assertRaisesRegex(ValueError, "lacks exact queued"):
            self.Analyze()

    def test_handoff_cannot_relabel_origin_or_size(self):
        for Field, Value in ((11, 0), (6, 213), (9, 0)):
            self.Make()
            Rows = self.ServerRows()
            next(Row for Row in Rows if Row[0] == 24 and Row[9] == 160)[Field] = Value
            self.ReplaceServer(Rows)
            with self.assertRaises(ValueError):
                self.Analyze()

    def test_duplicate_success_cannot_double_count(self):
        Rows = self.ServerRows()
        Index = next(I for I, Row in enumerate(Rows) if Row[0] == 24 and Row[9] == 160)
        Rows.insert(Index + 1, Rows[Index][:])
        self.ReplaceServer(Rows)
        with self.assertRaisesRegex(ValueError, "lacks exact queued"):
            self.Analyze()

    def test_complete_bytes_all_peers_same_host(self):
        Value = self.Analyze()
        self.assertEqual(Value["State"], "MEASURED_PASS")
        self.assertEqual(Value["Egress"]["Global"]["CompleteBytes"], 212 * 32 * 5)
        self.assertEqual(len(Value["Ingress"]["Peers"]), 32)
        self.assertEqual(Value["Egress"]["Phases"][0]["ForcedCharacterMessages"], 32)

    def test_excessive_forced_traffic(self):
        self.Make(Repeat=17)
        with self.assertRaisesRegex(ValueError, "all-interval"):
            self.Analyze()

    def test_complete_byte_not_payload_budget(self):
        self.Make(Size=20481)
        with self.assertRaisesRegex(ValueError, "all-interval"):
            self.Analyze()

    def test_rejected_cannot_hide_demand(self):
        self.Make(Rejected=True)
        with self.assertRaisesRegex(ValueError, "rejected"):
            self.Analyze()

    def test_missing_last_phase_end(self):
        self.Make(MissingEnd=True)
        with self.assertRaisesRegex(ValueError, "incomplete"):
            self.Analyze()

    def test_different_host_cannot_merge(self):
        self.Clients[31][2].write_text(self.Clock("CLIENT", 31, 131, "OTHERHOST"))
        with self.assertRaisesRegex(ValueError, "host/QPC"):
            self.Analyze()

    def test_missing_one_domain_fails(self):
        self.Clients[31][2].write_text("")
        with self.assertRaisesRegex(ValueError, "partial"):
            self.Analyze()

    def test_historical_absence_remains_unmeasured(self):
        self.Log.write_text("")
        for _, _, Log in self.Clients.values():
            Log.write_text("")
        self.assertEqual(self.Analyze()["State"], "NOT_MEASURED")

    def test_global_burst_independent_from_peers(self):
        # Nine individually legal 16-message peer bursts exceed global128.
        Rows = [(100, Slot, 1, 40, 1) for Slot in range(9) for _ in range(16)]
        with self.assertRaisesRegex(ValueError, "all-interval"):
            Direction([iter(Rows)], 10000000)

    def test_integer_reference_agreement_including_ties(self):
        Random = random.Random(37)
        for _ in range(100):
            Rows = sorted((Random.randrange(20), Random.randrange(1, 8)) for _ in range(25))
            BucketValue = Bucket(7, 10000, 13)
            for At, Amount in Rows:
                BucketValue.Add(Amount, At)
            Exact = max(sum(Value for _, Value in Rows[I:J+1]) * 13 -
                        7 * (Rows[J][0] - Rows[I][0])
                        for I in range(len(Rows)) for J in range(I, len(Rows)))
            self.assertEqual(BucketValue.Required, Exact)

    def test_duplicate_clock(self):
        self.Log.write_text(self.Log.read_text() * 2)
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.Analyze()

    def test_zero_frequency(self):
        self.Log.write_text(self.Log.read_text().replace("frequency=10000000", "frequency=0"))
        with self.assertRaisesRegex(ValueError, "unavailable"):
            self.Analyze()

    def test_native_boundary_and_complete_adapter_basis(self):
        Root = Path(__file__).resolve().parents[3]
        Native = (Root / "src/network/GameNetworkingSocketsTransport.cpp").read_text()
        Start = Native.index("TransportOperationResult GameNetworkingSocketsTransport::Send(")
        Send = Native[Start:Native.index("std::size_t GameNetworkingSocketsTransport::PollEvents", Start)]
        self.assertIn('case k_EResultOK:\n\t\t\tRecordOrdinary("OrdinaryReliableSent")', Send)
        self.assertIn('Message.Traffic() != TrafficClass::StructuralReplication', Send)
        self.assertIn('Payload.size() + AdapterEnvelopeBytes', Send)
        self.assertIn('if (!runtime_detail::PublicationLatencySelected(Message.Destination())) return;', Send)
        self.assertNotIn('RecordOrdinary("OrdinaryReliableSent")', Send[:Send.index('case k_EResultOK:')])

    def test_client_closes_reload_before_deliberate_overload_callback(self):
        Root = Path(__file__).resolve().parents[3]
        Player = (Root / "src/host/player/PlayerHost.cpp").read_text()
        Loop = Player[Player.index('while (Runtime->ProcessService->Alive)'):]
        self.assertLess(Loop.index('GetAttributeValue("ScaleOverloadEnabled")'), Loop.index('Runtime->Step()'))
        self.assertIn('std::string_view{}, Overload)', Loop)


if __name__ == "__main__":
    unittest.main()
