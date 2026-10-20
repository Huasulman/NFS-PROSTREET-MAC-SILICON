#!/usr/bin/env python3
"""Desensambla un rango con contexto para revisar límites de funciones.

Uso (desde la raíz del proyecto):
    python3 -I tools/disasm/annotate.py <inicio> <fin>

Marca en cada dirección:
    === sub_X        inicio de función según XenonRecomp (ppc_func_mapping.cpp)
    [pdata 0xN]      inicio con entrada en .pdata y su tamaño
    <- bl/b/bc ...   quién llama o salta aquí (indicando si viene de otra función)
    <- switch X      destino de una tabla de saltos
    [manual 0xN]     inicio corregido a mano (config/manual_functions.tsv)
"""
import bisect
import re
import struct
import subprocess
import sys

IMAGE = "analysis/default_image.bin"
MAPPING = "ppc/ppc_func_mapping.cpp"
SWITCHES = "config/ProStreet_switch_tables.toml"
IMPORTS = "analysis/imports.tsv"
IMAGE_BASE = 0x82000000
TEXT_START, TEXT_END = 0x82140000, 0x82ADCED4
PDATA_START, PDATA_SIZE = 0x820F1400, 0x430C0


def sext(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    start, end = int(sys.argv[1], 16), int(sys.argv[2], 16)
    image = open(IMAGE, "rb").read()
    word = lambda a: struct.unpack_from(">I", image, a - IMAGE_BASE)[0]

    starts = sorted(int(a, 16) for a in re.findall(r"\{ (0x[0-9A-F]+), sub_", open(MAPPING).read()))
    names = {int(a, 16): n for a, _, n in (l.split("\t") for l in open(IMPORTS).read().splitlines())}
    pdata = {}
    for off in range(0, PDATA_SIZE, 8):
        begin, data = struct.unpack_from(">II", image, PDATA_START - IMAGE_BASE + off)
        if begin:
            pdata[begin] = ((data >> 8) & 0x3FFFFF) * 4

    def owner(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return starts[i] if i >= 0 else None

    refs = {}
    for a in range(TEXT_START, TEXT_END, 4):
        x = word(a)
        op = x >> 26
        if op == 18 and not x & 2:
            t, kind = a + sext(x & 0x03FFFFFC, 26), "bl" if x & 1 else "b"
        elif op == 16 and not x & 2:
            t, kind = a + sext(x & 0xFFFC, 16), "bcl" if x & 1 else "bc"
        else:
            continue
        if start <= t < end:
            refs.setdefault(t, []).append((kind, a))

    text = open(SWITCHES).read()
    for block in text.split("[[switch]]")[1:]:
        base = int(re.search(r"base = (0x[0-9A-Fa-f]+)", block).group(1), 16)
        targets = re.findall(r"0x[0-9A-Fa-f]+", re.search(r"labels = \[(.*?)\]", block, re.S).group(1))
        default = int(re.search(r"default = (0x[0-9A-Fa-f]+)", block).group(1), 16)
        for i, t in enumerate([int(t, 16) for t in targets] + ([default] if default else [])):
            if start <= t < end:
                refs.setdefault(t, []).append((f"switch {base:X} caso {i if i < len(targets) else 'default'}", base))

    listing = subprocess.run(["tools/disasm/disasm", IMAGE, hex(start), hex(end)],
                             capture_output=True, text=True, check=True).stdout.splitlines()
    for line in listing:
        addr = int(line[:8], 16)
        if addr in starts:
            extra = f"  [pdata 0x{pdata[addr]:X}]" if addr in pdata else ""
            print(f"=== {names.get(addr, f'sub_{addr:X}')}{extra}")
        notes = []
        for kind, src in refs.get(addr, []):
            if kind.startswith("switch"):
                notes.append(kind)
            else:
                same = owner(src) == owner(addr)
                notes.append(f"{kind} {src:X}" + ("" if same else f" (desde sub_{owner(src):X})"))
        print(line + ("    <- " + ", ".join(notes) if notes else ""))


if __name__ == "__main__":
    main()
