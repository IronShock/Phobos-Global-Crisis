"""Shared development tasks; native build settings remain in Phobos.vcxproj."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent


def run(*args, cwd=ROOT):
    subprocess.run(args, cwd=cwd, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("build", help="Build the Win32 DLL with MSBuild")
    build.add_argument("--configuration", choices=("Debug", "DevBuild", "Release"), default="Debug")
    commands.add_parser("docs", help="Build HTML in docs/_build/html")
    commands.add_parser("docs-locale", help="Update the inherited zh_CN translations")
    commands.add_parser("clean", help="Remove only native build output directories")
    commands.add_parser("setup-vscode", help="Copy missing local VS Code settings")
    args = parser.parse_args()

    if args.command == "build":
        if os.name != "nt":
            parser.exit(2, "DLL builds require Windows and Visual Studio/MSVC; docs and maintenance tasks are portable.\n")
        run("cmd.exe", "/d", "/c", "call", r"scripts\run_msbuild.bat",
            "/maxCpuCount", "/consoleloggerparameters:NoSummary",
            f"/property:Configuration={args.configuration}")
    elif args.command == "docs":
        run(sys.executable, "-m", "sphinx", "-M", "html", "docs", "docs/_build")
    elif args.command == "docs-locale":
        run(sys.executable, "-m", "sphinx", "-b", "gettext", ".", "_build/gettext", cwd=ROOT / "docs")
        run(sys.executable, "-m", "sphinx_intl", "update", "-p", "_build/gettext", "-l", "zh_CN",
            cwd=ROOT / "docs")
    elif args.command == "clean":
        for name in ("Debug", "DevBuild", "Release"):
            output = ROOT / name
            if output.is_symlink():
                raise OSError(f"Refusing to clean a linked output directory: {output}")
            if output.exists():
                shutil.rmtree(output)
                print(f"Removed {name}/")
    elif args.command == "setup-vscode":
        for name in ("c_cpp_properties", "settings"):
            target = ROOT / ".vscode" / f"{name}.json"
            example = target.with_name(f"{name}.example.json").read_bytes()
            try:
                with target.open("xb") as destination:
                    destination.write(example)
                print(f"Created {target.name}; adjust local toolchain/game paths as needed.")
            except FileExistsError:
                print(f"Kept existing {target.name}")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
    except OSError as error:
        print(error, file=sys.stderr)
        sys.exit(1)
