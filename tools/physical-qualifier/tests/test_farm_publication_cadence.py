import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from farm_publication_join import (
    CreateDatabase, PhaseWindows, RelationshipCadence, RootMarkers,
)


RUN = "run-a"
PHASES = ("baseline", "load", "resident", "evict", "reload")
WINDOWS = [{"Phase": Phase, "StartTick": 100 + 20 * Index,
            "EndTick": 104 + 20 * Index}
           for Index, Phase in enumerate(PHASES)]


class FarmPublicationCadenceTests(unittest.TestCase):
    def setUp(self):
        self.Temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.Temp.cleanup)
        self.Root = Path(self.Temp.name)
        self.Log = self.Root / "server.stdout.log"
        self.Database = CreateDatabase(self.Root / "cadence.sqlite3")
        self.addCleanup(self.Database.close)

    def AddState(self, Slot, Object, Tick, Nanoseconds, Lifetime=0):
        self.Database.execute("""INSERT INTO Produced
            (CS,CG,OS,OG,Tick,Seq,Material,Control,Forced,Origin,
             BuiltNs,ProducedNs,AcceptedNs,HandledNs,ClientSlot,Lifetime)
            VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            (Slot + 1, 1, Object, 1, Tick, Tick, 1, 1, 0, 0,
             Nanoseconds - 2, Nanoseconds - 1, Nanoseconds - 1,
             Nanoseconds, Slot, Lifetime))

    def Markers(self):
        Ready = {Slot: ((Slot + 1, 1), (1, 1)) for Slot in range(32)}
        Lines = []
        for Index in range(8):
            Owner = Index * 4
            First = Owner // 8 * 8
            Lines.append(
                f"[Qualification:Scale] event=tracked_root run={RUN} index={Index} "
                f"client_index={Owner} peer_slot={Owner + 1} peer_generation=1 "
                f"neighborhood={Owner // 8 + 1} recipient_first={First} "
                f"recipient_last={First + 7} object_slot={100 + Index} "
                "object_generation=1 tick=50\n")
        self.Log.write_text("".join(Lines), encoding="utf-8")
        return Ready, Lines

    def test_five_phase_tick_boundaries_are_strict(self):
        Lines = []
        for Index, Phase in enumerate(PHASES):
            Start = 100 + Index * 20
            Lines += [
                f"[Qualification:Scale] event=phase_start run={RUN} phase={Phase} "
                f"tick={Start} monotonic_us={Start * 1000}\n",
                f"[Qualification:Scale] event=phase_end run={RUN} phase={Phase} "
                f"tick={Start + 4} monotonic_us={(Start + 4) * 1000}\n",
            ]
        self.Log.write_text("".join(Lines), encoding="utf-8")
        self.assertEqual(PhaseWindows(self.Log, RUN), WINDOWS)
        self.Log.write_text("".join(Lines[:-1]), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "incomplete"):
            PhaseWindows(self.Log, RUN)
        self.Log.write_text("".join(Lines + [Lines[0]]), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate"):
            PhaseWindows(self.Log, RUN)

    def test_unmarked_relationship_reports_local_gaps_and_phase_edges_without_gate(self):
        for Tick, Ns in ((99, 100), (100, 200), (102, 400), (104, 500), (105, 700)):
            self.AddState(0, 100, Tick, Ns)
        Result = RelationshipCadence(self.Database, WINDOWS, None, 32)
        self.assertEqual(Result["RootIdentity"], "NOT_MEASURED")
        self.assertEqual(Result["CanonicalVerdict"], "NOT_MEASURED")
        self.assertEqual(Result["RelationshipCount"], 1)
        Baseline = Result["Relationships"][0]["Phases"][0]
        self.assertEqual(Baseline["States"], 3)
        self.assertEqual((Baseline["WithinPairs"], Baseline["EntryPairs"],
                          Baseline["ExitPairs"]), (2, 1, 1))
        self.assertEqual(Baseline["MaximumHandledGapNs"], 200)
        self.assertEqual(Baseline["MaximumTickDelta"], 2)
        self.AddState(0, 100, 101, 450)
        Result = RelationshipCadence(self.Database, WINDOWS, None, 32)
        self.assertEqual(Result["Relationships"][0]["StaleHandled"], 1)
        self.assertEqual(Result["Relationships"][0]["Phases"][0]["States"], 3)
        self.assertEqual(Result["Relationships"][0]["Phases"][0]["MaximumHandledGapNs"], 200)

    def test_root_marker_requires_eight_full_identities_and_ready_owner(self):
        Ready, Lines = self.Markers()
        self.Log.write_text("unrelated server log\n", encoding="utf-8")
        self.assertIsNone(RootMarkers(self.Log, RUN, 32, Ready))
        self.Log.write_text("".join(Lines), encoding="utf-8")
        Roots = RootMarkers(self.Log, RUN, 32, Ready)
        self.assertEqual(len(Roots), 8)
        self.assertEqual(Roots[(100, 1)]["RecipientSlots"], tuple(range(8)))
        self.Log.write_text("".join(Lines[:-1]), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "eight active roots"):
            RootMarkers(self.Log, RUN, 32, Ready)
        self.Log.write_text("".join(Lines[:-1] + [Lines[0]]), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "identity"):
            RootMarkers(self.Log, RUN, 32, Ready)

    def test_retired_relationship_lifetime_does_not_create_cross_reentry_gap(self):
        self.AddState(0, 100, 100, 100, Lifetime=0)
        self.AddState(0, 100, 102, 200, Lifetime=0)
        self.AddState(0, 100, 103, 900_000_000, Lifetime=1)
        self.AddState(0, 100, 104, 900_000_100, Lifetime=1)
        Result = RelationshipCadence(self.Database, WINDOWS, None, 32)
        self.assertEqual(Result["RelationshipCount"], 2)
        self.assertTrue(all(Row["Phases"][0]["MaximumHandledGapNs"] == 100
                            for Row in Result["Relationships"]))

    def test_complete_64_root_relationships_apply_canonical_limits(self):
        Roots = {(100 + Index, 1): {
            "OwnerSlot": Index * 4,
            "RecipientSlots": tuple(range(Index // 2 * 8, Index // 2 * 8 + 8))}
            for Index in range(8)}
        for (Object, _), Marker in Roots.items():
            for Slot in Marker["RecipientSlots"]:
                for Window in WINDOWS:
                    for Tick in (Window["StartTick"] - 1, Window["StartTick"],
                                 Window["StartTick"] + 2, Window["EndTick"],
                                 Window["EndTick"] + 1):
                        self.AddState(Slot, Object, Tick, Tick * 1_000_000)
        Result = RelationshipCadence(self.Database, WINDOWS, Roots, 32)
        self.assertEqual(Result["RootCoverage"], "OBSERVED")
        self.assertEqual(Result["CanonicalVerdict"], "PASS")
        self.assertEqual(Result["RelationshipCount"], 64)
        self.Database.execute("""UPDATE Produced SET HandledNs=HandledNs+300000000
            WHERE CS=1 AND OS=100 AND Tick>=102""")
        Result = RelationshipCadence(self.Database, WINDOWS, Roots, 32)
        self.assertEqual(Result["CanonicalVerdict"], "FAIL")
        self.assertEqual(Result["CanonicalViolations"][0]["Phase"], "baseline")
        self.Database.execute("""UPDATE Produced SET HandledNs=HandledNs-300000000
            WHERE CS=1 AND OS=100 AND Tick>=102""")
        self.Database.execute("""UPDATE Produced SET HandledNs=HandledNs+300000000
            WHERE CS=1 AND OS=100 AND Tick>=100""")
        Result = RelationshipCadence(self.Database, WINDOWS, Roots, 32)
        self.assertEqual(Result["CanonicalVerdict"], "NOT_MEASURED")
        self.assertTrue(Result["AmbiguousBoundaryGaps"])


if __name__ == "__main__":
    unittest.main()
