# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Time sections of the shipped locomotion callback using a private Windows product copy."""
import argparse
import csv
import hashlib
from pathlib import Path
import shutil
import tempfile
import time

from SpatialConstructionPrecision import EditorHost, Probe, Start, Windows


def Insert(Source, Anchor, Replacement):
    if Source.count(Anchor) != 1:
        raise ValueError(f"Runtime script no longer matches {Anchor!r}")
    return Source.replace(Anchor, Replacement)


def Prepare(Engine, Folder):
    Folder.mkdir()
    for File in [Engine, *Engine.parent.glob("*.dll")]:
        shutil.copyfile(File, Folder / File.name)
    shutil.copytree(Engine.parent / "runtime", Folder / "runtime")
    shutil.copytree(Engine.parent / "shaders", Folder / "shaders")
    Script = Folder / "runtime" / "DefaultLocomotion.luau"
    Source = Script.read_text(encoding="utf-8")
    Source = Insert(Source, "local SimulationConnection", "local Ticks, Batch = 0, {}\n\tlocal SimulationConnection")
    Source = Insert(Source, "local Character = LocalPlayer.Character", "local A = os.clock()\n\t\tlocal Character = LocalPlayer.Character")
    Source = Insert(Source, "local Motion = Character:Move(Translation, Velocity)",
                    "local B = os.clock()\n\t\tlocal Motion = Character:Move(Translation, Velocity)\n\t\tlocal C = os.clock()")
    for Marker, Label in (("Character.Velocity = Motion.Velocity", "V"),
                          ("Character.FloorNormal = Motion.FloorNormal", "F"),
                          ("Character.Grounded = Motion.HasFloor", "G"),
                          ("if Character.Grounded and Character.Velocity.Y < 0 then", "H")):
        Source = Insert(Source, Marker, f"local {Label} = os.clock()\n\t\t" + Marker)
    Anchor = "\n\tend)\n\n\treturn function()"
    Source = Insert(Source, Anchor, """
        local D = os.clock()
        Ticks += 1
        if Ticks > 192 and Ticks <= 576 then
            table.insert(Batch, string.format("%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%d,%d,%d",
                (B-A)*1e9, (C-B)*1e9, (V-C)*1e9, (F-V)*1e9, (G-F)*1e9, (H-G)*1e9, (D-H)*1e9,
                if WasGrounded then 1 else 0, if Motion.HasFloor then 1 else 0, if Motion.Velocity.Y < 0 then 1 else 0))
            if #Batch == 8 then
                print("[Workspace:Locomotion] " .. Ticks .. " " .. table.concat(Batch, " "))
                table.clear(Batch)
            end
        end""" + Anchor)
    Script.write_text(Source, encoding="utf-8")
    return Folder / Engine.name


def Measure(Win, Binary, Project):
    with EditorHost(Binary) as Host:
        Process = int(Host.Process._handle)
        Win.Check(Win.Kernel.SetProcessAffinityMask(Process, 0xffff0000))
        Main = Win.MainThread(Host.Process.pid)
        try:
            Win.Check(Win.Kernel.SetThreadAffinityMask(Main, 1 << 24))
            Win.VerifyAffinity(Process, Main, 0xffff0000, 24)
            Session = Start(Host, Project, False, Probe)
            Samples = {}
            Deadline = time.monotonic() + 20
            try:
                while len(Samples) < 384:
                    Packet = Host.Call("PollPlayDiagnostics", PlaySessionId=Session)
                    for Row in Packet["Diagnostics"]:
                        Message = Row.get("Message", "")
                        if Row.get("Severity") == "Error":
                            raise RuntimeError(Message)
                        if "[Workspace:Locomotion] " in Message:
                            End, *Values = Message.split("[Workspace:Locomotion] ")[1].split()
                            if len(Values) != 8:
                                raise ValueError("Incomplete locomotion batch")
                            for Offset, Value in enumerate(Values, int(End) - 7):
                                Samples[Offset] = tuple(map(int, Value.split(",")))
                    if time.monotonic() >= Deadline:
                        raise TimeoutError(f"Captured {len(Samples)} locomotion callbacks")
                    time.sleep(.035)
                return [Samples[Tick] for Tick in range(193, 577)]
            finally:
                Host.Call("StopPlaySession", PlaySessionId=Session)
        finally:
            Win.Kernel.CloseHandle(Main)


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--engine", type=Path, action="append", required=True)
    Parser.add_argument("--output", type=Path, required=True)
    Args = Parser.parse_args()
    Win = Windows()
    Win.Check(Win.Kernel.SetProcessAffinityMask(Win.Kernel.GetCurrentProcess(), 1 << 8))
    with tempfile.TemporaryDirectory(prefix="glocomotion-") as Scratch, Args.output.open("x", newline="") as Output:
        Table = csv.writer(Output)
        Table.writerow(["Run", "EngineSHA256", "LocomotionSHA256", "Callback", "InputNs", "MoveNs", "StepTestNs", "SetVelocityNs",
                        "SetFloorNs", "SetGroundedNs", "ClampNs", "WasGrounded", "HasFloor", "Descending"])
        Binaries = {}
        for Run, Engine in enumerate(Args.engine, 1):
            Engine = Engine.resolve(strict=True)
            if Engine not in Binaries:
                Binaries[Engine] = Prepare(Engine, Path(Scratch) / f"Product{Run}")
            Digest = hashlib.sha256(Engine.read_bytes()).hexdigest()
            ModuleDigest = hashlib.sha256((Engine.parent / "runtime/DefaultLocomotion.luau").read_bytes()).hexdigest()
            Values = Measure(Win, Binaries[Engine], Path(Scratch) / f"Project{Run}")
            Table.writerows((Run, Digest, ModuleDigest, Tick, *Segment) for Tick, Segment in enumerate(Values, 193))
            Output.flush()
