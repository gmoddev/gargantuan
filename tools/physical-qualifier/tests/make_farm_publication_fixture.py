"""Small 32-client binary fixture for the offline PowerShell adoption tests."""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from farm_publication_trace import RECORD


def Record(Stage, Ns, CS=0, CG=0, OS=0, OG=0, Bytes=0,
           Tick=101, Seq=0, Due=0, Control=0, Material=0, Frame=0):
    return RECORD.pack(Stage, 0, CS, CG, OS, OG, Bytes, Ns,
                       Tick, Seq, Due, Control, Material, Frame)


def Trace(PathValue, RunId, Role, Slot, Nonce, Records):
    Header = (f"format=GargantuanFarmPublicationV1\trun={RunId}\trole={Role}"
              f"\tslot={Slot}\tnonce={Nonce}\tcount={len(Records)}"
              "\tdropped=0\tdecode_failures=0\n").encode("ascii")
    PathValue.write_bytes(Header + b"".join(Records))


def Main(ManifestPath, ServerRoot, ClientRoot):
    Manifest = json.loads(Path(ManifestPath).read_text(encoding="utf-8"))
    RunId = Manifest["RunId"]
    Nonces = Manifest["Nonces"]
    if len(Nonces) != 32:
        raise ValueError("fixture requires 32 client nonces")
    Server = [Record(1, 100, Tick=101)]
    for Slot, Nonce in enumerate(Nonces):
        CS, OS, Seq, Frame = Slot + 1, Slot + 100, Slot + 19, Slot + 11
        Ns = 200 + 10 * Slot
        Server += [Record(3, Ns, CS=CS, CG=1, OS=OS, OG=1,
                          Due=101),
                   Record(4, Ns + 1, OS=OS, OG=1, Seq=Seq, Control=3),
                   Record(5, Ns + 2, OS=OS, OG=1, Seq=Seq),
                   Record(6, Ns + 3, CS=CS, CG=1, OS=OS, OG=1,
                          Seq=Seq, Due=101, Material=7),
                   Record(8, Ns + 4, CS=CS, CG=1, OS=OS, OG=1,
                          Bytes=74, Seq=Seq, Control=3, Material=7, Frame=Frame)]
        Client = [Record(9, 1000, CS=Slot + 2, CG=1, OS=OS, OG=1,
                         Bytes=74, Seq=Seq, Control=3, Material=7, Frame=Frame),
                  Record(10, 1100, CS=Slot + 2, CG=1, OS=OS, OG=1,
                         Bytes=74, Seq=Seq, Control=3, Material=7, Frame=Frame)]
        Trace(Path(ClientRoot) / f"publication-service-{Slot}.bin",
              RunId, "CLIENT", Slot, Nonce, Client)
    Trace(Path(ServerRoot) / "publication-service.bin", RunId, "SERVER", -1, 0, Server)


if __name__ == "__main__":
    Main(*sys.argv[1:])
