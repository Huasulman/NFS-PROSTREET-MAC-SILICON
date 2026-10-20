#!/usr/bin/env python3
"""Genera pruebas de las instrucciones VMX añadidas a XenonRecomp.

Toma fragmentos reales del código recompilado (ppc/*.cpp), los envuelve en un
programa de prueba y compara su resultado con una implementación de referencia
escrita en el orden de elementos del invitado (big-endian).

Uso:
    python3 -I tests/gen_vmx_tests.py <carpeta_ppc> <salida.cpp>
"""
import glob
import re
import sys

# Instrucción -> (referencia en C++, tipo de entrada)
# En las referencias: A, B, M = registros de entrada (copias previas), D = resultado esperado.
# gu32/gs32/gu16/gu8/gf32(reg, k) leen el elemento k del invitado; su32/su16/su8 lo escriben.
REFERENCES = {
    "vaddsws": ("for (int k = 0; k < 4; k++) su32(D, k, sat32(int64_t(gs32(A, k)) + gs32(B, k)));", "int"),
    "vandc": ("for (int k = 0; k < 16; k++) su8(D, k, gu8(A, k) & ~gu8(B, k));", "int"),
    "vsel128": ("for (int k = 0; k < 16; k++) su8(D, k, (gu8(A, k) & ~gu8(M, k)) | (gu8(B, k) & gu8(M, k)));", "int"),
    "vctuxs": ("for (int k = 0; k < 4; k++) su32(D, k, ctuxs(gf32(B, k), UIMM));", "float"),
    "vpkswss": ("for (int k = 0; k < 8; k++) su16(D, k, uint16_t(sat16(k < 4 ? gs32(A, k) : gs32(B, k - 4))));", "int"),
    "vpkuwus": ("for (int k = 0; k < 8; k++) su16(D, k, satu16(k < 4 ? gu32(A, k) : gu32(B, k - 4)));", "int"),
    "vslo": ("{ int sh = (gu8(B, 15) >> 3) & 0xF; for (int k = 0; k < 16; k++) su8(D, k, k + sh < 16 ? gu8(A, k + sh) : 0); }", "int"),
    "vcmpgtuw": ("for (int k = 0; k < 4; k++) su32(D, k, gu32(A, k) > gu32(B, k) ? 0xFFFFFFFFu : 0u);", "int"),
    "vcmpbfp": ("for (int k = 0; k < 4; k++) su32(D, k, cmpbfp(gf32(A, k), gf32(B, k)));", "float"),
    "vcmpbfp128": ("for (int k = 0; k < 4; k++) su32(D, k, cmpbfp(gf32(A, k), gf32(B, k)));", "float"),
    "vupkd3d128": ("if ((UIMM >> 2) == 3) { sf32(D, 0, half(gu16(B, 6))); sf32(D, 1, half(gu16(B, 7))); sf32(D, 2, 0.0f); sf32(D, 3, 1.0f); }"
                   " else for (int k = 0; k < 4; k++) sf32(D, k, half(gu16(B, 4 + k)));", "int"),
    # Tipo 3 (2 half floats): (x, y) -> una palabra, x en la mitad alta. UIMM = shift (3 = palabra x).
    # El resto de VD se conserva.
    "vpkd3d128": ("D = M; su32(D, 3 - UIMM, (uint32_t(f2h(gf32(B, 0))) << 16) | f2h(gf32(B, 1)));", "float"),
}
# Instrucciones con la forma (VD, VB, UIMM).
UNARY_IMM = {"vctuxs", "vupkd3d128"}

HEADER = r"""// Generado por tests/gen_vmx_tests.py. No editar a mano.
#include <bit>
#include <cmath>
#include <climits>
#include <cstdio>
#include <cstring>
#include <random>
#include "ppc_context.h"

// Los vectores se guardan con los 16 bytes invertidos: el elemento k del invitado
// es el elemento (N - 1 - k) en memoria del anfitrión.
static uint32_t gu32(const PPCVRegister& r, int k) { return r.u32[3 - k]; }
static int32_t gs32(const PPCVRegister& r, int k) { return r.s32[3 - k]; }
static float gf32(const PPCVRegister& r, int k) { return r.f32[3 - k]; }
static uint16_t gu16(const PPCVRegister& r, int k) { return r.u16[7 - k]; }
static uint8_t gu8(const PPCVRegister& r, int k) { return r.u8[15 - k]; }
static void sf32(PPCVRegister& r, int k, float v) { r.f32[3 - k] = v; }
static void su32(PPCVRegister& r, int k, uint32_t v) { r.u32[3 - k] = v; }
static void su16(PPCVRegister& r, int k, uint16_t v) { r.u16[7 - k] = v; }
static void su8(PPCVRegister& r, int k, uint8_t v) { r.u8[15 - k] = v; }

static int32_t sat32(int64_t v) { return v > INT_MAX ? INT_MAX : v < INT_MIN ? INT_MIN : int32_t(v); }
static int16_t sat16(int32_t v) { return v > SHRT_MAX ? SHRT_MAX : v < SHRT_MIN ? SHRT_MIN : int16_t(v); }
static uint16_t satu16(uint32_t v) { return v > 0xFFFF ? 0xFFFF : uint16_t(v); }
static uint32_t ctuxs(float f, int uimm)
{
    if (std::isnan(f)) return 0;
    double v = double(f) * double(1u << uimm);
    return v <= 0 ? 0 : v >= 4294967295.0 ? UINT_MAX : uint32_t(v);
}

static uint32_t cmpbfp(float a, float b)
{
    return (a <= b ? 0u : 0x80000000u) | (a >= -b ? 0u : 0x40000000u);
}
// Conversión por hardware (F16C / NEON) como referencia independiente.
static float half(uint16_t h) { return float(std::bit_cast<_Float16>(h)); }
// float -> half con truncamiento; NaN y |f| > 65504 saturan a 0x7FFF (como el float16_4 de XenonRecomp).
static uint16_t f2h(float f)
{
    uint16_t sign = (std::bit_cast<uint32_t>(f) >> 16) & 0x8000;
    double a = std::fabs(double(f));
    if (std::isnan(f) || a > 65504.0) return sign | 0x7FFF;
    if (a < std::ldexp(1.0, -14)) return sign | uint16_t(a / std::ldexp(1.0, -24));
    int e;
    double m = std::frexp(a, &e);
    return sign | uint16_t((e - 1 + 15) << 10) | uint16_t((m * 2 - 1) * 1024);
}

static std::mt19937 rng(12345);
static float randomFloat()
{
    static const float special[] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 65504.0f, 65520.0f, -65519.0f, 6.1e-5f, 6e-6f, -3e-7f, 1e-9f, 65535.5f, 4294967040.0f, 5e9f, -5e9f, NAN, INFINITY, -INFINITY };
    if (rng() % 4 == 0) return special[rng() % (sizeof(special) / sizeof(special[0]))];
    return std::ldexp(float(int32_t(rng())) / float(INT_MAX), int(rng() % 40) - 8);
}
static void randomize(PPCVRegister& r, bool floats)
{
    for (int i = 0; i < 4; i++)
    {
        if (floats) r.f32[i] = randomFloat();
        else r.u32[i] = rng() % 3 == 0 ? (rng() % 2 ? 0x7FFFFFF0u + rng() % 32 : 0x80000000u + rng() % 32) : uint32_t(rng());
    }
}

static int failures = 0;
static void check(const char* name, const char* text, const PPCVRegister& got, const PPCVRegister& expected)
{
    if (std::memcmp(got.u8, expected.u8, 16) == 0) return;
    if (failures++ < 10)
    {
        std::printf("FALLO %s (%s)\n  obtenido: ", name, text);
        for (int k = 0; k < 4; k++) std::printf("%08X ", gu32(got, k));
        std::printf("\n  esperado: ");
        for (int k = 0; k < 4; k++) std::printf("%08X ", gu32(expected, k));
        std::printf("\n");
    }
}
"""


def find_snippets(ppc_dir, ins, limit=8):
    """Devuelve hasta `limit` fragmentos distintos (por forma de operandos) de la instrucción."""
    found, shapes = [], set()
    pattern = re.compile(r"\t// " + re.escape(ins) + r" ([^\n]+)\n((?:\t(?!// )[^\n]*\n)+)")
    for path in sorted(glob.glob(f"{ppc_dir}/ppc_recomp.*.cpp")):
        for m in pattern.finditer(open(path).read()):
            ops = [o.strip() for o in m.group(1).split(",")]
            regs = [o for o in ops if re.fullmatch(r"v\d+", o)]
            # Forma: aliasing de registros, p. ej. (0,1,2) o (0,0,2), más los inmediatos.
            shape = tuple(regs.index(r) for r in regs) + tuple(o for o in ops if o not in regs)
            if shape not in shapes:
                shapes.add(shape)
                found.append((m.group(1), ops, m.group(2)))
            if len(found) >= limit:
                return found
    return found


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    ppc_dir, out_path = sys.argv[1], sys.argv[2]
    out = [HEADER, "int main()\n{\n    int tests = 0;"]
    for ins, (reference, kind) in REFERENCES.items():
        snippets = find_snippets(ppc_dir, ins)
        if not snippets:
            print(f"aviso: no se encontró {ins} en {ppc_dir}")
            continue
        for text, ops, code in snippets:
            regs = [o for o in ops if re.fullmatch(r"v\d+", o)]
            d = regs[0]
            if ins in UNARY_IMM:
                a, b, m, uimm = None, regs[1], None, int(ops[2], 0)
                if ins == "vupkd3d128" and uimm >> 2 not in (3, 5):
                    continue  # solo los formatos half float añadidos en este proyecto
            elif ins == "vpkd3d128":
                if ops[2] != "3":
                    continue  # solo el formato float16_2 añadido en este proyecto
                a, b, m, uimm = None, regs[1], d, int(ops[4], 0)
            else:
                a, b = regs[1], regs[2]
                m = regs[3] if len(regs) > 3 else None
                uimm = 0
            floats = "true" if kind == "float" else "false"
            body = code.replace("\n\t", "\n        ").strip()
            out.append(f"""
    for (int trial = 0; trial < 20000; trial++)
    {{
        PPCContext ctx{{}};
        PPCRegister temp{{}};
        PPCVRegister vTemp{{}};
        (void)temp; (void)vTemp;
        for (auto* r : {{ &ctx.{d}, &ctx.{b}{', &ctx.' + a if a else ''}{', &ctx.' + m if m else ''} }}) randomize(*r, {floats});
        const PPCVRegister A = ctx.{a or b}, B = ctx.{b}, M = ctx.{m or d};
        (void)A; (void)M;
        const int UIMM = {uimm};
        (void)UIMM;
        PPCVRegister D{{}};
        {reference}
        {body}
        check("{ins}", "{text}", ctx.{d}, D);
        tests++;
    }}""")
    out.append('    std::printf("%d pruebas, %d fallos\\n", tests, failures);\n    return failures != 0;\n}\n')
    open(out_path, "w").write("\n".join(out))
    print(f"Pruebas escritas en {out_path}")


if __name__ == "__main__":
    main()
