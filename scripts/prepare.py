"""Standard Git preparation for upstream dependencies and the GC reference."""

import argparse
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent
UPSTREAM = Path("deps/Phobos")
REFERENCE = Path("build/reference")
REFERENCE_REVISION = "b242f51225d70ae8da83826166e813b5027d69a6"


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args],
                                   encoding="utf-8", errors="surrogateescape").strip()


def location(root, relative):
    path = root
    for part in relative.parts:
        path /= part
        if path.is_symlink() or path.resolve() != path.absolute():
            raise ValueError(f"Refusing linked checkout path: {path}")
    return path


def clean_checkout(path, revision=None):
    if Path(git(path, "rev-parse", "--show-toplevel")).resolve() != path.resolve():
        raise ValueError(f"Not an independent checkout: {path}")
    if revision and git(path, "rev-parse", "HEAD") != revision:
        raise ValueError(f"Wrong revision in {path}; existing work is never reset")
    if git(path, "status", "--porcelain", "--untracked-files=all", "--ignore-submodules=all"):
        raise ValueError(f"Checkout has local changes: {path}")


def check(root=ROOT):
    root = Path(root).resolve()
    path = location(root, UPSTREAM)
    entry = git(root, "ls-files", "--stage", "--", UPSTREAM.as_posix()).split()
    if len(entry) != 4 or entry[0] != "160000" or entry[2] != "0":
        raise ValueError("deps/Phobos must be a pinned Git submodule")
    clean_checkout(path, entry[1])
    states = git(root, "submodule", "status", "--recursive", "--", UPSTREAM.as_posix())
    for line in states.splitlines():
        # strip() removes the leading clean-state space only on the first line.
        if line.startswith(("-", "+", "U")):
            raise ValueError(f"Submodule is uninitialized or at the wrong pin: {line}")
        fields = line.split()
        nested = location(root, Path(fields[1]))
        clean_checkout(nested, fields[0])
    return path


def prepare(root=ROOT):
    root = Path(root).resolve()
    path = location(root, UPSTREAM)
    if path.exists() and any(path.iterdir()):
        clean_checkout(path)
        sdk = location(root, UPSTREAM / "YRpp")
        if sdk.exists() and any(sdk.iterdir()):
            clean_checkout(sdk)
    subprocess.run(["git", "-C", str(root), "submodule", "update", "--init", "--recursive", "--", UPSTREAM.as_posix()], check=True)
    return check(root)


def check_reference(root=ROOT):
    root = Path(root).resolve()
    path = location(root, REFERENCE)
    clean_checkout(path, REFERENCE_REVISION)
    registered = git(root, "worktree", "list", "--porcelain", "-z").split("\0")
    if not any(record.startswith("worktree ") and Path(record[9:]).resolve() == path
               for record in registered):
        raise ValueError(f"Reference is not a worktree of this repository: {path}")
    return path


def prepare_reference(root=ROOT):
    root = Path(root).resolve()
    path = location(root, REFERENCE)
    if path.exists():
        return check_reference(root)
    available = subprocess.run(["git", "-C", str(root), "cat-file", "-e", REFERENCE_REVISION + "^{commit}"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if available.returncode:
        subprocess.run(["git", "-C", str(root), "fetch", "--depth=1", "origin", REFERENCE_REVISION], check=True)
    path.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["git", "-C", str(root), "worktree", "add", "--detach", str(path), REFERENCE_REVISION], check=True)
    return check_reference(root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "check", "prepare-reference", "check-reference"))
    args = parser.parse_args()
    action = {"prepare": prepare, "check": check, "prepare-reference": prepare_reference, "check-reference": check_reference}
    try:
        print(action[args.command]())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Preparation failed: {error}\n")


if __name__ == "__main__":
    main()
