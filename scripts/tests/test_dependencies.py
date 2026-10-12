"""Dependency preparation safety checks using local Git fixtures only."""

import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[1] / "dependencies.py"
SPEC = importlib.util.spec_from_file_location("dependencies", MODULE_PATH)
dependencies = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(dependencies)


def git(path, *arguments):
    return subprocess.run(
        ["git", "-C", str(path), *arguments], check=True,
        capture_output=True, text=True,
    ).stdout.strip()


def repository(path):
    path.mkdir()
    git(path, "init", "--quiet")
    git(path, "config", "user.name", "Dependency fixture")
    git(path, "config", "user.email", "fixture@example.invalid")
    git(path, "config", "commit.gpgSign", "false")
    (path / "source.txt").write_text("baseline\n", encoding="utf-8")
    git(path, "add", ".")
    git(path, "commit", "--quiet", "-m", "Fixture baseline")
    return git(path, "rev-parse", "HEAD")


class DependencyTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name).resolve()
        self.root = self.directory / "project"
        self.root.mkdir()
        self.yrpp = self.directory / "yrpp-origin"
        self.yrpp_revision = repository(self.yrpp)
        self.phobos = self.directory / "phobos-origin"
        self.old_phobos_revision = repository(self.phobos)
        (self.phobos / ".gitmodules").write_text(
            '[submodule "YRpp"]\n\tpath = YRpp\n'
            f'\turl = {self.yrpp.as_uri()}\n', encoding="utf-8",
        )
        git(self.phobos, "add", ".gitmodules")
        git(self.phobos, "update-index", "--add", "--cacheinfo",
            "160000", self.yrpp_revision, "YRpp")
        git(self.phobos, "commit", "--quiet", "-m", "Add YRpp gitlink")
        self.phobos_revision = git(self.phobos, "rev-parse", "HEAD")
        self.lock = {"schema": 1, "profiles": {}}
        for profile in dependencies.PROFILES:
            self.lock["profiles"][profile] = {
                "phobos": {"url": self.phobos.as_uri(), "revision": self.phobos_revision,
                           "path": f".deps/{profile}/phobos"},
                "yrpp": {"url": self.yrpp.as_uri(), "revision": self.yrpp_revision,
                         "path": f".deps/{profile}/phobos/YRpp"},
            }
        self.write_lock()

    def write_lock(self):
        (self.root / "dependencies.lock.json").write_text(
            json.dumps(self.lock), encoding="utf-8",
        )

    def prepare(self, profile="upstream"):
        return dependencies.prepare(profile, self.root)

    def check(self, profile="upstream"):
        return dependencies.check(profile, self.root)

    def test_prepare_and_check_exact_clean_pins(self):
        paths = self.prepare()
        self.assertEqual(self.check(), paths)
        for name, path in paths.items():
            self.assertEqual(git(path, "rev-parse", "HEAD"),
                             self.lock["profiles"]["upstream"][name]["revision"])
            self.assertEqual(git(path, "rev-parse", "--abbrev-ref", "HEAD"), "HEAD")

    def test_prepare_is_idempotent_and_does_not_fetch_existing_checkouts(self):
        self.prepare()
        with mock.patch.object(dependencies, "_git", wraps=dependencies._git) as calls:
            self.prepare()
        self.assertFalse(any("fetch" in call.args[1:] for call in calls.call_args_list))

    def test_check_missing_does_not_fetch(self):
        with mock.patch.object(dependencies, "_git", wraps=dependencies._git) as calls:
            with self.assertRaisesRegex(dependencies.DependencyError, "Missing phobos checkout"):
                self.check()
        self.assertFalse(any("fetch" in call.args[1:] for call in calls.call_args_list))

    def test_dirty_checkouts_are_preserved_and_refused(self):
        paths = self.prepare()
        for name in ("phobos", "yrpp"):
            with self.subTest(name=name):
                source = paths[name] / "source.txt"
                original = source.read_bytes()
                source.write_bytes(b"local changes\n")
                for action in (self.check, self.prepare):
                    with self.assertRaisesRegex(dependencies.DependencyError, f"{name} checkout is dirty"):
                        action()
                self.assertEqual(source.read_bytes(), b"local changes\n")
                source.write_bytes(original)

    def test_untracked_files_are_refused(self):
        paths = self.prepare()
        (paths["phobos"] / "local.txt").write_text("keep", encoding="utf-8")
        with self.assertRaisesRegex(dependencies.DependencyError, "phobos checkout is dirty"):
            self.prepare()

    def test_wrong_head_is_refused_without_reset(self):
        paths = self.prepare()
        self.lock["profiles"]["upstream"]["phobos"]["revision"] = self.old_phobos_revision
        self.write_lock()
        for action in (self.check, self.prepare):
            with self.assertRaisesRegex(dependencies.DependencyError, "HEAD is"):
                action()
        self.assertEqual(git(paths["phobos"], "rev-parse", "HEAD"), self.phobos_revision)

    def test_enclosing_project_is_not_a_dependency_checkout(self):
        git(self.root, "init", "--quiet")
        target = self.root / ".deps/upstream/phobos"
        target.mkdir(parents=True)
        (target / "local.txt").write_text("keep", encoding="utf-8")
        for action in (self.check, self.prepare):
            with self.assertRaisesRegex(dependencies.DependencyError, "not its own checkout"):
                action()
        self.assertEqual((target / "local.txt").read_text(), "keep")
        self.assertFalse((target / ".git").exists())

    def test_unsafe_paths_are_rejected_before_git(self):
        for value in ("../outside", "/tmp/outside", ".deps/../outside",
                      ".deps//upstream/phobos", ".deps/./upstream/phobos",
                      r".deps\upstream\phobos", "C:/outside", ".deps/upstream/elsewhere"):
            with self.subTest(value=value):
                self.lock["profiles"]["upstream"]["phobos"]["path"] = value
                self.write_lock()
                with mock.patch.object(dependencies, "_git") as calls:
                    with self.assertRaises(dependencies.DependencyError):
                        self.prepare()
                calls.assert_not_called()

    def test_symlink_path_is_refused(self):
        outside = self.directory / "outside"
        outside.mkdir()
        try:
            (self.root / ".deps").symlink_to(outside, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"Symlinks unavailable: {error}")
        with self.assertRaisesRegex(dependencies.DependencyError, "linked dependency path"):
            self.prepare()
        self.assertEqual(list(outside.iterdir()), [])

    def test_git_metadata_symlink_is_refused(self):
        paths = self.prepare()
        original = paths["yrpp"] / ".git"
        moved = self.directory / "saved-git"
        original.rename(moved)
        try:
            original.symlink_to(moved, target_is_directory=True)
        except OSError as error:
            moved.rename(original)
            self.skipTest(f"Symlinks unavailable: {error}")
        with self.assertRaisesRegex(dependencies.DependencyError, "linked Git metadata"):
            self.prepare()

    def test_only_full_commit_pins_are_allowed(self):
        for revision in ("main", "a" * 39, "z" * 40, None):
            with self.subTest(revision=revision):
                self.lock["profiles"]["upstream"]["phobos"]["revision"] = revision
                self.write_lock()
                with self.assertRaisesRegex(dependencies.DependencyError, "40-character commit SHA"):
                    self.prepare()

    def test_schema_and_missing_entries_are_rejected(self):
        self.lock["schema"] = True
        self.write_lock()
        with self.assertRaisesRegex(dependencies.DependencyError, "schema 1"):
            self.check()
        self.lock["schema"] = 1
        del self.lock["profiles"]["upstream"]["yrpp"]
        self.write_lock()
        with self.assertRaisesRegex(dependencies.DependencyError, "Missing yrpp"):
            self.check()

    def test_upstream_gitlink_must_match_lock(self):
        (self.yrpp / "source.txt").write_text("next revision\n", encoding="utf-8")
        git(self.yrpp, "commit", "--quiet", "-am", "Next YRpp revision")
        self.lock["profiles"]["upstream"]["yrpp"]["revision"] = git(self.yrpp, "rev-parse", "HEAD")
        self.write_lock()
        with self.assertRaisesRegex(dependencies.DependencyError, "gitlink does not match"):
            self.prepare()
        target = self.root / ".deps/upstream/phobos/YRpp"
        self.assertTrue(not target.exists() or not list(target.iterdir()))

    def test_reference_can_use_an_independent_yrpp_revision(self):
        (self.yrpp / "source.txt").write_text("reference headers\n", encoding="utf-8")
        git(self.yrpp, "commit", "--quiet", "-am", "Reference YRpp revision")
        self.lock["profiles"]["reference"]["yrpp"]["revision"] = git(self.yrpp, "rev-parse", "HEAD")
        self.write_lock()
        paths = self.prepare("reference")
        self.assertEqual(self.check("reference"), paths)

    def test_failed_initial_fetch_is_retryable_without_partial_checkout(self):
        self.lock["profiles"]["upstream"]["phobos"]["revision"] = "0" * 40
        self.write_lock()
        with self.assertRaises(dependencies.DependencyError):
            self.prepare()
        parent = self.root / ".deps/upstream"
        self.assertEqual(list(parent.iterdir()), [])
        self.lock["profiles"]["upstream"]["phobos"]["revision"] = self.phobos_revision
        self.write_lock()
        self.prepare()
        self.check()

    def test_failed_yrpp_fetch_keeps_phobos_and_empty_placeholder_retryable(self):
        self.lock["profiles"]["upstream"]["yrpp"]["url"] = (self.directory / "missing-origin").as_uri()
        self.write_lock()
        with self.assertRaises(dependencies.DependencyError):
            self.prepare()
        phobos = self.root / ".deps/upstream/phobos"
        self.assertEqual(git(phobos, "rev-parse", "HEAD"), self.phobos_revision)
        self.assertEqual(list((phobos / "YRpp").iterdir()), [])
        self.assertFalse(list(phobos.glob(".prepare-*")))
        self.lock["profiles"]["upstream"]["yrpp"]["url"] = self.yrpp.as_uri()
        self.write_lock()
        self.prepare()
        self.check()

    def test_cli_dispatches_profile(self):
        with mock.patch.object(dependencies, "check", return_value={}) as check:
            dependencies.main(["check", "--profile", "reference"])
        check.assert_called_once_with("reference")


if __name__ == "__main__":
    unittest.main()
