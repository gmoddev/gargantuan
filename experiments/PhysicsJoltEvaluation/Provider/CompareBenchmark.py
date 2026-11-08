"""Compare provider benchmark observations without ranking unmatched materials."""

import argparse
import json
import math
from pathlib import Path


COMMON_FIELDS = (
    "mean_y",
    "center_y",
    "max_pin_error",
    "mean_edge_strain",
    "max_edge_strain",
)
RUBBER_FIELDS = (
    "mean_volume_ratio",
    "mean_tetra_volume_ratio",
    "max_tetra_volume_error",
    "mean_diagonal_strain",
)


def ReadSummary(PathValue: Path) -> dict[str, str]:
    Lines = PathValue.read_text(encoding="utf-8").splitlines()
    Prefix = "[Physics:ProviderBenchmark] summary "
    Matches = [Line[len(Prefix) :] for Line in Lines if Line.startswith(Prefix)]
    if len(Matches) != 1:
        raise ValueError(f"{PathValue}: expected exactly one complete summary")
    Summary = dict(Token.split("=", 1) for Token in Matches[0].split())
    for Field in COMMON_FIELDS + RUBBER_FIELDS + ("mean_total_ms",):
        if Field not in Summary or not math.isfinite(float(Summary[Field])):
            raise ValueError(f"{PathValue}: missing or nonfinite {Field}")
    return Summary


def Main() -> None:
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("jolt_log", type=Path)
    Parser.add_argument("xpbd_log", type=Path)
    Parser.add_argument("--envelope", type=Path,
                        help="JSON maximum pair errors and absolute physical ranges for each quality field")
    Arguments = Parser.parse_args()
    Jolt = ReadSummary(Arguments.jolt_log)
    Xpbd = ReadSummary(Arguments.xpbd_log)
    if Jolt.get("backend") != "jolt" or Xpbd.get("backend") != "xpbd":
        raise ValueError("Log arguments must be Jolt then XPBD")
    for Field in ("scene", "coupling", "frames", "warmup_frames"):
        if Jolt.get(Field) != Xpbd.get(Field):
            raise ValueError(f"Mismatched {Field}: {Jolt.get(Field)} vs {Xpbd.get(Field)}")
    if Jolt.get("warmup_frames") is None:
        raise ValueError("Both logs need an explicit warmup frame count")
    Rubber = Jolt["scene"].startswith("rubber")
    Coupled = Jolt["coupling"] == "static-sphere"
    Fields = COMMON_FIELDS + (RUBBER_FIELDS if Rubber else ())
    if Coupled:
        Fields += ("max_poststep_penetration", "final_min_surface_distance")
    Errors = {Field: abs(float(Jolt[Field]) - float(Xpbd[Field])) for Field in Fields}
    BudgetMatched = int(Jolt["workers_requested"]) == int(Xpbd["workers_requested"]) == \
        int(Xpbd["xpbd_workers_actual"])
    ReceiptPairMatched = all(Jolt.get(Field) == Xpbd.get(Field) for Field in
                             ("host", "os", "build_stamp", "binary_id", "run_pair")) and \
        Jolt.get("host") not in (None, "unknown") and \
        Jolt.get("binary_id") not in (None, "unknown") and \
        Jolt.get("run_pair") not in (None, "unpaired") and \
        Jolt.get("timing_instrumented") == "0" and \
        Xpbd.get("timing_instrumented") == "0"
    Result: dict[str, object] = {
        "scene": Jolt["scene"],
        "coupling": Jolt["coupling"],
        "frames": int(Jolt["frames"]),
        "warmup_frames": int(Jolt["warmup_frames"]),
        "timing_instrumented": (Jolt.get("timing_instrumented") != "0" or
                                Xpbd.get("timing_instrumented") != "0"),
        "jolt_workers_requested": int(Jolt["workers_requested"]),
        "xpbd_workers_requested": int(Xpbd["workers_requested"]),
        "xpbd_workers_actual": int(Xpbd["xpbd_workers_actual"]),
        "xpbd_jobs_dispatched": int(Xpbd["xpbd_jobs_dispatched"]),
        "xpbd_mean_worker_utilization": float(Xpbd["xpbd_mean_worker_utilization"]),
        "jolt_constraints": int(Jolt["constraints"]),
        "xpbd_constraints": int(Xpbd["constraints"]),
        "worker_count_matched": BudgetMatched,
        "receipt_pair_matched": ReceiptPairMatched,
        "jolt_host": Jolt.get("host"),
        "xpbd_host": Xpbd.get("host"),
        "jolt_build_stamp": Jolt.get("build_stamp"),
        "xpbd_build_stamp": Xpbd.get("build_stamp"),
        "jolt_binary_id": Jolt.get("binary_id"),
        "xpbd_binary_id": Xpbd.get("binary_id"),
        "run_pair": Jolt.get("run_pair"),
        "material_topology_comparable": not Rubber and Jolt["constraints"] == Xpbd["constraints"],
        "jolt_mean_total_ms": float(Jolt["mean_total_ms"]),
        "xpbd_mean_total_ms": float(Xpbd["mean_total_ms"]),
        "quality_absolute_errors": Errors,
        "quality_comparable": None,
        "timing_winner": None,
    }
    if Arguments.envelope:
        Envelope = json.loads(Arguments.envelope.read_text(encoding="utf-8"))
        if not isinstance(Envelope, dict):
            raise ValueError("Envelope must be a JSON object")
        for Field in Fields:
            if Field not in Envelope or not isinstance(Envelope[Field], (float, int)) or \
                    not math.isfinite(Envelope[Field]) or Envelope[Field] < 0:
                raise ValueError(f"Envelope needs a finite nonnegative {Field} threshold")
        AbsoluteRanges = Envelope.get("absolute_ranges")
        if not isinstance(AbsoluteRanges, dict):
            raise ValueError("Envelope needs absolute_ranges for every quality field")
        for Field in Fields:
            Range = AbsoluteRanges.get(Field)
            if not isinstance(Range, list) or len(Range) != 2 or \
                    any(not isinstance(Value, (int, float)) or
                        not math.isfinite(Value) for Value in Range) or Range[0] > Range[1]:
                raise ValueError(f"Envelope needs a finite [minimum, maximum] absolute range for {Field}")
        AbsoluteQualityPassed = all(
            AbsoluteRanges[Field][0] <= float(Summary[Field]) <= AbsoluteRanges[Field][1]
            for Field in Fields for Summary in (Jolt, Xpbd)
        )
        ContactBandReached = not Coupled or (
            int(Jolt["observed_contact_band"]) == 1 and
            int(Xpbd["observed_contact_band"]) == 1 and
            float(Jolt["initial_min_surface_distance"]) > 0 and
            float(Xpbd["initial_min_surface_distance"]) > 0
        )
        if Coupled and ("maximum_allowed_penetration" not in Envelope or
                        not isinstance(Envelope["maximum_allowed_penetration"], (int, float)) or
                        not math.isfinite(Envelope["maximum_allowed_penetration"]) or
                        Envelope["maximum_allowed_penetration"] < 0):
            raise ValueError("Coupled envelope needs finite nonnegative maximum_allowed_penetration")
        PenetrationBounded = not Coupled or (
            float(Jolt["max_poststep_penetration"]) <= Envelope["maximum_allowed_penetration"] and
            float(Xpbd["max_poststep_penetration"]) <= Envelope["maximum_allowed_penetration"]
        )
        Comparable = all(Errors[Field] <= Envelope[Field] for Field in Fields) and \
            AbsoluteQualityPassed and ContactBandReached and PenetrationBounded
        Result["quality_envelope"] = {Field: Envelope[Field] for Field in Fields}
        Result["absolute_quality_ranges"] = {
            Field: AbsoluteRanges[Field] for Field in Fields}
        Result["absolute_quality_passed"] = AbsoluteQualityPassed
        Result["contact_band_reached"] = ContactBandReached
        Result["penetration_bounded"] = PenetrationBounded
        Result["quality_comparable"] = Comparable
        if Comparable and BudgetMatched and ReceiptPairMatched and \
                Result["material_topology_comparable"] and not Coupled:
            JoltTime = float(Jolt["mean_total_ms"])
            XpbdTime = float(Xpbd["mean_total_ms"])
            Result["timing_winner"] = "jolt" if JoltTime < XpbdTime else \
                "xpbd" if XpbdTime < JoltTime else "tie"
    print(json.dumps(Result, indent=2, sort_keys=True))


if __name__ == "__main__":
    Main()
