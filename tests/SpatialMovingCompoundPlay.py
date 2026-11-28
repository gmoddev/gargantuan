# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Compare public dynamic-grid admission with the same scene on two released EditorHosts."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import tempfile
import time

from SpatialConstructionOrdinaryFrames import EditorHost


Scene = r'''
local V = Vector3.new
local Region = Instance.new("SpatialRegion")
Region.Parent = game.Workspace
Region.Gravity = 0
for _,Pattern in {"Solid","Fragmented"} do
    for _,Count in {128,256,512,1024,2048,4096} do
        local G = Instance.new("BlockGrid")
        G.CellSize = if Count==4096 then .5 else 1
        G.Parent = Region
        if Pattern=="Solid" then
            G:Fill(V(0,0,0),V(16,math.max(1,Count//256),math.min(16,Count//16)),"Brick")
        else
            for Base=0,Count-1,512 do
                local E=G:BeginEdit()
                for I=Base,math.min(Count-1,Base+511) do
                    E:SetBlock(V(2*(I%16),2*(I//256),2*((I//16)%16)),"Brick")
                end
                E:Commit()
            end
        end
        local Prior=G:GetRevision()
        local Ok,Reason=pcall(function() G:SetMotionMode("Dynamic",Prior) end)
        local S=G:GetAssemblyState()
        if Ok then
            assert(S.Mode=="Dynamic" and math.abs(S.Mass-Count*G.CellSize^3)<.02)
            G:SetAssemblyVelocity(V(2,0,0),V(0,.5,0),G:GetRevision())
            assert(G:GetAssemblyState().LinearVelocity.X>1.99)
        else
            assert(G:GetRevision()==Prior and S.Mode=="Static" and G:GetBlock(V(0,0,0)).Definition=="Brick")
        end
        print(string.format("[Spatial:CompoundPlay] %s %d %s %.4f %s",Pattern,Count,tostring(Ok),S.Mass,if Ok then "ok" else tostring(Reason)))
        G:Destroy()
    end
end
Region:Destroy()
print("[Spatial:CompoundPlay] finished")
'''
Marker = re.compile(r"^\[Spatial:CompoundPlay\] (Solid|Fragmented) (\d+) (true|false) ([\d.]+) (.+)$")


def Capture(Engine, Project, Profile, Interactions, ContactSide, Mass, Bounds=False):
    Rows = []
    Recent = []
    with EditorHost(Engine) as Host:
        Host.Call("Handshake")
        Root = Host.Call("CreateProject", Destination=Project.as_posix(), Name="Moving compound")["Root"]
        Host.Call("GetSnapshot")
        Definition = next(Row for Row in Host.Call("GetSchema")["Definitions"]
                          if Row["Kind"]=="Class" and Row["Namespace"]=="Engine" and Row["Name"]=="Script")
        if Bounds:
            Source=(Path(__file__).parent/"blockgrid/gameplay/MovingCompoundBounds.luau").read_text(encoding="utf-8")
        elif Mass:
            Source=(Path(__file__).parent/"blockgrid/gameplay/MovingCompoundMass.luau").read_text(encoding="utf-8")
        elif Interactions:
            Text=(Path(__file__).parent/"blockgrid/gameplay/MovingCompoundContacts.luau").read_text(encoding="utf-8")
            Source=f"local Side = {ContactSide or (8 if Profile=='original' else 16)}\n"+Text
        else:
            Source=Scene
        Host.Call("CreateInstance", Parent=Root, Name="MotionProbe", ClassSchemaId=Definition["SchemaId"],
                  DefinitionVersion=Definition["DefinitionVersion"], InitialSource=Source)
        Packet=Host.Call("StartPlaySession", PersistenceMode="Disabled")
        Session=Packet["PlaySessionId"]
        try:
            Deadline=time.monotonic()+30
            while time.monotonic()<Deadline:
                for Entry in Packet.get("Diagnostics",[]):
                    Message=Entry.get("Message","")
                    if Entry.get("Severity") in ("Error","Warning") or Message.startswith("[Spatial:ContactStage]"):
                        Recent=(Recent+[Message])[-12:]
                    if Entry.get("Severity")=="Error":
                        raise RuntimeError(Message)
                    if (Bounds and Message.startswith("[Spatial:CompoundBounds] ") or
                            Interactions and Message.startswith("[Spatial:CompoundContacts] ") or
                            Mass and Message.startswith("[Spatial:CompoundMass] ")):
                        return {Key:float(Value) for Key,Value in re.findall(r"(\w+)=([-\d.]+)",Message)}
                    if Message=="[Spatial:CompoundPlay] finished":
                        if len(Rows)!=12: raise ValueError(f"Missing admission rows: {Rows}")
                        return Rows
                    Match=Marker.match(Message)
                    if Match:
                        Pattern,Count,Accepted,ObservedMass,Reason=Match.groups()
                        Count=int(Count)
                        Accepted=Accepted=="true"
                        if Accepted != (Count<=128 or (Pattern=="Solid" and Profile=="merged")):
                            raise ValueError(f"Unexpected {Profile} admission: {Message}")
                        Rows.append(dict(Pattern=Pattern,Cells=Count,Accepted=Accepted,Mass=float(ObservedMass),Reason=Reason))
                time.sleep(.02)
                Packet=Host.Call("PollPlayDiagnostics",PlaySessionId=Session)
            raise TimeoutError(f"Incomplete public compound run: {Rows} {Recent}")
        finally:
            Host.Call("StopPlaySession",PlaySessionId=Session)


if __name__=="__main__":
    Parser=argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--engine",type=Path,required=True)
    Parser.add_argument("--profile",choices=("original","merged"),required=True)
    Parser.add_argument("--output",type=Path,required=True)
    Checks=Parser.add_mutually_exclusive_group()
    Checks.add_argument("--interactions",action="store_true")
    Checks.add_argument("--mass",action="store_true")
    Checks.add_argument("--bounds",action="store_true")
    Parser.add_argument("--contact-side",type=int,choices=(8,16))
    Args=Parser.parse_args()
    Engine=Args.engine.resolve(strict=True)
    with Engine.open("rb") as File: Digest=hashlib.file_digest(File,"sha256").hexdigest()
    with tempfile.TemporaryDirectory(prefix="gcompound-") as Directory:
        Rows=Capture(Engine,Path(Directory)/"Scene",Args.profile,Args.interactions,Args.contact_side,Args.mass,Args.bounds)
    with Args.output.open("x",encoding="utf-8") as File:
        json.dump(dict(Profile=Args.profile,EngineSHA256=Digest,Observations=Rows),File,indent=2)
        File.write("\n")
    if Args.bounds: print(f"[Spatial:CompoundBounds] {Rows}")
    elif Args.mass: print(f"[Spatial:CompoundMass] {Rows}")
    elif Args.interactions: print(f"[Spatial:CompoundContacts] {Rows}")
    else: print(f"[Spatial:CompoundPlay] {len(Rows)} checked admissions, {sum(R['Accepted'] for R in Rows)} dynamic")
