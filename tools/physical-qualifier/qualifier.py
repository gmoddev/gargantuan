"""Qualification-only LAN barrier. Python 3.12+, standard library only."""
import argparse
import base64
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import selectors
import shutil
import socket
import struct
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
from dependency import Load

Legacy, Transport = Load()
MAX_FRAME, MAX_LOG = Transport.MAX_FRAME, Transport.MAX_LOG
Digest, Save, Hidden = Transport.Digest, Transport.Save, Transport.Hidden
Journal, Channel = Transport.Journal, Transport.Channel
BASE_HEAD = "14644a369f9e7bfb9a81c21354adae62902d63d7"
OVERLAY = "2ED31AE67E0F99619940BBB130CD451DB37FEF3A5CEEDAB475E682C3FBEE7003"
GNS_PIN = "2cb93a06350bb065db53abdb0d87cf297e0bfd34"
PROBE_SHA = "F130DC868A807FFA4EF10887079162C562230854AE17C013559452791993E969"
PHASE1_BASE_HEAD = "e082e6b3ab4e5345c03daa1a9bf630d270cb95f0"
PHASE1_OVERLAY = "10D0CB47ED24D8A249735C49BC55DD52A600AA9DAB05480261435C6E3483775F"
PHASE1_PROBE_SHA = "0BCAD6DE1475E2E2A8A6C481D726AD0AF77904FEE2111A551E89207F7A1D61CD"
PHASE1_SOURCE_ARCHIVE = "f1-native-source.zip"
PHASE1_RUNTIME_MANIFEST_SHA = "E105DBA76473990C5AE3AB410762851C7183396FF894A093E526ED26B335D7A9"
SERVER_ADDRESS = "10.253.3.2"
CLIENT_ADDRESS = "10.253.3.1"
SERVER_PORT = 39450
CLIENT_CAPTURE_DURATION = 40
CLIENT_CAPTURE_STOP_TIMEOUT = 45
PHASE1_CAPTURE_DURATION = 70
PHASE1_CAPTURE_STOP_TIMEOUT = 75
CAPTURE_SERVICE_STOP_TIMEOUT = 45
PHASE1_CLASSIFICATION = "FOUR_CLIENT_PHASE1_ONLY"


def IsPhase1(Config):
    return Config.get("QualificationMode") == "PHASE1"


def CaptureDirections(File, Role, IncludeTuples=False):
    """Count the qualified UDP tuple directions in an Ethernet pcapng file."""
    LocalAddress, PeerAddress = ((CLIENT_ADDRESS, SERVER_ADDRESS) if Role == "CLIENT"
                                 else (SERVER_ADDRESS, CLIENT_ADDRESS))
    Interfaces = []
    Endian = None
    Counts = {"Outbound": 0, "Inbound": 0}
    Tuples = {}
    with open(File, "rb") as Stream:
        Data = Stream.read()
    Offset = 0
    while Offset + 12 <= len(Data):
        RawType = Data[Offset:Offset + 4]
        if RawType == b"\x0a\x0d\x0d\x0a":
            ByteOrder = Data[Offset + 8:Offset + 12]
            if ByteOrder == b"\x4d\x3c\x2b\x1a":
                Endian = "<"
            elif ByteOrder == b"\x1a\x2b\x3c\x4d":
                Endian = ">"
            else:
                raise ValueError("pcapng section has invalid byte-order magic")
            BlockType = 0x0A0D0D0A
        else:
            if Endian is None:
                raise ValueError("pcapng data precedes its section header")
            BlockType = struct.unpack_from(Endian + "I", Data, Offset)[0]
        BlockLength = struct.unpack_from(Endian + "I", Data, Offset + 4)[0]
        if (BlockLength < 12 or BlockLength % 4 or Offset + BlockLength > len(Data) or
                struct.unpack_from(Endian + "I", Data, Offset + BlockLength - 4)[0] != BlockLength):
            raise ValueError("pcapng block length is malformed")
        if BlockType == 1:
            LinkType = struct.unpack_from(Endian + "H", Data, Offset + 8)[0]
            Interfaces.append(LinkType)
        elif BlockType == 6:
            InterfaceId, _, _, CapturedLength, _ = struct.unpack_from(Endian + "IIIII", Data, Offset + 8)
            if InterfaceId >= len(Interfaces):
                raise ValueError("pcapng packet refers to an unknown interface")
            if Interfaces[InterfaceId] != 1:
                raise ValueError("qualification capture is not Ethernet")
            PacketStart = Offset + 28
            PacketEnd = PacketStart + CapturedLength
            if PacketEnd > Offset + BlockLength - 4:
                raise ValueError("pcapng packet data exceeds its block")
            Packet = Data[PacketStart:PacketEnd]
            if len(Packet) >= 14:
                EtherType = struct.unpack_from(">H", Packet, 12)[0]
                Layer3 = 14
                while EtherType in (0x8100, 0x88A8, 0x9100):
                    if len(Packet) < Layer3 + 4:
                        break
                    EtherType = struct.unpack_from(">H", Packet, Layer3 + 2)[0]
                    Layer3 += 4
                if EtherType == 0x0800 and len(Packet) >= Layer3 + 20:
                    HeaderLength = (Packet[Layer3] & 0x0F) * 4
                    if (Packet[Layer3] >> 4 == 4 and HeaderLength >= 20 and
                            len(Packet) >= Layer3 + HeaderLength + 8 and Packet[Layer3 + 9] == 17):
                        Fragment = struct.unpack_from(">H", Packet, Layer3 + 6)[0] & 0x1FFF
                        if Fragment == 0:
                            Source = str(ipaddress.IPv4Address(Packet[Layer3 + 12:Layer3 + 16]))
                            Destination = str(ipaddress.IPv4Address(Packet[Layer3 + 16:Layer3 + 20]))
                            SourcePort, DestinationPort = struct.unpack_from(">HH", Packet, Layer3 + HeaderLength)
                            Direction = None
                            ClientPort = None
                            if Role == "CLIENT":
                                if (Source == LocalAddress and Destination == PeerAddress and
                                        DestinationPort == SERVER_PORT):
                                    Direction, ClientPort = "Outbound", SourcePort
                                elif (Source == PeerAddress and Destination == LocalAddress and
                                      SourcePort == SERVER_PORT):
                                    Direction, ClientPort = "Inbound", DestinationPort
                            else:
                                if (Source == LocalAddress and Destination == PeerAddress and
                                        SourcePort == SERVER_PORT):
                                    Direction, ClientPort = "Outbound", DestinationPort
                                elif (Source == PeerAddress and Destination == LocalAddress and
                                      DestinationPort == SERVER_PORT):
                                    Direction, ClientPort = "Inbound", SourcePort
                            if Direction:
                                Counts[Direction] += 1
                                Tuple = Tuples.setdefault(str(ClientPort), {"Outbound": 0, "Inbound": 0})
                                Tuple[Direction] += 1
        Offset += BlockLength
    if Offset != len(Data) or not Interfaces:
        raise ValueError("pcapng capture is incomplete or has no interfaces")
    return (Counts, Tuples) if IncludeTuples else Counts


def ValidateConfig(Config):
    uuid.UUID(Config["RunId"])
    if Config.get("QualificationMode") not in (None, "PHASE1"):
        raise ValueError("unsupported qualification mode")
    if Config.get("ReadinessClients", 1) not in (1, 4):
        raise ValueError("readiness requires exactly one or four clients")
    if IsPhase1(Config):
        if (Config.get("ReadinessClients") != 4 or
                Config.get("ResultClassification") != PHASE1_CLASSIFICATION or
                Config["RunTimeout"] != 90):
            raise ValueError("Phase 1 requires four clients, fixed classification and run lease")
    elif (Config.get("ReadinessClients", 1) == 4 and
            Config.get("ResultClassification") != "FOUR_CLIENT_READINESS_ONLY") or (
            Config.get("ReadinessClients", 1) == 1 and "ResultClassification" in Config):
        raise ValueError("result classification must match the readiness client count")
    if len(Config["Token"]) != 64:
        raise ValueError("expected a 256-bit token")
    for Address in [Config["CoordinatorHost"], *Config["PeerIps"].values()]:
        Parsed = ipaddress.IPv4Address(Address)
        if Parsed.is_unspecified or Parsed in ipaddress.ip_network("10.253.3.0/30"):
            raise ValueError("control channel must bind normal LAN, not qualification fiber")
    ExpectedHash = PHASE1_PROBE_SHA if IsPhase1(Config) else PROBE_SHA
    if Config["Endpoint"] != "10.253.3.2:39450" or Config["ArtifactSHA256"].upper() != ExpectedHash:
        raise ValueError("probe hash does not match the fixed qualification mode")
    if not 1 <= Config["Port"] <= 65535:
        raise ValueError("invalid coordination port")
    for Key in ("StageTimeout", "RunTimeout"):
        if not 1 <= Config[Key] <= 600:
            raise ValueError("timeout must be between 1 and 600 seconds")


def Coordinator(Config):
    return Legacy.Coordinator(Config, ValidateConfig)


class ProbeGroup:
    """Expose completion only after all four locally owned client probes exit."""

    def __init__(self, Processes):
        self.Processes = tuple(Processes)
        self.pid = self.Processes[0].pid

    def poll(self):
        Codes = [Process.poll() for Process in self.Processes]
        if any(Code is None for Code in Codes):
            return None
        return next((Code for Code in Codes if Code), 0)

    @property
    def returncode(self):
        return self.poll()


class LocalRun:
    def __init__(self, Config, Log):
        self.Config, self.Log = Config, Log
        self.Probe = self.Capture = None
        self.Probes = []
        self.ClientNonces = []
        self.CaptureArmed = False
        self.Files = []
        self.FinalizeAt = None
        self.HookHashes = {}
        self.CleanupResult = None

    def Check(self):
        Config = self.Config
        Probe = Path(Config["ProbePath"])
        if Probe.name != "gargantuan_physical_gns_funding_probe.exe" or Digest(Probe) != Config["ArtifactSHA256"]:
            raise ValueError("probe path/hash mismatch")
        ManifestPath = Path(Config["SourceManifest"])
        Manifest = json.loads(ManifestPath.read_text(encoding="utf-8-sig"))
        ExpectedHead = PHASE1_BASE_HEAD if IsPhase1(Config) else BASE_HEAD
        ExpectedOverlay = PHASE1_OVERLAY if IsPhase1(Config) else OVERLAY
        if (Manifest["BaseHead"] != ExpectedHead or Manifest["OverlayArchiveSha256"].upper() != ExpectedOverlay or
                Manifest["GnsPin"] != GNS_PIN):
            raise ValueError("source manifest provenance mismatch")
        if IsPhase1(Config) and (Manifest.get("Contract") != "F1" or
                                 Manifest.get("ProbeSHA256", "").upper() != PHASE1_PROBE_SHA):
            raise ValueError("Phase 1 requires the F1 probe source manifest")
        if IsPhase1(Config) and (Manifest.get("SourceArchive") != PHASE1_SOURCE_ARCHIVE or
                                 Digest(ManifestPath.parent / PHASE1_SOURCE_ARCHIVE) != PHASE1_OVERLAY):
            raise ValueError("Phase 1 native source archive hash mismatch")
        if IsPhase1(Config):
            RuntimeHashes = Manifest.get("RuntimeSha256")
            if not isinstance(RuntimeHashes, dict) or hashlib.sha256(json.dumps(
                    RuntimeHashes, sort_keys=True, separators=(",", ":")).encode()).hexdigest().upper() != PHASE1_RUNTIME_MANIFEST_SHA:
                raise ValueError("Phase 1 runtime manifest mismatch")
            for Name, Expected in RuntimeHashes.items():
                if not re.fullmatch(r"[A-Za-z0-9_.-]+", Name) or Digest(
                        Path(Config["WorkDir"]) / "runtime" / Name) != Expected:
                    raise ValueError("Phase 1 runtime file hash mismatch: " + Name)
        Clients = Config.get("ReadinessClients", 1)
        Fixed = (["server", "10.253.3.2", "39450", str(Clients)]
                 if Config["Role"] == "SERVER" else
                 ["client", "10.253.3.2", "39450", str(Config["Nonce"]),
                  "1" if IsPhase1(Config) else "0"])
        if not IsPhase1(Config):
            Fixed.append("--readiness-smoke")
        if Config["ProbeArgs"] != Fixed:
            raise ValueError("probe arguments do not match the fixed qualification mode")
        if IsPhase1(Config) and Config["Role"] == "CLIENT":
            if Config["CaptureCommand"][-3] != "duration:" + str(PHASE1_CAPTURE_DURATION):
                raise ValueError("Phase 1 requires the fixed client capture duration")
        if Config["Role"] == "CLIENT" and not 1 <= Config["Nonce"] <= 2147483648 - Clients:
            raise ValueError("invalid nonce")
        if not Path(Config["WorkDir"]).is_dir():
            raise ValueError("missing runtime working directory")
        shutil.copyfile(ManifestPath, self.Log.Directory / "source-manifest.json")
        if Config.get("CaptureCommand"):
            if not Path(Config["CaptureCommand"][0]).is_file():
                raise ValueError("capture executable missing")
        elif not Config.get("CaptureStart") or not Config.get("CaptureStop"):
            raise ValueError("capture command or local start/stop hooks required")
        else:
            for Name in ("CaptureStart", "CaptureStop"):
                for Argument in Config[Name]:
                    if Argument.lower().endswith(".ps1") and not Path(Argument).is_file():
                        raise ValueError("missing local capture hook: " + Argument)
                    if Argument.lower().endswith(".ps1"):
                        self.HookHashes[Argument] = Digest(Argument)
                        self.Log.Write("CAPTURE_HOOK_PROVENANCE", Path=Argument, SHA256=self.HookHashes[Argument])
        self.Log.Write("PROVENANCE", ArtifactSHA256=Digest(Probe), BaseHead=ExpectedHead,
                       OverlaySHA256=ExpectedOverlay, GnsPin=GNS_PIN)

    def Hook(self, Name):
        Command = [Value.replace("{EvidenceDir}", str(self.Log.Directory)).replace("{RunId}", self.Config.get("RunId", ""))
                   .replace("{EndpointPid}", str(os.getpid()))
                   for Value in self.Config[Name]]
        if "-File" in Command:
            Index = Command.index("-File")
            Script = Command[Index + 1]
            if Digest(Script) != self.HookHashes[Script]:
                raise ValueError("capture hook changed after staging")
            # Execute this hash-verified local code as an ordinary inline command.
            # No Set-ExecutionPolicy, Bypass flag, or machine policy modification.
            Quote = lambda Value: "'" + Value.replace("'", "''") + "'"
            Code = "& {\n"
            Code += Path(Script).read_text(encoding="utf-8-sig") + "\n} "
            Code += " ".join(Quote(Value) for Value in Command[Index + 2:])
            Encoded = base64.b64encode(Code.encode("utf-16le")).decode("ascii")
            Command = Command[:Index] + ["-EncodedCommand", Encoded]
        with (self.Log.Directory / (Name + ".log")).open("wb") as Output:
            Completed = subprocess.run(Command, stdout=Output, stderr=subprocess.STDOUT,
                                       timeout=(CAPTURE_SERVICE_STOP_TIMEOUT if Name == "CaptureStop" else 10),
                                       check=False, **Hidden())
        if Completed.returncode:
            raise RuntimeError(Name + " hook failed")
        if Name == "CaptureStop" and len(Command) >= 2 and Command[1] == "stop":
            try:
                Receipt = json.loads((self.Log.Directory / (Name + ".log")).read_text())
            except (OSError, ValueError) as Error:
                raise RuntimeError("capture service stop acknowledgement is invalid") from Error
            if (Receipt.get("Success") is not True or Receipt.get("Operation") != "stop" or
                    Receipt.get("RunId") != self.Config.get("RunId") or Receipt.get("State") != "stopped"):
                raise RuntimeError("capture service did not acknowledge completed export")

    def ArmCapture(self):
        self.CaptureArmed = True  # Stop hook also runs after partially failed startup.
        if self.Config.get("CaptureCommand"):
            Command = [Value.replace("{EvidenceDir}", str(self.Log.Directory))
                       for Value in self.Config["CaptureCommand"]]
            Output = (self.Log.Directory / "capture.log").open("wb")
            self.Files.append(Output)
            self.Capture = subprocess.Popen(Command, stdout=Output, stderr=subprocess.STDOUT, **Hidden())
            Deadline = time.monotonic() + 3
            while not (self.Log.Directory / "client.pcapng").exists():
                if self.Capture.poll() is not None or time.monotonic() >= Deadline:
                    raise RuntimeError("capture did not become active")
                time.sleep(0.02)
        else:
            self.Hook("CaptureStart")
        self.Log.Write("CAPTURE_LIVE")

    def Start(self):
        Env = dict(os.environ, GARGANTUAN_GNS_LIFECYCLE_TRACE="1")
        self.Started = time.monotonic()
        Clients = self.Config.get("ReadinessClients", 1)
        Nonces = (range(self.Config["Nonce"], self.Config["Nonce"] + Clients)
                  if self.Config["Role"] == "CLIENT" else (None,))
        for Nonce in Nonces:
            Args = list(self.Config["ProbeArgs"])
            if Nonce is not None:
                Args[3] = str(Nonce)
                Args[4] = "1" if IsPhase1(self.Config) and Nonce == self.Config["Nonce"] else "0"
                self.ClientNonces.append(Nonce)
            Stem = "probe" if Clients == 1 else ("probe-server" if Nonce is None else "probe-client-" + str(Nonce))
            Output = (self.Log.Directory / (Stem + ".stdout.log")).open("wb")
            Error = (self.Log.Directory / (Stem + ".stderr.log")).open("wb")
            self.Files.extend((Output, Error))
            Process = subprocess.Popen([self.Config["ProbePath"], *Args],
                                       cwd=self.Config["WorkDir"], env=Env,
                                       stdout=Output, stderr=Error, **Hidden())
            self.Probes.append(Process)
            self.Log.Write("PROBE_STARTED", Pid=Process.pid, Args=Args,
                           MonotonicNs=time.monotonic_ns(), UnixNs=time.time_ns())
        self.Probe = ProbeGroup(self.Probes) if Clients == 4 and self.Config["Role"] == "CLIENT" else self.Probes[0]

    def ServerLive(self):
        Stem = "probe-server" if self.Config.get("ReadinessClients", 1) == 4 else "probe"
        Text = (self.Log.Directory / (Stem + ".stderr.log")).read_text(errors="replace")
        if "event=listening" not in Text:
            return False
        # Verify the actual socket is owned by this locally launched PID.
        Command = ("$ErrorActionPreference='Stop'; $E=Get-NetUDPEndpoint -OwningProcess " +
                   str(self.Probe.pid) + " | Where-Object {$_.LocalAddress -eq '10.253.3.2' -and $_.LocalPort -eq 39450}; "
                   "if($E){exit 0}else{exit 1}")
        Check = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", Command],
                               capture_output=True, timeout=4, **Hidden())
        return Check.returncode == 0 and self.Probe.poll() is None

    def Status(self):
        for File in self.Log.Directory.glob("*.log"):
            if File.stat().st_size > MAX_LOG:
                raise ValueError("probe/capture log bound exceeded")
        if self.Capture is not None and self.Capture.poll() is not None:
            if (self.Config.get("ReadinessClients", 1) != 4 or
                    self.Capture.returncode != 0 or self.Probe is None or
                    self.Probe.poll() is None):
                raise RuntimeError("capture exited before probe finalization")
        if self.Config.get("ReadinessClients", 1) == 4 and self.Config["Role"] == "CLIENT":
            for Nonce, Process in zip(self.ClientNonces, self.Probes):
                if Process.poll() not in (None, 0):
                    raise RuntimeError("client probe exited unsuccessfully: " + str(Nonce))
        if self.Probe is not None and self.Probe.poll() is None:
            if time.monotonic() - self.Started > (65 if IsPhase1(self.Config) else 25):
                raise TimeoutError("local probe deadline exceeded")
            if self.FinalizeAt and time.monotonic() >= self.FinalizeAt:
                raise TimeoutError("probe did not finish after CLIENT_DONE")

    def Result(self):
        Clients = self.Config.get("ReadinessClients", 1)
        Phase1 = IsPhase1(self.Config)
        Classification = PHASE1_CLASSIFICATION if Phase1 else "FOUR_CLIENT_READINESS_ONLY"
        if Clients == 4 and self.Config["Role"] == "CLIENT":
            Reports = []
            for Nonce, Process in zip(self.ClientNonces, self.Probes):
                Stem = "probe-client-" + str(Nonce)
                Stdout = (self.Log.Directory / (Stem + ".stdout.log")).read_text(errors="replace")
                Stderr = (self.Log.Directory / (Stem + ".stderr.log")).read_text(errors="replace")
                Proof = ("[Probe:Result] pass=1 scope=Phase1-only" in Stdout and
                         "[Probe:Summary] role=client" in Stdout) if Phase1 else (
                         "[Probe:Readiness] result=pass role=client ready=1 expected=1 clean_remote_shutdown=1" in Stdout)
                Passed = Process.returncode == 0 and Proof and "[Probe:Cleanup] good=1" in Stdout
                Reports.append((Passed, Stderr))
            return {"Classification": Classification,
                    "Success": len(Reports) == 4 and all(Passed for Passed, _ in Reports),
                    "ExitCode": self.Probe.returncode, "EvidencePath": str(self.Log.Directory),
                    "Pid": self.Probe.pid, "ClientNonces": self.ClientNonces,
                    "Detail": "\n".join(str(Nonce) + ": " + Stderr[-400:]
                                        for Nonce, (_, Stderr) in zip(self.ClientNonces, Reports))[-2048:]}
        Stem = "probe-server" if Clients == 4 else "probe"
        Stdout = (self.Log.Directory / (Stem + ".stdout.log")).read_text(errors="replace")
        Stderr = (self.Log.Directory / (Stem + ".stderr.log")).read_text(errors="replace")
        Success = self.Probe.returncode == 0 and "[Probe:Cleanup] good=1" in Stdout
        if Phase1:
            def Fields(Label):
                Lines = re.findall(r"^\[Probe:" + Label + r"\] (.*)$", Stdout, re.MULTILINE)
                return [dict(re.findall(r"(\w+)=([^\s]+)", Line)) for Line in Lines]
            Curves, Peers, Admissions = Fields("ServiceCurve"), Fields("PeerService"), Fields("Admission")
            Curve = Curves[0] if len(Curves) == 1 else {}
            Admission = Admissions[0] if len(Admissions) == 1 else {}
            try:
                PeerProof = (len(Peers) == 4 and len({P["slot"] for P in Peers}) == 4 and
                             all(int(P["grants"]) >= 3 and
                                 int(P["qualified_grants"]) == int(P["grants"]) and
                                 int(P["completed_grants"]) == int(P["grants"]) and
                                 int(P["structural_first"]) > 0 and
                                 int(P["structural_ack"]) == int(P["structural_first"]) and
                                 int(P["running_us"]) > 0 and
                                 int(P["max_run_deficit_byte_us"]) <= int(Curve["peer_running_bound_byte_us"])
                                 for P in Peers))
                PoolProof = (Curve["contract"] == "F1" and Curve["verdict"] == "PASS" and
                             Curve["pool_curve"] == "derived-from-four-native-grant-curves" and
                             int(Curve["peer_rate_Bps"]) == 16 * 1024 * 1024 and
                             int(Curve["pool_rate_Bps"]) == 64 * 1024 * 1024 and
                             int(Curve["quantum_B"]) == 1248 and
                             int(Curve["startup_us"]) == 5000 and int(Curve["run_us"]) == 1000 and
                             int(Curve["peer_finite_intercept_byte_us"]) == 101911296000 and
                             int(Curve["peer_running_bound_byte_us"]) == 18025216000 and
                             int(Curve["pool_running_bound_byte_us"]) ==
                             4 * int(Curve["peer_running_bound_byte_us"]) and
                             int(Curve["pool_common_run_us"]) > 0 and
                             int(Curve["pool_episodes"]) >= 3)
                Conservation = (int(Admission["accepted"]) == int(Admission["retired"]) and
                                int(Admission["terminal"]) == 0 and
                                int(Admission["outstanding"]) == 0 and
                                int(Admission["grants"]) == 0 and
                                int(Admission["grants_high_water"]) == 4)
            except (KeyError, ValueError):
                PeerProof = PoolProof = Conservation = False
            Success = (Success and "[Probe:Result] pass=1 scope=Phase1-only" in Stdout and
                       PeerProof and PoolProof and Conservation)
        else:
            Success = Success and "[Probe:Readiness] result=pass" in Stdout
        if Clients == 4 and not Phase1:
            Success = (Success and
                       "[Probe:Readiness] result=pass role=server ready=4 expected=4" in Stdout)
        return {**({"Classification": Classification} if Clients == 4 else {}),
                "Success": Success, "ExitCode": self.Probe.returncode,
                "EvidencePath": str(self.Log.Directory), "Pid": self.Probe.pid,
                "Detail": Stderr[-2048:]}

    def Cleanup(self):
        if self.CleanupResult is not None:
            return list(self.CleanupResult)
        Errors = []
        Owned = list(self.Probes) if self.Probes else ([self.Probe] if self.Probe else [])
        if (self.Config.get("ReadinessClients", 1) == 4 and
                self.Capture is not None and self.Capture.poll() is None and
                self.Config.get("CaptureCommand")):
            try:
                # Windows terminate() kills dumpcap while it may be writing an
                # enhanced packet block, including on abort. Its own bounded
                # duration stop closes pcapng before validation and manifesting.
                self.Capture.wait(timeout=(PHASE1_CAPTURE_STOP_TIMEOUT if IsPhase1(self.Config)
                                           else CLIENT_CAPTURE_STOP_TIMEOUT))
            except subprocess.TimeoutExpired:
                Errors.append("client capture did not finalize within its bounded duration")
        if self.Capture is not None and self.Capture.poll() is not None and self.Capture.returncode != 0:
            Errors.append("client capture exited unsuccessfully")
        for Process in [*Owned, self.Capture]:
            if Process is not None and Process.poll() is None:
                try:
                    Process.terminate()
                    Process.wait(timeout=3)
                except Exception:
                    try:
                        Process.kill()
                        Process.wait(timeout=3)
                    except Exception as Error:
                        Errors.append(str(Error))
        if self.CaptureArmed and not self.Config.get("CaptureCommand"):
            try:
                self.Hook("CaptureStop")
            except Exception as Error:
                Errors.append(str(Error))
        if self.CaptureArmed:
            CaptureName = "client.pcapng" if self.Config.get("CaptureCommand") else "worker-capture.pcapng"
            try:
                Four = self.Config.get("ReadinessClients", 1) == 4
                Capture = CaptureDirections(self.Log.Directory / CaptureName, self.Config["Role"], Four)
                Directions, Tuples = Capture if Four else (Capture, None)
                self.Log.Write("CAPTURE_DIRECTIONS", **Directions)
                if not Directions["Outbound"] or not Directions["Inbound"]:
                    Errors.append("capture missed a qualified direction: " + json.dumps(Directions, sort_keys=True))
                if Four:
                    self.Log.Write("CAPTURE_TUPLES", Tuples=Tuples)
                    if len(Tuples) != 4 or any(not Row["Outbound"] or not Row["Inbound"]
                                               for Row in Tuples.values()):
                        Errors.append("capture lacks four bidirectional client tuples")
            except Exception as Error:
                Errors.append("capture direction validation failed: " + str(Error))
        for File in self.Files:
            File.close()
        self.Log.Write("LOCAL_CLEANUP", Errors=Errors)
        self.CleanupResult = list(Errors)
        return list(Errors)


def Endpoint(Config):
    return Legacy.Endpoint(Config, ValidateConfig, LocalRun)


def Stage(Args):
    Clients = getattr(Args, "clients", 1)
    Phase1 = getattr(Args, "phase1", False)
    if Clients not in (1, 4) or not 1 <= Args.nonce <= 2147483648 - Clients:
        raise ValueError("invalid readiness client count or nonce range")
    if Phase1 and Clients != 4:
        raise ValueError("Phase 1 requires exactly four clients")
    if Args.server_capture_client:
        if Args.server_capture_start or Args.server_capture_stop:
            raise ValueError("choose the privileged capture client or the existing hook pair")
    elif not Args.server_capture_start or not Args.server_capture_stop:
        raise ValueError("both existing capture hooks are required without the privileged capture client")
    Directory = Path(Args.output)
    Directory.mkdir(parents=True, exist_ok=False)
    Shared = {"RunId": str(uuid.uuid4()), "Token": secrets.token_hex(32),
              "CoordinatorHost": Args.client_lan, "PeerIps": {"CLIENT": Args.client_lan, "SERVER": Args.server_lan},
              "Port": Args.port, "Endpoint": "10.253.3.2:39450",
              "ArtifactSHA256": PHASE1_PROBE_SHA if Phase1 else PROBE_SHA,
              "StageTimeout": 300, "RunTimeout": 90 if Phase1 else 60}
    if Clients == 4:
        Shared["ReadinessClients"] = 4
        Shared["ResultClassification"] = PHASE1_CLASSIFICATION if Phase1 else "FOUR_CLIENT_READINESS_ONLY"
    if Phase1:
        Shared["QualificationMode"] = "PHASE1"
    ValidateConfig(Shared)
    Save(Directory / "coordinator.json", {**Shared, "EvidenceDir": str(Directory / "coordinator-evidence")})
    ClientBundle = Args.client_bundle
    Client = {**Shared, "Role": "CLIENT", "Nonce": Args.nonce,
              "ProbePath": str(Path(ClientBundle) / "gargantuan_physical_gns_funding_probe.exe"),
              "SourceManifest": str(Path(ClientBundle) / "source-manifest.json"), "WorkDir": ClientBundle,
              "ProbeArgs": ["client", "10.253.3.2", "39450", str(Args.nonce),
                            "1" if Phase1 else "0"] + ([] if Phase1 else ["--readiness-smoke"]),
              "EvidenceDir": str(Directory / "client-evidence"),
              "CaptureCommand": [r"C:\Program Files\Wireshark\dumpcap.exe", "-i", Args.capture_device,
                                 "-s", "0", "-f", "udp and host 10.253.3.1 and host 10.253.3.2 and port 39450",
                                 "-a", "duration:" + str(PHASE1_CAPTURE_DURATION if Phase1 else
                                                           CLIENT_CAPTURE_DURATION if Clients == 4 else 90),
                                 "-w", "{EvidenceDir}/client.pcapng"]}
    Save(Directory / "client.json", Client)
    ServerBundle = Args.server_bundle
    CaptureStart = ([Args.server_capture_client, "start", "{EvidenceDir}", "{RunId}", "{EndpointPid}"] if Args.server_capture_client else
                    ["powershell.exe", "-NoProfile", "-NonInteractive", "-File", Args.server_capture_start, "{EvidenceDir}", "Start"])
    CaptureStop = ([Args.server_capture_client, "stop", "{EvidenceDir}", "{RunId}", "{EndpointPid}"] if Args.server_capture_client else
                   ["powershell.exe", "-NoProfile", "-NonInteractive", "-File", Args.server_capture_stop, "{EvidenceDir}", "Stop"])
    Server = {**Shared, "Role": "SERVER", "ProbePath": ServerBundle + r"\gargantuan_physical_gns_funding_probe.exe",
              "SourceManifest": ServerBundle + r"\source-manifest.json", "WorkDir": ServerBundle,
              "ProbeArgs": ["server", "10.253.3.2", "39450", str(Clients)] + ([] if Phase1 else ["--readiness-smoke"]),
              "EvidenceDir": Args.server_evidence,
              "CaptureStart": CaptureStart,
              "CaptureStop": CaptureStop}
    Save(Directory / "server.json", Server)
    print("[Qualification:Stage] Configurations written; no listeners, captures, or probes started", flush=True)


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Sub = Parser.add_subparsers(dest="mode", required=True)
    for Mode in ("coordinator", "endpoint"):
        Child = Sub.add_parser(Mode)
        Child.add_argument("config")
    Child = Sub.add_parser("stage")
    for Name in ("output", "client-lan", "server-lan", "client-bundle", "server-bundle", "server-evidence",
                 "capture-device"):
        Child.add_argument("--" + Name, required=True)
    Child.add_argument("--server-capture-client")
    Child.add_argument("--server-capture-start")
    Child.add_argument("--server-capture-stop")
    Child.add_argument("--port", type=int, default=39451)
    Child.add_argument("--nonce", type=int, default=92707)
    Child.add_argument("--clients", type=int, choices=(1, 4), default=1)
    Child.add_argument("--phase1", action="store_true")
    Args = Parser.parse_args()
    try:
        if Args.mode == "stage":
            Stage(Args)
            return 0
        Config = json.loads(Path(Args.config).read_text(encoding="utf-8-sig"))
        return Coordinator(Config) if Args.mode == "coordinator" else Endpoint(Config)
    except (Exception, KeyboardInterrupt) as Error:
        print("[Qualification:Failure] " + (str(Error) or type(Error).__name__), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(Main())
