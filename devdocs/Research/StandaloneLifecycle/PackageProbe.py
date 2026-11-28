# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Exercise installed Windows package entry points on loopback in private copies."""
import argparse
import ctypes
import hashlib
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tempfile
import time


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    for Name in ("offline", "client", "server"):
        Parser.add_argument("--" + Name, type=Path, required=True)
    Args = Parser.parse_args()
    ctypes.windll.kernel32.SetErrorMode(0x8003)
    Window = subprocess.STARTUPINFO()
    Window.dwFlags = subprocess.STARTF_USESHOWWINDOW
    Window.wShowWindow = subprocess.SW_HIDE
    Options = dict(startupinfo=Window, creationflags=subprocess.CREATE_NEW_CONSOLE,
                   env=dict(os.environ, SDL_AUDIODRIVER="dummy", SDL_LOGGING="*=info"))
    with tempfile.TemporaryDirectory(prefix="StandaloneSession-") as Temporary:
        Root = Path(Temporary)
        for Name in ("offline", "client", "server"):
            shutil.copytree(getattr(Args, Name), Root / Name)
        Offline = Root / "offline/GargantuanPlayer.exe"
        Player = Root / "client/GargantuanPlayer.exe"
        Server = Root / "server/GargantuanServer.exe"
        for Program in (Offline, Player, Server):
            print("[Lifecycle:Package] " + Program.parent.name + " sha256=" + hashlib.sha256(Program.read_bytes()).hexdigest())

        def Run(Program, Arguments, Expected=0):
            Done = subprocess.run([Program, *Arguments], cwd=Program.parent, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, timeout=30, **Options)
            Output = Done.stdout.decode(errors="replace")
            if Done.returncode != Expected:
                raise RuntimeError(f"{Program.name} exited {Done.returncode}\n{Output[-1800:]}")
            return Output

        Run(Offline, ["--headless", "--startup-smoke"])
        Denied = Run(Player, ["--headless", "--server-bind", "127.0.0.1:0"], 2)
        if "cannot host" not in Denied:
            raise RuntimeError(Denied)
        print("[Lifecycle:Package] direct_start=0 player_attempt_to_bind=denied")
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as Vacant:
            Vacant.bind(("127.0.0.1", 0))
            Endpoint = f"127.0.0.1:{Vacant.getsockname()[1]}"
        Log = Root / "Server.log"
        with Log.open("wb") as Stream:
            Host = subprocess.Popen([Server, "--bind", Endpoint, "--max-ticks", "480"], cwd=Server.parent,
                                    stdout=Stream, stderr=subprocess.STDOUT, **Options)
            try:
                Deadline = time.monotonic() + 10
                while b"StartupWallMilliseconds=" not in Log.read_bytes():
                    if Host.poll() is not None or time.monotonic() > Deadline:
                        raise RuntimeError("Server startup: " + Log.read_text(errors="replace")[-1000:])
                    time.sleep(.05)
                for Cycle in (1, 2):
                    Output = Run(Player, ["--headless", "--connect", Endpoint, "--max-frames", "60"])
                    if "Client installed verified terrain collision before gameplay readiness" not in Output:
                        raise RuntimeError("Client failed world readiness: " + Output[-1600:])
                    print(f"[Lifecycle:Package] join={Cycle} terrain_ready=true player_exit=0")
                if Host.wait(timeout=20) != 0:
                    raise RuntimeError("Server failed")
            finally:
                if Host.poll() is None:
                    Host.kill()
                    Host.wait(timeout=5)
        Output = Log.read_text(errors="replace")
        if Output.count("Server admitted gameplay after verified terrain readiness") < 2:
            raise RuntimeError("Second session did not pass server admission: " + Output[-1800:])
        Summary = re.findall(r"\[Network:Session\][^\r\n]*", Output)
        print("[Lifecycle:Package] two_admissions=true server_exit=0", *Summary[-1:])
        # Closed loopback endpoint, healthy package. Recovery (the two joins above)
        # is exercised through new host processes. No app shell exists in this Player.
        Failed = Run(Player, ["--headless", "--connect", Endpoint, "--max-frames", "10"], 7)
        if not any(Word in Failed.lower() for Word in ("timed out", "failed", "closed")):
            raise RuntimeError("Missing connection failure diagnostic")
        print("[Lifecycle:Package] stopped_server_join=failed exit=7")


if __name__ == "__main__":
    Main()
