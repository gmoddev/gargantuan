# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Compare two Release builds of SpatialRenderProjectionAblation.cpp on one logical CPU."""
import argparse
import csv
import hashlib
from pathlib import Path
import subprocess


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--reference", required=True, type=Path)
    Parser.add_argument("--candidate", required=True, type=Path)
    Parser.add_argument("--cpu", type=int, default=24)
    Parser.add_argument("--output", required=True, type=Path)
    Args = Parser.parse_args()
    Products = {"Reference": Args.reference.resolve(), "Candidate": Args.candidate.resolve()}
    Hashes = {Name: hashlib.sha256(PathValue.read_bytes()).hexdigest() for Name, PathValue in Products.items()}
    Fields = ["Block", "Position", "Cohort", "Sha256", "Scene", "Samples", "ElapsedNs",
              "ValidationNs", "MutationNs", "AllocationCalls", "RequestedBytes"]
    with Args.output.open("x", newline="", encoding="utf-8") as Output:
        Rows = csv.writer(Output)
        Rows.writerow(Fields)
        for Block in range(8):
            First = "Reference" if Block % 2 == 0 else "Candidate"
            Second = "Candidate" if Block % 2 == 0 else "Reference"
            for Position, Cohort in enumerate((First, Second, Second, First), 1):
                Result = subprocess.run([str(Products[Cohort]), str(Args.cpu)], text=True,
                                        capture_output=True, check=False)
                if Result.returncode:
                    raise RuntimeError(f"{Cohort} at block {Block + 1} position {Position}: {Result.stderr}")
                OutputRows = list(csv.reader(Result.stdout.splitlines()))
                if [Row[0] for Row in OutputRows] != ["Camera", "Transforms", "Resources"] or any(
                        len(Row) != 7 or int(Row[1]) != 400 for Row in OutputRows):
                    raise ValueError(f"Unexpected committed sample counts in {Result.stdout}")
                for Row in OutputRows:
                    Rows.writerow([Block + 1, Position, Cohort, Hashes[Cohort], *Row])
                Output.flush()


if __name__ == "__main__":
    Main()
