"""Append bounded clock metadata to offline fixtures, never a live farm log."""

import json
from pathlib import Path
import re
import sys


def Append(ManifestPath, ServerRoot, ClientRoot):
    Manifest = json.loads(Path(ManifestPath).read_text(encoding="utf-8-sig"))
    Run = Manifest["RunId"]
    ServerPath = Path(ServerRoot) / "server.stdout.log"
    Server = ServerPath.read_text(encoding="utf-8-sig").splitlines()
    Clients = {Slot: (Path(ClientRoot) / f"client-{Slot:02d}.stdout.log").read_text(
        encoding="utf-8-sig").splitlines() for Slot in range(32)}
    Fields = lambda Line: dict(re.findall(r"([a-z_]+)=([^\s]+)", Line))
    Connections = {}
    for Line in Server:
        if Line.startswith("[Qualification:Server] event=ready "):
            Row = Fields(Line)
            Connections[(int(Row["nonce"]) & 0xffffffff) - 1] = (
                Row["connection_slot"], Row["connection_generation"])
    for Slot in range(32):
        if Slot not in Connections:
            Connections[Slot] = (str(Slot + 1), "1")
            Server.append(f"[Qualification:Server] event=ready run={Run} nonce={Manifest['Nonces'][Slot]} "
                          f"connection_slot={Slot + 1} connection_generation=1")
        if not any(Line.startswith("[Qualification:Client] event=ready ") for Line in Clients[Slot]):
            Clients[Slot].append(f"[Qualification:Client] event=ready run_id={Run} slot={Slot} "
                                 f"nonce={Manifest['Nonces'][Slot]}")
    def Native(Role, Slot, Epoch, Sequence, Stage, Stamp, Kind, Result=-1):
        CS, CG = Connections[Slot] if Role == "server" else ("1", "1")
        return (f"[Qualification:FarmClock] event=native run={Run} role={Role} "
                f"slot={-1 if Role == 'server' else Slot} epoch={Epoch} sequence={Sequence} "
                f"request={(Epoch - 1) * 4 + Sequence} connection_slot={CS} connection_generation={CG} "
                f"stage={Stage} monotonic_ns={Stamp} kind={Kind} bytes=80 result={Result}")
    for Epoch, Phase in enumerate(("baseline", "load", "resident", "evict", "reload"), 1):
        Starts = [Fields(Line) for Line in Server if Line.startswith(
            "[Qualification:Scale] event=phase_start ") and Fields(Line).get("phase") == Phase]
        if Starts:
            Start = int(Starts[0]["monotonic_us"])
        else:
            Start = Epoch * 1000000
            Server.extend([f"[Qualification:Scale] event=phase_start run={Run} phase={Phase} tick={Epoch * 1000} monotonic_us={Start}",
                           f"[Qualification:Scale] event=phase_end run={Run} phase={Phase} tick={Epoch * 1000 + 500} monotonic_us={Start + 500000}"])
        Server.extend([
            f"[Qualification:FarmClock] event=calibration_start run={Run} epoch={Epoch} next_phase={Phase} monotonic_us={Start - 900}",
            f"[Qualification:FarmClock] event=quiesce_complete run={Run} epoch={Epoch} count=32 monotonic_us={Start - 800}",
            f"[Qualification:FarmClock] event=calibration_complete run={Run} epoch={Epoch} count=32"])
        for Slot in range(32):
            for Sequence in range(1, 5):
                Base = (Start - 700) * 1000 + Slot * 1000 + Sequence * 100
                Server.extend([Native("server", Slot, Epoch, Sequence, "GnsReceive", Base + 100, 3),
                               Native("server", Slot, Epoch, Sequence, "GnsBefore", Base + 200, 4),
                               Native("server", Slot, Epoch, Sequence, "GnsQueued", Base + 220, 4, 1)])
                Clients[Slot].extend([Native("client", Slot, Epoch, Sequence, "GnsBefore", Base + 50, 3),
                                      Native("client", Slot, Epoch, Sequence, "GnsQueued", Base + 70, 3, 1),
                                      Native("client", Slot, Epoch, Sequence, "GnsReceive", Base + 300, 4)])
    Server.append(f"[Qualification:FarmClock] event=terminal run={Run} role=server slot=-1 records=1920 overflow=0 invalid_decode=0")
    ServerPath.write_text("\n".join(Server) + "\n", encoding="utf-8")
    for Slot, Lines in Clients.items():
        Lines.append(f"[Qualification:FarmClock] event=terminal run={Run} role=client slot={Slot} records=60 overflow=0 invalid_decode=0")
        (Path(ClientRoot) / f"client-{Slot:02d}.stdout.log").write_text("\n".join(Lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    Append(*sys.argv[1:])
