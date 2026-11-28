#!/usr/bin/env python3
"""Measure native CLI packages on an owned FirstCompleteGame copy and check Hybrid delivery."""

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import socket
import statistics
import subprocess
import time
import uuid
from pathlib import Path


BuildEnrollment = "GARGANTUAN_RUNTIME_PACKAGE_BUILD_ENROLLMENT"
RuntimeEnrollment = "GARGANTUAN_RUNTIME_PACKAGE_RUNTIME_ENROLLMENT"
# Public RFC 8032 test vector and synthetic content key for this owned fixture.
SigningSeed = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"
SigningPublic = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"
TestContentKey = "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"
ReadMarker = "rd1_source_only_crafting_c3de"
CompiledMarker = "rd1_source_only_controller_07a0"
ProtectedMarker = "rd1_source_only_procedural_fb51"
ProtectedLiteral = "rd1_client_procedural_impl_df85"
PrivateMarker = "rd1_server_private_inventory_a312"
PrivateLiteral = "rd1_private_inventory_runtime_0d3a"
IntermediateMarker = "rd1_authoring_intermediate_c7a8"
ClientProof = "RD1_CLIENT_EXECUTED"
GuiProof = "RD1_GUI_EXECUTED"
ServerProof = "RD1_SERVER_EXECUTED"
Markers = (ReadMarker, CompiledMarker, ProtectedMarker, ProtectedLiteral, PrivateMarker, PrivateLiteral, IntermediateMarker)
StartupPattern = re.compile(rb"\[Runtime:(Player|Server)\] StartupWallMilliseconds=(\d+)")
CreateFlags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0


def Require(Condition, Message):
    if not Condition:
        raise RuntimeError(Message)


def ReadJson(File):
    return json.loads(File.read_bytes())


def WriteJson(File, Value):
    File.write_text(json.dumps(Value, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")


def Digest(File):
    with File.open("rb") as Stream:
        return hashlib.file_digest(Stream, "sha256").hexdigest()


def ResourceId(Name):
    return hashlib.sha256(f"Gargantuan.RuntimeDistributionQualification.v1:{Name}".encode()).hexdigest()[:32]


def NativeSchemaId(Class):
    def Hash(Domain, Seed):
        for Byte in (Domain + "Engine\0" + Class).encode():
            Seed = ((Seed ^ Byte) * 1099511628211) & ((1 << 64) - 1)
        return Seed

    return f"{Hash('Gargantuan.SchemaId.Native.High.v1', 14695981039346656037):016x}" + \
        f"{Hash('Gargantuan.SchemaId.Native.Low.v1', 1099511628211):016x}"


def Node(Name, Class, Properties=None, Children=None):
    return {"Name": Name, "ClassName": Class, "ClassSchemaId": NativeSchemaId(Class),
            "ClassDefinitionVersion": 1, "Properties": Properties or {}, "Attributes": {}, "Extensions": [],
            "CustomProperties": [], "Tags": [], "Children": Children or []}


def CodeNode(Name, Class, Content, Context=None):
    Props = {"SourceId": {"String": ResourceId(Name)}, "Source": {"String": Content}}
    if Context is not None:
        Props["RunContext"] = {"EnumItem": ["RunContext", Context]}
    return Node(Name, Class, Props)


def WalkNodes(Root, Path=()):
    for Child in Root["Children"]:
        ChildPath = (*Path, Child["Name"])
        yield Child, ChildPath
        yield from WalkNodes(Child, ChildPath)


def MakeFixture(Sample, Destination, ExtraScripts):
    shutil.copytree(Sample, Destination)
    Settings = Destination / ".gargantuan"
    Document = Settings / "project.instance.json"
    World = ReadJson(Document)
    Require(World["ClassSchemaId"] == NativeSchemaId("DataModel"), "Fixture schema identity changed")
    GameScripts = next(Child for Child in World["Children"] if Child["Name"] == "GameScripts")
    RunContextEncoding = next(Child["Properties"]["RunContext"]["EnumItem"][0] for Child in GameScripts["Children"]
                              if Child["Name"] == "RoundManager")
    Public = next((Child for Child in World["Children"] if Child["ClassName"] == "ReplicatedStorage"), None)
    if Public is None:
        Public = Node("ReplicatedStorage", "ReplicatedStorage")
        World["Children"].append(Public)
    Public["Children"].extend((
        CodeNode("RD1Craft", "ModuleScript", f"-- {ReadMarker}\nreturn {{ Ingredients = 30 }}\n"),
        CodeNode("RD1Procedural", "ModuleScript", f"-- {ProtectedMarker}\n"
                 "local Craft = require('./RD1Craft')\n"
                 f"return {{ Value = Craft.Ingredients + 12, Origin = '{ProtectedLiteral}' }}\n"),
    ))
    Controller = CodeNode("RD1Controller", "Script", f"""-- {CompiledMarker}
local Procedural = require('@game/ReplicatedStorage/RD1Procedural')
assert(Procedural.Value == 42 and Procedural == require('@game/ReplicatedStorage/RD1Procedural'))
task.wait(0)
assert(script.Name == 'RD1Controller' and Procedural.Value == 42)
warn('{ClientProof}')
""", "Client")
    Server = CodeNode("RD1AuthoritativeInventory", "Script", f"""-- {PrivateMarker}
local Rules = require('./RD1InventoryRules')
assert(Rules.Count == 12 and Rules.Provenance == '{PrivateLiteral}')
warn('{ServerProof}')
""", "Server")
    PrivateModule = CodeNode("RD1InventoryRules", "ModuleScript", f"-- {PrivateMarker}\n"
                             f"return {{ Count = 12, Provenance = '{PrivateLiteral}' }}\n")
    GameScripts["Children"].extend((Controller, Server, PrivateModule))
    Starter = next((Child for Child in World["Children"] if Child["ClassName"] == "StarterGui"), None)
    if Starter is None:
        Starter = Node("StarterGui", "StarterGui")
        World["Children"].append(Starter)
    GuiScript = CodeNode("RD1GuiScript", "Script", f"""
local Procedural = require('@game/ReplicatedStorage/RD1Procedural')
assert(Procedural.Value == require('./RD1GuiModule').Answer)
warn('{GuiProof}')
""", "Client")
    Starter["Children"].append(Node("PrivateGuiFolder", "Folder", Children=[
        CodeNode("PrivateGuiModule", "ModuleScript", f"-- {PrivateMarker}\nreturn '{PrivateLiteral}'")]))
    Starter["Children"].append(Node("RD1Gui", "ScreenGui", Children=[
        GuiScript, CodeNode("RD1GuiModule", "ModuleScript", "return {Answer = 42}")]))
    for Index in range(ExtraScripts):
        GameScripts["Children"].append(CodeNode(f"RD1Controller{Index:04}", "Script", f"""
local State = {Index}
game:GetService('RunService').PostSimulation:Connect(function()
    State = (State * 48271 + 1) % 2147483647
end)
""", "Client"))
    for Script, Path in WalkNodes(World):
        if Script["ClassName"] in {"Script", "ModuleScript"}:
            Props = Script["Properties"]
            Props.setdefault("SourceId", {"String": ResourceId("/".join(Path))})
            if "RunContext" in Props:
                Props["RunContext"]["EnumItem"][0] = RunContextEncoding
    WriteJson(Document, World)
    Catalog = ReadJson(Settings / "assets" / "catalog.json")
    WriteJson(Settings / "package-audience.json", {"Version": 1,
              "SharedAssets": [Asset["AssetId"] for Asset in Catalog["Assets"]]})
    (Destination / "rd1-authoring-intermediate.txt").write_text(IntermediateMarker, encoding="utf-8")
    return ReadJson(Settings / "project.json")["ProjectId"], Document


def WritePolicy(File, SessionIdentity, Mixed):
    AuthoringPolicy = {"Version": 1, "SessionIdentity": SessionIdentity}
    if Mixed:
        AuthoringPolicy.update({
            "Groups": {"ReadableSamples": {"Members": [ResourceId("RD1Craft")], "Representation": "Source"},
                       "HotGameplay": {"Members": [ResourceId("RD1Controller"), ResourceId("RD1GuiScript"), ResourceId("RD1GuiModule")],
                                       "Representation": "Compiled"}},
            "Overrides": {ResourceId("RD1Procedural"): {"Representation": "Protected", "Diagnostics": "PrivateSymbols"}},
        })
    WriteJson(File, AuthoringPolicy)


def WriteEnrollment(File, Mode, Project, Signing=False, Trust=True, Key=TestContentKey):
    Rows = ["GargantuanRuntimePackageEnrollmentV1", f"mode\t{Mode}"]
    Scope = f"{Project}\t*\t{Project}\t"
    if Signing:
        Rows.append(f"signer\t{Scope}rd1-test-sign\t{SigningSeed}")
    if Trust:
        Rows.append(f"trust\t{Scope}rd1-test-sign\t{SigningPublic}")
    if Key:
        Rows.append(f"content\t{Scope}rd1-test-key\t{Key}")
    with os.fdopen(os.open(File, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "w", encoding="ascii", newline="\n") as Stream:
        Stream.write("\n".join(Rows) + "\n")


def Environment(*, Build=None, Runtime=None):
    Result = os.environ.copy()
    Result.pop(BuildEnrollment, None)
    Result.pop(RuntimeEnrollment, None)
    if Build is not None:
        Result[BuildEnrollment] = str(Build)
    if Runtime is not None:
        Result[RuntimeEnrollment] = str(Runtime)
    return Result


def Run(Command, Name, Directory, Reports, Env, Deny=False, Timeout=90):
    Start = time.perf_counter_ns()
    Result = subprocess.run([str(Part) for Part in Command], cwd=Directory, env=Env, check=False,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=Timeout, creationflags=CreateFlags)
    Duration = (time.perf_counter_ns() - Start) / 1e6
    (Reports / f"{Name}.log").write_bytes(Result.stdout)
    Require((Result.returncode != 0) == Deny, f"Unexpected status {Result.returncode} for {Name}. See its log.")
    return {"WallMs": round(Duration, 3), "Exit": Result.returncode}, Result.stdout


def ScanMarkers(Package):
    Needles = {Name: Name.encode() for Name in Markers}
    TailLength = max(map(len, Needles.values())) - 1
    Found = {Marker: [] for Marker in Markers}
    for File in sorted(Package.rglob("*")):
        if not File.is_file():
            continue
        Remaining = Needles.copy()
        with File.open("rb") as Contents:
            Tail = b""
            while Remaining and (Block := Contents.read(128 * 1024)):
                Window = Tail + Block
                for Name, Pattern in tuple(Remaining.items()):
                    if Pattern in Window:
                        Found[Name].append(File.relative_to(Package).as_posix())
                        del Remaining[Name]
                Tail = Window[-TailLength:]
    return Found


def PackageExecutable(Package):
    Manifest = ReadJson(Package / "game.package.json")
    for Candidate in ("GargantuanPlayer", "GargantuanServer"):
        Name = Candidate + (".exe" if os.name == "nt" else "")
        if (Package / Name).is_file():
            return Package / Name
    raise RuntimeError(f"Packaged native executable missing for format {Manifest['PackageFormatVersion']}")


def Build(Options, Name, Target, Preset, Project, Output, PolicyFile, Reports, Enrollment,
          Signed=False, Protected=False, Legacy=False, Deny=False):
    Distribution = Options.ServerDistribution if Target == "DedicatedServer" else Options.PlayerDistribution
    Command = [Options.Packager, "build", "--project", Project, "--output", Output, "--runtime", Distribution,
               "--configuration", "Release"]
    if not Legacy:
        Policy = {"Version": 1, "Target": Target, "Preset": Preset, "PublisherTrust": "DigestOnly", "ModLoading": "Disabled"}
        if Signed:
            Policy.update({"PublisherTrust": "Required", "SigningKeyId": "rd1-test-sign"})
        if Protected:
            Mode = "HostProvisioned" if Target == "DedicatedServer" else "LocalProvisioned"
            Policy.update({"KeyAccess": Mode, "ContentKeyId": "rd1-test-key"})
        WriteJson(PolicyFile, Policy)
        Command.extend(("--distribution", PolicyFile))
    Timing, OutputText = Run(Command, Name, Reports, Reports, Environment(Build=Enrollment), Deny=Deny)
    if Deny:
        return Timing, OutputText
    ManifestPath = Output / "game.package.json"
    Hash = Digest(ManifestPath)
    if not Legacy:
        Receipt = next((json.loads(Line) for Line in OutputText.splitlines()
                        if Line.startswith(b"{") and b'"ManifestSha256"' in Line), None)
        Require(Receipt and Receipt["Ok"] and Receipt["ManifestSha256"] == Hash, f"Missing exact package receipt for {Name}")
        if "PrivateDiagnosticsPath" in Receipt:
            Timing["PrivateDiagnosticsPath"] = Receipt["PrivateDiagnosticsPath"]
    Timing.update({"ManifestSha256": Hash, "PackageBytes": sum(File.stat().st_size for File in Output.rglob("*") if File.is_file()),
                   "ExecutableSha256": Digest(PackageExecutable(Output))})
    return Timing, OutputText


def TrustArguments(Receipt, Signed, Legacy=False):
    if Signed:
        return ["--require-package-publisher"]
    return [] if Legacy else ["--expected-manifest-sha256", Receipt["ManifestSha256"]]


def Validate(Packager, Package, Name, Reports, Env, Trust=(), Deny=False):
    return Run([Packager, "validate", Package, *Trust], f"{Name}-validate", Reports, Reports, Env, Deny=Deny)


def CheckPackage(Output, Preset, Signed, Legacy=False, Distributed=False):
    Found = ScanMarkers(Output)
    Require(not Found[IntermediateMarker], "Authoring intermediate file entered the package")
    if Distributed:
        Require(not Found[PrivateMarker] and not Found[PrivateLiteral], "Private server marker entered the distributed Player")
        Require(not (Output / "content" / "game.instance.json").exists(), "Distributed Player contains a project snapshot")
    if Legacy or Preset == "Open":
        Require(Found[ReadMarker] and Found[CompiledMarker] and Found[ProtectedMarker], "Open package did not publish the intended source")
    else:
        Require(not Found[CompiledMarker] and not Found[ProtectedMarker], "A compiled or protected script comment was distributed")
        Require(bool(Found[ReadMarker]) == (Preset == "Hybrid"), "Crafting representation differs from resolved policy")
        if Preset in {"Hybrid", "Protected"}:
            Require(not Found[ProtectedLiteral], "Protected semantic literal was emitted in cleartext")
    if not Legacy:
        Manifest = ReadJson(Output / "game.package.json")
        Require(Manifest["PackageFormatVersion"] == 4, "Managed package is not format 4")
        Catalog = ReadJson(Output / "content" / "runtime-scripts.json")
        ById = {Row["Id"]: Row for Row in Catalog["Scripts"]}
        Expected = {"RD1Craft": "Source", "RD1Controller": "Compiled", "RD1Procedural": "Protected", "RD1GuiScript": "Compiled", "RD1GuiModule": "Compiled"}
        for Name, MixedRepresentation in Expected.items():
            Id = ResourceId(Name)
            Desired = MixedRepresentation if Preset == "Hybrid" else "Source" if Preset == "Open" else Preset
            Require(Id in ById and ById[Id]["Representation"] == Desired, f"Wrong catalog representation for {Name}")
            if Desired != "Source":
                Artifact = Output / "content" / "scripts" / (Id + (".luac" if Desired == "Compiled" else ".gse"))
                Prefix = b"\x09\x03" if Desired == "Compiled" else b"GRS1"
                with Artifact.open("rb") as Stream:
                    Require(Stream.read(len(Prefix)) == Prefix, f"Wrong actual artifact for {Name}")
        if Distributed:
            for Name in ("RD1AuthoritativeInventory", "RD1InventoryRules", "PrivateGuiModule"):
                for Suffix in (".luac", ".gse"):
                    Require(not (Output / "content" / "scripts" / (ResourceId(Name) + Suffix)).exists(),
                            f"Private artifact for {Name} entered the distributed package")
                Require(ResourceId(Name) not in ById, "Private script identity entered the distributed catalog")
        Require((Output / "game.package.sig").is_file() == Signed, "Signature file does not match publisher policy")
    return Found


def Startup(Package, Name, Reports, Env, Trust, Server=False):
    Host = [PackageExecutable(Package), "--startup-smoke", *Trust]
    Host += ["--max-ticks", "30"] if Server else ["--headless", "--max-frames", "60"]
    Timing, Log = Run(Host, f"{Name}-startup", Package, Reports, Env)
    Require((ServerProof if Server else ClientProof).encode() in Log, f"Runtime proof missing for {Name}")
    if not Server:
        Require(GuiProof.encode() in Log, f"GUI clone proof missing for {Name}")
    Observed = StartupPattern.search(Log)
    Require(Observed, f"Engine startup timing missing for {Name}")
    Timing["EngineStartupMs"] = int(Observed[2])
    return Timing


def Loopback(Server, Player, Reports, HostEnv, PlayerEnv):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as Socket:
        Socket.bind(("127.0.0.1", 0))
        Endpoint = f"127.0.0.1:{Socket.getsockname()[1]}"
    ServerLog = Reports / "hybrid-loopback-server.log"
    Started = time.perf_counter_ns()
    with ServerLog.open("wb") as Log:
        Process = subprocess.Popen([str(PackageExecutable(Server)), "--bind", Endpoint, "--max-ticks", "720",
                                    "--require-package-publisher"], cwd=Server, env=HostEnv, stdout=Log,
                                   stderr=subprocess.STDOUT, creationflags=CreateFlags)
        try:
            Deadline = time.monotonic() + 20
            while not StartupPattern.search(ServerLog.read_bytes()):
                Require(Process.poll() is None and time.monotonic() < Deadline, "GNS Server did not enter the startup loop")
                time.sleep(0.05)
            ClientRuns = []
            for Attempt in ("join", "reconnect"):
                Elapsed, ClientLog = Run([PackageExecutable(Player), "--connect", Endpoint, "--headless", "--max-frames", "180",
                                          "--require-package-publisher"], f"hybrid-loopback-player-{Attempt}", Player, Reports,
                                         PlayerEnv, Timeout=25)
                Require(ClientProof.encode() in ClientLog and GuiProof.encode() in ClientLog,
                        f"Distributed Player did not execute package-backed Controller and GUI scripts on {Attempt}")
                Require(all(Value.encode() not in ClientLog for Value in (ServerProof, PrivateMarker, PrivateLiteral)),
                        "Private server execution or source marker appeared in Player diagnostics")
                ClientRuns.append({"Attempt": Attempt, **Elapsed})
            Require(Process.wait(timeout=15) == 0 and ServerProof.encode() in ServerLog.read_bytes(),
                    "Authoritative GNS Server did not complete its fixture")
        finally:
            if Process.poll() is None:
                Process.kill()
                Process.wait(timeout=5)
    return {"PlayerRuns": ClientRuns, "RolePairWallMs": round((time.perf_counter_ns() - Started) / 1e6, 3)}


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--packager", type=Path, required=True, dest="Packager")
    Parser.add_argument("--player-distribution", type=Path, required=True, dest="PlayerDistribution")
    Parser.add_argument("--server-distribution", type=Path, required=True, dest="ServerDistribution")
    Parser.add_argument("--sample", type=Path, default=Path(__file__).resolve().parents[1] / "samples" / "FirstCompleteGame", dest="Sample")
    Parser.add_argument("--reports", type=Path, required=True, dest="Reports")
    Parser.add_argument("--repeats", type=int, default=3, dest="Repeats")
    Parser.add_argument("--extra-scripts", type=int, default=0, dest="ExtraScripts")
    Parser.add_argument("--loopback", action="store_true", dest="Loopback")
    Options = Parser.parse_args()
    if not 1 <= Options.Repeats <= 10 or not 0 <= Options.ExtraScripts <= 2000:
        Parser.error("choose 1–10 repeats and 0–2000 extra scripts")
    for Name in ("Packager", "PlayerDistribution", "ServerDistribution", "Sample", "Reports"):
        setattr(Options, Name, getattr(Options, Name).absolute())
    Reports = Options.Reports
    Reports.mkdir(parents=True, exist_ok=False)
    Project = Reports / "FirstCompleteGame"
    ProjectId, Document = MakeFixture(Options.Sample, Project, Options.ExtraScripts)
    SessionIdentity = uuid.uuid4().hex
    AuthoringPolicy = Document.parent / "runtime-distribution.json"
    Policy = Reports / "request.json"
    Secrets = Reports / "synthetic-enrollment"
    Secrets.mkdir(mode=0o700)
    LocalBuild = Secrets / "local-build.tsv"
    HostBuild = Secrets / "host-build.tsv"
    LocalRuntime = Secrets / "local-runtime.tsv"
    HostRuntime = Secrets / "host-runtime.tsv"
    TrustOnly = Secrets / "trust-only.tsv"
    WrongKey = Secrets / "wrong-key.tsv"
    for File, Mode, Signing, Key in (
        (LocalBuild, "Local", True, TestContentKey), (HostBuild, "Host", True, TestContentKey),
        (LocalRuntime, "Local", False, TestContentKey), (HostRuntime, "Host", False, TestContentKey),
        (TrustOnly, "Local", False, None), (WrongKey, "Local", False, "fe" * 32),
    ):
        WriteEnrollment(File, Mode, ProjectId, Signing=Signing, Key=Key)
    LocalEnv = Environment(Runtime=LocalRuntime)
    HostEnv = Environment(Runtime=HostRuntime)
    Cases = (("LegacySource", "Open", False, False, True), ("Open", "Open", False, False, False),
             ("Compiled", "Compiled", False, False, False), ("SignedCompiled", "Compiled", True, False, False),
             ("Protected", "Protected", True, True, False), ("Hybrid", "Hybrid", True, True, False))
    Evidence = {"Platform": platform.platform(), "Machine": platform.machine(), "PackagerSha256": Digest(Options.Packager),
                "ExtraScripts": Options.ExtraScripts, "CachePolicy": "OS cache uncontrolled", "Profiles": {}, "Denials": {}}
    for Label, Preset, Signed, Protected, Legacy in Cases:
        WritePolicy(AuthoringPolicy, SessionIdentity, Preset == "Hybrid")
        Output = Reports / (Label + "-Package")
        Samples = []
        for Index in range(Options.Repeats):
            RunName = f"{Label}-{Index + 1:02}"
            Receipt, _ = Build(Options, RunName + "-build", "OfflinePlayer", Preset, Project, Output, Policy,
                               Reports, LocalBuild, Signed, Protected, Legacy)
            Trust = TrustArguments(Receipt, Signed, Legacy)
            Verified, _ = Validate(Options.Packager, Output, RunName, Reports, LocalEnv, Trust)
            Started = Startup(Output, RunName, Reports, LocalEnv, Trust)
            Samples.append({"Build": Receipt, "Validate": Verified, "Startup": Started})
        Manifests = {RunValue["Build"]["ManifestSha256"] for RunValue in Samples}
        Require(len(Manifests) == (Options.Repeats if Protected else 1),
                f"Determinism or randomized protection contract failed for {Label}")
        MarkersFound = CheckPackage(Output, Preset, Signed, Legacy)
        if Preset == "Hybrid":
            LastBuild = Samples[-1]["Build"]
            Symbols = Path(LastBuild["PrivateDiagnosticsPath"]).resolve()
            Require(Symbols.parent == Reports and Symbols.is_file(), "Private symbols are outside the expected sibling directory")
            Index = ReadJson(Symbols)
            Nodes = [ReadJson(Document)]
            Source = None
            while Nodes:
                CurrentNode = Nodes.pop()
                if CurrentNode["Properties"].get("SourceId", {}).get("String") == ResourceId("RD1Procedural"):
                    Source = CurrentNode["Properties"]["Source"]["String"]
                Nodes.extend(CurrentNode["Children"])
            Require(Source is not None and Index["ManifestSha256"] == LastBuild["ManifestSha256"] and
                    any(Row["Id"] == ResourceId("RD1Procedural") and Row["SourceSha256"] == hashlib.sha256(Source.encode()).hexdigest()
                        for Row in Index["Scripts"]), "Private symbol identity differs from the captured script")
        Evidence["Profiles"][Label] = {"Runs": Samples, "Markers": MarkersFound,
            "BuildMedianMs": statistics.median(RunValue["Build"]["WallMs"] for RunValue in Samples),
            "StartupMedianMs": statistics.median(RunValue["Startup"]["EngineStartupMs"] for RunValue in Samples)}
    # The normal matrix above retains the direct-start controls. This second
    # projection uses the same policy/enrollment and an optional authored menu.
    DirectDocument = Document.read_bytes()
    World = ReadJson(Document)
    Entry = CodeNode("TitleController", "Script", """
-- application-source-only-marker
local Title = Instance.new("ScreenGui")
Title.Parent = script.Parent
assert(require('./TitleText') == "Ready")
assert(game:SetMainMenu(Title).Ok)
task.defer(function()
    print("[Lifecycle:SignedPackage] Title ready")
    assert(game:GetService("ApplicationService"):Quit().Ok)
end)
""", "Client")
    Entry["Properties"]["Enabled"] = {"Bool": True}
    Entry["Properties"]["RunContext"]["EnumItem"][0] = next(
        Item["Properties"]["RunContext"]["EnumItem"][0] for Item, _ in WalkNodes(World)
        if "RunContext" in Item["Properties"])
    Module = CodeNode("TitleText", "ModuleScript", "-- application-module-only-marker\nreturn 'Ready'")
    World["Children"].append(Node("StandaloneTitle", "Folder", Children=[Entry, Module]))
    World["Properties"]["ApplicationEntry"] = Entry["Properties"]["SourceId"]
    WriteJson(Document, World)
    WritePolicy(AuthoringPolicy, SessionIdentity, False)
    Evidence["Application"] = {}
    Roles = ["OfflinePlayer", "DedicatedServer"] + (["DistributedPlayer"] if Options.Loopback else [])
    for Role in Roles:
        Label = "Application-" + Role
        Output = Reports / Label
        IsServer = Role == "DedicatedServer"
        Enrollment, Env = (HostBuild, HostEnv) if IsServer else (LocalBuild, LocalEnv)
        Receipt, _ = Build(Options, Label + "-build", Role, "Protected", Project, Output,
                           Policy, Reports, Enrollment, Signed=True, Protected=True)
        Trust = ["--require-package-publisher"]
        Validate(Options.Packager, Output, Label, Reports, Env, Trust)
        Manifest = ReadJson(Output / "game.package.json")
        Catalog = ReadJson(Output / "content" / "runtime-scripts.json")
        Ids = {Row["Id"] for Row in Catalog["Scripts"]}
        AppIds = {ResourceId("TitleText"), ResourceId("TitleController")}
        Require((Ids.isdisjoint(AppIds) if IsServer else AppIds <= Ids), "Wrong application script audience")
        File = Output / "content" / "application.json"
        Require(File.exists() != IsServer, "Wrong application template audience")
        if not IsServer:
            Require(Manifest["PackageFormatVersion"] == 6, "Unexpected managed application package version")
            Raw = File.read_bytes()
            Require(b"application-source-only-marker" not in Raw and b"application-module-only-marker" not in Raw,
                    "Protected application source survived projection")
            _, Log = Run([PackageExecutable(Output), "--headless", "--max-frames", "100", *Trust],
                         Label + "-startup", Output, Reports, Env)
            Require(b"[Lifecycle:SignedPackage] Title ready" in Log, "Compiled menu or sibling module failed to run")
        Evidence["Application"][Role] = Receipt
    Document.write_bytes(DirectDocument)
    WriteJson(Reports / "Qualification.json", Evidence)

    def Reject(Name, Package, Env):
        Timing, _ = Validate(Options.Packager, Package, Name, Reports, Env, Deny=True)
        Evidence["Denials"][Name] = Timing

    Reject("untrusted-unsigned-compiled", Reports / "Compiled-Package", Environment())
    Reject("unenrolled-signed-compiled", Reports / "SignedCompiled-Package", Environment())
    Reject("protected-missing-key", Reports / "Protected-Package", Environment(Runtime=TrustOnly))
    Reject("protected-wrong-key", Reports / "Protected-Package", Environment(Runtime=WrongKey))
    ProtectedTitle = Reports / "Application-OfflinePlayer"
    def RejectTitle(Name, Env):
        Label = "application-" + Name
        Reject(Label, ProtectedTitle, Env)
        StartupDenied, Log = Run([PackageExecutable(ProtectedTitle), "--headless", "--max-frames", "30",
                                 "--require-package-publisher"], Label + "-startup", ProtectedTitle, Reports, Env, Deny=True)
        Require(b"[Lifecycle:SignedPackage] Title ready" not in Log, "Rejected package executed the application")
        Evidence["Denials"][Label + "-startup"] = StartupDenied

    for Name, Env in (("untrusted-signer", Environment()), ("missing-key", Environment(Runtime=TrustOnly)),
                      ("wrong-key", Environment(Runtime=WrongKey))):
        RejectTitle(Name, Env)
    for Name, Artifact in (("template", ProtectedTitle / "content" / "application.json"),
                           ("module", ProtectedTitle / "content" / "scripts" / (ResourceId("TitleText") + ".gse"))):
        Original = Artifact.read_bytes()
        try:
            Artifact.write_bytes(Original[:-1] + bytes([Original[-1] ^ 1]))
            RejectTitle("tampered-" + Name, LocalEnv)
        finally:
            Artifact.write_bytes(Original)
    ProtectedPackage = Reports / "Protected-Package"
    Artifact = ProtectedPackage / "content" / "scripts" / (ResourceId("RD1Procedural") + ".gse")
    Original = Artifact.read_bytes()
    try:
        Artifact.write_bytes(Original[:-1] + bytes([Original[-1] ^ 1]))
        Reject("protected-tampered-ciphertext", ProtectedPackage, LocalEnv)
    finally:
        Artifact.write_bytes(Original)
    PriorPackage = Reports / "Compiled-Package"
    PriorDigest = Digest(PriorPackage / "game.package.json")
    OriginalDocument = Document.read_bytes()
    try:
        WritePolicy(AuthoringPolicy, SessionIdentity, False)
        World = ReadJson(Document)
        Controller = next(NodeValue for NodeValue, Path in WalkNodes(World) if Path[-1] == "RD1Controller")
        Controller["Properties"]["Source"]["String"] = "local ="
        WriteJson(Document, World)
        Failed, FailureLog = Build(Options, "rollback-compile-failure", "OfflinePlayer", "Compiled", Project,
                                   PriorPackage, Policy, Reports, LocalBuild, Deny=True)
        Require(b"CompilationFailed" in FailureLog and Digest(PriorPackage / "game.package.json") == PriorDigest,
                "A failed compilation did not preserve the preceding package")
        Evidence["Denials"]["rollback-compile-failure"] = Failed
    finally:
        Document.write_bytes(OriginalDocument)

    WritePolicy(AuthoringPolicy, SessionIdentity, True)
    Server = Reports / "Hybrid-Server"
    ServerReceipt, _ = Build(Options, "Hybrid-Server-build", "DedicatedServer", "Hybrid", Project, Server, Policy,
                             Reports, HostBuild, Signed=True, Protected=True)
    Validate(Options.Packager, Server, "Hybrid-Server", Reports, HostEnv, ["--require-package-publisher"])
    Evidence["Server"] = {"Build": ServerReceipt, "Markers": CheckPackage(Server, "Hybrid", True),
                          "Startup": Startup(Server, "Hybrid-Server", Reports, HostEnv, ["--require-package-publisher"], True)}
    if Options.Loopback:
        Client = Reports / "Hybrid-DistributedPlayer"
        ClientReceipt, _ = Build(Options, "Hybrid-DistributedPlayer-build", "DistributedPlayer", "Hybrid", Project,
                                 Client, Policy, Reports, LocalBuild, Signed=True, Protected=True)
        Validate(Options.Packager, Client, "Hybrid-DistributedPlayer", Reports, LocalEnv, ["--require-package-publisher"])
        Evidence["DistributedPlayer"] = {"Build": ClientReceipt, "Markers": CheckPackage(Client, "Hybrid", True, Distributed=True)}
        Evidence["Loopback"] = Loopback(Server, Client, Reports, HostEnv, LocalEnv)
    WriteJson(Reports / "Qualification.json", Evidence)
    print(f"[Runtime:Qualification] {Reports / 'Qualification.json'}")


if __name__ == "__main__":
    Main()
