#!/usr/bin/env python3
"""Extract .text from an AICore ELF .o and emit GNU assembler (.s) with .byte directives."""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
from pathlib import Path


def _read_elf_text(obj: Path) -> tuple[bytes, list[tuple[str, int, int]]]:
    data = obj.read_bytes()
    if data[:4] != b"\x7fELF":
        raise ValueError(f"not ELF: {obj}")

    ei_class = data[4]
    if ei_class != 2:
        raise ValueError("expected ELF64")

    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", data, 0x3A)[0]
    e_shnum = struct.unpack_from("<H", data, 0x3C)[0]
    e_shstrndx = struct.unpack_from("<H", data, 0x3E)[0]

    shstr_off = struct.unpack_from("<Q", data, e_shoff + e_shstrndx * e_shentsize + 0x18)[0]

    def sh_name(idx: int) -> str:
        off = shstr_off + idx
        end = data.index(b"\x00", off)
        return data[off:end].decode("ascii", errors="replace")

    text_off = 0
    text_size = 0
    text_shndx = 0
    symtab: list[tuple[str, int, int]] = []

    for i in range(e_shnum):
        base = e_shoff + i * e_shentsize
        sh_type = struct.unpack_from("<I", data, base + 0x04)[0]
        sh_offset = struct.unpack_from("<Q", data, base + 0x18)[0]
        sh_size = struct.unpack_from("<Q", data, base + 0x20)[0]
        name = sh_name(struct.unpack_from("<I", data, base)[0])

        if name == ".text":
            text_off = sh_offset
            text_size = sh_size
            text_shndx = i
        elif sh_type == 2:  # SHT_SYMTAB
            link = struct.unpack_from("<I", data, base + 0x28)[0]
            entsize = struct.unpack_from("<Q", data, base + 0x28 + 8)[0] or 24
            str_base = struct.unpack_from("<Q", data, e_shoff + link * e_shentsize + 0x18)[0]
            count = sh_size // entsize
            for j in range(count):
                ent = sh_offset + j * entsize
                st_name, st_info, _st_other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", data, ent)
                if st_shndx != text_shndx or st_value == 0:
                    continue
                bind = st_info >> 4
                if bind == 0:  # LOCAL — skip compiler-internal labels
                    continue
                nm_off = str_base + st_name
                nm_end = data.index(b"\x00", nm_off)
                symtab.append((data[nm_off:nm_end].decode("ascii", errors="replace"), st_value, st_size))

    if text_size == 0:
        raise ValueError(f".text not found in {obj}")

    return data[text_off : text_off + text_size], symtab


def _emit_gas(text: bytes, symtab: list[tuple[str, int, int]], out: Path, src: Path) -> None:
    lines = [
        f"/* Auto-generated from {src.name} — AICore machine code (.text as .byte) */",
        f"/* Source object: {src} */",
        ".section .text",
    ]
    symtab = sorted(symtab, key=lambda x: x[1])
    sym_idx = 0
    pos = 0
    while pos < len(text):
        while sym_idx < len(symtab) and symtab[sym_idx][1] == pos:
            name, _val, _size = symtab[sym_idx]
            if name and not name.startswith("$"):
                lines.append(f".global {name}")
                lines.append(f"{name}:")
            sym_idx += 1
        chunk = text[pos : pos + 16]
        hexbytes = ", ".join(f"0x{b:02x}" for b in chunk)
        lines.append(f"    .byte {hexbytes}")
        pos += len(chunk)

    out.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _emit_objdump_listing(obj: Path, out: Path, llvm_objdump: str) -> None:
    proc = subprocess.run(
        [llvm_objdump, "-l", str(obj)],
        check=False,
        capture_output=True,
        text=True,
    )
    header = f"/* llvm-objdump -l listing for {obj.name} (source line map; opcodes may be <not available>) */\n"
    out.write_text(header + proc.stdout + proc.stderr, encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description="Convert AICore .o .text section to .s")
    ap.add_argument("obj", type=Path)
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("--llvm-objdump", default="llvm-objdump")
    ap.add_argument("--also-disasm", action="store_true", help="also write .disasm.s with llvm-objdump -l")
    args = ap.parse_args()

    text, symtab = _read_elf_text(args.obj)
    _emit_gas(text, symtab, args.output, args.obj)
    print(f"wrote {args.output} ({len(text)} bytes .text, {len(symtab)} symbols)")

    if args.also_disasm:
        dis_path = args.output.with_suffix(".disasm.s")
        _emit_objdump_listing(args.obj, dis_path, args.llvm_objdump)
        print(f"wrote {dis_path}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
