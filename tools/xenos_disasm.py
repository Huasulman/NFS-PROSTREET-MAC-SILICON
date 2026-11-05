#!/usr/bin/env python3
"""Desensamblador del microcódigo de shaders de Xenos (GPU de la Xbox 360).

Formato según Xenia (ucode.h) y freedreno (instr-a2xx.h, misma arquitectura que Adreno 200):
  - Control de flujo: instrucciones de 48 bits, dos por cada 3 palabras, al principio.
  - Instrucciones de ALU o de lectura (fetch): 96 bits (3 palabras); EXEC las ejecuta por
    grupos, y su campo `sequence` dice cuáles son fetch.

Uso:
    python3 -I tools/xenos_disasm.py analysis/shaders/*.bin
Los archivos son los que guarda el runtime con PROSTREET_SHADERS (palabras big-endian).
"""
import struct
import sys

CF_OPCODES = ["NOP", "EXEC", "EXEC_END", "COND_EXEC", "COND_EXEC_END", "COND_PRED_EXEC",
              "COND_PRED_EXEC_END", "LOOP_START", "LOOP_END", "COND_CALL", "RETURN", "COND_JMP",
              "ALLOC", "COND_EXEC_PRED_CLEAN", "COND_EXEC_PRED_CLEAN_END", "MARK_VS_FETCH_DONE"]
EXECS = {1, 2, 3, 4, 5, 6, 13, 14}
EXEC_ENDS = {2, 4, 6, 14}
ALLOC_TYPES = ["none", "position", "interpolators/colors", "memory"]

VECTOR_OPS = ["add", "mul", "max", "min", "seq", "sgt", "sge", "sne", "frc", "trunc", "floor",
              "mad", "cndeq", "cndge", "cndgt", "dp4", "dp3", "dp2add", "cube", "max4",
              "setp_eq_push", "setp_ne_push", "setp_gt_push", "setp_ge_push", "kill_eq",
              "kill_gt", "kill_ge", "kill_ne", "dst", "maxa"]
VECTOR_SOURCES = {"dp2add": 3, "mad": 3, "cndeq": 3, "cndge": 3, "cndgt": 3, "frc": 1, "trunc": 1,
                  "floor": 1}
SCALAR_OPS = ["adds", "adds_prev", "muls", "muls_prev", "muls_prev2", "maxs", "mins", "seqs",
              "sgts", "sges", "snes", "frcs", "truncs", "floors", "exp", "logc", "log", "rcpc",
              "rcpf", "rcp", "rsqc", "rsqf", "rsq", "maxas", "maxasf", "subs", "subs_prev",
              "setp_eq", "setp_ne", "setp_gt", "setp_ge", "setp_inv", "setp_pop", "setp_clr",
              "setp_rstr", "kills_eq", "kills_gt", "kills_ge", "kills_ne", "kills_one", "sqrt",
              "op41", "mulsc0", "mulsc1", "addsc0", "addsc1", "subsc0", "subsc1", "sin", "cos",
              "retain_prev"]
FETCH_OPS = {0: "vfetch", 1: "tfetch", 16: "getBorderColorFrac", 17: "getCompTexLOD",
             18: "getGradients", 19: "getWeights", 24: "setTexLOD", 25: "setGradientsH",
             26: "setGradientsV"}
COMPONENTS = "xyzw"


def bits(value, start, count):
    return (value >> start) & ((1 << count) - 1)


def swizzle(value):
    """Swizzle de 8 bits de la ALU: cada componente es relativo a la identidad."""
    text = "".join(COMPONENTS[(bits(value, i * 2, 2) + i) & 3] for i in range(4))
    return "" if text == "xyzw" else "." + text


def destination_swizzle(value):
    """Swizzle de destino de un fetch: 3 bits por componente (0-3 = xyzw, 4 = 0, 5 = 1, 7 = no escribir)."""
    return "".join("xyzw01?_"[bits(value, i * 3, 3)] for i in range(4))


def mask(value):
    return "".join(COMPONENTS[i] if (value >> i) & 1 else "_" for i in range(4))


def alu_source(number, register, is_temporary, swiz, negate):
    if is_temporary:
        name = f"r{register & 0x3F}"
        if register & 0x80:
            name = f"|{name}|"
    else:
        name = f"c{register}"
    return ("-" if negate else "") + name + swizzle(swiz)


def export_name(shader_type, register):
    if shader_type == "vs":
        return {62: "oPos", 63: "oPts"}.get(register, f"o{register}")
    return {61: "oDepth"}.get(register, f"oC{register}")


def disassemble_alu(words, shader_type):
    w0, w1, w2 = words
    vector_op = bits(w2, 24, 5)
    scalar_op = bits(w0, 26, 6)
    export = bits(w0, 15, 1)
    sources = [
        alu_source(1, bits(w2, 16, 8), bits(w2, 31, 1), bits(w1, 16, 8), bits(w1, 26, 1)),
        alu_source(2, bits(w2, 8, 8), bits(w2, 30, 1), bits(w1, 8, 8), bits(w1, 25, 1)),
        alu_source(3, bits(w2, 0, 8), bits(w2, 29, 1), bits(w1, 0, 8), bits(w1, 24, 1)),
    ]
    vector_mask, scalar_mask = bits(w0, 16, 4), bits(w0, 20, 4)
    vector_dest, scalar_dest = bits(w0, 0, 6), bits(w0, 8, 6)
    out = []
    if vector_mask or not scalar_mask:
        name = VECTOR_OPS[vector_op] if vector_op < len(VECTOR_OPS) else f"vop{vector_op}"
        count = VECTOR_SOURCES.get(name, 2)
        dest = export_name(shader_type, vector_dest) if export else f"r{vector_dest}"
        clamp = "_sat" if bits(w0, 24, 1) else ""
        out.append(f"{name}{clamp} {dest}.{mask(vector_mask)}, {', '.join(sources[:count])}")
    if scalar_mask:
        name = SCALAR_OPS[scalar_op] if scalar_op < len(SCALAR_OPS) else f"sop{scalar_op}"
        # En una exportación, la parte escalar escribe en el mismo registro que la vectorial
        # (p. ej. o2 = (2 * color.rgb, color.a)); scalar_dest solo cuenta para registros normales.
        dest = export_name(shader_type, vector_dest) if export else f"r{scalar_dest}"
        clamp = "_sat" if bits(w0, 25, 1) else ""
        out.append(f"{name}{clamp} {dest}.{mask(scalar_mask)}, {sources[2]}")
    if bits(w1, 28, 1):
        out = [f"({'p' if bits(w1, 27, 1) else '!p'}) {line}" for line in out]
    return " + ".join(out)


def disassemble_fetch(words):
    w0, w1, w2 = words
    opcode = bits(w0, 0, 5)
    name = FETCH_OPS.get(opcode, f"fetch{opcode}")
    source, dest = bits(w0, 5, 6), bits(w0, 12, 6)
    const_index = bits(w0, 20, 5)
    if opcode == 0:  # vfetch
        const_select = bits(w0, 25, 2)
        source_component = COMPONENTS[bits(w0, 30, 2)]
        fmt, stride, offset = bits(w1, 16, 6), bits(w2, 0, 8), bits(w2, 8, 22)
        signed = "signed" if bits(w1, 14, 1) else "unsigned"
        normalized = "int" if bits(w1, 13, 1) else "norm"
        return (f"vfetch r{dest}.{destination_swizzle(bits(w1, 0, 12))}, r{source}.{source_component}, "
                f"vf{const_index * 3 + const_select} (formato {fmt}, {signed} {normalized}, "
                f"stride {stride * 4} B, offset {offset * 4} B)")
    source_swizzle = "".join(COMPONENTS[bits(w0, 26 + i * 2, 2)] for i in range(3))
    dimension = {0: " (1D)", 2: " (3D)", 3: " (cubo)"}.get(bits(w2, 14, 2), "") if opcode == 1 else ""
    return f"{name} r{dest}.{destination_swizzle(bits(w1, 0, 12))}, r{source}.{source_swizzle}, tf{const_index}{dimension}"


def disassemble(path):
    data = open(path, "rb").read()
    words = list(struct.unpack(f">{len(data) // 4}I", data))
    shader_type = "ps" if "/ps_" in path or path.startswith("ps_") else "vs"
    print(f"; {path}: {'píxeles' if shader_type == 'ps' else 'vértices'}, {len(words)} palabras")

    # Control de flujo: hasta la primera dirección de instrucción que use un EXEC.
    cf, first_instruction, i = [], len(words) // 3, 0
    while i * 3 + 2 < len(words) and i < first_instruction:
        d0, d1, d2 = words[i * 3: i * 3 + 3]
        for low, high in ((d0, d1 & 0xFFFF), ((d1 >> 16) | ((d2 & 0xFFFF) << 16), d2 >> 16)):
            opcode = high >> 12
            cf.append((low, high, opcode))
            if opcode in EXECS and bits(low, 12, 3):
                first_instruction = min(first_instruction, bits(low, 0, 12))
        i += 1

    for low, high, opcode in cf:
        name = CF_OPCODES[opcode]
        if opcode in EXECS:
            address, count, sequence = bits(low, 0, 12), bits(low, 12, 3), bits(low, 16, 12)
            condition = ""
            if opcode in (3, 4, 13, 14):
                condition = f" b{bits(high, 2, 8)}=={bits(high, 10, 1)}"
            elif opcode in (5, 6):
                condition = f" p=={bits(high, 10, 1)}"
            print(f"  {name}{condition} @{address} x{count}")
            for n in range(count):
                start = (address + n) * 3
                instruction = words[start:start + 3]
                if len(instruction) < 3:
                    print(f"      ??? fuera del shader ({start})")
                    continue
                is_fetch = (sequence >> (n * 2)) & 1
                text = disassemble_fetch(instruction) if is_fetch else disassemble_alu(instruction, shader_type)
                print(f"    {address + n:3d}: {text}")
        elif opcode == 12:
            print(f"  ALLOC {ALLOC_TYPES[bits(high, 9, 2)]} x{bits(low, 0, 3) + 1}")
        elif opcode != 0:
            print(f"  {name} (0x{high:04X}{low:08X})")
        if opcode in EXEC_ENDS:
            break
    print()


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    for path in sys.argv[1:]:
        disassemble(path)


if __name__ == "__main__":
    main()
