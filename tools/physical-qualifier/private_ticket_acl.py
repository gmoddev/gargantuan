"""Windows ACL boundary for one-run Farm32 ticket material."""

import csv
import os
from pathlib import Path
import stat
import subprocess


def Run(Arguments, Environment=None):
    Hidden = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
    Result = subprocess.run(Arguments, check=True, capture_output=True, text=True,
                            timeout=15, env=Environment, **Hidden)
    return Result.stdout.strip()


def UserSid():
    Row = next(csv.reader([Run(["whoami", "/user", "/fo", "csv", "/nh"])]))
    if len(Row) != 2 or not Row[1].startswith("S-1-"):
        raise ValueError("[Qualification:FarmTickets] current user SID is unavailable")
    return Row[1]


def AssertPrivate(Root):
    Original = Path(Root)
    if Original.is_symlink() or (hasattr(Original, "is_junction") and Original.is_junction()):
        raise ValueError("[Qualification:FarmTickets] private root is a link")
    Root = Original.resolve(strict=True)
    if not Root.is_dir():
        raise ValueError("[Qualification:FarmTickets] private root is not a regular directory")
    if os.name != "nt":
        if stat.S_IMODE(Root.stat().st_mode) & 0o077:
            raise ValueError("[Qualification:FarmTickets] private root permits group or other access")
        return
    Sid = UserSid()
    Environment = dict(os.environ, FARM32_PRIVATE_ROOT=str(Root), FARM32_OWNER_SID=Sid)
    Script = r"""
$ErrorActionPreference = 'Stop'
$Acl = Get-Acl -LiteralPath $env:FARM32_PRIVATE_ROOT
if (-not $Acl.AreAccessRulesProtected) { throw 'private ticket ACL inherits access' }
$Owner = $Acl.GetOwner([Security.Principal.SecurityIdentifier]).Value
if ($Owner -cne $env:FARM32_OWNER_SID) { throw 'private ticket owner changed' }
$Allowed = @($env:FARM32_OWNER_SID, 'S-1-5-18', 'S-1-5-32-544', 'S-1-3-4')
$Observed = @($Acl.Access | ForEach-Object {
    $_.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value
})
if ($Observed.Count -lt 1 -or @($Observed | Where-Object { $_ -notin $Allowed }).Count -ne 0 -or
    $env:FARM32_OWNER_SID -notin $Observed) { throw 'private ticket ACL permits another principal' }
"""
    try:
        Run(["pwsh.exe", "-NoProfile", "-NonInteractive", "-Command", Script], Environment)
    except subprocess.CalledProcessError as Error:
        for Reason in ("private ticket ACL inherits access", "private ticket owner changed",
                       "private ticket ACL permits another principal"):
            if Reason in (Error.stderr or ""):
                raise ValueError("[Qualification:FarmTickets] " + Reason) from Error
        raise ValueError("[Qualification:FarmTickets] private ticket ACL is not confined") from Error


def Harden(Root):
    Root = Path(Root).resolve(strict=True)
    if os.name != "nt":
        Root.chmod(0o700)
    else:
        Sid = UserSid()
        try:
            Run(["icacls.exe", str(Root), "/grant:r", f"*{Sid}:(OI)(CI)F",
                 "*S-1-5-18:(OI)(CI)F", "*S-1-5-32-544:(OI)(CI)F"])
            Run(["icacls.exe", str(Root), "/inheritance:r"])
            Run(["icacls.exe", str(Root), "/setowner", f"*{Sid}"])
        except subprocess.CalledProcessError as Error:
            raise ValueError("[Qualification:FarmTickets] could not confine private ticket ACL") from Error
    AssertPrivate(Root)
