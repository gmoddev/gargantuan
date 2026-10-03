"""Mutation tests for retained evidence, independent of native producer code."""
import copy
import ast
import csv
import json
import shutil
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

import foundation3l_four_client as Replay


def Line(Kind, **Fields):
    return "[Probe:" + Kind + "] " + " ".join(f"{K}={V}" for K, V in Fields.items()) + "\n"


def Fixture():
    Rows = []
    Previous = {S: dict.fromkeys(Replay.FIELDS, 0) for S in range(1, 5)}
    Created = Retired = 0
    for Wave in range(32):
        Start = 1_000_000 + Wave * 300_000
        for Phase in range(3):
            for Slot in range(1, 5):
                Row = Previous[Slot].copy()
                Token = Wave * 4 + Slot
                Row.update(simulation_step=Wave * 3 + Phase, slot=Slot, generation=1,
                           consumed_us=Start + Phase * 31_000 + Slot, feedback_us=Start + Phase * 31_000,
                           present=1, valid=1, available=1, qualified=1, result_retired_bytes=0,
                           rate=18_874_368, max_run_deficit_byte_us=1000)
                if Phase == 0:
                    Created += Replay.GROUP
                    Row.update(debt_token=Token, debt_bytes=Replay.GROUP,
                               accepted_structural=(Wave + 1) * Replay.GROUP, active_grant_bytes=Replay.GROUP,
                               active_grant_started_us=Start, first_sent_in_grant=0, grant_first_us=0, grant_complete_us=0)
                elif Phase == 1:
                    Row.update(structural_first=(Wave + 1) * Replay.GROUP, first_sent_in_grant=Replay.GROUP,
                               grant_first_us=Start + 100, grant_complete_us=Start + 30_000,
                               last_completed_token=Token, last_completed_bytes=Replay.GROUP,
                               last_completed_activated_us=Start, last_completed_first_us=Start + 100,
                               last_completed_finish_us=Start + 30_000, last_completed_max_run_deficit_byte_us=1000,
                               running_us=(Wave + 1) * 29_900)
                else:
                    Retired += Replay.GROUP
                    Row.update(structural_ack=(Wave + 1) * Replay.GROUP, debt_token=0, debt_bytes=0,
                               active_grant_bytes=0, first_sent_in_grant=0, active_grant_started_us=0,
                               retired_token=Token, result_retired_bytes=Replay.GROUP, retirement_sequence=Wave + 1)
                Row.update(created=Created, retired=Retired, total_debt=Created - Retired,
                           grants=(Created - Retired) // Replay.GROUP)
                Previous[Slot] = Row
                Rows.append(Row)
    Text = Line("Result", **{"pass": 1}, scope="Phase1-only")
    Text += Line("Cleanup", good=1, created=Created, retired=Retired, terminal=0, outstanding=0, grants=0)
    Text += Line("Summary", role="server", nonce=0, waves=32, errors=0, overflow=0, invalid=0, floor_failure=0, samples=len(Rows))
    Text += Line("ServiceCurve", contract="F1", verdict="PASS", pool_curve="derived-from-four-native-grant-curves",
                 peer_rate_Bps=Replay.RATE, pool_rate_Bps=4 * Replay.RATE, quantum_B=1248, startup_us=5000, run_us=1000,
                 peer_finite_intercept_byte_us=Replay.FINITE_INTERCEPT, peer_running_bound_byte_us=Replay.RUN_BOUND,
                 pool_running_bound_byte_us=4 * Replay.RUN_BOUND, pool_common_run_us=32 * 29_900, pool_episodes=32)
    Text += Line("Admission", accepted=Created, retired=Retired, terminal=0, outstanding=0, grants=0, grants_high_water=4)
    for Slot in range(1, 5):
        Text += Line("Peer", slot=Slot, generation=1, nonce=100 + Slot, producer=int(Slot == 1))
        Text += Line("PeerService", slot=Slot, grants=32, qualified_grants=32, completed_grants=32,
                     structural_first=32 * Replay.GROUP, structural_ack=32 * Replay.GROUP,
                     running_us=32 * 29_900, max_run_deficit_byte_us=1000, retry=0)
    return Rows, Text


class FourClientTests(unittest.TestCase):
    def setUp(self):
        self.Temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.Temp.cleanup)
        self.Root = Path(self.Temp.name)
        self.Csv = self.Root / "server.csv"
        self.Log = self.Root / "server.log"
        self.Rows, self.Text = Fixture()

    def RunNative(self, Rows=None, Text=None):
        with self.Csv.open("w", newline="") as Stream:
            Writer = csv.DictWriter(Stream, fieldnames=Replay.FIELDS)
            Writer.writeheader()
            Writer.writerows(self.Rows if Rows is None else Rows)
        self.Log.write_text(self.Text if Text is None else Text)
        return Replay.ReplayNative(self.Csv, self.Log)

    def test_exact_128_grants_and_pool_derivation(self):
        Result = self.RunNative()
        self.assertEqual(Result["MaximumGrants"], 128)
        self.assertEqual(Result["CommonRunningUs"], 32 * 29_900)
        self.assertEqual(Result["MeasuredPoolMaximum"], "NOT_MEASURED")
        self.assertEqual(Result["Accepted"], Result["Retired"])

    def test_latched_failures_cannot_be_hidden_by_later_pass(self):
        for Field, Value in (("service_failed", 1), ("last_completed_failed", 1),
                             ("max_run_deficit_byte_us", Replay.RUN_BOUND + 1),
                             ("last_completed_max_run_deficit_byte_us", Replay.RUN_BOUND + 1)):
            with self.subTest(Field=Field):
                Rows = copy.deepcopy(self.Rows)
                Rows[4][Field] = Value
                with self.assertRaisesRegex(ValueError, "latched"):
                    self.RunNative(Rows)

    def test_missing_row_or_truncated_tail_fails(self):
        for Rows in (self.Rows[1:], self.Rows[:-4], self.Rows[:24] + self.Rows[36:]):
            with self.subTest(Count=len(Rows)), self.assertRaises(ValueError):
                self.RunNative(Rows)

    def test_wrong_generation_token_bytes_and_retirement_fail(self):
        for Index, Field, Value in ((4, "generation", 2), (4, "last_completed_token", 999),
                                    (4, "last_completed_bytes", Replay.GROUP - 1),
                                    (8, "result_retired_bytes", Replay.GROUP - 1),
                                    (8, "retired_token", 2), (4, "structural_ack", Replay.GROUP + 1)):
            with self.subTest(Field=Field):
                Rows = copy.deepcopy(self.Rows)
                Rows[Index][Field] = Value
                with self.assertRaises(ValueError):
                    self.RunNative(Rows)

    def test_finite_delay_fails_without_running_failure(self):
        Rows = copy.deepcopy(self.Rows)
        Rows[4]["last_completed_finish_us"] += 8000
        Rows[4]["feedback_us"] += 8000
        Rows[4]["consumed_us"] += 8000
        with self.assertRaisesRegex(ValueError, "finite"):
            self.RunNative(Rows)

    def test_active_sample_rejects_hidden_finite_stall(self):
        Rows = copy.deepcopy(self.Rows)
        Rows[0]["active_grant_started_us"] -= 10_000
        with self.assertRaisesRegex(ValueError, "finite"):
            self.RunNative(Rows)

    def test_source_summary_cannot_change_contract(self):
        for Old, New in (("run_us=1000", "run_us=6000"), ("pool_episodes=32", "pool_episodes=31"),
                         ("qualified_grants=32", "qualified_grants=31"), ("invalid=0", "invalid=1")):
            with self.subTest(Field=Old), self.assertRaises(ValueError):
                self.RunNative(Text=self.Text.replace(Old, New))

    def test_duplicate_completed_certificate_changes_fail(self):
        Rows = copy.deepcopy(self.Rows)
        Rows[8]["last_completed_max_run_deficit_byte_us"] = 999
        with self.assertRaisesRegex(ValueError, "certificate changed"):
            self.RunNative(Rows)

    def test_per_grant_deficit_resets_but_generation_diagnostics_do_not(self):
        Rows = copy.deepcopy(self.Rows)
        for Row in Rows:
            # A later independent grant can have a smaller maximum; the
            # exported connection maximum remains the earlier 1000 byte-us.
            if Row["last_completed_token"] >= 5:
                Row["last_completed_max_run_deficit_byte_us"] = 500
            Row["current_run_deficit_byte_us"] = 500 if Row["debt_token"] else 0
        self.assertEqual(self.RunNative(Rows)["MaximumGrants"], 128)
        for Field in ("running_us", "max_run_deficit_byte_us"):
            Broken = copy.deepcopy(Rows)
            Broken[12][Field] = 0  # Second grant after first grant retirement.
            with self.subTest(Field=Field), self.assertRaisesRegex(ValueError, "counters regressed"):
                self.RunNative(Broken)

    def test_missing_evidence_is_not_measured(self):
        self.assertEqual(Replay.Replay({}, "a" * 40)["State"], "NOT_MEASURED")

    def test_manifest_hash_size_and_path_confinement(self):
        File = self.Root / "input.txt"
        File.write_text("original evidence")
        Entry = {"Path": File.name, "SHA256": Replay.Digest(File), "Bytes": File.stat().st_size}
        Manifest = self.Root / "evidence-manifest.json"
        Manifest.write_text(json.dumps({"Files": [Entry]}))
        self.assertIn("input.txt", Replay.Manifest(self.Root, {}))
        for Mutate in ({**Entry, "SHA256": "0" * 64}, {**Entry, "Bytes": 0}, {**Entry, "Path": "../input.txt"}):
            Manifest.write_text(json.dumps({"Files": [Mutate]}))
            with self.assertRaises(ValueError):
                Replay.Manifest(self.Root, {})

    def test_gameplay_exact_sequences_and_protected_limits(self):
        File = self.Root / "client.csv"
        Rows = [f"{Kind},{N},10" for Kind in ("rpc", "event") for N in range(1, 81)]
        File.write_text("kind,sequence,rtt_ms\n" + "\n".join(Rows) + "\n")
        Text = Line("Result", **{"pass": 1}, scope="Phase1-only")
        Text += Line("Cleanup", good=1, created=0, retired=0, terminal=0, outstanding=0, grants=0)
        Text += Line("Summary", role="client", nonce=101, producer=1, waves=32, applied=32, moved=1,
                     rpc_n=80, event_n=80, errors=0, overflow=0, invalid=0, floor_failure=0)
        self.assertEqual(Replay.ReplayGameplay(File, Text, 101, True), {"rpc": 80, "event": 80})
        for Old, New in (("rpc,2,10", "rpc,3,10"), ("rpc,80,10", "rpc,80,501"),
                         ("event,80,10", "event,80,251"), ("rpc,1,10", "rpc,1,NaN")):
            File.write_text("kind,sequence,rtt_ms\n" + "\n".join(Rows).replace(Old, New) + "\n")
            with self.subTest(Old=Old), self.assertRaises(ValueError):
                Replay.ReplayGameplay(File, Text, 101, True)

    def test_packet_truncation_fails(self):
        def Block(Kind, Body):
            Body += b"\0" * ((-len(Body)) % 4)
            return struct.pack("<II", Kind, len(Body) + 12) + Body + struct.pack("<I", len(Body) + 12)
        Header = Block(0x0a0d0d0a, struct.pack("<IHHq", 0x1a2b3c4d, 1, 0, -1))
        Header += Block(1, struct.pack("<HHI", 1, 0, 65535))
        File = self.Root / "capture.pcapng"
        File.write_bytes(Header + Block(6, struct.pack("<IIIII", 0, 0, 0, 14, 14) + bytes(14)))
        self.assertEqual(Replay.FullPackets(File), 1)
        File.write_bytes(Header + Block(6, struct.pack("<IIIII", 0, 0, 0, 14, 15) + bytes(14)))
        with self.assertRaisesRegex(ValueError, "truncation"):
            Replay.FullPackets(File)

    def FullFixture(self):
        self.RunNative()
        Inputs = {Key: str(self.Root / Key) for Key in
                  ("ClientRoot", "ServerRoot", "CoordinatorRoot", "LifecycleHostRoot", "CandidateRoot")}
        for Value in Inputs.values():
            Path(Value).mkdir()
        Client, Server, Coordinator, Host, Candidate = [Path(Inputs[K]) for K in Inputs]
        def Save(File, Value):
            File.write_text(json.dumps(Value))
        Source = {"BaseHead": "a" * 40, "Contract": "F1", "GnsPin": "2cb93a06350bb065db53abdb0d87cf297e0bfd34",
                  "SourceArchiveFormat": "F1_OWNED_TREE_V2", "RuntimeSha256": {}, "NativeDependenciesSha256": {}}
        for Name, Key in (("gargantuan_physical_gns_funding_probe.exe", "ProbeSHA256"),
                          ("f1-native-source.zip", "OverlayArchiveSha256")):
            File = Candidate / Name
            File.write_bytes(b"fixture artifact")
            Source[Key] = Replay.Digest(File)
        for Root in (Client, Server, Candidate):
            Save(Root / "source-manifest.json", Source)
        (Server / "probe-server.stdout.log").write_text(self.Text)
        Inputs["ServerCsv"] = str(self.Csv)
        Inputs["RawCsvSha256"] = {"Server": Replay.Digest(self.Csv)}
        Accepted = self.Root / "admissions.csv"
        self.WriteAdmissions(Accepted)
        Inputs["AdmissionsCsv"] = str(Accepted)
        Inputs["RawCsvSha256"]["Admissions"] = Replay.Digest(Accepted)
        (Server / "probe-server.stdout.log").write_text(self.Text + Line("AdmissionBoundary", format="F1_ACCEPTED_ACTIVATION_V1", records=128))
        shutil.copyfile(self.Csv, Server / "physical-gns-server.csv")
        shutil.copyfile(Accepted, Server / "physical-gns-server-admissions.csv")
        Inputs["ClientCsv"] = {}
        for Nonce in range(101, 105):
            File = self.Root / (str(Nonce) + ".csv")
            File.write_text("kind,sequence,rtt_ms\n" + ("".join(f"{K},{N},10\n" for K in ("rpc", "event")
                            for N in range(1, 81)) if Nonce == 101 else ""))
            Inputs["ClientCsv"][str(Nonce)] = str(File)
            Inputs["RawCsvSha256"][str(Nonce)] = Replay.Digest(File)
            shutil.copyfile(File, Client / f"physical-gns-client-{Nonce}.csv")
            Text = Line("Result", **{"pass": 1}, scope="Phase1-only")
            Text += Line("Cleanup", good=1, created=0, retired=0, terminal=0, outstanding=0, grants=0)
            Text += Line("Summary", role="client", nonce=Nonce, producer=int(Nonce == 101), waves=32, applied=32,
                         moved=1, rpc_n=80 if Nonce == 101 else 0, event_n=80 if Nonce == 101 else 0,
                         errors=0, overflow=0, invalid=0, floor_failure=0)
            (Client / f"probe-client-{Nonce}.stdout.log").write_text(Text)
        Run = "11111111-1111-4111-8111-111111111111"
        Lifecycle = "22222222-2222-4222-8222-222222222222"
        Result = {"Success": True, "ExitCode": 0, "Classification": "FOUR_CLIENT_PHASE1_ONLY"}
        ClientResult = {**Result, "ClientNonces": [101, 102, 103, 104]}
        Save(Client / "result.json", ClientResult)
        Save(Server / "result.json", Result)
        Endpoints = {"CLIENT": {**ClientResult, "RunId": Run, "Type": "CLIENT_DONE"},
                     "SERVER": {**Result, "RunId": Run, "Type": "SERVER_DONE"}}
        CoordinatorResult = {"Success": True, "Classification": "FOUR_CLIENT_PHASE1_ONLY", "Endpoints": Endpoints}
        Save(Coordinator / "result.json", CoordinatorResult)
        Journal = [{**V, "Event": "RECEIVE"} for V in Endpoints.values()]
        Journal += [{"Event": "SEND", "RunId": Run, "Type": "RUN_DONE", "Success": True}] * 2
        Journal += [{**CoordinatorResult, "Event": "RESULT"}]
        (Coordinator / "control.jsonl").write_text("\n".join(json.dumps(R) for R in Journal))
        def Evidence(File):
            return {"Path": str(File), "SHA256": Replay.Digest(File), "Bytes": File.stat().st_size}
        Outer = {"Success": True, "RunId": Lifecycle, "Results": [
            {"Success": True, "RunId": Lifecycle, "Type": "RESULT", "Role": "SERVER", "Evidence": [Evidence(Server / "result.json")]},
            {"Success": True, "RunId": Lifecycle, "Type": "RESULT", "Role": "CLIENT", "Evidence":
             [Evidence(Client / "result.json"), Evidence(Coordinator / "result.json")]}]}
        Save(Host / "result.json", Outer)
        Journal = [{"Event": "RECEIVE", "RunId": Lifecycle, "Type": "CLEANED", "Role": R, "Success": True}
                   for R in ("CLIENT", "SERVER")] + [{**Outer, "Event": "RESULT"}]
        (Host / "control.jsonl").write_text("\n".join(json.dumps(R) for R in Journal))
        Inputs["LifecycleQualification"] = str(self.Root / "qualification.json")
        Save(Path(Inputs["LifecycleQualification"]), {"Success": True, "LifecycleRunId": Lifecycle, "Codes": {"Host": 0},
             "Final": {R: {"RunId": Lifecycle, "Status": "IDLE", "Reason": "NONE"} for R in ("CLIENT", "SERVER")}})
        Inputs["Cleanup"] = str(self.Root / "cleanup.json")
        Save(Path(Inputs["Cleanup"]), {"PhysicalRunId": Run, "SshExited": True, "BothListenersClear": True,
                                     "StoppedUnixMs": 1_800_000_000_000})
        def Block(Kind, Body):
            Body += bytes((-len(Body)) % 4)
            return struct.pack("<II", Kind, len(Body) + 12) + Body + struct.pack("<I", len(Body) + 12)
        Capture = Block(0x0a0d0d0a, struct.pack("<IHHq", 0x1a2b3c4d, 1, 0, -1))
        Capture += Block(1, struct.pack("<HHI", 1, 0, 65535))
        import ipaddress
        for Port in range(60001, 60005):
            for Reverse in (False, True):
                SourceIp, DestIp = ("10.253.3.2", "10.253.3.1") if Reverse else ("10.253.3.1", "10.253.3.2")
                Ip = bytes([0x45, 0]) + struct.pack(">HHHBBH", 28, 0, 0, 64, 17, 0)
                Ip += ipaddress.IPv4Address(SourceIp).packed + ipaddress.IPv4Address(DestIp).packed
                Udp = struct.pack(">HHHH", 39450 if Reverse else Port, Port if Reverse else 39450, 8, 0)
                Frame = bytes(12) + b"\x08\x00" + Ip + Udp
                Capture += Block(6, struct.pack("<IIIII", 0, 0, 0, len(Frame), len(Frame)) + Frame)
        (Client / "client.pcapng").write_bytes(Capture)
        (Server / "worker-capture.pcapng").write_bytes(Capture)
        (Server / "worker-capture.etl").write_bytes(b"fixture ETL")
        (Server / "capture-summary.txt").write_text("Total Events Lost 0\n")
        Save(Server / "CaptureStop.log", {"Success": True, "Operation": "stop", "RunId": Run, "State": "stopped"})
        Save(Server / "netsh-owner.json", {"TraceMaximumMiB": 256, "NoWrapThresholdMiB": 240,
             "MiniportIfIndex": 19, "CapturePort": 39450, "CaptureLayers": ["NDIS physical miniport"]})
        (Client / "capture.log").write_text("Packets received/dropped on interface 'test': 8/0 (pcap:0/dumpcap:0/flushed:0/ps_ifdrop:0)\n")
        for Root, Value in ((Client, ClientResult), (Server, Result)):
            Journal = [{"Event": "RESULT", **Value}, {"Event": "LOCAL_CLEANUP", "Errors": []},
                       {"Event": "CAPTURE_TUPLES", "Tuples": {str(P): {"Inbound": 1, "Outbound": 1} for P in range(60001, 60005)}},
                       {"Event": "PROVENANCE", "ArtifactSHA256": Source["ProbeSHA256"], "BaseHead": Source["BaseHead"],
                        "OverlaySHA256": Source["OverlayArchiveSha256"], "GnsPin": Source["GnsPin"]},
                       {"Event": "RECEIVE", "Type": "RUN_DONE", "Success": True, "RunId": Run},
                       {"Event": "STATE", "Value": "COMPLETE"}]
            (Root / "control.jsonl").write_text("\n".join(json.dumps(R) for R in Journal))
        self.SealInputs(Inputs)
        return Inputs

    def WriteAdmissions(self, File):
        with File.open("w", newline="") as Stream:
            Writer = csv.writer(Stream)
            Writer.writerow(["slot", "generation", "token", "bytes", "activated_us"])
            for Wave in range(32):
                for Slot in range(1, 5):
                    Writer.writerow([Slot, 1, Wave * 4 + Slot, Replay.GROUP, 1_000_000 + Wave * 300_000])

    def SealInputs(self, Inputs):
        for Key in ("ClientRoot", "ServerRoot", "CoordinatorRoot", "LifecycleHostRoot"):
            Root = Path(Inputs[Key])
            Rows = [{"Path": F.name, "SHA256": Replay.Digest(F), "Bytes": F.stat().st_size} for F in Root.iterdir()
                    if F.name != "evidence-manifest.json"]
            (Root / "evidence-manifest.json").write_text(json.dumps({"Files": Rows}))

    def FullReplay(self, Inputs):
        # Exercise the production tuple parser without loading unrelated live
        # coordinator dependency startup. Only the archive verifier is mocked:
        # its owned-tree validation has its own exhaustive source-package suite.
        import ipaddress
        Source = ast.parse((Replay.TOOLS / "qualifier.py").read_text())
        Function = next(N for N in Source.body if isinstance(N, ast.FunctionDef) and N.name == "CaptureDirections")
        Module = types.ModuleType("qualifier")
        Module.__dict__.update(ipaddress=ipaddress, struct=struct, CLIENT_ADDRESS="10.253.3.1",
                               SERVER_ADDRESS="10.253.3.2", SERVER_PORT=39450)
        exec(compile(ast.Module(body=[Function], type_ignores=[]), "qualifier.py", "exec"), Module.__dict__)
        sys.path.insert(0, str(Replay.TOOLS))
        import f1_candidate_source
        with patch.object(f1_candidate_source, "VerifyArchive") as Verify, patch.dict(sys.modules, {"qualifier": Module}):
            Result = Replay.Replay(Inputs, "a" * 40)
            Verify.assert_called_once()
        return Result

    def test_full_raw_receipt_chain(self):
        Result = self.FullReplay(self.FullFixture())
        self.assertEqual(Result["State"], "MEASURED_PASS")
        self.assertEqual(Result["Native"]["MaximumGrants"], 128)
        self.assertEqual(len(Result["Capture"]["SERVER"]["Tuples"]), 4)
        self.assertNotEqual(Result["RunId"], Result["LifecycleRunId"])
        self.assertTrue(Result["CompletedUtc"].endswith("+00:00"))
        self.assertEqual(Result["CompletionClockDomain"], "CONTROLLING_HOST_UTC")
        self.assertEqual(Result["CompletedUtc"], "2027-01-15T08:00:00+00:00")

    def test_validly_resealed_capture_loss_fails(self):
        Inputs = self.FullFixture()
        for Key, Name, Bad in (("ClientRoot", "capture.log", "Packets received/dropped on interface 'test': 8/1 (pcap:1/dumpcap:0/flushed:0/ps_ifdrop:0)\n"),
                               ("ServerRoot", "capture-summary.txt", "Total Events Lost 1\n")):
            File = Path(Inputs[Key]) / Name
            Original = File.read_text()
            File.write_text(Bad)
            self.SealInputs(Inputs)
            with self.subTest(Name=Name), self.assertRaisesRegex(ValueError, "loss"):
                self.FullReplay(Inputs)
            File.write_text(Original)

    def test_full_replay_raw_pin_source_cleanup_and_lifecycle_failures(self):
        Inputs = self.FullFixture()
        Mutated = copy.deepcopy(Inputs)
        Mutated["RawCsvSha256"]["Server"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "CSV pin"):
            self.FullReplay(Mutated)
        for File, Key, Value in ((Path(Inputs["Cleanup"]), "BothListenersClear", False),
                                  (Path(Inputs["LifecycleQualification"]), "Success", False),
                                  (Path(Inputs["CandidateRoot"]) / "source-manifest.json", "BaseHead", "b" * 40)):
            Original = File.read_text()
            Data = json.loads(Original)
            Data[Key] = Value
            File.write_text(json.dumps(Data))
            with self.subTest(Key=Key), self.assertRaises(ValueError):
                self.FullReplay(Inputs)
            File.write_text(Original)

    def test_missing_cleaned_event_cannot_be_replaced_by_success_summary(self):
        Inputs = self.FullFixture()
        File = Path(Inputs["LifecycleHostRoot"])/"control.jsonl"
        File.write_text("\n".join(File.read_text().splitlines()[1:]))
        self.SealInputs(Inputs)
        with self.assertRaisesRegex(ValueError, "CLEANED"):
            self.FullReplay(Inputs)

    def test_admission_activation_cannot_be_moved_or_reclassified(self):
        Inputs = self.FullFixture()
        File = Path(Inputs["AdmissionsCsv"])
        Original = File.read_text()
        for Replacement in ("1000001", str(2**64 - 1), "0"):
            File.write_text(Original.replace("1000000", Replacement))
            Inputs["RawCsvSha256"]["Admissions"] = Replay.Digest(File)
            shutil.copyfile(File, Path(Inputs["ServerRoot"]) / "physical-gns-server-admissions.csv")
            self.SealInputs(Inputs)
            with self.subTest(Activation=Replacement), self.assertRaises(ValueError):
                self.FullReplay(Inputs)

    def test_missing_or_duplicate_admission_evidence_fails(self):
        Inputs = self.FullFixture()
        File = Path(Inputs["AdmissionsCsv"])
        Lines = File.read_text().splitlines()
        for Changed in (Lines[:1] + Lines[2:], Lines + Lines[1:2]):
            File.write_text("\n".join(Changed) + "\n")
            Inputs["RawCsvSha256"]["Admissions"] = Replay.Digest(File)
            shutil.copyfile(File, Path(Inputs["ServerRoot"]) / "physical-gns-server-admissions.csv")
            self.SealInputs(Inputs)
            with self.assertRaises(ValueError):
                self.FullReplay(Inputs)


if __name__ == "__main__":
    unittest.main()
