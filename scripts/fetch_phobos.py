"""Fetch and verify the exact prebuilt Phobos runtime pinned by this repository."""

import argparse
import hashlib
import http.client
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.error
import urllib.request
import zipfile


ROOT = Path(__file__).resolve().parent.parent
METADATA = Path("deps/phobos-runtime.json")
OUTPUT = Path("build/upstream-runtime.zip")


def check_pin(root, pin):
    entry = subprocess.check_output(
        ["git", "-C", str(root), "ls-files", "--stage", "--", "deps/Phobos"],
        encoding="utf-8", errors="surrogateescape").strip().split()
    if len(entry) != 4 or entry[0] != "160000" or entry[2] != "0":
        raise ValueError("deps/Phobos must be a pinned Git submodule")
    if entry[1] != pin["revision"]:
        raise ValueError(f"Phobos runtime revision {pin['revision']} does not match "
                         f"deps/Phobos gitlink {entry[1]}; update the pinned runtime metadata")


def check_output(path):
    for component in (path, *path.parents):
        if component.is_symlink():
            raise ValueError(f"Refusing linked runtime archive path: {component}")
    if path.exists() and not path.is_file():
        raise ValueError(f"Runtime archive path is not a file: {path}")


def verify_archive(path, pin):
    digest = hashlib.sha256()
    with path.open("rb") as archive:
        for block in iter(lambda: archive.read(1024 * 1024), b""):
            digest.update(block)
    if digest.hexdigest() != pin["sha256"]:
        raise ValueError(f"Phobos runtime SHA256 mismatch: {path}")
    try:
        with zipfile.ZipFile(path) as archive:
            names = archive.namelist()
            if len(names) != len(pin["members"]) or set(names) != set(pin["members"]):
                raise ValueError(f"Unexpected Phobos runtime ZIP members: {path}")
            damaged = archive.testzip()
            if damaged:
                raise ValueError(f"Corrupt Phobos runtime ZIP member: {damaged}")
    except (zipfile.BadZipFile, RuntimeError, NotImplementedError) as error:
        raise ValueError(f"Invalid Phobos runtime ZIP: {path}: {error}") from error


def download(url, path):
    with urllib.request.urlopen(url, timeout=60) as response, path.open("wb") as archive:
        shutil.copyfileobj(response, archive)


def fetch(root=ROOT, output=None):
    root = Path(root).resolve()
    pin = json.loads((root / METADATA).read_text(encoding="utf-8"))
    check_pin(root, pin)
    path = Path(output) if output is not None else OUTPUT
    if not path.is_absolute():
        path = root / path
    path = Path(os.path.abspath(path))
    check_output(path)
    if path.exists():
        verify_archive(path, pin)
        print(f"Verified cached Phobos runtime: {path}")
        return path

    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix=path.name + ".", suffix=".part",
                                         dir=path.parent, delete=False) as archive:
            temporary = Path(archive.name)
        try:
            download(pin["url"], temporary)
        except (urllib.error.URLError, http.client.HTTPException, OSError) as error:
            raise ValueError(f"Could not download pinned Phobos artifact {pin['artifact_id']}: "
                             f"{error}. The artifact may be unavailable or expired; "
                             "restore this exact artifact or update the pin explicitly.") from error
        verify_archive(temporary, pin)
        check_output(path)
        if path.exists():
            raise ValueError(f"Runtime archive appeared during download; refusing to replace it: {path}")
        os.replace(temporary, path)
        print(f"Verified downloaded Phobos runtime: {path}")
        return path
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="Archive path (default: build/upstream-runtime.zip)")
    args = parser.parse_args()
    try:
        fetch(output=args.output)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Phobos runtime fetch failed: {error}\n")


if __name__ == "__main__":
    main()
