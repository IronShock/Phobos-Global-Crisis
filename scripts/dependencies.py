"""Prepare or verify pinned Phobos/YRpp checkouts without changing existing work."""

import argparse
import json
from pathlib import Path, PureWindowsPath
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent
PROFILES = ("upstream", "reference")


class DependencyError(Exception):
    """The dependency lock or a checkout does not meet its recorded contract."""


def _linked(path):
    # resolve() also catches directory junctions on Windows versions of Python
    # that do not expose Path.is_junction(). Broken symlinks need the first test.
    return path.is_symlink() or (path.exists() and path.resolve() != path.absolute())


def _git(path, *arguments):
    result = subprocess.run(
        ["git", "-C", str(path), *arguments], capture_output=True, text=True,
    )
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise DependencyError(f"Git failed in {path}: {detail}")
    return result.stdout.strip()


def _safe_path(root, value):
    if not isinstance(value, str) or not value or "\\" in value:
        raise DependencyError(f"Invalid root-relative dependency path: {value!r}")
    parts = value.split("/")
    if (parts[0] != ".deps" or len(parts) < 2
            or any(part in ("", ".", "..") for part in parts)
            or PureWindowsPath(value).drive or ":" in value):
        raise DependencyError(f"Dependency path must stay within .deps/: {value!r}")
    path = root
    for part in parts:
        path = path / part
        if _linked(path):
            raise DependencyError(f"Refusing linked dependency path: {path}")
        if path.exists() and not path.is_dir():
            raise DependencyError(f"Dependency path is not a directory: {path}")
    return path


def _load(root, profile, lock_path):
    root = Path(root).resolve()
    if profile not in PROFILES:
        raise DependencyError(f"Unknown dependency profile: {profile}")
    lock_path = Path(lock_path) if lock_path is not None else root / "dependencies.lock.json"
    try:
        lock = json.loads(lock_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise DependencyError(f"Cannot read dependency lock {lock_path}: {error}") from error
    if not isinstance(lock, dict) or type(lock.get("schema")) is not int or lock["schema"] != 1:
        raise DependencyError("Dependency lock must use schema 1")
    profiles = lock.get("profiles")
    if not isinstance(profiles, dict) or not isinstance(profiles.get(profile), dict):
        raise DependencyError(f"Dependency lock has no {profile} profile")
    entries = {}
    for name in ("phobos", "yrpp"):
        entry = profiles[profile].get(name)
        if not isinstance(entry, dict):
            raise DependencyError(f"Missing {name} entry in {profile} profile")
        revision = entry.get("revision")
        if not isinstance(revision, str) or not re.fullmatch(r"[0-9a-fA-F]{40}", revision):
            raise DependencyError(f"{name} revision must be a full 40-character commit SHA")
        url = entry.get("url")
        if (not isinstance(url, str) or not url.strip() or url.startswith("-")
                or any(character in url for character in ("\n", "\r", "\0"))):
            raise DependencyError(f"Invalid {name} repository URL")
        path = _safe_path(root, entry.get("path"))
        expected = root / ".deps" / profile / "phobos"
        if name == "yrpp":
            expected = expected / "YRpp"
        if path != expected:
            raise DependencyError(f"{name} path must be {expected.relative_to(root)}")
        entries[name] = {"url": url, "revision": revision.lower(), "path": path}
    return entries


def _empty(path):
    return path.is_dir() and not any(path.iterdir())


def _verify_checkout(name, entry):
    path = entry["path"]
    if not path.exists() or _empty(path):
        raise DependencyError(f"Missing {name} checkout at {path}; run dependencies prepare")
    if _linked(path / ".git"):
        raise DependencyError(f"Refusing linked Git metadata: {path / '.git'}")
    # git -C alone can resolve an enclosing repository when the nested checkout is absent.
    top = Path(_git(path, "rev-parse", "--show-toplevel")).resolve()
    if top != path.resolve():
        raise DependencyError(f"{path} is not its own checkout (Git resolved {top})")
    head = _git(path, "rev-parse", "HEAD")
    if head != entry["revision"]:
        raise DependencyError(
            f"{name} HEAD is {head}; expected {entry['revision']}. "
            "Existing checkouts are never reset; relocate the checkout or resolve it manually."
        )
    if _git(path, "cat-file", "-t", "HEAD") != "commit":
        raise DependencyError(f"{name} pin must identify a commit")
    arguments = ["status", "--porcelain=v1", "--untracked-files=all"]
    if name == "phobos":
        # The lock checks YRpp independently; reference deliberately mixes revisions.
        arguments.append("--ignore-submodules=all")
    if _git(path, *arguments):
        raise DependencyError(f"{name} checkout is dirty: {path}; preserve or resolve local changes")


def _verify_gitlink(entries):
    output = _git(entries["phobos"]["path"], "ls-tree", "HEAD", "--", "YRpp")
    expected = f"160000 commit {entries['yrpp']['revision']}\tYRpp"
    if output != expected:
        raise DependencyError(
            "Upstream Phobos YRpp gitlink does not match the locked YRpp revision: "
            f"{output or 'missing gitlink'}"
        )


def _publish(staged, target):
    if not target.exists():
        staged.rename(target)
        return
    if not _empty(target):
        raise DependencyError(f"Dependency destination became populated: {target}")
    # Git creates an empty directory for a gitlink. Preserve that directory while
    # moving the completed checkout into it, including on Windows.
    moved = []
    try:
        for item in staged.iterdir():
            destination = target / item.name
            item.rename(destination)
            moved.append(destination)
    except OSError:
        for item in reversed(moved):
            item.rename(staged / item.name)
        raise


def _prepare_checkout(name, entry):
    target = entry["path"]
    if target.exists() and not _empty(target):
        _verify_checkout(name, entry)
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".prepare-", dir=target.parent) as temporary:
        staged = Path(temporary) / "checkout"
        staged.mkdir()
        _git(staged, "init", "--quiet")
        _git(staged, "fetch", "--depth=1", "--no-tags", "--", entry["url"], entry["revision"])
        if _git(staged, "cat-file", "-t", "FETCH_HEAD") != "commit":
            raise DependencyError(f"{name} revision does not identify a commit")
        _git(staged, "checkout", "--quiet", "--detach", entry["revision"])
        staged_entry = {**entry, "path": staged}
        _verify_checkout(name, staged_entry)
        _publish(staged, target)


def check(profile="upstream", root=ROOT, lock_path=None):
    """Verify exact clean pins without fetching; return {name: checkout Path}."""
    entries = _load(root, profile, lock_path)
    for name, entry in entries.items():
        _verify_checkout(name, entry)
    if profile == "upstream":
        _verify_gitlink(entries)
    return {name: entry["path"] for name, entry in entries.items()}


def prepare(profile="upstream", root=ROOT, lock_path=None):
    """Fetch missing checkouts only, refusing mismatched or modified existing work."""
    entries = _load(root, profile, lock_path)
    # Refuse invalid existing work before preparing any missing dependency.
    for name, entry in entries.items():
        if entry["path"].exists() and not _empty(entry["path"]):
            _verify_checkout(name, entry)
    _prepare_checkout("phobos", entries["phobos"])
    if profile == "upstream":
        _verify_gitlink(entries)
    _prepare_checkout("yrpp", entries["yrpp"])
    return check(profile, root, lock_path)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "check"))
    parser.add_argument("--profile", choices=PROFILES, default="upstream")
    args = parser.parse_args(argv)
    try:
        paths = {"prepare": prepare, "check": check}[args.command](args.profile)
    except (DependencyError, OSError) as error:
        parser.exit(1, f"Dependency error: {error}\n")
    for name, path in paths.items():
        print(f"{args.profile} {name}: {path}")


if __name__ == "__main__":
    main()
