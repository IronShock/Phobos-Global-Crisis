#!/usr/bin/env python3
"""Verify the fog-of-war hooks against gamemd.exe and the rest of the tree.

Usage: python scripts/fog_hooks_check.py [path/to/gamemd.exe] [file ...]

Checks, for every DEFINE_HOOK / DEFINE_HOOK_AGAIN in the given files (default:
src/Misc/Hooks.Fog.cpp):
  * the site exists in the executable image
  * the stolen byte range ends on an instruction boundary
  * no relative branch/call targets the middle of the range
  * no other hook in src/ overlaps the range
"""
import re
import struct
import sys
from pathlib import Path

import capstone

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_FILES = [ROOT / "src/Misc/Hooks.Fog.cpp"]
HOOK_RE = re.compile(
    r"DEFINE_HOOK(?:_AGAIN)?\(\s*(0x[0-9A-Fa-f]+)\s*,\s*([A-Za-z0-9_]+)\s*,\s*(0x[0-9A-Fa-f]+)")


def load_text(path):
    data = Path(path).read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = pe + 24
    image_base = struct.unpack_from("<I", data, opt + 28)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    sections = []
    for i in range(nsec):
        o = opt + opt_size + i * 40
        name = data[o:o + 8].rstrip(b"\0").decode("latin1")
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, o + 8)
        sections.append((name, va, vsize, raw, rsize))
    return data, image_base, sections


def va_to_offset(image_base, sections, va):
    rva = va - image_base
    for _, sva, vsize, raw, rsize in sections:
        if sva <= rva < sva + max(vsize, rsize):
            return raw + (rva - sva)
    return None


def main():
    args = [a for a in sys.argv[1:]]
    exe = Path(args[0]) if args and args[0].lower().endswith(".exe") else Path(r"D:\Global Crisis\gamemd.exe")
    files = [Path(a) for a in args if not a.lower().endswith(".exe")] or DEFAULT_FILES

    data, image_base, sections = load_text(exe)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.skipdata = True

    # Collect every hook in the tree to detect structural overlaps.
    all_hooks = []
    for p in (ROOT / "src").rglob("*.cpp"):
        try:
            text = p.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        for m in HOOK_RE.finditer(text):
            all_hooks.append((int(m.group(1), 16), int(m.group(3), 16), m.group(2), p))

    # Relative branches inside .text, to find targets in the middle of a patch.
    text_sec = next(s for s in sections if s[0] == ".text")
    text_off = text_sec[3]
    text_va = image_base + text_sec[1]
    text_size = text_sec[2]
    targets = {}
    for insn in md.disasm(data[text_off:text_off + text_size], text_va):
        if insn.mnemonic in ("jmp", "call", "je", "jne", "jz", "jnz", "jl", "jle", "jg", "jge",
                             "jb", "jbe", "ja", "jae", "js", "jns", "jo", "jno", "jp", "jnp",
                             "loop", "loope", "loopne"):
            op = insn.op_str
            if op.startswith("0x"):
                try:
                    targets.setdefault(int(op, 16), []).append(insn.address)
                except ValueError:
                    pass

    problems = 0
    for path in files:
        text = path.read_text(encoding="utf-8", errors="ignore")
        for m in HOOK_RE.finditer(text):
            va, name, size = int(m.group(1), 16), m.group(2), int(m.group(3), 16)
            off = va_to_offset(image_base, sections, va)
            issues = []
            if off is None:
                issues.append("address outside the image")
            else:
                covered = 0
                for insn in md.disasm(data[off:off + size], va):
                    covered += insn.size
                    relative = (insn.mnemonic.startswith("j")
                                or insn.mnemonic in ("call", "loop", "loope", "loopne"))
                    if relative and insn.op_str.startswith("0x"):
                        issues.append(f"stolen instruction {insn.mnemonic} {insn.op_str} is a relative branch")
                    if covered > size:
                        break
                if covered != size:
                    issues.append(f"stolen {size} bytes split an instruction (consumed {covered})")
                for t in range(va + 1, va + size):
                    if t in targets:
                        issues.append(f"branch target 0x{t:08X} inside range (from "
                                      + ", ".join(f"0x{a:08X}" for a in targets[t][:3]) + ")")
            for ova, osize, oname, opath in all_hooks:
                if (ova, osize, path.name) == (va, size, path.name):
                    continue
                if ova < va + size and ova + osize > va:
                    issues.append(f"overlaps {oname} ({opath.relative_to(ROOT)}) 0x{ova:08X}+{osize:#x}")
            status = "OK  " if not issues else "FAIL"
            if issues:
                problems += 1
            print(f"{status} 0x{va:08X} +{size:#04x} {name}")
            for issue in issues:
                print(f"       {issue}")

    print(f"\n{problems} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
