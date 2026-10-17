"""Pinned runtime checks with an uninitialized gitlink and local archive fixtures."""

import hashlib
import http.client
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock
import urllib.error
import warnings
import zipfile


spec = importlib.util.spec_from_file_location("gc_fetch_phobos", Path(__file__).parents[1] / "fetch_phobos.py")
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)


def make_archive(names):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        for name in names:
            archive.writestr(name, b"fixture runtime bytes")
    return buffer.getvalue()


class FetchPhobosTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="gc runtime fixture ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        subprocess.run(["git", "-C", str(self.root), "init", "--quiet"], check=True)
        self.pin = json.loads((runtime.ROOT / runtime.METADATA).read_text(encoding="utf-8"))
        subprocess.run(["git", "-C", str(self.root), "update-index", "--add", "--cacheinfo",
                        "160000", self.pin["revision"], "deps/Phobos"], check=True)
        self.data = make_archive(self.pin["members"])
        self.metadata = self.root / runtime.METADATA
        self.metadata.parent.mkdir()
        self.write_pin(self.data)
        self.output = self.root / runtime.OUTPUT

    def write_pin(self, data):
        self.pin["sha256"] = hashlib.sha256(data).hexdigest()
        self.metadata.write_text(json.dumps(self.pin), encoding="utf-8")

    def cache(self, data):
        self.output.parent.mkdir(parents=True, exist_ok=True)
        self.output.write_bytes(data)

    def symlink(self, path, target, directory=False):
        try:
            path.symlink_to(target, target_is_directory=directory)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"Host cannot create a symlink: {error}")

    def test_valid_cache_skips_download_without_initializing_submodule(self):
        self.cache(self.data)
        with mock.patch.object(runtime, "download") as download:
            self.assertEqual(runtime.fetch(self.root), self.output)
        download.assert_not_called()
        self.assertFalse((self.root / "deps/Phobos").exists())

    def test_pin_mismatch_refuses_before_download_or_creating_output(self):
        self.pin["revision"] = "0" * 40
        self.write_pin(self.data)
        with mock.patch.object(runtime, "download") as download:
            with self.assertRaisesRegex(ValueError, "does not match.*gitlink"):
                runtime.fetch(self.root)
        download.assert_not_called()
        self.assertFalse(self.output.parent.exists())

    def test_corrupt_cache_is_preserved_and_refused(self):
        self.cache(b"corrupt")
        with mock.patch.object(runtime, "download") as download:
            with self.assertRaisesRegex(ValueError, "SHA256 mismatch"):
                runtime.fetch(self.root)
        download.assert_not_called()
        self.assertEqual(self.output.read_bytes(), b"corrupt")

    def test_malformed_zip_is_refused_even_when_digest_matches(self):
        self.write_pin(b"not a ZIP")
        self.cache(b"not a ZIP")
        with self.assertRaisesRegex(ValueError, "Invalid.*ZIP"):
            runtime.fetch(self.root)

    def test_missing_or_duplicate_zip_members_are_refused(self):
        for members in (self.pin["members"][:-1], self.pin["members"] + [self.pin["members"][0]]):
            with self.subTest(members=members):
                with warnings.catch_warnings():
                    warnings.simplefilter("ignore", UserWarning)
                    data = make_archive(members)
                self.write_pin(data)
                self.cache(data)
                with self.assertRaisesRegex(ValueError, "Unexpected.*ZIP members"):
                    runtime.fetch(self.root)

    def test_crc_damage_is_refused_even_when_digest_matches(self):
        data = self.data.replace(b"fixture runtime bytes", b"damaged runtime bytes", 1)
        self.write_pin(data)
        self.cache(data)
        with self.assertRaisesRegex(ValueError, "Corrupt.*ZIP member"):
            runtime.fetch(self.root)

    def test_download_is_verified_before_atomic_publication(self):
        def download(url, path):
            self.assertEqual(url, self.pin["url"])
            self.assertFalse(self.output.exists())
            self.assertEqual(path.parent, self.output.parent)
            path.write_bytes(self.data)

        with mock.patch.object(runtime, "download", side_effect=download):
            self.assertEqual(runtime.fetch(self.root), self.output)
        self.assertEqual(self.output.read_bytes(), self.data)
        self.assertEqual(list(self.output.parent.iterdir()), [self.output])

    def test_failed_or_corrupt_download_does_not_publish_archive(self):
        def failed_download(url, path):
            path.write_bytes(b"partial")
            raise urllib.error.URLError("HTTP 410 Gone")

        for action, message in ((failed_download, "unavailable or expired"),
                                (mock.Mock(side_effect=http.client.IncompleteRead(b"partial")),
                                 "unavailable or expired"),
                                (lambda url, path: path.write_bytes(b"corrupt"), "SHA256 mismatch")):
            with self.subTest(message=message):
                with mock.patch.object(runtime, "download", side_effect=action):
                    with self.assertRaisesRegex(ValueError, message):
                        runtime.fetch(self.root)
                self.assertFalse(self.output.exists())
                self.assertEqual(list(self.output.parent.iterdir()), [])

    def test_archive_created_during_download_is_preserved(self):
        def download(url, path):
            path.write_bytes(self.data)
            self.output.write_bytes(b"concurrent work")

        with mock.patch.object(runtime, "download", side_effect=download):
            with self.assertRaisesRegex(ValueError, "appeared during download"):
                runtime.fetch(self.root)
        self.assertEqual(self.output.read_bytes(), b"concurrent work")
        self.assertEqual(list(self.output.parent.iterdir()), [self.output])

    def test_symlink_output_is_refused_without_touching_target(self):
        target = self.root / "local-work.zip"
        target.write_bytes(b"keep this")
        self.output.parent.mkdir()
        self.symlink(self.output, target)
        with mock.patch.object(runtime, "download") as download:
            with self.assertRaisesRegex(ValueError, "Refusing linked"):
                runtime.fetch(self.root)
        download.assert_not_called()
        self.assertEqual(target.read_bytes(), b"keep this")
        self.assertTrue(self.output.is_symlink())

    def test_symlink_parent_is_refused(self):
        target = self.root / "other"
        target.mkdir()
        self.symlink(self.output.parent, target, directory=True)
        with self.assertRaisesRegex(ValueError, "Refusing linked"):
            runtime.fetch(self.root)
        self.assertEqual(list(target.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
