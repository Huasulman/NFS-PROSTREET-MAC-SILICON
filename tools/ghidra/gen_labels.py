#!/usr/bin/env python3
"""Reúne lo que sabemos del ejecutable en un TSV que lee ProStreetSetup.java en Ghidra.

Uso (desde la raíz del proyecto, después de ejecutar xex_dump y XenonRecomp):
    python3 -I tools/ghidra/gen_labels.py > analysis/ghidra_labels.tsv

Formato de cada línea (separada por tabuladores):
    section   inicio  fin     nombre  permisos (r/w/x)
    function  dirección       nombre
    label     dirección       nombre
    bookmark  dirección       categoría  texto
"""
import re
import sys

XEX_DUMP = "analysis/xex_dump.txt"
IMPORTS = "analysis/imports.tsv"
MAPPING = "ppc/ppc_func_mapping.cpp"
CONFIG = "config/ProStreet.toml"
FIX_LOG = "analysis/fix_bounds_log.txt"
RECOMP_LOG = "analysis/recomp_log.txt"

PERMISSIONS = {".text": "rx", ".data": "rw", ".tls": "rw", ".XBMOVIE": "rw"}


def out(*fields):
    print("\t".join(str(f) for f in fields))


def main():
    dump = open(XEX_DUMP).read()
    for name, start, end in re.findall(r"^\s+(\.\w+)\s+0x([0-9A-F]+) - 0x([0-9A-F]+)", dump, re.M):
        out("section", "0x" + start, "0x" + end, name, PERMISSIONS.get(name, "r"))
    entry = re.search(r"Entry point:\s+0x([0-9A-F]+)", dump).group(1)

    # Todas las funciones que conoce XenonRecomp (para que Ghidra y el código generado coincidan).
    named = {}
    for addr, size, name in (l.split("\t") for l in open(IMPORTS).read().splitlines()):
        named[int(addr, 16)] = name
    config = open(CONFIG).read()
    for key, addr in re.findall(r"^(\w+)_address = (0x[0-9A-F]+)", config, re.M):
        named[int(addr, 16)] = "__" + key
    named[int(entry, 16)] = "xex_entry_point"

    for addr in sorted(int(a, 16) for a in re.findall(r"\{ (0x[0-9A-F]+), sub_", open(MAPPING).read())):
        out("function", f"0x{addr:08X}", named.pop(addr, f"sub_{addr:08X}"))
    for addr, name in sorted(named.items()):
        out("function" if name.startswith("__imp__") or name == "xex_entry_point" else "label", f"0x{addr:08X}", name)

    # Casos para revisar a mano.
    for fn, sw, why in re.findall(r"sub_([0-9A-F]+) \(switch en 0x([0-9A-F]+)\): (.+)", open(FIX_LOG).read()):
        out("bookmark", "0x" + sw, "Revisar límites", f"switch en sub_{fn}: {why}")
    bases = sorted(set(re.findall(r"ERROR: Switch case at ([0-9A-F]+)", open(RECOMP_LOG).read())))
    for base in bases:
        out("bookmark", "0x" + base, "Switch con error", "XenonRecomp: un destino de este switch cae fuera de la función")


if __name__ == "__main__":
    sys.exit(main())
