"""Deliver the actual editable Engine source in the mandatory v1 runtime notice."""
import argparse
import base64
import hashlib
import io
import re
import zipfile
from pathlib import Path


Marker = "-----BEGIN GARGANTUAN ENGINE SOURCE ZIP-----"
EndMarker = "-----END GARGANTUAN ENGINE SOURCE ZIP-----"
MplDigest = "e6b6566f085df5746515c6d7e2edcaec0d2b77d527ac40d91409d783fb6c8508"


def CheckArchive(Data, Revision):
    if not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", Revision):
        raise ValueError("Supply the full build revision")
    with zipfile.ZipFile(io.BytesIO(Data)) as Archive:
        Names = Archive.namelist()
        if (len(Names) != len(set(Names)) or not any(Name.startswith("src/") for Name in Names)
                or not any(Name.startswith("include/") for Name in Names)):
            raise ValueError("Archive lacks unique editable Engine source files")
        for Name in Names:
            if re.search(r"(^/|\\|:|(^|/)\.\.(/|$))", Name):
                raise ValueError("Invalid archive member " + Name)
        Readme = Archive.read("Readme.txt").decode()
        License = Archive.read("LICENSE.txt").decode().replace("\r\n", "\n")
        if Revision not in Readme or hashlib.sha256(License.encode()).hexdigest() != MplDigest:
            raise ValueError("The embedded revision or full MPL does not match")
        Archive.read("DependencyRevisions.tsv")
        if Archive.testzip():
            raise ValueError("Damaged covered-source archive")


def BuildNotice(License, ArchivePath, Revision):
    Data = ArchivePath.read_bytes()
    CheckArchive(Data, Revision)
    if hashlib.sha256(License.encode()).hexdigest() != MplDigest:
        raise ValueError("Expected the complete reviewed MPL 2.0 text")
    Digest = hashlib.sha256(Data).hexdigest()
    Encoded = base64.b64encode(Data).decode("ascii")
    return fr"""Matching covered source

Gargantuan Engine build revision {Revision}
The editable Engine source and covered modifications accompany this copy under MPL 2.0.
Recover the ZIP offline from this file. On Windows, open PowerShell in the game or
runtime folder containing the notices folder and run the following four lines:

$Text = Get-Content -Raw -LiteralPath 'notices/Gargantuan.txt'
$Data = ($Text -split '(?m)^-----BEGIN GARGANTUAN ENGINE SOURCE ZIP-----\r?$', 2)[1] -split '(?m)^-----END GARGANTUAN ENGINE SOURCE ZIP-----\r?$', 2
$Zip = Join-Path ([IO.Path]::GetTempPath()) ('Gargantuan-Engine-Source-{Revision[:7]}-' + [Guid]::NewGuid() + '.zip')
$Bytes = [Convert]::FromBase64String($Data[0]); $Out = [IO.File]::Open($Zip, 'CreateNew'); try {{ $Out.Write($Bytes, 0, $Bytes.Length) }} finally {{ $Out.Dispose() }}; $Zip

Alternatively, decode the Base64 lines between the two marker lines using any standard
Base64 decoder. Save the ZIP outside the game folder. Open it at the printed path and read
Readme.txt, LICENSE.txt and DependencyRevisions.tsv.
SHA-256 of the ZIP: {Digest}
Keep this notice with redistributed copies. No account, network connection or payment is required.

{License}
{Marker}
""" + "\n".join(Encoded[Index:Index + 100] for Index in range(0, len(Encoded), 100)) + "\n" + EndMarker + "\n"


def ReadArchive(Notice, Revision):
    Start = "\n" + Marker + "\n"
    End = "\n" + EndMarker + "\n"
    if Notice.count(Start) != 1 or Notice.count(End) != 1 or not Notice.endswith(End):
        raise ValueError("Missing or duplicate source attachment")
    Header, Encoded = Notice.split(Start)
    Data = base64.b64decode("".join(Encoded.removesuffix(End).splitlines()), validate=True)
    if ("Gargantuan Engine build revision " + Revision not in Header
            or "SHA-256 of the ZIP: " + hashlib.sha256(Data).hexdigest() not in Header):
        raise ValueError("Recipient revision or recovered source digest differs")
    CheckArchive(Data, Revision)
    with zipfile.ZipFile(io.BytesIO(Data)) as Archive:
        License = Archive.read("LICENSE.txt").decode().replace("\r\n", "\n")
        if License not in Header:
            raise ValueError("Delivered notice does not contain the complete matching MPL")
    return Data


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("license", type=Path)
    Parser.add_argument("archive", type=Path)
    Parser.add_argument("revision")
    Parser.add_argument("notice", type=Path)
    Args = Parser.parse_args()
    Args.notice.write_text(BuildNotice(Args.license.read_text(encoding="utf-8"),
                                      Args.archive, Args.revision), encoding="utf-8", newline="\n")
