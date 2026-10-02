"""Socket-free adversarial tests for the native Farm32 clock exchange."""

import importlib.util
from pathlib import Path
import unittest

SOURCE = Path(__file__).resolve().parents[1] / "farm_clock_exchange.py"
SPEC = importlib.util.spec_from_file_location("farm_clock_exchange", SOURCE)
Clock = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(Clock)
RUN = "12345678-1234-1234-1234-123456789abc"


def Native(Role, Slot, Peer, Epoch, Sequence, Request, Stage, Stamp, Kind, Result=-1):
    return (f"[Qualification:FarmClock] event=native run={RUN} role={Role} slot={Slot} "
            f"epoch={Epoch} sequence={Sequence} request={Request} "
            f"connection_slot={Peer} connection_generation=1 stage={Stage} "
            f"monotonic_ns={Stamp} kind={Kind} bytes=80 result={Result}\n")


def Fixture(Peers=2, Probes=2):
    Server = []
    Clients = {}
    for Slot in range(Peers):
        Nonce = (0x1234 << 32) | (Slot + 1)
        Server.append(f"[Qualification:Server] event=ready run={RUN} nonce={Nonce} "
                      f"connection_slot={Slot + 10} connection_generation=1\n")
        Clients[Slot] = [f"[Qualification:Client] event=ready run_id={RUN} slot={Slot} "
                         f"nonce={Nonce}\n"]
    Server.append(f"[Qualification:FarmClock] event=calibration_complete run={RUN} epoch=1 count={Peers}\n")
    Server.append(f"[Qualification:FarmClock] event=calibration_start run={RUN} epoch=1 "
                  "next_phase=baseline monotonic_us=900\n")
    Server.append(f"[Qualification:Scale] event=phase_start run={RUN} phase=baseline monotonic_us=2000\n")
    Server.append(f"[Qualification:Scale] event=phase_end run={RUN} phase=baseline monotonic_us=3000\n")
    for Slot in range(Peers):
        for Sequence in range(1, Probes + 1):
            Request = Sequence
            Base = 1000000 + Slot * 10000 + Sequence * 1000
            Server.extend([
                Native("server", -1, Slot + 10, 1, Sequence, Request, "GnsReceive", Base + 100, 3),
                Native("server", -1, Slot + 10, 1, Sequence, Request, "GnsBefore", Base + 200, 4),
                Native("server", -1, Slot + 10, 1, Sequence, Request, "GnsQueued", Base + 220, 4, 1),
            ])
            Clients[Slot].extend([
                Native("client", Slot, 1, 1, Sequence, Request, "GnsBefore", Base + 50, 3),
                Native("client", Slot, 1, 1, Sequence, Request, "GnsQueued", Base + 70, 3, 1),
                Native("client", Slot, 1, 1, Sequence, Request, "GnsReceive", Base + 300, 4),
            ])
    Server.append(f"[Qualification:FarmClock] event=terminal run={RUN} role=server slot=-1 "
                  f"records={Peers * Probes * 3} overflow=0\n")
    for Slot in Clients:
        Clients[Slot].append(f"[Qualification:FarmClock] event=terminal run={RUN} role=client "
                             f"slot={Slot} records={Probes * 3} overflow=0\n")
    return Server, Clients


class FarmClockExchangeTests(unittest.TestCase):
    def test_interval_is_causal_without_path_symmetry(self):
        self.assertEqual(Clock.OffsetInterval(100, 180, 220, 450), (-80, 230))
        with self.assertRaisesRegex(ValueError, "empty"):
            Clock.OffsetInterval(100, 100, 300, 200)

    def test_complete_two_peer_exchange(self):
        Server, Clients = Fixture()
        Result = Clock.Analyze(Server, Clients, RUN, Peers=2, Epochs=1, Probes=2)
        self.assertEqual(Result["Status"], "BOUNDED_AT_PROBE")
        self.assertEqual(len(Result["Samples"]), 4)
        self.assertEqual(Result["PhaseLongOffset"], "NOT_MEASURED")
        self.assertEqual(Result["OneWayLatency"], "NOT_MEASURED")

    def test_missing_native_stage_is_rejected(self):
        Server, Clients = Fixture()
        Clients[0] = [Line for Line in Clients[0]
                      if not ("sequence=1 " in Line and "stage=GnsReceive" in Line)]
        with self.assertRaisesRegex(ValueError, "trace incomplete|path incomplete"):
            Clock.Analyze(Server, Clients, RUN, Peers=2, Epochs=1, Probes=2)

    def test_duplicate_native_stage_is_rejected(self):
        Server, Clients = Fixture()
        Server.insert(-1, next(Line for Line in Server if "stage=GnsReceive" in Line))
        Server[-1] = Server[-1].replace("records=12", "records=13")
        with self.assertRaisesRegex(ValueError, "duplicate native"):
            Clock.Analyze(Server, Clients, RUN, Peers=2, Epochs=1, Probes=2)

    def test_identity_mismatch_is_rejected(self):
        Server, Clients = Fixture()
        Clients[1][0] = Clients[1][0].replace("slot=1", "slot=0")
        with self.assertRaisesRegex(ValueError, "client ready identity"):
            Clock.Analyze(Server, Clients, RUN, Peers=2, Epochs=1, Probes=2)

    def test_nonce_prefix_mismatch_is_rejected(self):
        Server, Clients = Fixture()
        Clients[1][0] = Clients[1][0].replace(str((0x1234 << 32) | 2),
                                                  str((0x1235 << 32) | 2))
        with self.assertRaisesRegex(ValueError, "client ready identity"):
            Clock.Analyze(Server, Clients, RUN, Peers=2, Epochs=1, Probes=2)

    def test_request_identity_reuse_is_rejected(self):
        Server, Clients = Fixture()
        Server = [Line.replace("sequence=2 request=2", "sequence=2 request=1") for Line in Server]
        Clients = {Slot: [Line.replace("sequence=2 request=2", "sequence=2 request=1")
                          for Line in Lines] for Slot, Lines in Clients.items()}
        with self.assertRaisesRegex(ValueError, "request identity reused"):
            Clock.Analyze(Server, Clients, RUN, Peers=2, Epochs=1, Probes=2)

    def test_missing_all_peer_barrier_is_rejected(self):
        Server, Clients = Fixture()
        Server = [Line for Line in Server if "event=calibration_complete" not in Line]
        with self.assertRaisesRegex(ValueError, "barrier incomplete"):
            Clock.Analyze(Server, Clients, RUN, Peers=2, Epochs=1, Probes=2)

    def test_probe_in_measured_phase_is_rejected(self):
        Server, Clients = Fixture()
        Server = [Line.replace("monotonic_us=2000", "monotonic_us=1000") for Line in Server]
        with self.assertRaisesRegex(ValueError, "entered measured phase"):
            Clock.Analyze(Server, Clients, RUN, Peers=2, Epochs=1, Probes=2)

    def test_unaccepted_native_send_is_rejected(self):
        Server, Clients = Fixture()
        Clients[0] = [Line.replace("result=1", "result=2") if
                      "sequence=1 " in Line and "stage=GnsQueued" in Line else Line
                      for Line in Clients[0]]
        with self.assertRaisesRegex(ValueError, "send was not accepted"):
            Clock.Analyze(Server, Clients, RUN, Peers=2, Epochs=1, Probes=2)

    def test_overflow_is_rejected(self):
        Server, Clients = Fixture()
        Clients[0][-1] = Clients[0][-1].replace("overflow=0", "overflow=1")
        with self.assertRaisesRegex(ValueError, "trace incomplete"):
            Clock.Analyze(Server, Clients, RUN, Peers=2, Epochs=1, Probes=2)


if __name__ == "__main__":
    unittest.main()
