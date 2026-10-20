#!/usr/bin/env python3
"""Lista las funciones generadas que se solapan con la siguiente y las clasifica.

Uso (desde la raíz del proyecto, después de recompilar):
    python3 -I tools/check_overlaps.py

Una función se solapa si su tamaño generado (líneas `// instrucción` × 4) pasa del inicio
de la siguiente función del mapping. Los tipos que se aceptan, porque el código duplicado
es correcto:

    finally   La función tiene .pdata (y ese es su tamaño) y contiene una segunda entrada a
              la que se llama con `bl` desde fuera. Es un manejador `__finally`: la entrada de
              .pdata la usa __C_specific_handler al desenrollar y salta al cuerpo de la otra,
              que la función padre llama en el camino normal.
    caída     La palabra anterior a la segunda entrada no es blr ni `b`: la primera entrada
              ejecuta unas instrucciones y sigue en la segunda (p. ej. `neg r12,r3` delante
              de __chkstk).

Cualquier otro solapamiento sale como "sin explicar" y el script termina con código 1.
"""
import bisect
import re
import struct
import sys

sys.path.insert(0, "tools")
from fix_switch_bounds import IMAGE_BASE, TEXT_START, TEXT_END, branch_target, generated_sizes, pdata_functions

IMAGE = "analysis/default_image.bin"
MAPPING = "ppc/ppc_func_mapping.cpp"


def main():
    image = open(IMAGE, "rb").read()
    word = lambda a: struct.unpack_from(">I", image, a - IMAGE_BASE)[0]
    starts = sorted(int(x, 16) for x in re.findall(r"\{ (0x[0-9A-F]+), sub_", open(MAPPING).read()))
    sizes = generated_sizes("ppc")
    pdata = pdata_functions(image)

    bl_from = {}
    for a in range(TEXT_START, TEXT_END, 4):
        x = word(a)
        if x >> 26 == 18 and x & 3 == 1:
            bl_from.setdefault(branch_target(x & ~1, a), []).append(a)

    counts, unexplained = {}, []
    for i, fn in enumerate(starts[:-1]):
        nxt, size = starts[i + 1], sizes.get(fn, 0)
        if fn + size <= nxt:
            continue
        before = word(nxt - 4)
        if pdata.get(fn) == size and any(not fn <= a < fn + size for a in bl_from.get(nxt, [])):
            kind = "finally"
        elif before != 0x4E800020 and not (before >> 26 == 18 and before & 3 == 0):
            kind = "caída"
        else:
            kind = "sin explicar"
            unexplained.append(fn)
        counts[kind] = counts.get(kind, 0) + 1
        print(f"sub_{fn:X}  tamaño 0x{size:X}  siguiente sub_{nxt:X}  {kind}")

    print(f"\n{sum(counts.values())} solapamientos: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    sys.exit(1 if unexplained else 0)


if __name__ == "__main__":
    main()
