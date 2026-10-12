"""Export and reconstruct the supplied GC source using pinned comparison trees.

This preserves a reference build; it does not port the delta to current Phobos.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parent.parent
ARTIFACT = Path("integration/reference")
ASSEMBLY = Path("build/gc-reference")
RECEIPT = ".gc-reference.json"
CORE_FILES = {"Phobos.sln", "Phobos.vcxproj", "LICENSE.md", "CREDITS.md"}


class ReferenceError(Exception):
    """A source, dependency, patch or assembly failed preservation checks."""


def _git(path, *args):
    environment = {key: value for key, value in os.environ.items()
                   if key not in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR")}
    environment.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
    result = subprocess.run(
        ["git", "-c", "core.autocrlf=false", "-c", "core.safecrlf=false",
         "-C", str(path), *args], capture_output=True,
        env=environment,
    )
    if result.returncode:
        raise ReferenceError(result.stderr.decode("utf-8", "replace").strip())
    return result.stdout


def _path(value):
    if not isinstance(value, str) or not value or "\\" in value or ":" in value:
        raise ReferenceError(f"Invalid artifact path: {value!r}")
    parts = value.split("/")
    if PurePosixPath(value).is_absolute() or any(p in ("", ".", "..", ".git", ".DS_Store") for p in parts):
        raise ReferenceError(f"Unsafe artifact path: {value!r}")
    return value


def _normalize(data):
    # Preserve arbitrary legacy encodings; Git's NUL heuristic identifies binary.
    return data if b"\0" in data else data.replace(b"\r\n", b"\n")


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def _safe_location(root, path):
    """Reject symlinks and Windows junctions, including linked ancestors."""
    try:
        relative = path.relative_to(root)
    except ValueError as error:
        raise ReferenceError(f"Path leaves workspace: {path}") from error
    current = root
    for part in relative.parts:
        current = current / part
        junction = getattr(current, "is_junction", lambda: False)
        if current.is_symlink() or junction() or current.resolve() != current:
            raise ReferenceError(f"Refusing linked path: {current}")


def _json(value):
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")


def _core(path):
    return path in CORE_FILES or path.startswith(("src/", "lib/"))


def _source(path):
    return _core(path) or path.startswith("YRpp/")


def _tracked(root):
    result = {"phobos": {}, "yrpp": {}}
    untracked = _git(root, "ls-files", "--others", "--exclude-standard", "-z", "--", "src", "lib", "YRpp")
    if untracked:
        first = os.fsdecode(untracked.split(b"\0", 1)[0])
        raise ReferenceError(f"Untracked source input: {first}; git add source files before export/verify")
    for raw in _git(root, "ls-files", "-z").split(b"\0"):
        if not raw:
            continue
        path = os.fsdecode(raw)
        if path.startswith("YRpp/"):
            domain, relative = "yrpp", path[5:]
        elif _core(path):
            domain, relative = "phobos", path
        else:
            continue
        _path(relative)
        file = root / path
        _safe_location(root, file)
        if file.exists():
            if not file.is_file():
                raise ReferenceError(f"Source is not a file: {path}")
            result[domain][relative] = _normalize(file.read_bytes())
    return result


def _dependencies(root):
    try:
        import dependencies
        paths = dependencies.check(profile="reference", root=root)
    except (ImportError, ValueError, OSError) as error:
        raise ReferenceError(f"Cannot check reference dependencies: {error}") from error
    except Exception as error:
        # DependencyError belongs to the sibling module and is kept at this boundary.
        raise ReferenceError(f"Reference dependency check failed: {error}") from error
    lock = json.loads((root / "dependencies.lock.json").read_text(encoding="utf-8"))
    entries = lock["profiles"]["reference"]
    return paths, {name: entries[name]["revision"].lower() for name in ("phobos", "yrpp")}


def _base(paths, pins):
    trees = {}
    for name in ("phobos", "yrpp"):
        files = {}
        for record in _git(paths[name], "ls-tree", "-r", "-z", pins[name]).split(b"\0"):
            if not record:
                continue
            header, raw = record.split(b"\t", 1)
            mode, kind, object_id = header.split()
            relative = os.fsdecode(raw)
            if name == "phobos" and not _core(relative):
                continue
            if kind != b"blob":
                raise ReferenceError(f"Unsupported comparison entry: {name}/{relative}")
            _path(relative)
            if mode not in (b"100644", b"100755"):
                raise ReferenceError(f"Unsupported comparison mode: {name}/{relative}")
            files[relative] = _normalize(_git(paths[name], "cat-file", "blob", object_id.decode()))
        trees[name] = files
    return trees


def _write_tree(folder, tree):
    for relative, data in tree.items():
        target = folder / _path(relative)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)


def _init(folder):
    _git(folder, "init", "--quiet")
    # We do normalization ourselves, even for INIs and non-UTF8 source.
    (folder / ".git/info/attributes").write_text("* -text\n", encoding="ascii")


def _patch(base, current, temporary):
    temporary.mkdir()
    _init(temporary)
    _write_tree(temporary, base)
    _git(temporary, "add", "--all")
    for relative in base.keys() - current.keys():
        (temporary / relative).unlink()
    _write_tree(temporary, current)
    # Add new paths to the index with intent-to-add so the patch includes them.
    if current.keys() - base.keys():
        _git(temporary, "add", "--intent-to-add", "--", *sorted(current.keys() - base.keys()))
    return _git(temporary, "diff", "--binary", "--full-index", "--no-ext-diff", "--no-renames", "--")


def _hashes(trees):
    return {name: {path: _sha(data) for path, data in sorted(tree.items())}
            for name, tree in trees.items()}


def export(root=ROOT):
    """Write comparison patches and a complete source preservation manifest."""
    root = Path(root).resolve()
    folder = root / ARTIFACT
    _safe_location(root, folder)
    previous = _read_bundle(root, folder) if folder.exists() else None
    paths, pins = _dependencies(root)
    current = _tracked(root)
    base = _base(paths, pins)
    build = root / "build"
    _safe_location(root, build)
    build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".gc-export-", dir=build) as tmp:
        patches = {name: _patch(base[name], current[name], Path(tmp) / name)
                   for name in ("phobos", "yrpp")}
    manifest = {
        "schema": 1,
        "purpose": "comparison-only; historical comparison is not proven ancestry",
        "snapshot_commit": _git(root, "rev-parse", "HEAD").decode().strip(),
        "normalization": "CRLF to LF for files without NUL; binary bytes unchanged",
        "domains": {"phobos": "src/, lib/, Phobos.sln, Phobos.vcxproj, LICENSE.md, CREDITS.md",
                    "yrpp": "tracked YRpp/ files"},
        "base_refs": pins,
        "files": _hashes(current),
        "patches": {f"{name}.patch": _sha(data) for name, data in patches.items()},
    }
    folder.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".gc-publish-", dir=build) as tmp:
        staged = Path(tmp) / "bundle"
        staged.mkdir()
        for name, data in patches.items():
            (staged / f"{name}.patch").write_bytes(data)
        (staged / "manifest.json").write_bytes(_json(manifest))
        _read_bundle(root, staged)
        _safe_location(root, folder)
        backup_directory = None
        if previous is not None:
            if _read_bundle(root, folder) != previous:
                raise ReferenceError("Reference artifacts changed during export")
            backup_directory = Path(tempfile.mkdtemp(prefix=".gc-export-backup-", dir=build))
            backup = backup_directory / "previous"
            folder.rename(backup)
        elif folder.exists():
            raise ReferenceError("Reference artifact destination became populated")
        try:
            staged.rename(folder)
        except OSError:
            if previous is not None:
                try:
                    backup.rename(folder)
                except OSError as error:
                    raise ReferenceError(f"Previous artifacts preserved at {backup}; restore failed: {error}") from error
                backup_directory.rmdir()
            raise
        if backup_directory is not None:
            shutil.rmtree(backup_directory)
    return manifest


def _read_bundle(root, folder):
    try:
        _safe_location(root, folder)
        if not folder.is_dir() or {item.name for item in folder.iterdir()} != {"manifest.json", "phobos.patch", "yrpp.patch"}:
            raise ReferenceError(f"Reference artifact folder has unowned or missing files: {folder}")
        for item in folder.iterdir():
            _safe_location(root, item)
        raw = (folder / "manifest.json").read_bytes()
        manifest = json.loads(raw)
        if manifest.get("schema") != 1:
            raise ReferenceError("Reference manifest must use schema 1")
        for name in ("phobos", "yrpp"):
            for path, digest in manifest["files"][name].items():
                _path(path)
                if name == "phobos" and not _core(path):
                    raise ReferenceError(f"Path outside preservation domain: {path}")
                if not isinstance(digest, str) or len(digest) != 64:
                    raise ReferenceError(f"Invalid hash for {path}")
        patches = {}
        for name in ("phobos", "yrpp"):
            data = (folder / f"{name}.patch").read_bytes()
            if _sha(data) != manifest["patches"][f"{name}.patch"]:
                raise ReferenceError(f"Modified {name} patch; export again")
            patches[name] = data
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
        raise ReferenceError(f"Cannot read reference artifacts: {error}") from error
    return manifest, _sha(raw), patches


def _load(root):
    manifest, digest, patches = _read_bundle(root, root / ARTIFACT)
    paths, pins = _dependencies(root)
    if pins != manifest["base_refs"]:
        raise ReferenceError("Reference pins changed; export again")
    if _hashes(_tracked(root)) != manifest["files"]:
        raise ReferenceError("Source snapshot changed; export again")
    return manifest, digest, patches, paths, pins


def _expected(manifest):
    return {**manifest["files"]["phobos"],
            **{f"YRpp/{path}": digest for path, digest in manifest["files"]["yrpp"].items()}}


def _actual(folder, source_only=False):
    result = {}
    for path in folder.rglob("*"):
        _safe_location(folder, path)
        relative = path.relative_to(folder).as_posix()
        if path.is_file() and relative != RECEIPT and (not source_only or _source(relative)):
            data = path.read_bytes()
            result[relative] = _sha(_normalize(data) if _source(relative) else data)
    return result


def _extra_directories(folder):
    """Retain even empty build/IDE directories outside the source domains."""
    result = []
    for path in folder.rglob("*"):
        _safe_location(folder, path)
        relative = path.relative_to(folder).as_posix()
        if path.is_dir() and relative.split("/", 1)[0] not in ("src", "lib", "YRpp"):
            result.append(relative)
    return sorted(result)


def _project(folder):
    try:
        project = ET.parse(folder / "Phobos.vcxproj")
    except (OSError, ET.ParseError) as error:
        raise ReferenceError(f"Cannot parse reconstructed project: {error}") from error
    for node in project.iter():
        if node.tag.rsplit("}", 1)[-1] not in ("ClCompile", "ResourceCompile"):
            continue
        include = node.get("Include")
        if include is None:
            continue
        relative = _path(include.replace("\\", "/"))
        if not (folder / relative).is_file():
            raise ReferenceError(f"Project input is missing: {relative}")


def _owned(folder):
    if folder.is_symlink() or not folder.is_dir():
        raise ReferenceError(f"Assembly destination is not an owned directory: {folder}")
    try:
        _safe_location(folder, folder / RECEIPT)
        receipt = json.loads((folder / RECEIPT).read_text(encoding="utf-8"))
        sources = _actual(folder, source_only=True)
        if receipt.get("schema") != 1 or sources != receipt["files"]:
            raise ReferenceError("Existing assembly was modified; preserve or remove it manually")
        return receipt
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
        raise ReferenceError(f"Assembly lacks valid ownership receipt: {error}") from error


def verify(root=ROOT):
    """Check source freshness, dependency pins and reconstructed build inputs."""
    root = Path(root).resolve()
    manifest, digest, _, _, _ = _load(root)
    target = root / ASSEMBLY
    _safe_location(root, target)
    receipt = _owned(target)
    if receipt.get("manifest_sha256") != digest or receipt["files"] != _expected(manifest):
        raise ReferenceError("Assembly is stale; assemble again")
    _project(target)
    return target


def assemble(root=ROOT):
    """Apply patches only inside an isolated staging repository; publish after checks."""
    root = Path(root).resolve()
    manifest, digest, patches, paths, pins = _load(root)
    target = root / ASSEMBLY
    _safe_location(root, target)
    previous = _owned(target) if target.exists() or target.is_symlink() else None
    expected = _expected(manifest)
    if (previous is not None and previous.get("manifest_sha256") == digest
            and previous["files"] == expected):
        _project(target)
        return target
    previous_files = _actual(target) if previous is not None else {}
    previous_directories = _extra_directories(target) if previous is not None else []
    extras = {path: digest for path, digest in previous_files.items() if not _source(path)}
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".gc-assemble-", dir=target.parent) as tmp:
        staged = Path(tmp) / "assembled"
        staged.mkdir()
        base = _base(paths, pins)
        _write_tree(staged, base["phobos"])
        _write_tree(staged / "YRpp", base["yrpp"])
        _init(staged)
        for name in ("phobos", "yrpp"):
            if not patches[name]:
                continue
            patch = Path(tmp) / f"{name}.patch"
            patch.write_bytes(patches[name])
            arguments = ["apply", "--binary", "--whitespace=nowarn"]
            if name == "yrpp":
                arguments.append("--directory=YRpp")
            _git(staged, *arguments, str(patch))
        shutil.rmtree(staged / ".git")
        if _actual(staged) != expected:
            raise ReferenceError("Reconstructed file set or bytes do not match snapshot")
        _project(staged)
        (staged / RECEIPT).write_bytes(_json({"schema": 1, "manifest_sha256": digest, "files": expected}))
        for relative in previous_directories:
            (staged / relative).mkdir(parents=True, exist_ok=True)
        for relative, file_digest in extras.items():
            source = target / relative
            _safe_location(root, source)
            destination = staged / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
            if _sha(destination.read_bytes()) != file_digest:
                raise ReferenceError(f"Non-source file changed during reconstruction: {relative}")
        if _actual(staged) != {**expected, **extras}:
            raise ReferenceError("Non-source files were not preserved during reconstruction")
        # Recheck inputs before publication, including edits concurrent with staging.
        if _load(root)[1] != digest:
            raise ReferenceError("Reference manifest changed during assembly")
        backup_directory = None
        if previous is not None:
            if (_owned(target) != previous or _actual(target) != previous_files
                    or _extra_directories(target) != previous_directories):
                raise ReferenceError("Existing assembly changed during reconstruction")
            backup_directory = Path(tempfile.mkdtemp(prefix=".gc-assembly-backup-", dir=target.parent))
            backup = backup_directory / "previous"
            target.rename(backup)
        elif target.exists() or target.is_symlink():
            raise ReferenceError("Assembly destination became populated")
        try:
            staged.rename(target)
        except OSError:
            if previous is not None:
                try:
                    backup.rename(target)
                except OSError as error:
                    raise ReferenceError(f"Previous assembly preserved at {backup}; restore failed: {error}") from error
                backup_directory.rmdir()
            raise
        if backup_directory is not None:
            shutil.rmtree(backup_directory)
    return target


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("export", "assemble", "verify"))
    args = parser.parse_args(argv)
    try:
        result = {"export": export, "assemble": assemble, "verify": verify}[args.command]()
    except (ReferenceError, OSError) as error:
        parser.exit(1, f"Reference error: {error}\n")
    print(f"GC reference {args.command}: {ARTIFACT if args.command == 'export' else result}")


if __name__ == "__main__":
    main()
