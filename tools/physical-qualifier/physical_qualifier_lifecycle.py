"""Locally installed Foundation 2B catalog for one physical readiness run.

The control workflow carries capability names only. Every executable, argument,
and evidence path comes from this endpoint's fixed, prequalified local file.
"""

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

from agent_coordinator.workflow import Catalog


Processes = {}
Streams = {}


def Settings():
    Config = json.loads((Path.cwd() / ".lifecycle" / "physical.json").read_text(encoding="utf-8"))
    Expected = {"Role", "Python", "Tool", "ToolSHA256", "Coordinator", "Endpoint", "RunId"}
    if (Config.get("Role") == "SERVER" and "BrokerLabel" in Config):
        Expected.add("BrokerLabel")
    if set(Config) != Expected or Config["Role"] not in ("CLIENT", "SERVER"):
        raise ValueError("invalid installed physical catalog settings")
    if "BrokerLabel" in Config and not re.fullmatch(r"[0-9a-f]{16}", Config["BrokerLabel"]):
        raise ValueError("invalid worker broker label")
    Tool = Path(Config["Tool"]).resolve()
    if not Tool.is_file() or Tool.name != "qualifier.py":
        raise ValueError("physical qualifier source missing")
    with Tool.open("rb") as Stream:
        if hashlib.file_digest(Stream, "sha256").hexdigest().upper() != Config["ToolSHA256"]:
            raise ValueError("physical qualifier source pin changed")
    if not Path(Config["Python"]).is_file():
        raise ValueError("physical qualifier runtime missing")
    if Config["Role"] == "CLIENT":
        ConfigFile = Path(Config["Coordinator"])
        if not ConfigFile.is_file():
            raise ValueError("local coordinator config missing")
        if json.loads(ConfigFile.read_text(encoding="utf-8"))["RunId"] != Config["RunId"]:
            raise ValueError("local coordinator run mismatch")
    ConfigFile = Path(Config["Endpoint"])
    if not ConfigFile.is_file():
        raise ValueError("local endpoint config missing")
    if json.loads(ConfigFile.read_text(encoding="utf-8"))["RunId"] != Config["RunId"]:
        raise ValueError("local physical run mismatch")
    return Config


def Metadata(File):
    File = Path(File)
    with File.open("rb") as Stream:
        Digest = hashlib.file_digest(Stream, "sha256").hexdigest().upper()
    return {"Path": str(File), "SHA256": Digest, "Bytes": File.stat().st_size}


def Evidence(Config, Name):
    Item = json.loads(Path(Config[Name]).read_text(encoding="utf-8"))
    return Path(Item["EvidenceDir"])


def Start(Config, Name, Mode):
    if Name in Processes:
        raise ValueError("physical process already started")
    Log = Path.cwd() / ".lifecycle" / ("physical-" + Name)
    Out = Log.with_suffix(".stdout.log").open("wb")
    Err = Log.with_suffix(".stderr.log").open("wb")
    Streams[Name] = (Out, Err)
    Flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    Processes[Name] = subprocess.Popen(
        [Config["Python"], "-B", "-u", Config["Tool"], Mode, Config[Name]],
        cwd=str(Path(Config["Tool"]).parent), stdout=Out, stderr=Err,
        creationflags=Flags)
    return Processes[Name]


def WaitEvent(Directory, Event, Process, Context, MessageType=None):
    File = Directory / "control.jsonl"
    while True:
        Context.Check()
        if File.is_file():
            for Line in File.read_text(encoding="utf-8").splitlines():
                Row = json.loads(Line)
                if Row.get("Event") == Event and (MessageType is None or Row.get("Type") == MessageType):
                    return
        if Process.poll() is not None:
            raise RuntimeError("physical process exited before " + Event)
        time.sleep(0.05)


def WaitResult(Config, Name, Process, Context):
    while Process.poll() is None:
        Context.Check()
        time.sleep(0.1)
    ResultFile = Evidence(Config, Name) / "result.json"
    if not ResultFile.is_file():
        raise RuntimeError("physical result missing for " + Name)
    Result = json.loads(ResultFile.read_text(encoding="utf-8"))
    return {"Success": Process.returncode == 0 and Result.get("Success") is True,
            "Evidence": [Metadata(ResultFile)]}


def ClientStart(Parameters, Context):
    Config = Settings()
    if Config["Role"] != "CLIENT" or Parameters:
        raise ValueError("unauthorized client start")
    Coordinator = Start(Config, "Coordinator", "coordinator")
    WaitEvent(Evidence(Config, "Coordinator"), "LISTENING", Coordinator, Context)
    Endpoint = Start(Config, "Endpoint", "endpoint")
    WaitEvent(Evidence(Config, "Endpoint"), "SEND", Endpoint, Context, "STAGE_READY")
    return {"Success": True, "Evidence": []}


def ServerRun(Parameters, Context):
    Config = Settings()
    if Config["Role"] != "SERVER" or Parameters:
        raise ValueError("unauthorized server run")
    if "BrokerLabel" in Config:
        Label = Config["BrokerLabel"]
        Request = Path.cwd() / ".lifecycle" / ("physical-broker-" + Label + ".start.json")
        Response = Path.cwd() / ".lifecycle" / ("physical-broker-" + Label + ".start-result.json")
        if Request.exists() or Response.exists():
            raise ValueError("worker broker start already consumed")
        with Request.open("x", encoding="utf-8") as Stream:
            json.dump({"RunId": Config["RunId"], "Action": "START"}, Stream)
        try:
            while not Response.is_file():
                Context.Check()
                time.sleep(0.05)
            Row = json.loads(Response.read_text(encoding="utf-8"))
            if Row.get("RunId") != Config["RunId"] or not isinstance(Row.get("ReturnCode"), int):
                raise ValueError("invalid worker broker result")
            ResultFile = Evidence(Config, "Endpoint") / "result.json"
            if not ResultFile.is_file():
                raise RuntimeError("physical worker result missing")
            Result = json.loads(ResultFile.read_text(encoding="utf-8"))
            return {"Success": Row["ReturnCode"] == 0 and Result.get("Success") is True,
                    "Evidence": [Metadata(ResultFile)]}
        except BaseException:
            Cancel = Path.cwd() / ".lifecycle" / ("physical-broker-" + Label + ".cancel.json")
            if not Cancel.exists():
                with Cancel.open("x", encoding="utf-8") as Stream:
                    json.dump({"RunId": Config["RunId"], "Action": "CANCEL"}, Stream)
            raise
    return WaitResult(Config, "Endpoint", Start(Config, "Endpoint", "endpoint"), Context)


def ClientResult(Parameters, Context):
    Config = Settings()
    if Config["Role"] != "CLIENT" or Parameters:
        raise ValueError("unauthorized client result")
    Results = [WaitResult(Config, Name, Processes[Name], Context)
               for Name in ("Endpoint", "Coordinator")]
    return {"Success": all(Item["Success"] for Item in Results),
            "Evidence": [File for Item in Results for File in Item["Evidence"]]}


def Cleanup():
    for Process in Processes.values():
        if Process.poll() is None:
            Process.terminate()
            try:
                Process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                Process.kill()
                Process.wait(timeout=3)
    for Pair in Streams.values():
        for Stream in Pair:
            Stream.close()


def GetCatalog():
    return Catalog({"physical.client-start.v1": ClientStart,
                    "physical.server-run.v1": ServerRun,
                    "physical.client-result.v1": ClientResult}, Cleanup)
