#!/usr/bin/env python3
"""Genera runtime/generated/imports.cpp a partir del XEX y del código recompilado.

- Una implementación débil (PPC_WEAK_FUNC) para cada `__imp__X` que pide ppc/: registra la
  llamada y devuelve 0. Las implementaciones reales del runtime la reemplazan al enlazar.
- La tabla de "slots" de import: direcciones del juego que el cargador de la Xbox rellena al
  arrancar, con el thunk de una función o la dirección de una variable exportada por el kernel.

Uso (desde la raíz del proyecto, después de recompilar):
    python3 -I tools/gen_imports.py game/default.xex analysis/default_image.bin ppc runtime/generated
"""
import os
import re
import struct
import sys

IMAGE_BASE = 0x82000000
TEXT_START, TEXT_END = 0x82140000, 0x82ADCED4
EXPORT_TABLES = {
    "xboxkrnl.exe": "tools/XenonRecomp/XenonUtils/xbox/xboxkrnl_table.inc",
    "xam.xex": "tools/XenonRecomp/XenonUtils/xbox/xam_table.inc",
}


def u32(d, o):
    return struct.unpack_from(">I", d, o)[0]


def u16(d, o):
    return struct.unpack_from(">H", d, o)[0]


def load_exports(path):
    """ordinal -> (nombre, es_variable)"""
    exports = {}
    for ordinal, name, kind in re.findall(r"XE_EXPORT\(\w+,\s*(0x[0-9A-Fa-f]+),\s*(\w+),\s*(\w+)\)", open(path).read()):
        exports[int(ordinal, 16)] = (name, kind == "kVariable")
    return exports


def import_records(xex):
    """Lista de (librería, [direcciones de registro]) de la cabecera de imports del XEX."""
    count = u32(xex, 0x14)
    headers = {u32(xex, 0x18 + i * 8): u32(xex, 0x1C + i * 8) for i in range(count)}
    off = headers[0x000103FF]
    str_size, lib_count = u32(xex, off + 4), u32(xex, off + 8)
    names = [n.decode() for n in xex[off + 12:off + 12 + str_size].split(b"\0") if n]
    o = off + 12 + str_size
    libraries = []
    for _ in range(lib_count):
        size, name_index, rec_count = u32(xex, o), u16(xex, o + 0x24), u16(xex, o + 0x26)
        libraries.append((names[name_index], [u32(xex, o + 0x28 + 4 * i) for i in range(rec_count)]))
        o += size
    return libraries


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    xex = open(sys.argv[1], "rb").read()
    image = open(sys.argv[2], "rb").read()
    ppc_dir, out_dir = sys.argv[3], sys.argv[4]

    wanted = sorted(set(re.findall(r"PPC_EXTERN_FUNC\(__imp__(\w+)\)",
                                   open(os.path.join(ppc_dir, "ppc_recomp_shared.h")).read())))
    thunks = {}  # nombre -> dirección del thunk
    for line in open(os.path.join(ppc_dir, "ppc_func_mapping.cpp")):
        m = re.search(r"\{ (0x[0-9A-F]+), __imp__(\w+) \}", line)
        if m:
            thunks[m.group(2)] = int(m.group(1), 16)

    slots = []  # (slot, librería, nombre, es_variable)
    for library, records in import_records(xex):
        exports = load_exports(EXPORT_TABLES[library]) if library in EXPORT_TABLES else {}
        for record in records:
            if TEXT_START <= record < TEXT_END:
                continue  # thunk: XenonUtils ya lo convirtió en código
            # XenonUtils invierte el orden de bytes de cada registro en la imagen al cargarla.
            value = struct.unpack_from("<I", image, record - IMAGE_BASE)[0]
            if value >> 24 != 0:
                continue  # thunk: XenonUtils ya lo convirtió en código
            ordinal = value & 0xFFFF
            name, is_variable = exports.get(ordinal, (f"{library.split('.')[0]}_{ordinal:03X}", True))
            slots.append((record, library, name, is_variable))

    os.makedirs(out_dir, exist_ok=True)
    out = ["// Generado por tools/gen_imports.py. No editar a mano.",
           '#include "kernel/imports.h"', ""]
    for name in wanted:
        out.append(f'PPC_WEAK_FUNC(__imp__{name}) {{ ImportStub(ctx, base, "{name}"); }}')
    out += ["", "const ImportSlot kImportSlots[] = {"]
    for slot, library, name, is_variable in slots:
        thunk = 0 if is_variable else thunks.get(name, 0)
        out.append(f'    {{ 0x{slot:08X}, "{library}", "{name}", {"true" if is_variable else "false"}, 0x{thunk:08X} }},')
    out += ["};", f"const size_t kImportSlotCount = {len(slots)};", ""]
    open(os.path.join(out_dir, "imports.cpp"), "w").write("\n".join(out))

    variables = sum(1 for s in slots if s[3])
    print(f"{len(wanted)} funciones importadas, {len(slots)} slots ({variables} variables) "
          f"-> {os.path.join(out_dir, 'imports.cpp')}")


if __name__ == "__main__":
    main()
