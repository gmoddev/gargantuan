"""Exact Luau Remote offer/completion evidence for the Farm32 producer."""

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import farm_remote_cadence as Cadence


RUN = "12345678-1234-1234-1234-123456789abc"


def Lines(RpcDuration=50_000, EventDuration=40_000, EventGap=80_000,
          SlowLastRpc=False, SlowRpcIndices=(), SlowRpcDuration=500_001):
    Result = [f"[Qualification:Client] event=ready run_id={RUN} slot=0 nonce=123 "
              "connection_slot=1 connection_generation=1"]
    Sequence = 0
    for Phase in Cadence.PHASES:
        Rpc = []
        PreviousCompletion = 0
        for Index in range(1, 101):
            Started = PreviousCompletion + 1000
            Duration = SlowRpcDuration if (SlowLastRpc and Index == 100) or Index in SlowRpcIndices else RpcDuration
            PreviousCompletion = Started + Duration
            Rpc.append(f"{Index}:{Started * 1000}:{PreviousCompletion * 1000}:1")
        Events = []
        for Index in range(1, 5):
            Sequence += 1
            Completed = Index * EventGap
            Events.append(f"{Sequence}:{(Completed - EventDuration) * 1000}:{Completed * 1000}")
        for Kind, Records in (("rpc", Rpc), ("event", Events)):
            Chunks = [Records[Index:Index + 32] for Index in range(0, len(Records), 32)]
            Result.append(f"[Qualification:RemoteCadence] event=summary version=1 kind={Kind} "
                          f"phase={Phase} records={len(Records)} chunks={len(Chunks)}")
            for Index, Chunk in enumerate(Chunks, 1):
                Result.append(f"[Qualification:RemoteCadence] event=chunk version=1 kind={Kind} "
                              f"phase={Phase} index={Index} records={','.join(Chunk)}")
        Result.append(f"[Qualification:Producer] event=phase_metrics run_id={RUN} slot=0 "
                      f"nonce=123 phase={Phase} status=PASS remote_samples=100 "
                      "event_offers=4 event_acks=4 event_outstanding=0")
    return Result


class FarmRemoteCadenceTests(unittest.TestCase):
    def test_five_phase_luau_handler_trace_passes(self):
        Result = Cadence.ParseLines(Lines(), RUN)
        self.assertEqual(Result["Status"], "MEASURED_PASS")
        self.assertEqual(Result["Phases"]["baseline"]["RpcP95Ns"], 50_000_000)
        self.assertEqual(Result["Phases"]["baseline"]["EventMaxAckGapNs"], 80_000_000)
        self.assertEqual(Result["OtherClientsScaleRemoteRecipientService"], "NOT_MEASURED")
        self.assertEqual(Result["CrossHostOneWayLatency"], "NOT_MEASURED")

    def test_exact_thresholds_pass(self):
        Result = Cadence.ParseLines(Lines(RpcDuration=150_000,
                                           EventDuration=250_000, EventGap=250_000), RUN)
        self.assertEqual(Result["Status"], "MEASURED_PASS")

    def test_one_nanosecond_over_event_limit_fails(self):
        Values = Lines(EventDuration=250_000, EventGap=250_000)
        for Index, Value in enumerate(Values):
            if "event=chunk" in Value and "kind=event" in Value:
                Values[Index] = Value.replace("4:750000000:1000000000",
                                              "4:750000000:1000000001", 1)
                break
        Result = Cadence.ParseLines(Values, RUN)
        self.assertEqual(Result["Status"], "MEASURED_FAIL")
        self.assertEqual(Result["Phases"]["baseline"]["EventMaxRttNs"], 250_000_001)

    def test_rpc_threshold_failure_is_measured(self):
        Result = Cadence.ParseLines(Lines(SlowLastRpc=True), RUN)
        self.assertEqual(Result["Status"], "MEASURED_FAIL")
        self.assertEqual(Result["Phases"]["baseline"]["RpcMaxNs"], 500_001_000)

    def test_rpc_p95_threshold_is_independent_of_maximum(self):
        Values = Lines(RpcDuration=50_000, SlowRpcIndices=(95, 96, 97, 98, 99, 100),
                       SlowRpcDuration=150_001)
        Result = Cadence.ParseLines(Values, RUN)
        self.assertEqual(Result["Status"], "MEASURED_FAIL")
        self.assertEqual(Result["Phases"]["baseline"]["RpcP95Ns"], 150_001_000)
        self.assertLess(Result["Phases"]["baseline"]["RpcMaxNs"], 500_000_000)

    def test_rpc_p99_threshold_is_independent_of_p95_and_maximum(self):
        Result = Cadence.ParseLines(Lines(SlowRpcIndices=(99, 100),
                                           SlowRpcDuration=250_001), RUN)
        self.assertEqual(Result["Status"], "MEASURED_FAIL")
        self.assertEqual(Result["Phases"]["baseline"]["RpcP95Ns"], 50_000_000)
        self.assertEqual(Result["Phases"]["baseline"]["RpcP99Ns"], 250_001_000)
        self.assertLess(Result["Phases"]["baseline"]["RpcMaxNs"], 500_000_000)

    def test_event_round_trip_failure_is_measured(self):
        Result = Cadence.ParseLines(Lines(EventDuration=250_001, EventGap=250_001), RUN)
        self.assertEqual(Result["Status"], "MEASURED_FAIL")
        self.assertEqual(Result["Phases"]["baseline"]["EventMaxRttNs"], 250_001_000)

    def test_event_ack_gap_failure_is_measured(self):
        Result = Cadence.ParseLines(Lines(EventGap=250_001), RUN)
        self.assertEqual(Result["Status"], "MEASURED_FAIL")

    def test_missing_chunk_rejected(self):
        Values = Lines()
        Values.pop(next(Index for Index, Value in enumerate(Values)
                        if "event=chunk" in Value and "kind=rpc" in Value))
        with self.assertRaisesRegex(ValueError, "chunks missing"):
            Cadence.ParseLines(Values, RUN)

    def test_reused_event_identity_rejected(self):
        Values = Lines()
        for Index, Value in enumerate(Values):
            if "event=chunk" in Value and "kind=event" in Value:
                Values[Index] = Value.replace("2:120000000:160000000", "1:120000000:160000000", 1)
                break
        with self.assertRaisesRegex(ValueError, "Event offer identity"):
            Cadence.ParseLines(Values, RUN)

    def test_typed_metrics_must_match_raw_luau_count(self):
        Values = Lines()
        Values[-1] = Values[-1].replace("event_acks=4", "event_acks=3")
        with self.assertRaisesRegex(ValueError, "typed producer counts"):
            Cadence.ParseLines(Values, RUN)

    def test_index_hash_and_role_are_required(self):
        with tempfile.TemporaryDirectory() as Temporary:
            Root = Path(Temporary)
            Log = Root / "client-00.stdout.log"
            Log.write_text("\n".join(Lines()) + "\n", encoding="utf-8")
            Index = Root / "evidence-sha256.json"
            Value = {"Role": "Clients", "RunId": RUN, "Files": [{
                "Name": Log.name, "Bytes": Log.stat().st_size,
                "Sha256": hashlib.sha256(Log.read_bytes()).hexdigest()}]}
            Index.write_text(json.dumps(Value), encoding="utf-8")
            self.assertEqual(Cadence.Analyze(Index)["Status"], "MEASURED_PASS")
            Log.write_text(Log.read_text(encoding="utf-8") + "forged\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "hash or size"):
                Cadence.Analyze(Index)


if __name__ == "__main__":
    unittest.main()
