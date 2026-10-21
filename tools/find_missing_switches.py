#!/usr/bin/env python3
"""Busca tablas de saltos absolutas que XenonAnalyse no detectó y las añade al TOML de switch.

Patrón (base = dirección del lis):
    lis    r12, tabla@ha
    addi   r12, r12, tabla@l
    rlwinm r0, rX, 2, 0, 29        ; índice * 4
    lwzx   r0, r12, r0
    mtctr  r0
    bctr

El número de casos sale de la comprobación de rango anterior (cmplwi rX, N + bgt) si la hay;
si no, de la tabla incrustada: termina donde empieza el primer destino.

Uso (desde la raíz del proyecto):
    python3 -I tools/find_missing_switches.py analysis/default_image.bin config/ProStreet_switch_tables.toml
"""
import re
import struct
import sys

IMAGE_BASE = 0x82000000
TEXT_START, TEXT_END = 0x82140000, 0x82ADCED4
BEGIN = "# ---- AÑADIDAS POR tools/find_missing_switches.py ----"
MTCTR_R0, BCTR, LWZX_R0_R12_R0 = 0x7C0903A6, 0x4E800420, 0x7C0C002E


def sext16(value):
    return value - 0x10000 if value & 0x8000 else value


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    image = open(sys.argv[1], "rb").read()
    word = lambda a: struct.unpack_from(">I", image, a - IMAGE_BASE)[0]
    path = sys.argv[2]
    text = open(path).read()
    if BEGIN in text:
        text = text[:text.index(BEGIN)].rstrip("\n") + "\n"
    known = {int(b, 16) for b in re.findall(r"base = (0x[0-9A-Fa-f]+)", text)}

    found = []
    for bctr in range(TEXT_START + 0x14, TEXT_END, 4):
        if word(bctr) != BCTR or word(bctr - 4) != MTCTR_R0 or word(bctr - 8) != LWZX_R0_R12_R0:
            continue
        lis, addi, rlwinm = word(bctr - 0x14), word(bctr - 0x10), word(bctr - 0xC)
        # lis r12,X / addi r12,r12,Y / rlwinm r0,rX,2,0,29
        if lis >> 16 != 0x3D80 or addi >> 16 != 0x398C or (rlwinm & 0xFC00FFFF) != 0x5400103A or (rlwinm >> 16) & 0x1F != 0:
            continue
        base = bctr - 0x14
        if base in known:
            continue
        index_register = (rlwinm >> 21) & 0x1F
        table = ((lis & 0xFFFF) << 16) + sext16(addi & 0xFFFF)
        table &= 0xFFFFFFFF

        # ¿Hay comprobación de rango? cmplwi crN, rX, N justo antes (hasta 8 instrucciones atrás).
        count, default = None, 0
        for back in range(4, 0x24, 4):
            insn = word(base - back)
            # Un salto incondicional o un retorno es el final de otra función: no seguir.
            if (insn >> 26 == 18 and insn & 1 == 0) or insn == 0x4E800020:
                break
            if insn >> 26 == 10 and (insn >> 16) & 0x1F == index_register:  # cmpli
                count = (insn & 0xFFFF) + 1
                for forward in range(base - back + 4, base, 4):  # bgt hacia el default
                    b = word(forward)
                    if b >> 26 == 16:
                        default = forward + sext16(b & 0xFFFC)
                break

        if count is None:
            # Tabla incrustada: las entradas siguen hasta el primer destino.
            if not (TEXT_START <= table < TEXT_END):
                continue
            labels, address, first = [], table, None
            while address < TEXT_END:
                value = word(address)
                if not (TEXT_START <= value < TEXT_END) or (first is not None and address >= first):
                    break
                labels.append(value)
                first = value if first is None else min(first, value)
                address += 4
        else:
            labels = [word(table + 4 * i) for i in range(count)]
        if not labels or not all(TEXT_START <= l < TEXT_END for l in labels):
            print(f"  aviso: tabla en 0x{base:08X} no válida, se omite")
            continue
        found.append((base, index_register, default, labels))

    out = [text.rstrip("\n"), "", BEGIN]
    for base, register, default, labels in found:
        out += ["[[switch]]", f"base = 0x{base:08X}", f"r = {register}", f"default = 0x{default:X}", "labels = ["]
        out += [f"    0x{l:08X}," for l in labels]
        out += ["]", ""]
    open(path, "w").write("\n".join(out))
    print(f"{len(found)} tablas de saltos nuevas añadidas a {path}")
    for base, register, default, labels in found[:20]:
        print(f"  0x{base:08X}: r{register}, {len(labels)} casos{'' if default else ', sin default'}")


if __name__ == "__main__":
    main()
