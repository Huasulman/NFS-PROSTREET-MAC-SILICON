#!/usr/bin/env python3
"""Muestra la información de cabecera de un ejecutable XEX2 de Xbox 360.

Uso:
    python3 -I xex_info.py <default.xex>
"""
import struct
import sys

HEADER_NAMES = {
    0x000002FF: "RESOURCE_INFO",
    0x000003FF: "FILE_FORMAT_INFO",
    0x000005FF: "DELTA_PATCH_DESCRIPTOR",
    0x000080FF: "BOUNDING_PATH",
    0x00010001: "ORIGINAL_BASE_ADDRESS",
    0x00010100: "ENTRY_POINT",
    0x00010201: "IMAGE_BASE_ADDRESS",
    0x000103FF: "IMPORT_LIBRARIES",
    0x00018002: "CHECKSUM_TIMESTAMP",
    0x000183FF: "ORIGINAL_PE_NAME",
    0x000200FF: "STATIC_LIBRARIES",
    0x00020104: "TLS_INFO",
    0x00020200: "DEFAULT_STACK_SIZE",
    0x00020301: "DEFAULT_FILESYSTEM_CACHE_SIZE",
    0x00020401: "DEFAULT_HEAP_SIZE",
    0x00030000: "SYSTEM_FLAGS",
    0x00040006: "EXECUTION_INFO",
    0x00040201: "TITLE_WORKSPACE_SIZE",
    0x00040310: "GAME_RATINGS",
    0x00040404: "LAN_KEY",
    0x000405FF: "XBOX360_LOGO",
    0x000406FF: "MULTIDISC_MEDIA_IDS",
    0x000407FF: "ALTERNATE_TITLE_IDS",
    0x00040801: "ADDITIONAL_TITLE_MEMORY",
    0x00E10402: "EXPORTS_BY_NAME",
}
ENCRYPTION = {0: "ninguno", 1: "AES (normal)"}
COMPRESSION = {0: "ninguna", 1: "básica (bloques sin comprimir)", 2: "LZX", 3: "delta"}


def u32(d, o):
    return struct.unpack_from(">I", d, o)[0]


def u16(d, o):
    return struct.unpack_from(">H", d, o)[0]


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    d = open(sys.argv[1], "rb").read()
    if d[:4] != b"XEX2":
        raise SystemExit(f"No es XEX2 (magic = {d[:4]!r})")

    pe_offset, sec_offset, count = u32(d, 8), u32(d, 0x10), u32(d, 0x14)
    print(f"Tamaño del archivo:   {len(d):,} bytes")
    print(f"Module flags:         0x{u32(d, 4):08X}")
    print(f"Offset de datos PE:   0x{pe_offset:X}")
    print(f"Tamaño de imagen:     0x{u32(d, sec_offset + 4):X} ({u32(d, sec_offset + 4) / 2**20:.1f} MiB)")
    print(f"Dirección de carga:   0x{u32(d, sec_offset + 0x110):08X}")
    print()

    headers = {}
    for i in range(count):
        key, value = u32(d, 0x18 + i * 8), u32(d, 0x1C + i * 8)
        headers[key] = value

    for key, value in headers.items():
        name = HEADER_NAMES.get(key, f"desconocido 0x{key:08X}")
        if key == 0x00010100:
            print(f"Punto de entrada:     0x{value:08X}")
        elif key == 0x00010201:
            print(f"Base de imagen:       0x{value:08X}")
        elif key == 0x00020200:
            print(f"Pila por defecto:     0x{value:X}")
        elif key == 0x00020401:
            print(f"Heap por defecto:     0x{value:X}")
        elif key == 0x00030000:
            print(f"System flags:         0x{value:08X}")
        elif key == 0x000003FF:
            enc, comp = u16(d, value + 4), u16(d, value + 6)
            print(f"Cifrado:              {ENCRYPTION.get(enc, enc)}")
            print(f"Compresión:           {COMPRESSION.get(comp, comp)}")
        elif key == 0x000183FF:
            size = u32(d, value)
            pe_name = d[value + 4:value + size].split(b"\0")[0].decode()
            print(f"Nombre PE original:   {pe_name}")
        elif key == 0x00040006:
            title_id = u32(d, value + 12)
            media_id, version = u32(d, value), u32(d, value + 4)
            v = f"{version >> 28}.{(version >> 24) & 0xF}.{(version >> 8) & 0xFFFF}.{version & 0xFF}"
            print(f"Title ID:             {title_id:08X}")
            print(f"Media ID:             {media_id:08X}")
            print(f"Versión:              {v}")
            print(f"Disco:                {d[value + 18]} de {d[value + 19]}")
        elif key == 0x00020104:
            print(f"TLS:                  {u32(d, value)} slots, datos en 0x{u32(d, value + 4):08X}, "
                  f"tamaño 0x{u32(d, value + 8):X}")
        elif key in (0x000200FF, 0x000103FF):
            continue  # se muestran abajo
        else:
            print(f"{name + ':':<22}0x{value:08X}")

    if 0x000200FF in headers:
        off = headers[0x000200FF]
        size = u32(d, off)
        print("\nLibrerías estáticas (XDK enlazado en el ejecutable):")
        for o in range(off + 4, off + size, 16):
            name = d[o:o + 8].split(b"\0")[0].decode()
            maj, minr, build = u16(d, o + 8), u16(d, o + 10), u16(d, o + 12)
            qfe = d[o + 15]
            print(f"  {name:<10} {maj}.{minr}.{build}.{qfe}")

    if 0x000103FF in headers:
        off = headers[0x000103FF]
        str_size, lib_count = u32(d, off + 4), u32(d, off + 8)
        names = d[off + 12:off + 12 + str_size].split(b"\0")
        names = [n.decode() for n in names if n]
        print("\nLibrerías importadas (lo que el runtime tiene que implementar):")
        o = off + 12 + str_size
        for _ in range(lib_count):
            lib_size = u32(d, o)
            version = u32(d, o + 0x1C)
            name_index, rec_count = u16(d, o + 0x24), u16(d, o + 0x26)
            v = f"{version >> 28}.{(version >> 24) & 0xF}.{(version >> 8) & 0xFFFF}.{version & 0xFF}"
            print(f"  {names[name_index]:<14} v{v:<14} {rec_count} registros de import")
            o += lib_size


if __name__ == "__main__":
    main()
