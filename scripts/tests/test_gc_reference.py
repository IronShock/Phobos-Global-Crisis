"""Local Git fixtures exercise preservation and refusal boundaries."""

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location("gc_reference", SCRIPTS / "gc_reference.py")
reference = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reference)


def git(path, *arguments):
    result = subprocess.run(
        ["git", "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
         "-c", "core.autocrlf=false", "-C", str(path), *arguments],
        capture_output=True, check=True,
    )
    return result.stdout.decode().strip()


def write(root, relative, data):
    file = root / relative
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_bytes(data)


class ReferenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.phobos = self.root / ".deps/reference/phobos"
        self.yrpp = self.phobos / "YRpp"
        self.yrpp.mkdir(parents=True)
        git(self.yrpp, "init", "--quiet")
        write(self.yrpp, "StaticInits.cpp", b"int initial = 1;\n")
        write(self.yrpp, "remove.h", b"old\n")
        git(self.yrpp, "add", ".")
        git(self.yrpp, "commit", "--quiet", "-m", "YRpp base")
        yrpp_pin = git(self.yrpp, "rev-parse", "HEAD")
        git(self.phobos, "init", "--quiet")
        write(self.phobos, "src/source with space.cpp", b"int source = 1;\n")
        write(self.phobos, "src/version.rc", b"resource\n")
        write(self.phobos, "src/remove.cpp", b"removed\n")
        write(self.phobos, "src/binary.dat", b"binary\0old\r\n")
        write(self.phobos, "src/legacy.cpp", b"// \x81\xff\r\n")
        write(self.phobos, "lib/header.h", b"library\n")
        write(self.phobos, "LICENSE.md", b"license\n")
        write(self.phobos, "Phobos.sln", b"solution\n")
        self.project = b'''<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
<ItemGroup><ClCompile Include="src\\source with space.cpp" />
<ClCompile Include="YRpp\\StaticInits.cpp" />
<ResourceCompile Include="src\\version.rc" />
<ClInclude Include="missing-header.h" /></ItemGroup></Project>\n'''
        write(self.phobos, "Phobos.vcxproj", self.project)
        git(self.phobos, "add", "src", "lib", "LICENSE.md", "Phobos.sln", "Phobos.vcxproj")
        git(self.phobos, "update-index", "--add", "--cacheinfo", "160000", yrpp_pin, "YRpp")
        git(self.phobos, "commit", "--quiet", "-m", "Phobos base")
        phobos_pin = git(self.phobos, "rev-parse", "HEAD")
        lock = {"schema": 1, "profiles": {"reference": {
            "phobos": {"revision": phobos_pin, "url": str(self.phobos), "path": ".deps/reference/phobos"},
            "yrpp": {"revision": yrpp_pin, "url": str(self.yrpp), "path": ".deps/reference/phobos/YRpp"},
        }}}
        write(self.root, "dependencies.lock.json", json.dumps(lock).encode())
        git(self.root, "init", "--quiet")
        write(self.root, ".gitignore", b"/.deps/\n/build/\n")
        for folder in ("src", "lib"):
            for file in (self.phobos / folder).iterdir():
                write(self.root, f"{folder}/{file.name}", file.read_bytes())
        for file in ("LICENSE.md", "Phobos.sln", "Phobos.vcxproj"):
            write(self.root, file, (self.phobos / file).read_bytes())
        for file in self.yrpp.glob("*.cpp"):
            write(self.root, f"YRpp/{file.name}", file.read_bytes())
        write(self.root, "YRpp/remove.h", b"old\n")
        git(self.root, "add", ".")
        git(self.root, "commit", "--quiet", "-m", "GC baseline")
        write(self.root, "src/source with space.cpp", b"int source = 2;\r\n")
        write(self.root, "src/binary.dat", b"binary\0new\r\n")
        write(self.root, "src/legacy.cpp", b"// \x81\xff updated\r\n")
        write(self.root, "src/added.cpp", b"added\r\n")
        write(self.root, "YRpp/new.h", b"new\r\n")
        (self.root / "src/remove.cpp").unlink()
        (self.root / "YRpp/remove.h").unlink()
        git(self.root, "add", "src/added.cpp", "YRpp/new.h")

    def tearDown(self):
        self.temporary.cleanup()

    def test_roundtrip_domains_and_normalized_bytes(self):
        manifest = reference.export(self.root)
        self.assertIn("not proven ancestry", manifest["purpose"])
        target = reference.assemble(self.root)
        self.assertEqual(reference.verify(self.root), target)
        self.assertEqual((target / "src/source with space.cpp").read_bytes(), b"int source = 2;\n")
        self.assertEqual((target / "src/legacy.cpp").read_bytes(), b"// \x81\xff updated\n")
        self.assertEqual((target / "src/binary.dat").read_bytes(), b"binary\0new\r\n")
        self.assertTrue((target / "src/added.cpp").exists())
        self.assertTrue((target / "YRpp/new.h").exists())
        self.assertFalse((target / "src/remove.cpp").exists())
        self.assertFalse((target / "YRpp/remove.h").exists())
        self.assertFalse((target / ".git").exists())
        self.assertFalse((target / "dependencies.lock.json").exists())

    def test_patch_tampering_preserves_previous_assembly(self):
        reference.export(self.root)
        target = reference.assemble(self.root)
        before = reference._actual(target)
        patch = self.root / reference.ARTIFACT / "phobos.patch"
        patch.write_bytes(patch.read_bytes() + b"tamper\n")
        with self.assertRaisesRegex(reference.ReferenceError, "Modified phobos patch"):
            reference.assemble(self.root)
        self.assertEqual(reference._actual(target), before)

    def test_assembly_tampering_refuses_update(self):
        reference.export(self.root)
        target = reference.assemble(self.root)
        write(target, "src/added.cpp", b"manual edit\n")
        with self.assertRaisesRegex(reference.ReferenceError, "assembly was modified"):
            reference.verify(self.root)
        with self.assertRaisesRegex(reference.ReferenceError, "assembly was modified"):
            reference.assemble(self.root)
        self.assertEqual((target / "src/added.cpp").read_bytes(), b"manual edit\n")

    def test_extra_source_file_is_modification(self):
        reference.export(self.root)
        target = reference.assemble(self.root)
        write(target, "lib/extra.h", b"unowned\n")
        with self.assertRaises(reference.ReferenceError):
            reference.assemble(self.root)

    def _development_files(self, target):
        files = {
            "Debug/Phobos.dll": b"debug\0\r\n",
            "DevBuild/Phobos.pdb": b"symbols\0",
            "Release/Phobos.dll": b"release\0",
            ".vs/Phobos/state.bin": b"ide state\0\r\n",
            "Phobos.vcxproj.user": b"<Project>local paths</Project>\r\n",
        }
        for relative, data in files.items():
            write(target, relative, data)
        (target / ".vs/empty").mkdir()
        return files

    def test_repeat_build_preserves_outputs_and_ide_metadata(self):
        reference.export(self.root)
        target = reference.assemble(self.root)
        files = self._development_files(target)
        self.assertEqual(reference.verify(self.root), target)
        with mock.patch.object(reference, "_base", side_effect=AssertionError("unneeded reconstruction")):
            self.assertEqual(reference.assemble(self.root), target)
        for relative, data in files.items():
            self.assertEqual((target / relative).read_bytes(), data)
        self.assertTrue((target / ".vs/empty").is_dir())

    def test_reassembly_preserves_outputs_and_ide_metadata(self):
        reference.export(self.root)
        target = reference.assemble(self.root)
        files = self._development_files(target)
        write(self.root, "src/added.cpp", b"new reference\n")
        reference.export(self.root)
        self.assertEqual(reference.assemble(self.root), target)
        self.assertEqual((target / "src/added.cpp").read_bytes(), b"new reference\n")
        for relative, data in files.items():
            self.assertEqual((target / relative).read_bytes(), data)
        self.assertTrue((target / ".vs/empty").is_dir())
        reference.verify(self.root)

    def test_verify_does_not_read_locked_ide_state(self):
        reference.export(self.root)
        target = reference.assemble(self.root)
        self._development_files(target)
        original = Path.read_bytes

        def locked_ide_file(path):
            if path == target / ".vs/Phobos/state.bin":
                raise PermissionError("IDE state is locked")
            return original(path)

        with mock.patch.object(Path, "read_bytes", locked_ide_file):
            self.assertEqual(reference.verify(self.root), target)
            self.assertEqual(reference.assemble(self.root), target)

    def test_concurrent_ide_edit_refuses_replacement(self):
        reference.export(self.root)
        target = reference.assemble(self.root)
        self._development_files(target)
        write(self.root, "src/added.cpp", b"new reference\n")
        reference.export(self.root)
        original = reference.shutil.copy2

        def copy_then_edit(source, destination):
            result = original(source, destination)
            if Path(source).name == "Phobos.vcxproj.user":
                Path(source).write_bytes(b"concurrent IDE edit\r\n")
            return result

        with mock.patch.object(reference.shutil, "copy2", copy_then_edit):
            with self.assertRaisesRegex(reference.ReferenceError, "changed during reconstruction"):
                reference.assemble(self.root)
        self.assertEqual((target / "Phobos.vcxproj.user").read_bytes(), b"concurrent IDE edit\r\n")
        self.assertEqual((target / "src/added.cpp").read_bytes(), b"added\n")

    def test_assembly_source_line_endings_can_be_normalized(self):
        reference.export(self.root)
        target = reference.assemble(self.root)
        write(target, "src/added.cpp", b"added\r\n")
        self.assertEqual(reference.verify(self.root), target)
        self.assertEqual(reference.assemble(self.root), target)
        self.assertEqual((target / "src/added.cpp").read_bytes(), b"added\r\n")

    def test_stale_source_refuses_verify_and_assemble(self):
        reference.export(self.root)
        reference.assemble(self.root)
        write(self.root, "YRpp/new.h", b"changed again\n")
        for command in (reference.verify, reference.assemble):
            with self.assertRaisesRegex(reference.ReferenceError, "Source snapshot changed"):
                command(self.root)

    def test_owned_assembly_can_update_after_export(self):
        reference.export(self.root)
        reference.assemble(self.root)
        write(self.root, "src/added.cpp", b"updated\n")
        reference.export(self.root)
        target = reference.assemble(self.root)
        self.assertEqual((target / "src/added.cpp").read_bytes(), b"updated\n")
        reference.verify(self.root)

    def test_invalid_compile_input_preserves_previous(self):
        reference.export(self.root)
        target = reference.assemble(self.root)
        before = reference._actual(target)
        write(self.root, "Phobos.vcxproj", self.project.replace(b"src\\source with space.cpp", b"missing.cpp"))
        reference.export(self.root)
        with self.assertRaisesRegex(reference.ReferenceError, "Project input is missing"):
            reference.assemble(self.root)
        self.assertEqual(reference._actual(target), before)

    def test_unowned_destination_is_not_overwritten(self):
        reference.export(self.root)
        target = self.root / reference.ASSEMBLY
        target.mkdir(parents=True)
        write(target, "keep.txt", b"keep\n")
        with self.assertRaisesRegex(reference.ReferenceError, "ownership receipt"):
            reference.assemble(self.root)
        self.assertEqual((target / "keep.txt").read_bytes(), b"keep\n")

    def test_dependency_change_refused(self):
        reference.export(self.root)
        write(self.yrpp, "new.h", b"dependency edit\n")
        with self.assertRaisesRegex(reference.ReferenceError, "checkout is dirty"):
            reference.assemble(self.root)

    def test_nested_receipt_is_an_unowned_assembly_file(self):
        reference.export(self.root)
        target = reference.assemble(self.root)
        write(target, f"src/{reference.RECEIPT}", b"nested user file\n")
        with self.assertRaisesRegex(reference.ReferenceError, "assembly was modified"):
            reference.assemble(self.root)

    def test_untracked_header_refused_before_export_or_verify(self):
        reference.export(self.root)
        reference.assemble(self.root)
        write(self.root, "src/untracked.h", b"new header\n")
        for command in (reference.export, reference.verify):
            with self.assertRaisesRegex(reference.ReferenceError, "Untracked source input"):
                command(self.root)

    def test_export_publication_failure_restores_previous_bundle(self):
        reference.export(self.root)
        folder = self.root / reference.ARTIFACT
        before = {file.name: file.read_bytes() for file in folder.iterdir()}
        write(self.root, "src/added.cpp", b"next version\n")
        original = Path.rename

        def failing_rename(path, target):
            if path.name == "bundle":
                raise OSError("injected publication failure")
            return original(path, target)

        with mock.patch.object(Path, "rename", failing_rename):
            with self.assertRaisesRegex(OSError, "injected publication failure"):
                reference.export(self.root)
        self.assertEqual({file.name: file.read_bytes() for file in folder.iterdir()}, before)

    def test_extra_artifact_file_refuses_export(self):
        reference.export(self.root)
        extra = self.root / reference.ARTIFACT / "user.txt"
        extra.write_bytes(b"keep\n")
        with self.assertRaisesRegex(reference.ReferenceError, "unowned or missing files"):
            reference.export(self.root)
        self.assertEqual(extra.read_bytes(), b"keep\n")

    def test_assembly_publication_failure_restores_previous(self):
        reference.export(self.root)
        target = reference.assemble(self.root)
        before = reference._actual(target)
        write(self.root, "src/added.cpp", b"next version\n")
        reference.export(self.root)
        original = Path.rename

        def failing_rename(path, destination):
            if path.name == "assembled":
                raise OSError("injected assembly publication failure")
            return original(path, destination)

        with mock.patch.object(Path, "rename", failing_rename):
            with self.assertRaisesRegex(OSError, "injected assembly publication failure"):
                reference.assemble(self.root)
        self.assertEqual(reference._actual(target), before)

    def test_failed_export_rollback_keeps_recovery_copy(self):
        reference.export(self.root)
        folder = self.root / reference.ARTIFACT
        before = {file.name: file.read_bytes() for file in folder.iterdir()}
        write(self.root, "src/added.cpp", b"next version\n")
        original = Path.rename

        def failing_rename(path, destination):
            if path.name in ("bundle", "previous"):
                raise OSError("injected rename failure")
            return original(path, destination)

        with mock.patch.object(Path, "rename", failing_rename):
            with self.assertRaisesRegex(reference.ReferenceError, "Previous artifacts preserved"):
                reference.export(self.root)
        backups = list((self.root / "build").glob(".gc-export-backup-*/previous"))
        self.assertEqual(len(backups), 1)
        self.assertEqual({file.name: file.read_bytes() for file in backups[0].iterdir()}, before)

    def _link(self, link, target, directory=False):
        try:
            link.symlink_to(target, target_is_directory=directory)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"Symlink capability unavailable: {error}")

    def test_linked_patch_refuses_export_and_preserves_external_file(self):
        reference.export(self.root)
        patch = self.root / reference.ARTIFACT / "phobos.patch"
        external = self.root / "external.patch"
        external.write_bytes(patch.read_bytes())
        before = external.read_bytes()
        patch.unlink()
        self._link(patch, external)
        with self.assertRaisesRegex(reference.ReferenceError, "linked path"):
            reference.export(self.root)
        self.assertEqual(external.read_bytes(), before)

    def test_linked_artifact_ancestor_refused(self):
        external = self.root / "external-artifacts"
        external.mkdir()
        self._link(self.root / "integration", external, directory=True)
        with self.assertRaisesRegex(reference.ReferenceError, "linked path"):
            reference.export(self.root)
        self.assertEqual(list(external.iterdir()), [])

    def test_linked_build_folder_refused(self):
        external = self.root / "external-build"
        external.mkdir()
        self._link(self.root / "build", external, directory=True)
        with self.assertRaisesRegex(reference.ReferenceError, "linked path"):
            reference.export(self.root)
        self.assertEqual(list(external.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
