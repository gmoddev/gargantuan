"""Read a local Git candidate into private scratch for reproducible reference extraction."""
from contextlib import contextmanager
import io
from pathlib import Path
import subprocess
import tarfile
import tempfile


@contextmanager
def Snapshot(Root, Revision):
    Data = subprocess.check_output(["git", "-C", str(Root), "archive", Revision, "assets/classes", "assets/services", "src", "include"])
    with tempfile.TemporaryDirectory(prefix="CreatorApiSource-") as Temp:
        Directory = Path(Temp)
        with tarfile.open(fileobj=io.BytesIO(Data)) as Archive:
            for Entry in Archive.getmembers():
                if not Entry.isfile(): continue
                Target = (Directory/Entry.name).resolve()
                if not Target.is_relative_to(Directory.resolve()): raise ValueError("Invalid source archive path")
                Target.parent.mkdir(parents=True, exist_ok=True)
                Target.write_bytes(Archive.extractfile(Entry).read())
        yield Directory
