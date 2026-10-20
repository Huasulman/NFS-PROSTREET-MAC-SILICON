#!/usr/bin/env python3
"""Calcula los límites reales de las funciones que contienen tablas de saltos.

El analizador de XenonRecomp corta las funciones hoja (sin entrada en .pdata) en el
`bctr` de su switch, y el resto queda como "fragmentos" sueltos. Este script une cada
función con sus fragmentos y escribe la lista `functions = [...]` en el TOML principal.

Uso:
    python3 -I tools/fix_switch_bounds.py <imagen.bin> <ppc_func_mapping.cpp> \
        <switch_tables.toml> <config.toml>

Las funciones que no se pueden resolver con seguridad se listan para revisión manual.
Las correcciones hechas a mano van en config/manual_functions.tsv y tienen prioridad.
"""
import bisect
import os
import re
import struct
import sys

IMAGE_BASE = 0x82000000
TEXT_START, TEXT_END = 0x82140000, 0x82ADCED4
PDATA_START, PDATA_SIZE = 0x820F1400, 0x430C0
DATA_SECTIONS = [(0x82000400, 0x820F1354), (0x82AE0000, 0x8309C790)]  # .rdata, .data
BEGIN_MARK = "# BEGIN functions (generado por tools/fix_switch_bounds.py)"
END_MARK = "# END functions"
MANUAL_FILE = "manual_functions.tsv"  # junto al TOML principal


def load_manual(path):
    """Lee `dirección<TAB>tamaño<TAB>comentario`; las líneas con # se ignoran."""
    overrides = {}
    if os.path.exists(path):
        for line in open(path).read().splitlines():
            fields = line.split("#", 1)[0].split()
            if len(fields) >= 2:
                overrides[int(fields[0], 16)] = int(fields[1], 16)
    return overrides


def load_words(image):
    return lambda addr: struct.unpack_from(">I", image, addr - IMAGE_BASE)[0]


def sext(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def pdata_starts(image):
    starts = set()
    for off in range(0, PDATA_SIZE, 8):
        begin = struct.unpack_from(">I", image, PDATA_START - IMAGE_BASE + off)[0]
        if begin:
            starts.add(begin)
    return starts


def bl_targets(word):
    targets = set()
    for addr in range(TEXT_START, TEXT_END, 4):
        insn = word(addr)
        if insn >> 26 == 18 and insn & 3 == 1:  # bl (LK=1, AA=0)
            targets.add(addr + sext(insn & 0x03FFFFFC, 26))
    return targets


def data_pointers(image, word, starts):
    """Inicios de función referenciados desde datos (vtables, tablas de callbacks)."""
    found = set()
    for start, end in DATA_SECTIONS:
        for addr in range(start, end - 3, 4):
            value = word(addr)
            if TEXT_START <= value < TEXT_END and value in starts:
                found.add(value)
    return found


def branch_target(insn, addr):
    """Destino de un b/bc sin link, o None."""
    op = insn >> 26
    if insn & 3 != 0:
        return None
    if op == 18:
        return addr + sext(insn & 0x03FFFFFC, 26)
    if op == 16:
        return addr + sext(insn & 0xFFFC, 16)
    return None


def parse_switches(text):
    switches = {}
    for block in text.split("[[switch]]")[1:]:
        base = int(re.search(r"base = (0x[0-9A-Fa-f]+)", block).group(1), 16)
        default = int(re.search(r"default = (0x[0-9A-Fa-f]+)", block).group(1), 16)
        labels_text = re.search(r"labels = \[(.*?)\]", block, re.S).group(1)
        labels = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+", labels_text)]
        switches[base] = labels + ([default] if default else [])  # default = 0: sin caso por defecto
    return switches


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    image = open(sys.argv[1], "rb").read()
    word = load_words(image)
    starts = sorted(int(x, 16) for x in re.findall(r"\{ (0x[0-9A-F]+), sub_", open(sys.argv[2]).read()))
    switches = parse_switches(open(sys.argv[3]).read())
    switch_bases = sorted(switches)

    real = pdata_starts(image) | bl_targets(word) | data_pointers(image, word, set(starts))
    real -= {t for targets in switches.values() for t in targets}  # destinos de switch: no son funciones

    def next_start(addr):
        """Inicio de la siguiente función detectada después de addr (= fin del fragmento)."""
        i = bisect.bisect_right(starts, addr)
        end = starts[i] if i < len(starts) else TEXT_END
        while end - 4 > addr and word(end - 4) == 0:  # quitar relleno
            end -= 4
        return end

    def containing(addr):
        return starts[bisect.bisect_right(starts, addr) - 1]

    # Conservar lo generado en ejecuciones anteriores: con esas funciones ya aplicadas,
    # sus fragmentos desaparecen del mapping y no se volverían a detectar.
    config_path = sys.argv[4]
    config = open(config_path).read()
    previous = re.search(re.escape(BEGIN_MARK) + r"(.*?)" + re.escape(END_MARK), config, re.S)
    results = {}
    if previous:
        for a, s in re.findall(r"address = (0x[0-9A-F]+), size = (0x[0-9A-F]+)", previous.group(1)):
            results[int(a, 16)] = int(s, 16)
    previous_count = len(results)

    manual = []
    for sw_base in switch_bases:
        fn = containing(sw_base)
        end = fn + results[fn] if fn in results else next_start(fn)
        pending = [t for t in switches[sw_base] if not fn <= t < end]
        if not pending:
            continue
        if fn not in real:
            # Sin referencias directas: se acepta como función propia si la anterior termina
            # en blr/b y nada de la anterior salta hacia este fragmento.
            prev = starts[bisect.bisect_left(starts, fn) - 1]
            last = fn - 4
            while last > prev and word(last) == 0:
                last -= 4
            ends_cleanly = word(last) == 0x4E800020 or (word(last) >> 26 == 18 and word(last) & 3 == 0)
            jumps_in = any(t is not None and t >= fn
                           for t in (branch_target(word(a), a) for a in range(prev, fn, 4)))
            if not ends_cleanly or jumps_in:
                manual.append((fn, sw_base, "el switch está en la continuación de la función anterior"))
                continue
            real.add(fn)

        problem = None
        scanned = fn
        while pending:
            target = pending.pop()
            if fn <= target < end:
                continue
            if target < fn:
                problem = f"un destino (0x{target:X}) está antes del inicio"
                break
            new_end = next_start(target)
            crossed = [s for s in starts if end <= s < new_end and s in real]
            if crossed:
                problem = f"absorbería la función real sub_{crossed[0]:X}"
                break
            end = new_end
            # Revisar el código recién absorbido: saltos hacia adelante y otros switches.
            for addr in range(scanned, end, 4):
                t = branch_target(word(addr), addr)
                if t is not None and end <= t < TEXT_END and t not in real:
                    pending.append(t)
            for b in switch_bases[bisect.bisect_left(switch_bases, scanned):bisect.bisect_left(switch_bases, end)]:
                pending.extend(t for t in switches[b] if not fn <= t < end)
            scanned = end

        if problem:
            manual.append((fn, sw_base, problem))
        else:
            results[fn] = end - fn

    # Correcciones a mano (revisadas en Ghidra): tienen prioridad y reemplazan cualquier
    # función automática que se solape con ellas.
    overrides = load_manual(os.path.join(os.path.dirname(config_path), MANUAL_FILE))
    for a, s in overrides.items():
        for k in [k for k, ks in results.items() if k < a + s and a < k + ks]:
            del results[k]
        results[a] = s
    manual = [m for m in manual if not any(a <= m[1] < a + s for a, s in overrides.items())]

    lines = [BEGIN_MARK, "functions = ["]
    lines += [f"    {{ address = 0x{a:08X}, size = 0x{s:X} }},{' # manual' if a in overrides else ''}"
              for a, s in sorted(results.items())]
    lines += ["]", END_MARK]
    block = "\n".join(lines)

    if previous:
        config = re.sub(re.escape(BEGIN_MARK) + r".*?" + re.escape(END_MARK), lambda _: block, config, flags=re.S)
    else:
        config = config.rstrip("\n") + "\n\n" + block + "\n"
    open(config_path, "w").write(config)

    print(f"{len(results)} funciones con límites explícitos en {config_path} "
          f"({len(results) - previous_count} nuevas)")
    if manual:
        print(f"\n{len(manual)} para revisión manual:")
        for fn, sw_base, why in manual:
            print(f"  sub_{fn:X} (switch en 0x{sw_base:X}): {why}")


if __name__ == "__main__":
    main()
