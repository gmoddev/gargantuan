# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Check an actual debugger-paused Play viewport against two coincident colored walls."""
import argparse
import base64
import csv
import hashlib
from pathlib import Path
import tempfile

from SpatialCreatorTrial import Create, EditorHost


Source = """local World = game:GetService("Workspace")
local Room = Instance.new("SpatialRegion")
Room.Name = "DebugInterior"
Room.Parent = World
local function MakeWall(Parent, Tint)
    local Wall = Instance.new("Part")
    Wall.Anchored = true
    Wall.Size = Vector3.new(24, 24, 2)
    Wall.Position = Vector3.new(400, 64, 0)
    Wall.Color = Tint
    Wall.Parent = Parent
end
MakeWall(World, Color3.new(1, 0, 0))
MakeWall(Room, Color3.new(0, 0, 1))
local View = CFrame.new(Vector3.new(400, 64, 12), Vector3.new(400, 64, 0))
World.CurrentCamera.CFrame = View
game:GetService("RunService").PreRender:Connect(function() World.CurrentCamera.CFrame = View end)
World.CurrentSpatialRegion = Room -- capture default before this line
World.CurrentSpatialRegion = nil -- capture named before this line
World.CurrentSpatialRegion = Room -- capture restored default before this line
print("[Editor:PausedDomain] Released to a running named view")
"""


def Capture(Host, Session, Stage, Width, ExpectedWorld, Pause, EngineHash):
    Height = 96
    Host.Call("ConfigureViewport", Width=Width, Height=Height)
    Selected = Host.Call("GetSpatialDomainState", PlaySessionId=Session)
    Frame = Host.Call("CaptureViewport")
    if (Selected["Name"] != ExpectedWorld or not Selected["Viewed"] or Frame["Mode"] != "Play"
            or Frame["PlaySessionId"] != Session or Frame["Format"] != "RGB8"
            or Frame["Width"] != Width or Frame["Height"] != Height):
        raise RuntimeError(f"Wrong session or native world at {Stage}: {Selected} {Frame['Mode']}")
    if Host.Call("GetPlayRootDebugState", PlaySessionId=Session)["DebugPause"] != Pause:
        raise RuntimeError(f"Capturing {Stage} advanced the suspended script")
    Pixels = base64.b64decode(Frame["Pixels"], validate=True)
    if len(Pixels) != Width * Height * 3:
        raise ValueError("Framebuffer byte count does not match the native viewport")
    Red = Blue = 0
    for Y in range(Height // 4, 3 * Height // 4):
        for X in range(Width // 4, 3 * Width // 4):
            Offset = 3 * (Y * Width + X)
            R, G, B = Pixels[Offset:Offset+3]
            Red += R > max(G, B) + 24
            Blue += B > max(G, R) + 24
    Expected, Opposite = (Blue, Red) if ExpectedWorld == "DebugInterior" else (Red, Blue)
    Sampled = (Width // 2) * (Height // 2)
    return dict(Stage=Stage, World=Selected["Name"], Activation=Selected["Activation"],
                PauseSerial=Pause["Serial"] if Pause else 0, Width=Width, Height=Height,
                Sampled=Sampled, Red=Red, Blue=Blue, Matched=Expected >= Sampled * .5 and Opposite == 0,
                PixelSha256=hashlib.sha256(Pixels).hexdigest(), EngineSha256=EngineHash)


def Observe(Engine, SameSize):
    EngineHash = hashlib.sha256(Engine.read_bytes()).hexdigest()
    Markers = {Line: Label.split(" -- capture ")[1].split(" before")[0]
               for Line, Label in enumerate(Source.splitlines(), 1) if " -- capture " in Label}
    Rows = []
    with tempfile.TemporaryDirectory(prefix="g-debug-domain-") as Scratch, EditorHost(Engine) as Host:
        Directory = Path(Scratch)
        Script = Directory / "PausedView.server.luau"
        Script.write_text(Source, encoding="utf-8")
        Create(Host, Directory / "Project", [Script])
        Transports = Host.Call("Handshake")["ViewportTransports"]
        Mailbox = next((T for T in Transports if T["Name"] == "LatestFrameMailbox" and
                        "BGRA8" in T["PixelFormats"]), None)
        Snapshot = Host.Call("GetSnapshot")["Snapshot"]
        Object = next(R["Id"] for R in Snapshot["Objects"] if R["Name"] == "PausedViewServer")
        Revision = Host.Call("GetScriptSource", Object=Object)["SourceVersion"]
        Host.Call("ConfigureViewport", Width=96, Height=96)
        Started = Host.Call("StartPlaySession", DebugRoot=dict(Object=Object, SourceVersion=Revision))
        Session = Started["PlaySessionId"]
        Pause = Started["DebugPause"]
        try:
            for _ in range(80):
                if not Pause:
                    break
                Stage = Markers.get(Pause["Source"]["Line"])
                if Stage:
                    World = "DebugInterior" if Stage == "named" else "Workspace"
                    Width = {"default": 120, "named": 128, "restored default": 136}[Stage]
                    if SameSize and Stage != "default":
                        Rows.append(Capture(Host, Session, Stage + " unchanged size", Rows[-1]["Width"],
                                            World, Pause, EngineHash))
                    Rows.append(Capture(Host, Session, Stage, Width, World, Pause, EngineHash))
                    if Stage == "named" and Mailbox:
                        Host.Call("OpenViewportTransport", Transport=Mailbox["Name"], Version=Mailbox["Version"],
                                  PixelFormat="BGRA8")
                        Host.Call("CloseViewportTransport")
                        Rows.append(Capture(Host, Session, "named reattachment", Width, World, Pause, EngineHash))
                Pause = Host.Call("ResumePlayRootDebug", PlaySessionId=Session, PauseSerial=Pause["Serial"],
                                  Action="NextLine")["DebugPause"]
            PrimaryStages = [R["Stage"] for R in Rows if not R["Stage"].endswith(" unchanged size")]
            ExpectedStages = ["default", "named"]
            if Mailbox:
                ExpectedStages.append("named reattachment")
            if Pause or PrimaryStages != ExpectedStages + ["restored default"]:
                raise RuntimeError(f"Debugger did not visit every capture point: {Pause}, {Rows}")
            Rows.append(Capture(Host, Session, "running named", 144, "DebugInterior", None, EngineHash))
        finally:
            Host.Call("StopPlaySession", PlaySessionId=Session)
    return Rows


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--engine", type=Path, required=True)
    Parser.add_argument("--output", type=Path, help="Write the observed rows to a new CSV")
    Parser.add_argument("--same-size", action="store_true", help="Also capture each chart transition without resizing")
    Args = Parser.parse_args()
    Rows = Observe(Args.engine.resolve(strict=True), Args.same_size)
    if Args.output:
        Args.output.parent.mkdir(parents=True, exist_ok=True)
        with Args.output.open("x", encoding="utf-8", newline="") as Output:
            Table = csv.DictWriter(Output, fieldnames=Rows[0])
            Table.writeheader()
            Table.writerows(Rows)
    for Row in Rows:
        print("[Editor:PausedDomain]", Row["Stage"], Row["World"], Row["Red"], Row["Blue"], Row["Matched"])
    raise SystemExit(0 if all(R["Matched"] for R in Rows) else 1)
