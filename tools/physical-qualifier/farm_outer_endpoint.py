"""Fixed, bounded endpoint operations for a staged Farm32 campaign.

This helper has no network-supplied command facility. The outer controller may
ask it to make/verify a private stage root, prove a control socket, or run one
of the pinned Farm32 host/role entrypoints. It never starts a capture itself.
"""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import time

from private_ticket_acl import AssertPrivate, Harden


SHA = re.compile(r"[0-9a-f]{64}\Z")
MAX_CONFIG = 65536
MAX_LOG = 8 * 1024 * 1024
MAX_SECONDS = 850


def Digest(File):
    with Path(File).open("rb") as Stream:
        return hashlib.file_digest(Stream, "sha256").hexdigest()


def ReadJson(File):
    File = Path(File).resolve(strict=True)
    if not File.is_file() or File.stat().st_size > MAX_CONFIG:
        raise ValueError("[Qualification:FarmOuter] missing or oversized input")
    return json.loads(File.read_text(encoding="utf-8"))


def Prepare(Root):
    Root = Path(Root).resolve()
    if Root.exists() or not Root.parent.is_dir():
        raise ValueError("[Qualification:FarmOuter] stage root already exists")
    Root.mkdir(mode=0o700)
    Harden(Root)


def Verify(Root, IndexPath):
    AssertPrivate(Root)
    Root = Path(Root).resolve(strict=True)
    Index = ReadJson(IndexPath)
    if not isinstance(Index, dict) or set(Index) != {"Format", "Version", "Files"} or \
            Index["Format"] != "GargantuanFarm32StageIndex" or Index["Version"] != 1 or \
            not isinstance(Index["Files"], list) or not 1 <= len(Index["Files"]) <= 20:
        raise ValueError("[Qualification:FarmOuter] invalid stage index")
    Seen = set()
    for Entry in Index["Files"]:
        if not isinstance(Entry, dict) or set(Entry) != {"Name", "Sha256"} or \
                not isinstance(Entry["Name"], str) or not re.fullmatch(r"[A-Za-z0-9._-]{1,96}", Entry["Name"]) or \
                Entry["Name"] in Seen or Entry["Name"] == Path(IndexPath).name or \
                not isinstance(Entry["Sha256"], str) or not SHA.fullmatch(Entry["Sha256"]):
            raise ValueError("[Qualification:FarmOuter] invalid stage member")
        Seen.add(Entry["Name"])
        File = Root / Entry["Name"]
        if File.is_symlink() or not File.is_file() or Digest(File) != Entry["Sha256"]:
            raise ValueError("[Qualification:FarmOuter] stage member hash mismatch")
    if {"dependency.py", "upstream.lock.json"} <= Seen:
        Spec = importlib.util.spec_from_file_location("farm32_stage_dependency", Root / "dependency.py")
        if Spec is None or Spec.loader is None:
            raise ValueError("[Qualification:FarmOuter] pinned coordinator dependency is unavailable")
        Module = importlib.util.module_from_spec(Spec)
        Spec.loader.exec_module(Module)
        Module.GetRoot()  # Verify every pinned upstream source file before a role starts.
    return Index


def Probe(Host, Port, RunId):
    if Host != "192.168.0.108" or Port != 39451 or not re.fullmatch(r"[0-9a-f-]{36}", RunId):
        raise ValueError("[Qualification:FarmOuter] unexpected normal-LAN probe")
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as Listener:
        Listener.bind((Host, Port))
        Listener.listen(1)
        Listener.settimeout(10)
        print("[Qualification:FarmOuter] CONTROL_LISTENING", flush=True)
        Connection, Peer = Listener.accept()
        with Connection:
            Connection.settimeout(3)
            Challenge = Connection.recv(128)
            if Peer[0] != "192.168.0.68" or Challenge != RunId.encode("ascii"):
                raise ValueError("[Qualification:FarmOuter] control probe identity mismatch")
            Connection.sendall(Challenge)


def StopOwned(Process):
    if Process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(["taskkill.exe", "/PID", str(Process.pid), "/T", "/F"],
                       capture_output=True, timeout=8,
                       creationflags=subprocess.CREATE_NO_WINDOW, check=False)
    else:
        Process.terminate()
    try:
        Process.wait(timeout=8)
    except subprocess.TimeoutExpired:
        Process.kill()
        Process.wait(timeout=5)


def Run(Root, IndexPath, ConfigPath, Action):
    Index = Verify(Root, IndexPath)
    Config = ReadJson(ConfigPath)
    if not isinstance(Config, dict) or set(Config) != {"Format", "Version", "Action", "RunnerName",
                                                     "RunnerSha256", "InputName", "InputSha256"} or \
            Config["Format"] != "GargantuanFarm32FixedLaunch" or Config["Version"] != 1 or \
            Config["Action"] != Action or Action not in ("host", "role"):
        raise ValueError("[Qualification:FarmOuter] invalid launch configuration")
    Root = Path(Root).resolve(strict=True)
    Listed = {Entry["Name"] for Entry in Index["Files"]}
    for Name in ("RunnerName", "InputName"):
        if not isinstance(Config[Name], str) or not re.fullmatch(r"[A-Za-z0-9._-]{1,96}", Config[Name]):
            raise ValueError("[Qualification:FarmOuter] unsafe launch member name")
        File = Root / Config[Name]
        if Config[Name] not in Listed or not File.is_file() or File.is_symlink() or \
                Digest(File) != Config[Name.replace("Name", "Sha256")]:
            raise ValueError("[Qualification:FarmOuter] launch member changed")
    if Config["RunnerName"] != "farm_campaign_runner.py":
        raise ValueError("[Qualification:FarmOuter] runner is not fixed")
    Input = ReadJson(Root / Config["InputName"])
    if Action == "host":
        if Input.get("Format") != "GargantuanFarm32Campaign" or Input.get("Version") != 1:
            raise ValueError("[Qualification:FarmOuter] host input mismatch")
    elif Input.get("Format") != "GargantuanFarm32Campaign" or Input.get("Version") != 1 or \
            Input.get("Role") not in ("SERVER", "CLIENT"):
        raise ValueError("[Qualification:FarmOuter] role input mismatch")
    Flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    Output = Root / (Action + ".stdout.log")
    Error = Root / (Action + ".stderr.log")
    with Output.open("xb") as Stdout, Error.open("xb") as Stderr:
        Process = subprocess.Popen([sys.executable, "-B", str(Root / Config["RunnerName"]),
                                    Action, str(Root / Config["InputName"])],
                                   cwd=str(Root), stdout=Stdout, stderr=Stderr,
                                   creationflags=Flags)
        Deadline = time.monotonic() + MAX_SECONDS
        try:
            while Process.poll() is None:
                if time.monotonic() >= Deadline or Output.stat().st_size > MAX_LOG or Error.stat().st_size > MAX_LOG:
                    raise TimeoutError("[Qualification:FarmOuter] owned process time or log bound")
                time.sleep(0.1)
            if Process.returncode:
                raise RuntimeError("[Qualification:FarmOuter] pinned " + Action + " failed")
        finally:
            StopOwned(Process)


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("Action", choices=("prepare", "verify", "digest", "probe", "host", "role"))
    Parser.add_argument("Root")
    Parser.add_argument("IndexOrPort", nargs="?")
    Parser.add_argument("ConfigOrRunId", nargs="?")
    Args = Parser.parse_args()
    if Args.Action == "prepare" and Args.IndexOrPort is None:
        Prepare(Args.Root)
    elif Args.Action == "verify" and Args.IndexOrPort is not None:
        Verify(Args.Root, Args.IndexOrPort)
    elif Args.Action == "digest" and Args.IndexOrPort is None:
        File = Path(Args.Root).resolve(strict=True)
        if File.is_symlink() or not File.is_file():
            raise ValueError("[Qualification:FarmOuter] digest target is not a file")
        print(Digest(File))
    elif Args.Action == "probe" and Args.ConfigOrRunId is not None:
        Probe(Args.Root, int(Args.IndexOrPort), Args.ConfigOrRunId)
    elif Args.Action in ("host", "role") and Args.ConfigOrRunId is not None:
        Run(Args.Root, Args.IndexOrPort, Args.ConfigOrRunId, Args.Action)
    else:
        Parser.error("invalid fixed endpoint operation arguments")


if __name__ == "__main__":
    try:
        Main()
    except (ValueError, RuntimeError, TimeoutError, OSError, json.JSONDecodeError) as Error:
        print(str(Error), file=sys.stderr)
        raise SystemExit(1)
