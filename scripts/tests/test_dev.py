"""Task dispatch checks; these do not run MSBuild or qualify a DLL."""

import importlib.util
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("gc_dev", Path(__file__).parents[1] / "dev.py")
dev = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dev)


class DevTasks(unittest.TestCase):
    def build(self, target, configuration):
        with patch.object(sys, "argv", ["dev.py", "build", "--target", target, "--configuration", configuration]), \
                patch.object(dev, "os", SimpleNamespace(name="nt", environ={})), \
                patch.object(dev, "run") as run:
            dev.main()
            return run.call_args

    def test_reference_retains_legacy_configurations(self):
        result = self.build("reference", "DevBuild")
        self.assertIn("/property:Configuration=DevBuild", result.args)
        self.assertEqual(result.kwargs["env"]["GC_BUILD_TARGET"], "reference")
        self.assertEqual(result.kwargs["env"]["GC_PYTHON"], sys.executable)
        self.assertFalse(any("BuildType=" in arg for arg in result.args))

    def test_upstream_devbuild_is_release_nightly(self):
        result = self.build("upstream", "DevBuild")
        self.assertIn("/property:Configuration=Release", result.args)
        self.assertIn("/property:BuildType=NIGHTLY", result.args)

    def test_upstream_release_is_release_build_type(self):
        result = self.build("upstream", "Release")
        self.assertIn("/property:Configuration=Release", result.args)
        self.assertIn("/property:BuildType=RELEASE", result.args)

    def test_legacy_is_explicit(self):
        result = self.build("legacy", "Debug")
        self.assertEqual(result.kwargs["env"]["GC_BUILD_TARGET"], "legacy")
        self.assertIn("/property:Configuration=Debug", result.args)

    def test_non_windows_build_refuses_before_dispatch(self):
        with patch.object(sys, "argv", ["dev.py", "build"]), \
                patch.object(dev, "os", SimpleNamespace(name="posix", environ={})), \
                patch.object(dev, "run") as run:
            with self.assertRaises(SystemExit) as result:
                dev.main()
            self.assertEqual(result.exception.code, 2)
            run.assert_not_called()

    def test_clean_keeps_source_and_dependency_checkouts(self):
        with tempfile.TemporaryDirectory(prefix="gc task fixture ") as directory:
            root = Path(directory).resolve()
            bases = (root, root / "build/gc-reference", root / ".deps/upstream/phobos")
            for base in bases:
                (base / "Debug").mkdir(parents=True)
                (base / "Debug/compiled.obj").write_text("compiled")
                (base / "source.cpp").write_text("source")
            with patch.object(dev, "ROOT", root), patch.object(sys, "argv", ["dev.py", "clean"]):
                dev.main()
            for base in bases:
                self.assertFalse((base / "Debug").exists())
                self.assertEqual((base / "source.cpp").read_text(), "source")


if __name__ == "__main__":
    unittest.main()
