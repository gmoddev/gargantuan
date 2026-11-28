# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Exercise an installed Linux Player on a dedicated, bounded tmpfs profile."""
import argparse
import errno
import json
from pathlib import Path
import shutil
import tempfile

from SpatialConstructionPackageSession import ManagedEnvironment, Run, WriteJson
from StandaloneLifecyclePlayer import PrepareProject


Title = """
local Menu = Instance.new("ScreenGui")
Menu.Parent = script.Parent
assert(game:SetMainMenu(Menu).Ok)
local Saves = game:GetService("WorldSaveService")
task.defer(function()
    local Listed = Saves:List():Await()
    assert(Listed.Ok)
    if #Listed.Value.Items == 0 then
        assert(Saves:Create({Name = "Original", Seed = "0017"}):Await().Ok)
        print("[Lifecycle:Disk] Original saved")
    else
        assert(#Listed.Value.Items == 1)
        local Original = Listed.Value.Items[1]
        assert(Original.Name == "Original" and Original.Seed == "0017")
        local Created = Saves:Create({Name = "Second"}):Await()
        if Created.Ok then
            assert(Created.Value.WorldId ~= Original.WorldId)
            print("[Lifecycle:Disk] Recovered " .. Original.WorldId)
        else
            assert(Created.Code == "StorageFull" or Created.Code == "UnknownOutcome", Created.Code)
            if Created.Code == "UnknownOutcome" then
                assert(Created.RecoveryId and Created.RecoveryId ~= "")
            else
                local After = Saves:List():Await()
                assert(After.Ok and #After.Value.Items == 1 and After.Value.Items[1].WorldId == Original.WorldId)
            end
            print("[Lifecycle:Disk] Full " .. Original.WorldId)
        end
    end
    assert(game:GetService("ApplicationService"):Quit().Ok)
end)
"""


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    for Name in ("project", "packager", "runtime", "profile-mount"):
        Parser.add_argument("--" + Name, type=lambda Value: Path(Value).resolve(strict=True), required=True)
    Options = Parser.parse_args()
    Volume = Options.profile_mount
    # This workload consumes free blocks only on its explicitly mounted scratch
    # filesystem. The package, logs and system temporary directory stay outside.
    Mounts = Path("/proc/self/mountinfo").read_text().splitlines()
    if not any(Row.split()[4] == str(Volume) and Row.split(" - ", 1)[1].startswith("tmpfs ") for Row in Mounts):
        Parser.error("profile-mount must be a separate Linux tmpfs mountpoint")
    if any(Volume.iterdir()) or not 128 * 1024 <= shutil.disk_usage(Volume).total <= 8 * 1024 * 1024:
        Parser.error("use an empty dedicated tmpfs between 128 KiB and 8 MiB")
    Options.cycles = 1
    with tempfile.TemporaryDirectory(prefix="gdf-") as Scratch, tempfile.TemporaryDirectory(dir=Volume) as Profile:
        Root = Path(Scratch)
        if Root.stat().st_dev == Volume.stat().st_dev:
            Parser.error("package scratch must be outside the profile volume")
        Project = PrepareProject(Options, Root, "Menu")
        File = Project / ".gargantuan/project.instance.json"
        Scene = json.loads(File.read_bytes())
        Folder = next(Item for Item in Scene["Children"] if Item["Name"] == "Application")
        Folder["Children"][0]["Properties"]["Source"]["String"] = Title
        WriteJson(File, Scene)
        Stage = Root / "Stage"
        Run([Options.packager, "build", "--project", Project, "--runtime", Options.runtime,
             "--output", Stage, "--configuration", "Release"], Root)
        Installed = Root / "Installed"
        Stage.rename(Installed)
        Args = [Installed / "GargantuanPlayer", "--headless", "--max-frames", "240", "--profile-root", Profile]
        def Play(Marker):
            Output = Run(Args, Root, ManagedEnvironment())
            Prefix = "[Lifecycle:Disk] " + Marker
            if Prefix not in Output or "Reached requested frame limit" in Output:
                raise RuntimeError(Output[-3500:])
            return Output.split(Prefix, 1)[1].splitlines()[0].strip()
        Play("Original saved")
        Ballast = Path(Profile) / "reserved-blocks"
        with Ballast.open("xb", buffering=0) as Stream:
            try:
                while True:
                    Stream.write(bytes(4096))
            except OSError as Failure:
                if Failure.errno != errno.ENOSPC:
                    raise
        try:
            Original = Play("Full ")
        finally:
            Ballast.unlink()
        assert len(Original) == 32 and Play("Recovered ") == Original
        print("[Lifecycle:Disk] Retained the original world through ENOSPC and a fresh-process retry")


if __name__ == "__main__":
    Main()
