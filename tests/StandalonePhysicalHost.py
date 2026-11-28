# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Exercise the separately staged installed host and guest on two Windows PCs."""
import argparse
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
from contextlib import ExitStack

from SpatialConstructionPackageSession import ConsoleCapture, ManagedEnvironment
from StandaloneHostInvitations import InvitationChannel


def Send(Message):
    # The orchestrator owns this encrypted SSH pipe. Never persist its frames.
    sys.stdout.buffer.write(json.dumps(Message).encode("ascii") + b"\n")
    sys.stdout.buffer.flush()


def Receive():
    Line = sys.stdin.buffer.readline(4097)
    if not Line.endswith(b"\n") or len(Line) > 4096:
        raise RuntimeError("Missing private launcher frame")
    return json.loads(Line)


def Start(Stack, Package, Args, Profile, Label, Extra=None, Capture=None):
    Log = Profile / (Label + ".log")
    File = Stack.enter_context(Log.open("xb"))
    Player = Package / "GargantuanPlayer.exe"
    Process = subprocess.Popen([str(Arg) for Arg in [Player, "--headless", "--max-frames", "1800", "--profile-root", Profile, *Args]],
        cwd=Package, env=ManagedEnvironment() | (Extra or {}), stdout=File, stderr=subprocess.STDOUT,
        **(Capture if Capture is not None else ConsoleCapture()))
    def Retire():
        if Process.poll() is None:
            Process.kill()
            Process.wait(timeout=8)
    Stack.callback(Retire)
    return Process, Log


def JoinOptions(Invitation):
    return (["--connect", Invitation["Endpoint"], "--join-proof-env", "GARGANTUAN_LAN_PLAYER_PROOF",
             "--server-key-env", "GARGANTUAN_LAN_SERVER_KEY", "--join-listing-source", "LAN",
             "--join-listing-id", Invitation["SessionID"], "--join-expires-ms", Invitation["ExpiresUnixMillis"]],
            {"GARGANTUAN_LAN_PLAYER_PROOF": Invitation["GuestProofs"][0],
             "GARGANTUAN_LAN_SERVER_KEY": Invitation["ServerPublicKeyHex"]})


def Host(Options, Stack):
    Root, Profile = Options.package, Options.profile
    Fixture = json.loads((Root / "HostFixture.json").read_bytes())
    Server = Root / "Server"
    Pin = hashlib.sha256((Server / "game.package.json").read_bytes()).hexdigest()
    assert Pin == Fixture["ServerManifestSha256"]
    Args = ["--local-server-root", Server, "--local-server-manifest-sha256", Pin]
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as Probe:
        Probe.bind((Options.address, Options.port))
        Endpoint = Options.address + ":" + str(Probe.getsockname()[1])
    Handle, Capture, GetFrame, _ = Stack.enter_context(InvitationChannel())
    Owner, OwnerLog = Start(Stack, Root / "SharedHost", [*Args, "--local-host-bind", Endpoint,
                            "--local-host-invitations", Handle], Profile, "Owner", Capture=Capture)
    Invitation = GetFrame()
    assert Invitation["Action"] == "Publish" and Invitation["Endpoint"] == Endpoint and Invitation["PackageSHA256"] == Pin
    Send(Invitation)
    Fresh = GetFrame()
    assert Fresh["Action"] == "Refresh" and Fresh["SessionID"] == Invitation["SessionID"] and Fresh["Seat"] == 1
    Send(Fresh)
    Done = Receive()
    assert Done["GuestComplete"] and Done["SessionID"] == Invitation["SessionID"]
    Join, Environment = JoinOptions(Invitation)
    Replay, ReplayLog = Start(Stack, Root / "SharedReplay", [*Args, *Join], Profile, "SpentGuest", Environment)
    assert Replay.wait(timeout=12) == 0 and "Spent guest invitation denied" in ReplayLog.read_text(errors="replace")
    assert Owner.wait(timeout=12) == 0 and "Owner retired shared world" in OwnerLog.read_text(errors="replace")
    End = GetFrame()
    if End["Action"] == "Refresh":
        End = GetFrame()
    assert End["Action"] == "Withdraw" and End["SessionID"] == Invitation["SessionID"]
    return dict(Endpoint=Endpoint, Host=socket.gethostname(), SessionID=Invitation["SessionID"],
                ServerManifestSha256=Pin, ReplayDenied=True, Withdrawn=True)


def Guest(Options, Stack):
    Initial = Receive()
    assert Initial["Action"] == "Publish" and Initial["Source"] == "LAN"
    Address, _ = Initial["Endpoint"].rsplit(":", 1)
    assert Address == Options.address
    Handle, Capture, _, Deliver = Stack.enter_context(InvitationChannel())
    Join, Environment = JoinOptions(Initial)
    Player, Log = Start(Stack, Options.package, [*Join, "--join-refresh-channel", Handle], Options.profile,
                        "Guest", Environment, Capture)
    Fresh = Receive()
    assert Fresh["Action"] == "Refresh" and Fresh["SessionID"] == Initial["SessionID"] and Fresh["GuestProof"] != Initial["GuestProofs"][0]
    Deliver(Fresh)
    assert Player.wait(timeout=15) == 0
    Output = Log.read_text(errors="replace")
    assert all(Marker in Output for Marker in ("Remote beat advanced", "Owner GUI hydrated", "Reconnected with fresh native admission"))
    Receipt = dict(GuestComplete=True, SessionID=Initial["SessionID"], Guest=socket.gethostname(),
                   GuestManifestSha256=hashlib.sha256((Options.package / "game.package.json").read_bytes()).hexdigest(),
                   Reconnected=True, AuthorityAdvanced=True)
    Send(Receipt)
    return Receipt


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("role", choices=("host", "guest"))
    Parser.add_argument("--package", type=lambda P: Path(P).resolve(strict=True), required=True)
    Parser.add_argument("--profile", type=lambda P: Path(P).resolve(), required=True)
    Parser.add_argument("--address", required=True)
    Parser.add_argument("--port", type=int, default=0, help="Fixed host UDP port for a scoped LAN firewall rule")
    Options = Parser.parse_args()
    if not 0 <= Options.port <= 65535:
        Parser.error("UDP port must be in the range 0 through 65535")
    Address = ipaddress.IPv4Address(Options.address)
    if os.name != "nt" or Address.is_loopback or Address.is_unspecified or Address.is_multicast:
        Parser.error("supply the concrete Windows host interface for this two-device gate")
    Options.profile.mkdir(parents=True, exist_ok=False)
    Started = time.monotonic()
    with ExitStack() as Stack:
        Receipt = Host(Options, Stack) if Options.role == "host" else Guest(Options, Stack)
    Receipt["Seconds"] = round(time.monotonic() - Started, 3)
    (Options.profile / "Result.json").write_text(json.dumps(Receipt, indent=2))
    if Options.role == "host":
        Send(Receipt)
