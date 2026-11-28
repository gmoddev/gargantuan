"""Run the bounded procedural example through an existing qualified EditorHost."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import time

Root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Root / "tests"))
from SpatialConstructionOrdinaryFrames import EditorHost

Parser = argparse.ArgumentParser()
Parser.add_argument("--engine", type=Path, required=True)
Parser.add_argument("--output", type=Path, required=True)
Args = Parser.parse_args()
Source = (Root/"docs/examples/procedural-world.server.luau").read_text(encoding="utf-8")
Source += '\nlocal G=workspace:FindFirstChild("GeneratedFoundation")\nassert(G:GetBlock(Vector3.new(-8,0,-8)).Definition=="Stone")\nassert(G:GetBlock(Vector3.new(8,0,8))==nil)\nprint("[Docs:Generation] Verified native bulk fill and exclusive upper bound", workspace.Terrain:GetGridInfo().EditingAvailable)\n'
with tempfile.TemporaryDirectory(prefix="CreatorExample-") as Temp, EditorHost(Args.engine) as Host:
    Host.Call("Handshake")
    Parent = Host.Call("CreateProject", Destination=str(Path(Temp)/"Project"), Name="Creator example")["Root"]
    Host.Call("GetSnapshot")
    Schema = next(E for E in Host.Call("GetSchema")["Definitions"] if E["Kind"]=="Class" and E["Namespace"]=="Engine" and E["Name"]=="Script")
    Host.Call("CreateInstance", Parent=Parent, Name="Generation", ClassSchemaId=Schema["SchemaId"], DefinitionVersion=Schema["DefinitionVersion"], InitialSource=Source)
    Packet = Host.Call("StartPlaySession", PersistenceMode="Disabled")
    Session = Packet["PlaySessionId"]
    Messages = []
    try:
        for _ in range(120):
            for Entry in Packet.get("Diagnostics", []):
                if Entry.get("Severity")=="Error":
                    raise RuntimeError(Entry)
                Messages.append(Entry.get("Message", ""))
            if any("[Docs:Generation]" in M for M in Messages):
                break
            time.sleep(.01)
            Packet = Host.Call("PollPlayDiagnostics", PlaySessionId=Session)
        else:
            raise RuntimeError("Example verification did not complete")
    finally:
        Host.Call("StopPlaySession", PlaySessionId=Session)
Args.output.write_text(json.dumps({"EngineSHA256": hashlib.sha256(Args.engine.read_bytes()).hexdigest(), "Messages": Messages, "Status": "Executed"}, indent=2)+"\n", encoding="utf-8")
