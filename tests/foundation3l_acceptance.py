"""Offline final conjunction. Input summaries are never an executable replay seam.

The farm observation passed to Evaluate is produced by the fixed repository
replayer in this invocation. Evaluate is separated only for negative tests.
"""
from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import importlib.util
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tempfile
import uuid
import xml.etree.ElementTree as ET
import zipfile


class EvidenceError(ValueError):
    pass


def Require(Condition, Message):
    if not Condition:
        raise EvidenceError(Message)


def ObjectPairs(Pairs):
    Result = {}
    for Key, Value in Pairs:
        Require(Key not in Result, f"duplicate JSON field: {Key}")
        Result[Key] = Value
    return Result


def ReadBytes(PathValue, Maximum=8 * 1024 * 1024):
    File = Path(PathValue).absolute()
    for Part in (File, *File.parents):
        Require(not Part.is_symlink() and not getattr(Part, "is_junction", lambda: False)(),
                f"redirected evidence path: {Part}")
    Require(File.is_file() and File.stat().st_size <= Maximum, f"missing/oversized evidence: {File}")
    Data = File.read_bytes()
    Require(len(Data) <= Maximum, f"evidence changed size: {File}")
    return Data


def ReadJson(PathValue):
    return json.loads(ReadBytes(PathValue), object_pairs_hook=ObjectPairs,
                      parse_constant=lambda Value: (_ for _ in ()).throw(EvidenceError(f"invalid number: {Value}")))


def Digest(Data):
    return hashlib.sha256(Data).hexdigest()


def Hash(Value):
    Require(isinstance(Value, str) and re.fullmatch(r"[0-9a-f]{64}", Value), "invalid SHA-256")
    return Value


def Commit(Value):
    Require(isinstance(Value, str) and re.fullmatch(r"[0-9a-f]{40}", Value), "full source commit required")
    return Value


def Pinned(Root, Entry, Maximum=8 * 1024 * 1024):
    Require(isinstance(Entry, dict) and set(Entry) == {"Path", "Sha256"}, "invalid pinned file entry")
    Relative = PurePosixPath(Entry["Path"])
    Require(not Relative.is_absolute() and str(Relative) == Entry["Path"] and
            all(Part not in ("", ".", "..") for Part in Relative.parts) and
            ":" not in Entry["Path"] and "\\" not in Entry["Path"], "noncanonical evidence path")
    Data = ReadBytes(Path(Root).joinpath(*Relative.parts), Maximum)
    Require(Digest(Data) == Hash(Entry["Sha256"]), f"evidence hash mismatch: {Relative}")
    return Data


def JsonData(Data):
    return json.loads(Data, object_pairs_hook=ObjectPairs,
                      parse_constant=lambda Value: (_ for _ in ()).throw(EvidenceError(f"invalid number: {Value}")))


def Timestamp(Value):
    Require(isinstance(Value, str), "UTC timestamp missing")
    Parsed = datetime.fromisoformat(Value.replace("Z", "+00:00"))
    Require(Parsed.utcoffset() is not None, "timezone-less timestamp")
    return Parsed


COMMON = {
    "GameSessionTests", "FrozenRecoveryQuoteTests", "gargantuan_networking_contracts",
    "gargantuan_replication_relevance", "gargantuan_content_availability",
    "gargantuan_content_ownership", "gargantuan_remote", "gargantuan_character_networking",
}
FUNDED = {
    "gargantuan_gns_funded_ack", "gargantuan_gns_funded_ack_compatibility",
    "gargantuan_gns_funded_ack_four_grant", "gargantuan_gns_funded_ack_stats",
    "gargantuan_gns_failed_send_retry_safety", "gargantuan_f1_packet_tail",
    "gargantuan_f1_four_grant", "gargantuan_f1_mixed_traffic",
}
GNS_SAFETY = {
    "gargantuan_replication", "gargantuan_real_transport", "gargantuan_gns_failed_send_retry_safety",
    "gargantuan_f1_packet_tail", "gargantuan_f1_four_grant_sanitizer_safety",
    "gargantuan_f1_mixed_traffic", "gargantuan_gns_service_fairness",
    "gargantuan_remote_real_transport", "gargantuan_character_real_transport",
    "gargantuan_game_session_real_transport",
}
CI_JOBS = {
    "Windows x64 / MSVC 19.50+ / Release": (
        ".github/workflows/native-ci.yml", "native-ci-diagnostics", COMMON | FUNDED,
        "Run complete headless CTest contract"),
    "Ubuntu 24.04 / Clang 19 / ASan + UBSan / Headless": (
        ".github/workflows/native-ci.yml", "sanitizer-ci-diagnostics", COMMON,
        "Run complete sanitizer headless CTest contract"),
    "Ubuntu 24.04 / Clang 19 / ASan + UBSan + LSan / GNS": (
        ".github/workflows/gns-sanitizers.yml", "gns-sanitizer-diagnostics", GNS_SAFETY,
        "Run GNS transport sanitizer CTest contract"),
}


def ReadJUnit(Data, Required):
    Require(b"<!DOCTYPE" not in Data.upper() and b"<!ENTITY" not in Data.upper(), "XML declarations forbidden")
    Root = ET.fromstring(Data)
    Require(Root.tag in ("testsuite", "testsuites"), "invalid JUnit root")
    Cases = Root.findall(".//testcase")
    Require(0 < len(Cases) <= 10000, "invalid test count")
    Names = [Case.get("name") for Case in Cases]
    Require(all(Names) and len(Names) == len(set(Names)), "missing/duplicate test identity")
    Require(Required <= set(Names), f"missing required tests: {sorted(Required - set(Names))}")
    for Case in Cases:
        Require(not any(Case.find(Tag) is not None for Tag in ("failure", "error")) and
                (Case.get("name") not in Required or
                 (Case.get("status", "run") in ("run", "passed") and Case.find("skipped") is None)),
                f"test failed/skipped: {Case.get('name')}")
    for Suite in Root.iter("testsuite"):
        for Field in ("failures", "errors"):
            Require(int(Suite.get(Field, "0")) == 0, f"JUnit {Field} is nonzero")
    return len(Cases)


def VerifyCI(IndexPath, IndexSha256, ExpectedCommit):
    """Pinned raw GitHub API metadata plus original download ZIPs, never a PASS label."""
    Data = ReadBytes(IndexPath)
    Require(Digest(Data) == Hash(IndexSha256), "CI index pin mismatch")
    Index = JsonData(Data)
    Require(Index.get("Format") == "GargantuanFoundation3LCI" and Index.get("Version") == 1,
            "unknown CI inventory format")
    Entries = Index.get("Jobs")
    Require(isinstance(Entries, list) and len(Entries) == len(CI_JOBS), "CI job inventory incomplete")
    Root = Path(IndexPath).parent
    Seen = set()
    WorkflowRuns = {}
    Observed = []
    for Entry in Entries:
        Name = Entry.get("Name")
        Require(Name in CI_JOBS and Name not in Seen, "unknown/duplicate CI job")
        Seen.add(Name)
        Workflow, ArtifactName, Required, StepName = CI_JOBS[Name]
        Run = JsonData(Pinned(Root, Entry["Run"]))
        Jobs = JsonData(Pinned(Root, Entry["Jobs"]))
        Metadata = JsonData(Pinned(Root, Entry["ArtifactMetadata"]))
        Require(Run.get("repository", {}).get("full_name") == "gmoddev/gargantuan" and
                Run.get("head_sha") == ExpectedCommit and Run.get("path") == Workflow and
                Run.get("event") in ("push", "workflow_dispatch") and
                Run.get("status") == "completed" and Run.get("conclusion") == "success",
                f"CI is not successful on expected source: {Name}")
        Require(type(Run.get("id")) is int and Run["id"] > 0 and
                type(Run.get("run_attempt")) is int and Run["run_attempt"] > 0, "invalid CI run identity")
        Identity = (Run["id"], Run["run_attempt"])
        Require(WorkflowRuns.setdefault(Workflow, Identity) == Identity, "mixed attempts within one workflow")
        JobRows = Jobs.get("jobs", [])
        Require(Jobs.get("total_count") == len(JobRows) and 0 < len(JobRows) <= 20, "truncated CI job listing")
        ExpectedNames = {Key for Key, Value in CI_JOBS.items() if Value[0] == Workflow}
        Require({Item.get("name") for Item in JobRows} == ExpectedNames and len(JobRows) == len(ExpectedNames),
                "unexpected/missing workflow jobs")
        Matches = [Job for Job in JobRows if Job.get("name") == Name]
        Require(len(Matches) == 1, "required CI job missing/duplicated")
        Job = Matches[0]
        Require(Job.get("run_id") == Run["id"] and Job.get("run_attempt") == Run["run_attempt"] and
                Job.get("head_sha") == ExpectedCommit and Job.get("status") == "completed" and
                Job.get("conclusion") == "success", "stale/failed CI job")
        Steps = [Step for Step in Job.get("steps", []) if Step.get("name") == StepName]
        Require(len(Steps) == 1 and Steps[0].get("status") == "completed" and
                Steps[0].get("conclusion") == "success", "required CTest step missing/failed")
        Require(Metadata.get("name") == ArtifactName and Metadata.get("expired") is False and
                Metadata.get("workflow_run", {}).get("id") == Run["id"] and
                Metadata.get("workflow_run", {}).get("head_sha") == ExpectedCommit,
                "artifact provenance mismatch")
        Require(Timestamp(Job.get("started_at")) <= Timestamp(Metadata.get("created_at")) <=
                Timestamp(Job.get("completed_at")), "artifact came from a different job attempt")
        Archive = Pinned(Root, Entry["Archive"], 128 * 1024 * 1024)
        Require(Metadata.get("digest") == "sha256:" + Digest(Archive) and
                Metadata.get("size_in_bytes") == len(Archive), "GitHub artifact digest/size mismatch")
        import io
        with zipfile.ZipFile(io.BytesIO(Archive)) as Zip:
            Infos = Zip.infolist()
            Require(0 < len(Infos) <= 10000 and sum(Item.file_size for Item in Infos) <= 256 * 1024 * 1024,
                    "unbounded CI artifact")
            Names = [Item.filename for Item in Infos]
            Require(len(Names) == len(set(Names)), "duplicate ZIP member")
            for FileName in Names:
                PathName = PurePosixPath(FileName)
                Require(not PathName.is_absolute() and ".." not in PathName.parts and
                        "\\" not in FileName and ":" not in FileName, "invalid ZIP member")
            Xml = [Item for Item in Infos if PurePosixPath(Item.filename).name in
                   ("ctest-results.xml", "ctest-gns-results.xml")]
            Require(len(Xml) == 1 and Xml[0].file_size <= 16 * 1024 * 1024, "missing/ambiguous JUnit evidence")
            Sources = [Item for Item in Infos if PurePosixPath(Item.filename).name == "qualified-source-commit.txt"]
            Require(len(Sources) == 1 and Sources[0].file_size <= 128 and
                    Zip.read(Sources[0]).decode("ascii").strip() == ExpectedCommit,
                    "actual CI checkout differs from candidate source")
            Count = ReadJUnit(Zip.read(Xml[0]), Required)
        Observed.append({"Name": Name, "RunId": Run["id"], "Attempt": Run["run_attempt"],
                         "Event": Run["event"], "StartedUtc": Job["started_at"], "CompletedUtc": Job["completed_at"],
                         "ArtifactId": Metadata["id"], "Tests": Count, "ArchiveSha256": Digest(Archive)})
    return {"State": "MEASURED_PASS", "SourceCommit": ExpectedCommit, "Jobs": Observed,
            "InventorySha256": Digest(Data)}


def VerifyQualifiedPackage(IndexRoot, Entry, ExpectedCommit, CI):
    """Stream the original GitHub ZIP; never extract, execute, or trust its manifest alone."""
    Require(CI.get("State") == "MEASURED_PASS", "qualified package needs verified CI")
    Windows = next(Job for Job in CI["Jobs"] if Job["Name"].startswith("Windows "))
    Require(Windows["Event"] == "workflow_dispatch", "qualified package requires dispatch build")
    Metadata = JsonData(Pinned(IndexRoot, Entry["ArtifactMetadata"]))
    Require(Metadata.get("name") == "qualified-scale-" + ExpectedCommit and
            Metadata.get("expired") is False and Metadata.get("workflow_run", {}).get("id") == Windows["RunId"] and
            Metadata.get("workflow_run", {}).get("head_sha") == ExpectedCommit and
            Timestamp(Windows["StartedUtc"]) <= Timestamp(Metadata.get("created_at")) <= Timestamp(Windows["CompletedUtc"]),
            "qualified package does not belong to the verified Windows build")
    Item = Entry["Archive"]
    Relative = PurePosixPath(Item["Path"])
    Require(not Relative.is_absolute() and str(Relative) == Item["Path"] and
            ".." not in Relative.parts and ":" not in Item["Path"] and "\\" not in Item["Path"],
            "invalid package archive path")
    Archive = Path(IndexRoot).joinpath(*Relative.parts).absolute()
    for Part in (Archive, *Archive.parents):
        Require(not Part.is_symlink() and not getattr(Part, "is_junction", lambda: False)(), "redirected package path")
    Require(Archive.is_file() and Archive.stat().st_size <= 1024 * 1024 * 1024, "package archive exceeds 1 GiB")
    with Archive.open('rb') as File:
        ActualHash = hashlib.file_digest(File, 'sha256').hexdigest()
    Require(ActualHash == Hash(Item["Sha256"]) and Metadata.get("digest") == "sha256:" + ActualHash and
            Metadata.get("size_in_bytes") == Archive.stat().st_size, "qualified artifact ZIP digest mismatch")
    Pins = {}
    with zipfile.ZipFile(Archive) as Zip:
        Infos = Zip.infolist()
        Require(0 < len(Infos) <= 22000 and sum(Item.file_size for Item in Infos) <= 2 * 1024 ** 3,
                "unbounded qualified package inventory")
        Files = {}
        Folded = set()
        for Info in Infos:
            Name = Info.filename
            PathName = PurePosixPath(Name)
            Require(not PathName.is_absolute() and ".." not in PathName.parts and "\\" not in Name and
                    ":" not in Name and PathName.parts[0] in ("Player", "Server", "fixture", "qualification-descriptor.json") and
                    Name.casefold() not in Folded and Info.file_size <= 256 * 1024 ** 2 and
                    ((Info.external_attr >> 16) & 0o170000) != 0o120000, "invalid qualified ZIP member")
            Folded.add(Name.casefold())
            if not Info.is_dir():
                Files[Name] = Info
        # QualifiedScalePackage deliberately retains its two source fixture
        # files and descriptor beside the two deployment roots.
        ExpectedFiles = {'fixture/content/content.manifest.json', 'fixture/content/regions/scale.instance.json',
                         'qualification-descriptor.json'}
        Require(ExpectedFiles <= set(Files) and all(Files[Key].file_size <= 1024 * 1024 for Key in ExpectedFiles),
                "qualified source fixture/descriptor missing or oversized")
        Descriptor = JsonData(Zip.read('qualification-descriptor.json'))
        Require(Descriptor.get('format') == 'GargantuanQualifiedScalePackage' and Descriptor.get('version') == 1 and
                Descriptor.get('expected_clients') == 32 and Descriptor.get('content_objects') == 512 and
                Descriptor.get('content_bytes') == 273032 and
                Descriptor.get('phases') == ['baseline', 'load', 'resident', 'evict', 'reload'],
                "qualified workload descriptor differs from canonical workload")
        for Role in ("Player", "Server"):
            Name = Role + '/deployment-sha256.json'
            Require(Name in Files and Files[Name].file_size <= 4 * 1024 ** 2, "deployment inventory absent/oversized")
            Raw = Zip.read(Name)
            Manifest = JsonData(Raw)
            Require(Manifest.get('Format') == 'GargantuanFarmDeployment' and Manifest.get('Version') == 1 and
                    Manifest.get('SourceCommit') == ExpectedCommit and isinstance(Manifest.get('Files'), list) and
                    3 <= len(Manifest['Files']) <= 10000, "invalid qualified deployment inventory")
            Pins[Role + 'DeploymentSha256'] = Digest(Raw)
            ExpectedFiles.add(Name)
            Seen = set()
            for Record in Manifest['Files']:
                Relative = Record.get('Path')
                Require(isinstance(Relative, str) and Relative.casefold() not in Seen and
                        Relative != 'deployment-sha256.json', "duplicate qualified deployment member")
                Seen.add(Relative.casefold())
                Full = Role + '/' + Relative
                Require(Full in Files and Full not in ExpectedFiles and type(Record.get('Bytes')) is int and
                        Record['Bytes'] == Files[Full].file_size, "deployment member missing/size mismatch")
                with Zip.open(Full) as File:
                    Actual = hashlib.file_digest(File, 'sha256').hexdigest()
                Require(Actual == Hash(Record.get('Sha256', '').lower()), "deployment member hash mismatch")
                ExpectedFiles.add(Full)
                Field = {f'Gargantuan{Role}.exe': 'Sha256', 'game.package.json': 'PackageSha256',
                         'content/content.manifest.json': 'ContentManifestSha256'}.get(Relative)
                if Field:
                    Pins[Role + Field] = Actual
        Require(set(Files) == ExpectedFiles and len(Pins) == 8, "qualified package contains extra/missing files")
    return {"State": "MEASURED_PASS", "ArtifactId": Metadata["id"], "ArchiveSha256": ActualHash, "Pins": Pins}


def VerifyProvenance(IndexPath, IndexPin, Inputs, CI, Four, Source):
    Data = ReadBytes(IndexPath)
    Require(Digest(Data) == Hash(IndexPin), "provenance index pin mismatch")
    Index = JsonData(Data)
    Require(Index.get('Format') == 'GargantuanFoundation3LProvenance' and Index.get('Version') == 1,
            "unrecognized provenance index")
    Root = Path(IndexPath).parent
    Package = VerifyQualifiedPackage(Root, Index['QualifiedPackage'], Source, CI)
    Stages = []
    for Provider in ('Local', 'Node'):
        ManifestBytes = ReadBytes(Path(Inputs[Provider + 'ClientEvidenceRoot']) / 'run-manifest.json')
        Manifest = JsonData(ManifestBytes)
        Require(Manifest.get('SourceCommit') == Source and Manifest.get('Provider') == Provider,
                "provider provenance identity mismatch")
        for Key, Expected in Package['Pins'].items():
            Require(Manifest.get(Key) == Expected, f"{Provider}: deployed {Key} differs from CI package")
        Stage = JsonData(Pinned(Root, Index[Provider + 'ClientStage']))
        Require(Stage.get('Format') == 'GargantuanFarm32Campaign' and Stage.get('Version') == 1 and
                Stage.get('Role') == 'CLIENT' and Stage.get('SourceCommit') == Source and
                Stage.get('RunId') == Manifest.get('RunId') and Stage.get('ManifestSha256') == Digest(ManifestBytes),
                "controller stage does not bind the replayed provider manifest")
        Stages.append(Timestamp(Stage.get('CreatedUtc')))
    Sequence = {'State': 'NOT_MEASURED', 'Reason': 'four-client controlling-host completion timestamp absent'}
    if Four.get('State') == 'MEASURED_PASS' and Four.get('CompletedUtc') is not None:
        Require(Four.get('CompletionClockDomain') == 'CONTROLLING_HOST_UTC', 'four-client completion clock domain absent')
        Require(Timestamp(Four['CompletedUtc']) <= min(Stages), 'provider campaign was staged before fresh four-client completion')
        Sequence = {'State': 'MEASURED_PASS', 'ClockDomain': 'CONTROLLING_HOST_UTC',
                    'FourCompletedUtc': Four['CompletedUtc'], 'LocalCreatedUtc': Stages[0].isoformat(),
                    'NodeCreatedUtc': Stages[1].isoformat()}
    return {'Package': Package, 'Sequence': Sequence, 'InventorySha256': Digest(Data)}


def At(Value, Path):
    for Key in Path.split("."):
        if not isinstance(Value, dict) or Key not in Value:
            return None
        Value = Value[Key]
    return Value


PROVIDER_GATES = {
    "Workload.State": "MEASURED_PASS", "Ready": 32,
    "Admission.GrantLifecycleCoverage": "MEASURED_PASS",
    "Admission.AcceptedGrantWaitBound": "MEASURED_PASS",
    "Admission.FixedWorkloadFairness.State": "MEASURED_PASS",
    "F1.State": "MEASURED_PASS",
    "Publication.Status": "ACCEPTED_STATE_CHAIN_OBSERVED",
    "Publication.CharacterDueService.Verdict": "PASS",
    "Publication.OrdinaryDemand.State": "MEASURED_PASS",
    "Clock.Status": "BOUNDED_AT_PROBE", "Lifecycle.State": "MEASURED",
    "RemoteOwnership.State": "MEASURED", "ServerWorkTicks.Status": "MEASURED_PASS",
    "RemoteCadence.Status": "MEASURED_PASS", "Capture.Status": "MEASURED_PASS",
    "Recovery.FixedServiceRecovery": "MEASURED_PASS",
    "Recovery.StrictConvergenceSufficientProof": "MEASURED_PASS",
    "Recovery.SampledJournalRetention": "MEASURED_PASS",
    "Resources.Server.CompleteSweepLimitObservation": "WITHIN_ROLE_LIMIT",
    "Resources.Clients.CompleteSweepLimitObservation": "WITHIN_ROLE_LIMIT",
    "Resources.ServerHost.Classification": "BOUNDED_SAME_HOST_SNAPSHOT_SERIES",
    "Resources.ClientHost.Classification": "BOUNDED_SAME_HOST_SNAPSHOT_SERIES",
    "EvidenceRetention.Classification": "RECONCILED_ROLE_LOCAL_INDEX_BOUNDS_ONLY",
}


def EvaluateProviders(Observation, ExpectedCommit):
    Require(Observation.get("Format") == "GargantuanPhysicalFarmAcceptanceObservation" and
            Observation.get("Version") == 1, "unrecognized provider observation")
    Require(At(Observation, "WorkloadPinParity.SourceCommit") == ExpectedCommit and
            At(Observation, "WorkloadPinParity.State") == "MEASURED", "provider source mismatch")
    Ids = [str(uuid.UUID(Observation[Provider + "RunId"])) for Provider in ("Local", "Node")]
    Require(Ids[0] != Ids[1], "providers reused run identity")
    Gates = []
    for Provider in ("Local", "Node"):
        Value = Observation[Provider]
        for Key, Expected in PROVIDER_GATES.items():
            Actual = At(Value, Key)
            Gates.append({"Gate": f"{Provider}.{Key}", "State":
                          "MEASURED_PASS" if type(Actual) is type(Expected) and Actual == Expected else
                          "MEASURED_FAIL" if Actual in ("FAIL", "MEASURED_FAIL") else "NOT_MEASURED"})
        Accepted, Retired = Value.get("AcceptedBytes"), Value.get("RetiredBytes")
        Require(type(Accepted) is int and Accepted > 0 and Accepted == Retired,
                f"{Provider}: invalid terminal conservation")
        Require(type(At(Value, "Recovery.ExactRetainedWorkBytes")) is int and
                At(Value, "Recovery.ExactRetainedWorkBytes") > 0, f"{Provider}: exact recovery W absent")
    for Key, Expected in {
        "Local.Provider.State": "LOCAL_PINNED_PACKAGE_ONLY",
        "Node.Provider.State": "AUTHENTICATED_MANIFEST_RPC_MEASURED",
        "Node.Provider.RealTls": "NEGOTIATED_TLS_MANIFEST_RPC_MEASURED",
        "Node.Provider.ProcessResources.State": "MEASURED",
    }.items():
        Gates.append({"Gate": Key, "State": "MEASURED_PASS" if At(Observation, Key) == Expected else "NOT_MEASURED"})
    return Gates


def FinalObservation(SourceCommit, ProviderObservation, CI, FourClient, Provenance=None):
    Gates = EvaluateProviders(ProviderObservation, SourceCommit)
    Gates.append({"Gate": "ExactHeadCI", "State": CI["State"]})
    Gates.append({"Gate": "FreshFourClientMixed", "State": FourClient["State"]})
    Provenance = Provenance or {'Package': {'State': 'NOT_MEASURED'}, 'Sequence': {'State': 'NOT_MEASURED'}}
    Gates.extend({'Gate': Key, 'State': Provenance[Key]['State']} for Key in ('Package', 'Sequence'))
    if CI["State"] == "MEASURED_PASS":
        Require(CI.get("SourceCommit") == SourceCommit, "CI candidate mismatch")
    if FourClient["State"] == "MEASURED_PASS":
        Require(FourClient.get("SourceCommit") == SourceCommit, "four-client candidate mismatch")
        FourId = str(uuid.UUID(FourClient["RunId"]))
        Require(FourId not in (ProviderObservation["LocalRunId"], ProviderObservation["NodeRunId"]),
                "four-client/provider run identity reused")
        uuid.UUID(FourClient["LifecycleRunId"])
    Status = "FAIL" if any(Item["State"] == "MEASURED_FAIL" for Item in Gates) else (
        "PASS" if all(Item["State"] == "MEASURED_PASS" for Item in Gates) else "INCOMPLETE")
    return {"Format": "GargantuanFoundation3LFinalAcceptance", "Version": 1,
            "Status": Status, "SourceCommit": SourceCommit,
            "LocalRunId": ProviderObservation["LocalRunId"], "NodeRunId": ProviderObservation["NodeRunId"],
            "Gates": Gates, "CI": CI, "FourClient": FourClient,
            "Provenance": Provenance,
            "Scope": "canonical fixed Foundation 3L workload; no Foundation 3M or merge authorization",
            "ResourcePolicy": "measured canonical bounds; no invented CPU/RSS/NIC percentage SLA"}


def ReplayProviders(Root, Arguments, Destination):
    Required = {"LocalReportPath", "LocalServerEvidenceRoot", "LocalClientEvidenceRoot",
                "NodeReportPath", "NodeServerEvidenceRoot", "NodeClientEvidenceRoot"}
    Optional = {"NodeTlsMatchReceiptPath", "NodeTlsMatchReceiptSha256", "NodeStagePath", "NodeStageSha256",
                "NodeRunReceiptPath", "NodeRunReceiptSha256"}
    for Provider in ("Local", "Node"):
        Optional.update(Provider + Suffix for Suffix in ("ServerCaptureIndexPath", "ClientCaptureIndexPath",
                                                       "OuterCaptureReceiptPath", "CoordinatorResultPath"))
    Require(isinstance(Arguments, dict) and Required <= set(Arguments) and
            set(Arguments) <= Required | Optional and all(isinstance(Value, str) and Value for Value in Arguments.values()),
            "invalid raw farm replay arguments")
    Command = ["pwsh", "-NoLogo", "-NoProfile", "-NonInteractive", "-File",
               str(Root / "tests/PhysicalGameSessionFarmAcceptance.ps1")]
    for Key, Value in Arguments.items():
        Command.extend(("-" + Key, Value))
    Command.extend(("-OutputPath", str(Destination)))
    subprocess.run(Command, check=True, timeout=1800)
    return ReadJson(Destination)


def ReplayFourClient(Root, Inputs, Source):
    Script = Root / "tests/foundation3l_four_client.py"
    Require(Script.is_file(), "four-client raw replayer has not been integrated")
    subprocess.run(["git", "ls-files", "--error-unmatch", "tests/foundation3l_four_client.py"],
                   cwd=Root, check=True, stdout=subprocess.DEVNULL)
    Spec = importlib.util.spec_from_file_location("foundation3l_four_client", Script)
    Module = importlib.util.module_from_spec(Spec)
    sys.modules[Spec.name] = Module
    Spec.loader.exec_module(Module)
    Result = Module.Replay(Inputs, Source)
    Require(isinstance(Result, dict) and Result.get("State") in
            ("MEASURED_PASS", "MEASURED_FAIL", "NOT_MEASURED"), "invalid four-client replay result")
    return Result


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--source-commit", required=True)
    Parser.add_argument("--farm-inputs", required=True, type=Path,
                        help="JSON raw parameter map for the fixed offline farm acceptance script")
    Parser.add_argument("--ci-index", type=Path)
    Parser.add_argument("--ci-index-sha256")
    Parser.add_argument("--four-client-inputs", type=Path, help="raw input map for fixed four-client evidence replay")
    Parser.add_argument("--provenance-index", type=Path)
    Parser.add_argument("--provenance-index-sha256")
    Parser.add_argument("--output", required=True, type=Path)
    Args = Parser.parse_args()
    Source = Commit(Args.source_commit)
    Root = Path(__file__).resolve().parents[1]
    Head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=Root, text=True).strip()
    Require(Head == Source, "run the final replay from its exact reviewed source commit")
    Require(not subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=no"], cwd=Root),
            "tracked analyzer source is dirty")
    Require(not Args.output.exists(), "output already exists")
    for Part in (Args.output.absolute(), *Args.output.absolute().parents):
        Require(not Part.is_symlink() and not getattr(Part, "is_junction", lambda: False)(), "redirected output path")
    Inputs = ReadJson(Args.farm_inputs)
    # The output and transient replay receipt must never enter immutable inputs.
    for Key, Value in Inputs.items():
        if Key.endswith("EvidenceRoot"):
            Require(not Args.output.absolute().is_relative_to(Path(Value).absolute()), "output lies in evidence root")
    CI = {"State": "NOT_MEASURED", "Reason": "exact-head GitHub metadata/artifact inventory absent"}
    if Args.ci_index is not None:
        Require(Args.ci_index_sha256 is not None, "independent CI inventory pin required")
        CI = VerifyCI(Args.ci_index, Args.ci_index_sha256, Source)
    else:
        Require(Args.ci_index_sha256 is None, "CI pin without inventory")
    with tempfile.TemporaryDirectory(prefix="gargantuan-3l-final-") as Temporary:
        Farm = ReplayProviders(Root, Inputs, Path(Temporary) / "farm.json")
        Four = (ReplayFourClient(Root, ReadJson(Args.four_client_inputs), Source)
                if Args.four_client_inputs else
                {"State": "NOT_MEASURED", "Reason": "fresh four-client raw replay required"})
        Provenance = None
        if Args.provenance_index is not None:
            Require(Args.provenance_index_sha256 is not None, "independent provenance inventory pin required")
            Provenance = VerifyProvenance(Args.provenance_index, Args.provenance_index_sha256, Inputs, CI, Four, Source)
        else:
            Require(Args.provenance_index_sha256 is None, "provenance pin without inventory")
        Result = FinalObservation(Source, Farm, CI, Four, Provenance)
        Result["FarmReplaySha256"] = Digest(ReadBytes(Path(Temporary) / "farm.json"))
        Result["FarmInputMapPath"] = str(Args.farm_inputs.absolute())
        Result["FarmInputMapSha256"] = Digest(ReadBytes(Args.farm_inputs))
        if Args.four_client_inputs:
            Result["FourClientInputMapSha256"] = Digest(ReadBytes(Args.four_client_inputs))
        Result["AnalyzerSha256"] = Digest(ReadBytes(__file__))
        with Args.output.open("x", encoding="utf-8", newline="\n") as File:
            json.dump(Result, File, indent=2)
            File.write("\n")
    print(f"[Qualification:Foundation3L] {Result['Status']} output={Args.output}")
    return 0 if Result["Status"] == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(Main())
