# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Prove the original fixed schedule prefix survived a startup failure and hash its completed suffix."""
import argparse
import csv
from datetime import datetime, timezone
import gzip
import hashlib
import io
import json
from pathlib import Path
import shutil
import zipfile


def Digest(PathValue):
    with PathValue.open("rb") as Stream:
        return hashlib.file_digest(Stream, "sha256").hexdigest()


def Load(PathValue):
    Opener = gzip.open if PathValue.suffix == ".gz" else open
    with Opener(PathValue, "rt", encoding="utf-8", newline="") as Stream:
        return list(csv.DictReader(Stream))


def WriteSuffix(PathValue, Rows):
    def Table(Text):
        Writer = csv.DictWriter(Text, fieldnames=list(Rows[0]))
        Writer.writeheader()
        Writer.writerows(Rows)
    if PathValue.suffix == ".gz":
        with PathValue.open("xb") as Raw, gzip.GzipFile(filename="", fileobj=Raw, mode="wb", mtime=0) as Compressed:
            with io.TextIOWrapper(Compressed, encoding="utf-8", newline="") as Text:
                Table(Text)
    else:
        with PathValue.open("x", encoding="utf-8", newline="") as Text:
            Table(Text)


def UtcTimestamp(Value):
    return datetime.fromtimestamp(Value, timezone.utc).isoformat()


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    for Field in ("prior", "combined", "log", "output"):
        Parser.add_argument("--"+Field, type=Path, required=True)
    Args = Parser.parse_args()
    Events = [json.loads(Line) for Line in Args.log.read_text(encoding="utf-8").splitlines()]
    if (Events[0]["Event"] != "Resume" or Events[1]["Event"] != "FirstUnsampledPosition" or
            Events[-1]["Event"] != "Complete" or any(E["Event"] != "FailedBeforeWarmup" for E in Events[2:-1])):
        raise ValueError("Unexpected completion or recovery events")
    Prefix = Events[0]["Prefix"]
    End = Events[-1]["Positions"]
    if not 0 < Prefix < End or Events[1]["Ordinal"] != Prefix+1:
        raise ValueError("Invalid schedule cursor")
    Args.output.mkdir(parents=True, exist_ok=False)
    Receipt = dict(PrefixPositions=Prefix, SuffixPositions=End-Prefix, TotalPositions=End, Files={})
    NextPositions = []
    for Name in ("Processes.csv", "Ticks.csv.gz"):
        PriorFile = Args.prior/Name
        CombinedFile = Args.combined/Name
        Previous, Combined = Load(PriorFile), Load(CombinedFile)
        if (len(Previous) != Prefix or len(Combined) != End or Previous != Combined[:Prefix] or
                Digest(PriorFile) != Events[0]["PriorFiles"][Name]):
            raise ValueError("Previously recorded fields differ from the complete schedule")
        Suffix = Combined[Prefix:]
        NextPositions.append([Suffix[0][K] for K in ("Block", "Run", "Profile")])
        shutil.copyfile(PriorFile, Args.output/("Prefix"+Name))
        WriteSuffix(Args.output/("Suffix"+Name), Suffix)
        if Load(Args.output/("Suffix"+Name)) != Suffix:
            raise ValueError("Archived recovery suffix differs")
        Stat = PriorFile.stat()
        Created = getattr(Stat, "st_birthtime", None)
        Receipt["Files"][Name] = dict(PrefixSha256=Digest(Args.output/("Prefix"+Name)),
                                     SuffixSha256=Digest(Args.output/("Suffix"+Name)),
                                     CombinedAcquisitionSha256=Digest(CombinedFile),
                                     PrefixCreatedUtc=UtcTimestamp(Created) if Created is not None else None,
                                     PrefixLastWriteUtc=UtcTimestamp(Stat.st_mtime))
    if NextPositions[0] != NextPositions[1]:
        raise ValueError("Recovered process and native observations start at different positions")
    Block, Run, Profile = NextPositions[0]
    if Events[1]["Position"] != "B"+Block+"R"+Run:
        raise ValueError("Recorded retry position differs from the first completed suffix row")
    Receipt["FirstRecoveredPosition"] = dict(Block=int(Block), Run=int(Run), Profile=Profile)
    Receipt["Events"] = Events
    # Preserve Windows CSV line endings and the original gzip header byte for byte,
    # independent of repository text normalization on a receiving platform.
    Packet = Args.output/"RawBlocks.zip"
    Names = [Arm+Name for Arm in ("Prefix", "Suffix") for Name in ("Processes.csv", "Ticks.csv.gz")]
    with zipfile.ZipFile(Packet, "x", compression=zipfile.ZIP_DEFLATED) as Items:
        for Name in Names:
            Items.write(Args.output/Name, arcname=Name)
    with zipfile.ZipFile(Packet) as Items:
        for Name in Names:
            File = Args.output/Name
            if hashlib.sha256(Items.read(Name)).hexdigest() != Digest(File):
                raise ValueError("Recovery archive changed original bytes")
    for Name in Names:
        (Args.output/Name).unlink()
    Receipt["PacketSha256"] = Digest(Packet)
    with (Args.output/"Receipt.json").open("x", encoding="utf-8") as File:
        json.dump(Receipt, File, indent=2)
        File.write("\n")


if __name__ == "__main__":
    Main()
