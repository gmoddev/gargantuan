# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Send pointer input to a displayed application menu and inspect rendered pixels."""
import argparse
import base64
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from StandaloneLifecyclePlayer import PrepareProject
from SpatialConstructionPackageSession import ConsoleCapture, ManagedEnvironment, Run, WriteJson

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "samples" / "TerrainQuarry"))
from BuildProject import EditorHost


Entry = """
local Menu = Instance.new("ScreenGui")
Menu.Parent = script.Parent
local Button = Instance.new("TextButton")
Button.Size = UDim2.fromScale(1,1)
Button.BackgroundTransparency = 0
Button.BackgroundColor3 = Color3.new(1,0,0)
Button.Text = ""
Button.Parent = Menu
assert(game:SetMainMenu(Menu).Ok)
local Sessions = game:GetService("SessionService")
local Run = game:GetService("RunService")
task.defer(function()
    Button.Activated:Wait()
    assert(Sessions:StartLocal():Await().Ok)
    assert(Run:PauseGameplay():Await().Ok)
    Button.BackgroundColor3 = Color3.new(0,1,0)
    Button.Activated:Wait()
    assert(Run:IsGameplayPaused())
    assert(Run:ResumeGameplay():Await().Ok)
    assert(Sessions:Leave():Await().Ok)
    Button.BackgroundColor3 = Color3.new(0,0,1)
    Button.Activated:Wait()
    print("[Lifecycle:Input] Graphical title and suspended gameplay accepted pointer input")
    assert(game:GetService("ApplicationService"):Quit().Ok)
end)
"""


def AwaitPixel(Read, Channel):
    Deadline = time.monotonic() + 12
    Last = None
    while time.monotonic() < Deadline:
        Last = Read()
        if Last and Last[Channel] > 100 and all(Last[Channel] > Value * 2 for I, Value in enumerate(Last) if I != Channel):
            return
        time.sleep(0.025)
    raise RuntimeError(f"Displayed color did not reach channel {Channel}; last pixel {Last}")


def Studio(Engine, Project):
    with EditorHost(Engine) as Host:
        Host.Call("Handshake")
        Host.Call("OpenProject", Root=Project.as_posix())
        Host.Call("GetSnapshot")
        Host.Call("ConfigureViewport", Width=192, Height=144)
        Session = Host.Call("StartPlaySession", LaunchMode="Application")["PlaySessionId"]
        def Pixel():
            Frame = Host.Call("CaptureViewport")
            assert Frame["PlaySessionId"] == Session
            Bytes = base64.b64decode(Frame["Pixels"], validate=True)
            Offset = (24 * Frame["Width"] + 24) * 3
            return tuple(Bytes[Offset:Offset + 3])
        for Channel in range(3):
            AwaitPixel(Pixel, Channel)
            State = Host.Call("GetPlaySessionState")["Application"]
            assert State["GameplayPaused"] == (Channel == 1)
            for Action in ("Pressed", "Released"):
                Host.Call("SendPlayInput", PlaySessionId=Session, Type="PointerButton", Button="Left", State=Action, X=24, Y=24)
                if Action == "Pressed":
                    Pixel()
        Deadline = time.monotonic() + 3
        while Host.Call("GetPlaySessionState")["State"] == "Running" and time.monotonic() < Deadline:
            time.sleep(0.02)
        assert Host.Call("GetPlaySessionState")["State"] == "Stopped"
        Host.Call("StopPlaySession", PlaySessionId=Session)


def PlayerWindow(Process, LogPath):
    User = ctypes.WinDLL("user32", use_last_error=True)
    Handle = wintypes.HANDLE
    def Bind(Library, Method, Return, *Args):
        Function = getattr(Library, Method)
        Function.restype, Function.argtypes = Return, Args
        return Function
    Callback = ctypes.WINFUNCTYPE(wintypes.BOOL, Handle, wintypes.LPARAM)
    Enumerate = Bind(User, "EnumWindows", wintypes.BOOL, Callback, wintypes.LPARAM)
    ProcessId = Bind(User, "GetWindowThreadProcessId", wintypes.DWORD, Handle, ctypes.POINTER(wintypes.DWORD))
    GetRect = Bind(User, "GetClientRect", wintypes.BOOL, Handle, ctypes.POINTER(wintypes.RECT))
    GetTitle = Bind(User, "GetWindowTextW", ctypes.c_int, Handle, wintypes.LPWSTR, ctypes.c_int)
    Post = Bind(User, "PostMessageW", wintypes.BOOL, Handle, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
    Window = None
    @Callback
    def Visit(Current, _):
        nonlocal Window
        Pid, Rect = wintypes.DWORD(), wintypes.RECT()
        ProcessId(Current, ctypes.byref(Pid))
        Title = ctypes.create_unicode_buffer(128)
        if (Pid.value == Process.pid and GetTitle(Current, Title, len(Title)) and Title.value == "Gargantuan" and
                GetRect(Current, ctypes.byref(Rect)) and Rect.right >= 300 and Rect.bottom >= 200):
            Window = Current
            return False
        return True
    def Pixel():
        if Process.poll() is not None:
            raise RuntimeError(f"Player exited before displayed interaction: {Process.returncode}")
        if not Window:
            Enumerate(Visit, 0)
            if not Window:
                return None
        for Line in reversed(LogPath.read_text(errors="replace").splitlines()):
            if Line.startswith("[Lifecycle:PresentedPixel] "):
                return tuple(map(int, Line.split()[1:]))
        return None
    for Channel in range(3):
        AwaitPixel(Pixel, Channel)
        Point = 24 | (24 << 16)
        for Event, Buttons in ((0x200, 0), (0x201, 1), (0x202, 0)):
            assert Post(Window, Event, Buttons, Point)
            time.sleep(0.065)


def Main():
    Parser = argparse.ArgumentParser()
    for Name in ("project", "engine", "packager", "runtime"):
        Parser.add_argument("--" + Name, type=lambda Value: Path(Value).resolve(), required=True)
    Parser.add_argument("--installed", action="store_true")
    Options = Parser.parse_args()
    Options.cycles = 1
    with tempfile.TemporaryDirectory(prefix="gmi-") as Scratch:
        Root = Path(Scratch)
        Project = PrepareProject(Options, Root, "Menu")
        File = Project / ".gargantuan" / "project.instance.json"
        Scene = json.loads(File.read_bytes())
        Folder = next(Item for Item in Scene["Children"] if Item["Name"] == "Application")
        Folder["Children"][0]["Properties"]["Source"]["String"] = Entry
        WriteJson(File, Scene)
        if not Options.installed:
            Studio(Options.engine, Project)
        else:
            if os.name != "nt":
                raise RuntimeError("Installed input driver requires the Windows windowing API")
            Stage = Root / "Stage"
            Run([Options.packager, "build", "--project", Project, "--runtime", Options.runtime, "--output", Stage,
                 "--configuration", "Release"], Root)
            Installed = Root / "Installed"
            Stage.rename(Installed)
            Env = ManagedEnvironment()
            Env.pop("SDL_AUDIODRIVER", None)
            with (Root / "Player.log").open("w+b") as Log:
                Process = subprocess.Popen([Installed / "GargantuanPlayer.exe", "--sample-presented-pixel", "--profile-root", Root, "--max-frames", "2400"],
                                           cwd=Root, env=Env, stdout=Log, stderr=subprocess.STDOUT, **ConsoleCapture())
                try:
                    try:
                        PlayerWindow(Process, Root / "Player.log")
                    except RuntimeError as Error:
                        raise RuntimeError(str(Error) + "\n" + (Root / "Player.log").read_text(errors="replace")[-4500:]) from Error
                    assert Process.wait(timeout=6) == 0
                    Log.seek(0)
                    assert b"[Lifecycle:Input] Graphical title and suspended gameplay accepted pointer input" in Log.read()
                finally:
                    if Process.poll() is None:
                        Process.terminate()
                        Process.wait(timeout=5)
        print("[Lifecycle:Input] Verified three displayed colors and three native button activations")


if __name__ == "__main__":
    Main()

