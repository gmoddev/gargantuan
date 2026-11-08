"""Compare provider benchmark observations without ranking unmatched materials."""

import argparse
import json
import math
from pathlib import Path


QUALITY_FIELDS = (
    "mean_y",
    "center_y",
    "max_pin_error",
    "mean_edge_strain",
    "max_edge_strain",
    "mean_volume_ratio",
)


def ReadSummary(PathValue: Path) -> dict[str, str]:
    Lines = PathValue.read_text(encoding="utf-8").splitlines()
    Prefix = "[Physics:ProviderBenchmark] summary "
    Matches = [Line[len(Prefix) :] for Line in Lines if Line.startswith(Prefix)]
    if len(Matches) != 1:
        raise ValueError(f"{PathValue}: expected exactly one complete summary")
    Summary = dict(Token.split("=", 1) for Token in Matches[0].split())
    for Field in QUALITY_FIELDS + ("mean_total_ms",):
        if Field not in Summary or not math.isfinite(float(Summary[Field])):
            raise ValueError(f"{PathValue}: missing or nonfinite {Field}")
    return Summary


def Main() -> None:
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("jolt_log", type=Path)
    Parser.add_argument("xpbd_log", type=Path)
    Parser.add_argument("--envelope", type=Path,
                        help="JSON object of maximum absolute errors for every relevant quality field")
    Arguments = Parser.parse_args()
    Jolt = ReadSummary(Arguments.jolt_log)
    Xpbd = ReadSummary(Arguments.xpbd_log)
    if Jolt.get("backend") != "jolt" or Xpbd.get("backend") != "xpbd":
        raise ValueError("Log arguments must be Jolt then XPBD")
    for Field in ("scene", "coupling", "frames"):
        if Jolt.get(Field) != Xpbd.get(Field):
            raise ValueError(f"Mismatched {Field}: {Jolt.get(Field)} vs {Xpbd.get(Field)}")
    Fields = QUALITY_FIELDS if Jolt["scene"].startswith("rubber") else QUALITY_FIELDS[:-1]
    Errors = {Field: abs(float(Jolt[Field]) - float(Xpbd[Field])) for Field in Fields}
    BudgetMatched = int(Jolt["workers_requested"]) == int(Xpbd["workers_requested"]) == \
        int(Xpbd["xpbd_workers_actual"])
    Result: dict[str, object] = {
        "scene": Jolt["scene"],
        "coupling": Jolt["coupling"],
        "frames": int(Jolt["frames"]),
        "jolt_workers_requested": int(Jolt["workers_requested"]),
        "xpbd_workers_requested": int(Xpbd["workers_requested"]),
        "xpbd_workers_actual": int(Xpbd["xpbd_workers_actual"]),
        "worker_count_matched": BudgetMatched,
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
        Comparable = all(Errors[Field] <= Envelope[Field] for Field in Fields)
        Result["quality_envelope"] = {Field: Envelope[Field] for Field in Fields}
        Result["quality_comparable"] = Comparable
        if Comparable and BudgetMatched:
            JoltTime = float(Jolt["mean_total_ms"])
            XpbdTime = float(Xpbd["mean_total_ms"])
            Result["timing_winner"] = "jolt" if JoltTime < XpbdTime else \
                "xpbd" if XpbdTime < JoltTime else "tie"
    print(json.dumps(Result, indent=2, sort_keys=True))


if __name__ == "__main__":
    Main()
