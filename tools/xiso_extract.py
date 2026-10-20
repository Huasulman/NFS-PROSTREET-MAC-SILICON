#!/usr/bin/env python3
"""Lista o extrae el sistema de archivos XDVDFS de una ISO de Xbox / Xbox 360.

Uso:
    python3 -I xiso_extract.py list    <iso>
    python3 -I xiso_extract.py extract <iso> <carpeta_destino> [filtro]

El filtro opcional extrae solo las rutas que contengan ese texto (p. ej. "default.xex").
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Offsets conocidos de la partición de juego: XISO plano, XGD1, XGD2, XGD3.
PARTITION_OFFSETS = [0x0, 0x18300000, 0xFD90000, 0x2080000]
ATTR_DIRECTORY = 0x10


def find_partition(f):
    for base in PARTITION_OFFSETS:
        f.seek(base + 32 * SECTOR)
        if f.read(len(MAGIC)) == MAGIC:
            return base
    raise SystemExit("No se encontró una partición XDVDFS en la ISO.")


def read_dir(f, base, sector, size, prefix=""):
    """Recorre el árbol binario de un directorio y devuelve (ruta, sector, tamaño, es_dir)."""
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    entries = []
    pending = [0]
    seen = set()
    while pending:
        off = pending.pop()
        if off in seen or off + 14 > len(data):
            continue
        seen.add(off)
        left, right, start, length, attr, name_len = struct.unpack_from("<HHIIBB", data, off)
        if left == 0xFFFF and right == 0xFFFF:
            continue  # relleno hasta el siguiente sector
        name = data[off + 14: off + 14 + name_len].decode("latin-1")
        if left:
            pending.append(left * 4)
        if right:
            pending.append(right * 4)
        if not name or name in (".", "..") or "/" in name or "\\" in name:
            continue
        path = prefix + name
        is_dir = bool(attr & ATTR_DIRECTORY)
        entries.append((path, start, length, is_dir))
        if is_dir and length:
            entries.extend(read_dir(f, base, start, length, path + "/"))
    return entries


def list_files(f):
    base = find_partition(f)
    f.seek(base + 32 * SECTOR + len(MAGIC))
    root_sector, root_size = struct.unpack("<II", f.read(8))
    return base, sorted(read_dir(f, base, root_sector, root_size))


def extract(f, base, entries, dest, filtro=None):
    dest = os.path.realpath(dest)
    for path, start, length, is_dir in entries:
        if filtro and filtro.lower() not in path.lower():
            continue
        out = os.path.realpath(os.path.join(dest, path))
        if not out.startswith(dest + os.sep):
            print(f"  [omitido, ruta insegura] {path}")
            continue
        if is_dir:
            os.makedirs(out, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(out), exist_ok=True)
        f.seek(base + start * SECTOR)
        remaining = length
        with open(out, "wb") as o:
            while remaining:
                chunk = f.read(min(remaining, 8 * 1024 * 1024))
                if not chunk:
                    raise SystemExit(f"ISO truncada al leer {path}")
                o.write(chunk)
                remaining -= len(chunk)
        print(f"  {length:>12,}  {path}")


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("list", "extract"):
        raise SystemExit(__doc__)
    with open(sys.argv[2], "rb") as f:
        base, entries = list_files(f)
        print(f"Partición de juego en 0x{base:X}")
        if sys.argv[1] == "list":
            total = 0
            for path, _, length, is_dir in entries:
                if not is_dir:
                    total += length
                    print(f"  {length:>12,}  {path}")
            print(f"{sum(not e[3] for e in entries)} archivos, {total / 2**30:.2f} GiB")
        else:
            if len(sys.argv) < 4:
                raise SystemExit(__doc__)
            extract(f, base, entries, sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else None)


if __name__ == "__main__":
    main()
