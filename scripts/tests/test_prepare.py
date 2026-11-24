"""Preparation behavior with real local submodules and worktrees; no network."""

import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


spec = importlib.util.spec_from_file_location("gc_prepare", Path(__file__).parents[1] / "prepare.py")
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


def git(path, *args):
    return subprocess.run(["git", "-C", str(path), *args], check=True,
                          capture_output=True, text=True).stdout.strip()


def commit(path, message):
    git(path, "add", ".")
    git(path, "commit", "--quiet", "-m", message)
    return git(path, "rev-parse", "HEAD")


def repository(path):
    path.mkdir()
    git(path, "init", "--quiet")
    git(path, "config", "user.name", "Preparation fixture")
    git(path, "config", "user.email", "fixture@example.invalid")
    git(path, "config", "commit.gpgSign", "false")
    git(path, "config", "core.autocrlf", "false")
    (path / "source.txt").write_text("baseline\n", encoding="utf-8")
    return commit(path, "Baseline")


class PrepareTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="gc prepare fixture ")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name).resolve()
        self.sdk_origin = self.directory / "sdk-origin"
        self.sdk_previous = repository(self.sdk_origin)
        (self.sdk_origin / "source.txt").write_text("SDK pin\n", encoding="utf-8")
        self.sdk_pin = commit(self.sdk_origin, "SDK update")
        self.upstream_origin = self.directory / "upstream-origin"
        self.upstream_previous = repository(self.upstream_origin)
        git(self.upstream_origin, "-c", "protocol.file.allow=always", "submodule", "add",
            "--quiet", str(self.sdk_origin), "YRpp")
        self.upstream_pin = commit(self.upstream_origin, "SDK dependency")
        self.root = self.directory / "project"
        repository(self.root)
        (self.root / ".gitignore").write_text("/build/\n", encoding="utf-8")
        (self.root / "source.txt").write_text("GC reference source\n", encoding="utf-8")
        self.reference_pin = commit(self.root, "GC reference")
        git(self.root, "-c", "protocol.file.allow=always", "submodule", "add", "--quiet",
            str(self.upstream_origin), "deps/Phobos")
        commit(self.root, "Upstream dependency")
        self.upstream = self.root / "deps/Phobos"
        self.sdk = self.upstream / "YRpp"
        self.reference = self.root / "build/reference"
        # File transport is permitted for these temporary repositories only.
        transport = mock.patch.dict(os.environ, {"GIT_CONFIG_COUNT": "1",
            "GIT_CONFIG_KEY_0": "protocol.file.allow", "GIT_CONFIG_VALUE_0": "always"})
        transport.start()
        self.addCleanup(transport.stop)
        revision = mock.patch.object(prepare, "REFERENCE_REVISION", self.reference_pin)
        revision.start()
        self.addCleanup(revision.stop)

    def test_prepare_initializes_exact_nested_pins_and_is_idempotent(self):
        self.assertEqual(prepare.prepare(self.root), self.upstream)
        self.assertEqual(prepare.prepare(self.root), self.upstream)
        self.assertEqual(prepare.check(self.root), self.upstream)
        self.assertEqual(git(self.upstream, "rev-parse", "HEAD"), self.upstream_pin)
        self.assertEqual(git(self.sdk, "rev-parse", "HEAD"), self.sdk_pin)
        self.assertEqual(git(self.root, "status", "--porcelain"), "")

    def test_dirty_dependency_source_is_preserved_and_refused(self):
        prepare.prepare(self.root)
        for checkout in (self.upstream, self.sdk):
            with self.subTest(checkout=checkout.name):
                source = checkout / "source.txt"
                original = source.read_bytes()
                source.write_bytes(b"local work\n")
                for action in (prepare.prepare, prepare.check):
                    with self.assertRaises(ValueError):
                        action(self.root)
                    self.assertEqual(source.read_bytes(), b"local work\n")
                source.write_bytes(original)

    def test_untracked_dependency_work_is_preserved_and_refused(self):
        prepare.prepare(self.root)
        for checkout in (self.upstream, self.sdk):
            with self.subTest(checkout=checkout.name):
                local = checkout / "local.txt"
                local.write_bytes(b"keep this\n")
                for action in (prepare.prepare, prepare.check):
                    with self.assertRaises(ValueError):
                        action(self.root)
                    self.assertEqual(local.read_bytes(), b"keep this\n")
                local.unlink()

    def test_check_refuses_uninitialized_nested_dependency(self):
        self.assertFalse((self.sdk / ".git").exists())
        with self.assertRaises(ValueError):
            prepare.check(self.root)
        self.assertFalse((self.sdk / ".git").exists())

    def test_check_refuses_uninitialized_upstream(self):
        git(self.root, "submodule", "deinit", "--force", "--", "deps/Phobos")
        with self.assertRaises(ValueError):
            prepare.check(self.root)
        self.assertFalse((self.upstream / ".git").exists())

    def test_check_refuses_wrong_pins_without_resetting(self):
        prepare.prepare(self.root)
        for checkout, wrong, pinned in ((self.sdk, self.sdk_previous, self.sdk_pin),
                                         (self.upstream, self.upstream_previous, self.upstream_pin)):
            with self.subTest(checkout=checkout.name):
                git(checkout, "checkout", "--quiet", "--detach", wrong)
                with self.assertRaises(ValueError):
                    prepare.check(self.root)
                self.assertEqual(git(checkout, "rev-parse", "HEAD"), wrong)
                git(checkout, "checkout", "--quiet", "--detach", pinned)

    def test_parent_repository_is_not_an_initialized_dependency(self):
        git(self.root, "submodule", "deinit", "--force", "--", "deps/Phobos")
        local = self.upstream / "local.txt"
        local.write_bytes(b"unregistered work\n")
        for action in (prepare.prepare, prepare.check):
            with self.assertRaises(ValueError):
                action(self.root)
            self.assertEqual(local.read_bytes(), b"unregistered work\n")
        self.assertFalse((self.upstream / ".git").exists())

    def test_reference_is_pinned_readable_registered_and_ignored(self):
        self.assertEqual(prepare.prepare_reference(self.root), self.reference)
        self.assertEqual(prepare.prepare_reference(self.root), self.reference)
        self.assertEqual(prepare.check_reference(self.root), self.reference)
        self.assertEqual(git(self.reference, "rev-parse", "HEAD"), self.reference_pin)
        self.assertEqual((self.reference / "source.txt").read_text(), "GC reference source\n")
        self.assertEqual(git(self.root, "check-ignore", "build/reference/source.txt"),
                         "build/reference/source.txt")
        self.assertEqual(git(self.root, "status", "--porcelain"), "")

    def test_dirty_reference_is_preserved_and_refused(self):
        prepare.prepare_reference(self.root)
        source = self.reference / "source.txt"
        source.write_bytes(b"local reference edits\n")
        for action in (prepare.prepare_reference, prepare.check_reference):
            with self.assertRaises(ValueError):
                action(self.root)
            self.assertEqual(source.read_bytes(), b"local reference edits\n")

    def test_reference_in_unicode_root_is_registered(self):
        renamed = self.directory / "Krisztián project"
        self.root.rename(renamed)
        self.root = renamed
        expected = self.root / "build/reference"
        self.assertEqual(prepare.prepare_reference(self.root), expected)
        self.assertEqual(prepare.check_reference(self.root), expected)
        self.assertEqual(git(expected, "rev-parse", "HEAD"), self.reference_pin)
        self.assertEqual((expected / "source.txt").read_text(), "GC reference source\n")

    def test_wrong_reference_revision_is_preserved_and_refused(self):
        prepare.prepare_reference(self.root)
        wrong = git(self.root, "rev-parse", "HEAD")
        git(self.reference, "checkout", "--quiet", "--detach", wrong)
        for action in (prepare.prepare_reference, prepare.check_reference):
            with self.assertRaises(ValueError):
                action(self.root)
            self.assertEqual(git(self.reference, "rev-parse", "HEAD"), wrong)

    def test_independent_clone_is_not_a_registered_reference(self):
        self.reference.parent.mkdir()
        git(self.root, "clone", "--quiet", "--no-hardlinks", str(self.root), str(self.reference))
        git(self.reference, "checkout", "--quiet", "--detach", self.reference_pin)
        for action in (prepare.prepare_reference, prepare.check_reference):
            with self.assertRaises(ValueError):
                action(self.root)
            self.assertEqual(git(self.reference, "rev-parse", "HEAD"), self.reference_pin)
            self.assertTrue((self.reference / ".git").is_dir())

    def test_linked_dependency_path_is_refused(self):
        git(self.root, "submodule", "deinit", "--force", "--", "deps/Phobos")
        self.upstream.rmdir()
        try:
            self.upstream.symlink_to(self.upstream_origin, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"Symlinks unavailable: {error}")
        for action in (prepare.prepare, prepare.check):
            with self.assertRaises(ValueError):
                action(self.root)
        self.assertEqual(git(self.upstream_origin, "status", "--porcelain"), "")


if __name__ == "__main__":
    unittest.main()
