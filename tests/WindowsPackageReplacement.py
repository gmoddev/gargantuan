"""Exercise the actual Windows publisher, kernel handles, recovery and relocated Player."""
import argparse
import _winapi
import ctypes
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import time
import uuid
from contextlib import contextmanager
from ctypes import wintypes
from pathlib import Path


def Require(Value, Reason):
    if not Value:
        raise RuntimeError(Reason)


def WaitFor(Predicate, Process, Reason):
    Until = time.monotonic() + 15
    while time.monotonic() < Until:
        Result = Predicate()
        if Result:
            return Result
        if Process.poll() is not None:
            break
        time.sleep(.005)
    raise RuntimeError(Reason)


@contextmanager
def OpenFile(Pathname, Share):
    Kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    Open = Kernel.CreateFileW
    Open.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                     wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    Open.restype = wintypes.HANDLE
    Close = Kernel.CloseHandle
    Close.argtypes = [wintypes.HANDLE]
    Handle = Open(str(Pathname), 0x80000000, Share, None, 3, 0x80, None)
    if Handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        yield
    finally:
        Close(Handle)


def Main():
    Parser = argparse.ArgumentParser()
    for Key in ("packager", "runtime", "sample"):
        Parser.add_argument("--" + Key, type=Path, required=True)
    Options = Parser.parse_args()
    PackageTool, Distribution, Sample = (Value.resolve() for Value in (Options.packager, Options.runtime, Options.sample))
    with tempfile.TemporaryDirectory(prefix="Gpk") as Temporary:
        Root = Path(Temporary)
        Source = Root / "Origin"
        shutil.copytree(Sample, Source)
        # A normally saved project persists code identities before separate publisher processes open it.
        Document = Source / ".gargantuan" / "project.instance.json"
        World = json.loads(Document.read_bytes())
        Pending = [World]
        while Pending:
            Node = Pending.pop()
            Pending.extend(Node["Children"])
            if Node["ClassName"] in ("Script", "ModuleScript") and "SourceId" not in Node["Properties"]:
                Node["Properties"]["SourceId"] = {"String": uuid.uuid4().hex}
        Document.write_text(json.dumps(World, ensure_ascii=False), encoding="utf-8")
        Runtime = Root / "Runtime"
        shutil.copytree(Distribution, Runtime)
        Output = Root / "Café-茶"
        Journal = Output.with_name(Output.name + ".gpk-recovery")
        Events = []

        def Build(Configuration):
            return [PackageTool, "build", "--project", Source, "--output", Output, "--runtime", Runtime,
                    "--configuration", Configuration]

        def Start(Arguments):
            return subprocess.Popen(Arguments, cwd=Root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    creationflags=subprocess.CREATE_NO_WINDOW, env={**os.environ, "SDL_LOGGING": "info"})

        def Finish(Process, Success):
            Log = Process.communicate(timeout=30)[0].decode(errors="replace")
            Require((Process.returncode == 0) == Success, f"exit={Process.returncode} {Log[-2500:]}")
            return Log

        def Check(Name, Arguments, Success=True):
            Started = time.monotonic()
            Log = Finish(Start(Arguments), Success)
            Events.append([Name, round(time.monotonic() - Started, 4)])
            return Log

        def Identity():
            return hashlib.sha256((Output / "game.package.json").read_bytes()).hexdigest()

        ActualParent = Root / "ActualParent"
        Nested = ActualParent / "Nested"
        Nested.mkdir(parents=True)
        Marker = Nested / "preserved.txt"
        Marker.write_bytes(b"independent contents")
        Alias = Root / "IndirectParent"
        _winapi.CreateJunction(str(ActualParent), str(Alias))
        try:
            Redirected = Build("Release")
            Redirected[Redirected.index("--output") + 1] = Alias / "Nested" / "Game"
            Refusal = Check("redirected-ancestor-refused", Redirected, False)
            Require("Validate" in Refusal and "redirected" in Refusal, Refusal[-1400:])
            Require(list(Nested.iterdir()) == [Marker] and Marker.read_bytes() == b"independent contents",
                    "publisher changed a directory behind the redirected ancestor")
        finally:
            Alias.rmdir()

        for Number in range(12):
            Configuration = "Release" if Number % 2 else "Development"
            Check(f"publish-{Number}", Build(Configuration))
            Check(f"start-{Number}", [Output / "GargantuanPlayer.exe", "--startup-smoke", "--headless", "--max-frames", "5"])
        Original = Identity()
        Started = time.monotonic()
        with OpenFile(Output / "GargantuanPlayer.exe", 7):
            Writer = Start(Build("Development"))
            try:
                WaitFor(lambda: Journal.is_dir(), Writer, "publisher never reached the controlled locked rename")
                Require(Identity() == Original, "the locked destination changed")
                time.sleep(.15)
            except BaseException:
                Writer.kill()
                Writer.wait()
                raise
        Finish(Writer, True)
        Events.append(["temporary-hold-published", round(time.monotonic() - Started, 4)])
        Changed = Identity()
        Require(Changed != Original, "releasing the open child failed to publish the changed package")
        with OpenFile(Output / "GargantuanPlayer.exe", 3):
            Failure = Check("sustained-hold-refused", Build("Release"), False)
            Require("Finalize" in Failure and Identity() == Changed, "failed replacement lost its original package")
        Check("verify-prior", [PackageTool, "validate", Output])

        # Hold the staged child so we can terminate the packager in the real two-rename interval.
        Interrupted = Start(Build("Release"))
        try:
            def Candidate():
                return next((File for File in Root.glob(".gpk-gargantuan-package-tmp-*/GargantuanPlayer.exe") if File.is_file()), None)
            Staged = WaitFor(Candidate, Interrupted, "missing private staged Player")
            with OpenFile(Staged, 7):
                WaitFor(lambda: (Journal / "old").is_dir(), Interrupted, "interruption missed the rollback window")
                Require(not Output.exists(), "unexpected output before installation")
                Check("concurrent-writer-refused", Build("Release"), False)
                Interrupted.kill()
                Interrupted.wait(timeout=5)
        finally:
            if Interrupted.poll() is None:
                Interrupted.kill()
                Interrupted.wait(timeout=5)
            Interrupted.communicate()
        Foreign = Journal / "unrecognized.txt"
        Foreign.write_bytes(b"independently added file")
        Check("conflicting-recovery-preserved", Build("Release"), False)
        Require(Foreign.read_bytes() == b"independently added file" and not Output.exists() and
                hashlib.sha256((Journal / "old" / "game.package.json").read_bytes()).hexdigest() == Changed,
                "unrecognized recovery state modified the prior package")
        Foreign.unlink()
        with OpenFile(Runtime / "runtime" / "GargantuanSans.ttf", 6):
            Failed = Check("fresh-process-restore-then-staging-failure", Build("Release"), False)
            Require("Stage runtime:" in Failed, "unexpected staging disposition: " + Failed[-1500:])
            Restored = Identity()
            Require(Restored == Changed, "fresh publisher failed to restore the exact prior package")
        Check("recovered-overwrite", Build("Release"))
        Require(Identity() == Original and not Journal.exists(), "recovery did not publish the requested manifest")
        Check("recovered-integrity", [PackageTool, "validate", Output])
        Relocated = Root / "再配置"
        Output.rename(Relocated)
        Check("relocated-integrity", [PackageTool, "validate", Relocated])
        Check("relocated-start", [Relocated / "GargantuanPlayer.exe", "--startup-smoke", "--headless", "--max-frames", "5"])
        print("[Packaging:WindowsReplacement] " + json.dumps(Events), flush=True)
        print("[Packaging:WindowsReceipts] " + json.dumps({
            "Release": Original, "Development": Changed, "AfterRecovery": Restored,
            "RelocatedRelease": hashlib.sha256((Relocated / "game.package.json").read_bytes()).hexdigest(),
        }), flush=True)


if __name__ == "__main__":
    Main()
