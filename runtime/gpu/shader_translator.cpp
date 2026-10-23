#include "shader_translator.h"

#include <cstdio>
#include <set>
#include <sstream>

namespace gpu
{
    namespace
    {
        // Preludio común a todos los shaders generados.
        const char* kPrelude = R"MSL(#include <metal_stdlib>
using namespace metal;

struct DrawState
{
    uint primitive; uint indexSize; uint indexBase; uint indexEndian; uint indexOffset; uint vte;
    float viewportScale[4]; float viewportOffset[4]; float targetSize[2]; float windowOffset[2]; uint bools[8];
};

struct VSOut
{
    float4 position [[position]];
    float4 i0 [[user(i0)]]; float4 i1 [[user(i1)]]; float4 i2 [[user(i2)]]; float4 i3 [[user(i3)]];
    float4 i4 [[user(i4)]]; float4 i5 [[user(i5)]]; float4 i6 [[user(i6)]]; float4 i7 [[user(i7)]];
    float4 i8 [[user(i8)]]; float4 i9 [[user(i9)]]; float4 i10 [[user(i10)]]; float4 i11 [[user(i11)]];
    float4 i12 [[user(i12)]]; float4 i13 [[user(i13)]]; float4 i14 [[user(i14)]]; float4 i15 [[user(i15)]];
};

// Intercambio de bytes de la GPU (2 bits bajos de la dirección o campo endian de la constante).
inline uint gpu_swap(uint v, uint e)
{
    switch (e)
    {
    case 1: return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    case 2: return (v << 24) | ((v << 8) & 0x00FF0000u) | ((v >> 8) & 0x0000FF00u) | (v >> 24);
    case 3: return (v << 16) | (v >> 16);
    default: return v;
    }
}

inline uint load_word(device const uint* mem, uint physical, uint e) { return gpu_swap(mem[(physical & 0x1FFFFFFFu) >> 2], e); }
inline float load_float(device const uint* mem, uint physical, uint e) { return as_type<float>(load_word(mem, physical, e)); }

inline float unorm8(uint v) { return float(v & 0xFFu) / 255.0f; }
inline float snorm8(uint v) { return max(float(int(v << 24) >> 24) / 127.0f, -1.0f); }
inline float unorm16(uint v) { return float(v & 0xFFFFu) / 65535.0f; }
inline float snorm16(uint v) { return max(float(int(v << 16) >> 16) / 32767.0f, -1.0f); }

// Lectura de un vértice según el formato de Xenos. Los componentes que faltan valen (0, 0, 0, 1).
float4 fetch_vertex(device const uint* mem, uint addr, uint e, uint fmt, bool isSigned, bool isInteger)
{
    switch (fmt)
    {
    case 6: // 8_8_8_8
    {
        uint v = load_word(mem, addr, e);
        if (isInteger)
            return isSigned ? float4(float(int(v << 24) >> 24), float(int(v << 16) >> 24), float(int(v << 8) >> 24), float(int(v) >> 24))
                            : float4(float(v & 0xFF), float((v >> 8) & 0xFF), float((v >> 16) & 0xFF), float(v >> 24));
        return isSigned ? float4(snorm8(v), snorm8(v >> 8), snorm8(v >> 16), snorm8(v >> 24))
                        : float4(unorm8(v), unorm8(v >> 8), unorm8(v >> 16), unorm8(v >> 24));
    }
    case 7: // 2_10_10_10
    {
        uint v = load_word(mem, addr, e);
        return float4(float(v & 0x3FF) / 1023.0f, float((v >> 10) & 0x3FF) / 1023.0f, float((v >> 20) & 0x3FF) / 1023.0f, float(v >> 30) / 3.0f);
    }
    case 25: // 16_16
    {
        uint v = load_word(mem, addr, e);
        if (isInteger)
            return isSigned ? float4(float(int(v << 16) >> 16), float(int(v) >> 16), 0, 1) : float4(float(v & 0xFFFF), float(v >> 16), 0, 1);
        return isSigned ? float4(snorm16(v), snorm16(v >> 16), 0, 1) : float4(unorm16(v), unorm16(v >> 16), 0, 1);
    }
    case 26: // 16_16_16_16
    {
        uint a = load_word(mem, addr, e), b = load_word(mem, addr + 4, e);
        if (isInteger)
            return isSigned ? float4(float(int(a << 16) >> 16), float(int(a) >> 16), float(int(b << 16) >> 16), float(int(b) >> 16))
                            : float4(float(a & 0xFFFF), float(a >> 16), float(b & 0xFFFF), float(b >> 16));
        return isSigned ? float4(snorm16(a), snorm16(a >> 16), snorm16(b), snorm16(b >> 16))
                        : float4(unorm16(a), unorm16(a >> 16), unorm16(b), unorm16(b >> 16));
    }
    case 31: // 16_16_FLOAT
    {
        uint v = load_word(mem, addr, e);
        return float4(float(as_type<half>(ushort(v & 0xFFFF))), float(as_type<half>(ushort(v >> 16))), 0, 1);
    }
    case 32: // 16_16_16_16_FLOAT
    {
        uint a = load_word(mem, addr, e), b = load_word(mem, addr + 4, e);
        return float4(float(as_type<half>(ushort(a & 0xFFFF))), float(as_type<half>(ushort(a >> 16))),
                      float(as_type<half>(ushort(b & 0xFFFF))), float(as_type<half>(ushort(b >> 16))));
    }
    case 33: return float4(isSigned ? float(int(load_word(mem, addr, e))) : float(load_word(mem, addr, e)), 0, 0, 1); // 32
    case 36: return float4(load_float(mem, addr, e), 0, 0, 1);                                                    // 32_FLOAT
    case 37: return float4(load_float(mem, addr, e), load_float(mem, addr + 4, e), 0, 1);                         // 32_32_FLOAT
    case 57: return float4(load_float(mem, addr, e), load_float(mem, addr + 4, e), load_float(mem, addr + 8, e), 1); // 32_32_32_FLOAT
    case 38: return float4(load_float(mem, addr, e), load_float(mem, addr + 4, e), load_float(mem, addr + 8, e), load_float(mem, addr + 12, e));
    default: return float4(0, 0, 0, 1);
    }
}

// Swizzle de la constante de fetch de textura: 3 bits por componente (0-3 = xyzw, 4 = 0, 5 = 1).
float4 fetch_swizzle(float4 v, uint swizzle)
{
    float4 r;
    for (uint i = 0; i < 4; i++)
    {
        uint c = (swizzle >> (i * 3)) & 7;
        r[i] = c < 4 ? v[c] : (c == 5 ? 1.0f : 0.0f);
    }
    return r;
}

inline bool bool_constant(constant DrawState& st, uint index) { return (st.bools[index >> 5] >> (index & 31)) & 1; }
)MSL";

        constexpr uint32_t Bits(uint32_t value, uint32_t start, uint32_t count)
        {
            return (value >> start) & ((1u << count) - 1);
        }

        const char kComponents[] = "xyzw";

        // Swizzle de 8 bits de la ALU: cada componente es relativo a la identidad.
        std::string AluSwizzle(uint32_t value)
        {
            std::string text = ".";
            for (uint32_t i = 0; i < 4; i++)
                text += kComponents[(Bits(value, i * 2, 2) + i) & 3];
            return text;
        }

        uint32_t AluComponent(uint32_t swizzle, uint32_t index) { return (Bits(swizzle, index * 2, 2) + index) & 3; }

        struct Translator
        {
            bool pixel;
            const std::vector<uint32_t>& code;
            std::ostringstream out;
            std::ostringstream warnings;
            std::set<uint32_t> textures;
            uint32_t lastStride = 0;
            int indent = 1;

            Translator(bool pixel, const std::vector<uint32_t>& code) : pixel(pixel), code(code) {}

            std::ostream& Line()
            {
                for (int i = 0; i < indent; i++)
                    out << "    ";
                return out;
            }

            // Operando de la ALU: registro temporal (con valor absoluto opcional) o constante.
            std::string Source(uint32_t reg, bool temporary, uint32_t swizzle, bool negate)
            {
                std::string name;
                if (temporary)
                {
                    name = "r[" + std::to_string(reg & 0x3F) + "]";
                    if (reg & 0x80)
                        name = "fabs(" + name + ")";
                }
                else
                {
                    name = "c[" + std::to_string(reg) + "]";
                }
                return (negate ? "(-" : "(") + name + AluSwizzle(swizzle) + ")";
            }

            std::string ExportName(uint32_t reg)
            {
                if (!pixel)
                {
                    if (reg == 62)
                        return "opos";
                    if (reg < 16)
                        return "o[" + std::to_string(reg) + "]";
                    return "odiscard"; // 63 (tamaño de punto, etc.): se ignora
                }
                if (reg < 4)
                    return "oc[" + std::to_string(reg) + "]";
                return "odiscard"; // 61 = profundidad: aún no
            }

            void WriteMasked(const std::string& dest, const std::string& value, uint32_t mask, bool saturate)
            {
                for (uint32_t i = 0; i < 4; i++)
                {
                    if ((mask >> i) & 1)
                    {
                        Line() << dest << "." << kComponents[i] << " = "
                               << (saturate ? "saturate(" : "(") << value << "." << kComponents[i] << ");\n";
                    }
                }
            }

            void Alu(const uint32_t* w)
            {
                const uint32_t w0 = w[0], w1 = w[1], w2 = w[2];
                const uint32_t vectorOp = Bits(w2, 24, 5), scalarOp = Bits(w0, 26, 6);
                const bool exportData = Bits(w0, 15, 1);
                const uint32_t vectorMask = Bits(w0, 16, 4), scalarMask = Bits(w0, 20, 4);
                const uint32_t vectorDest = Bits(w0, 0, 6), scalarDest = Bits(w0, 8, 6);
                const bool vectorClamp = Bits(w0, 24, 1), scalarClamp = Bits(w0, 25, 1);
                const uint32_t reg3 = Bits(w2, 0, 8), swizzle3 = Bits(w1, 0, 8);
                const bool temporary3 = Bits(w2, 29, 1), negate3 = Bits(w1, 24, 1);

                const bool predicated = Bits(w1, 28, 1);
                if (predicated)
                {
                    Line() << "if (p == " << (Bits(w1, 27, 1) ? "true" : "false") << ")\n";
                }
                Line() << "{\n";
                indent++;
                Line() << "float4 s1 = " << Source(Bits(w2, 16, 8), Bits(w2, 31, 1), Bits(w1, 16, 8), Bits(w1, 26, 1)) << ";\n";
                Line() << "float4 s2 = " << Source(Bits(w2, 8, 8), Bits(w2, 30, 1), Bits(w1, 8, 8), Bits(w1, 25, 1)) << ";\n";
                Line() << "float4 s3 = " << Source(reg3, temporary3, swizzle3, negate3) << ";\n";

                // Parte vectorial.
                std::string v;
                switch (vectorOp)
                {
                case 0: v = "s1 + s2"; break;
                case 1: v = "s1 * s2"; break;
                case 2: v = "select(s2, s1, s1 >= s2)"; break;
                case 3: v = "select(s2, s1, s1 < s2)"; break;
                case 4: v = "select(float4(0), float4(1), s1 == s2)"; break;
                case 5: v = "select(float4(0), float4(1), s1 > s2)"; break;
                case 6: v = "select(float4(0), float4(1), s1 >= s2)"; break;
                case 7: v = "select(float4(0), float4(1), s1 != s2)"; break;
                case 8: v = "fract(s1)"; break;
                case 9: v = "trunc(s1)"; break;
                case 10: v = "floor(s1)"; break;
                case 11: v = "s1 * s2 + s3"; break;
                case 12: v = "select(s3, s2, s1 == 0.0f)"; break;
                case 13: v = "select(s3, s2, s1 >= 0.0f)"; break;
                case 14: v = "select(s3, s2, s1 > 0.0f)"; break;
                case 15: v = "float4(dot(s1, s2))"; break;
                case 16: v = "float4(dot(s1.xyz, s2.xyz))"; break;
                case 17: v = "float4(s1.x * s2.x + s1.y * s2.y + s3.x)"; break;
                case 19: v = "float4(max(max(s1.x, s1.y), max(s1.z, s1.w)))"; break;
                case 28: v = "float4(1.0f, s1.y * s2.y, s1.z, s2.w)"; break;
                case 29: // maxa: además fija el registro de direcciones
                    Line() << "a0 = clamp(int(floor(s1.w + 0.5f)), -256, 255);\n";
                    v = "select(s2, s1, s1 >= s2)";
                    break;
                case 24: case 25: case 26: case 27: // kill_*: descarta el píxel si algún componente cumple
                {
                    static const char* const kOps[] = { "==", ">", ">=", "!=" };
                    if (pixel)
                        Line() << "if (any(s1 " << kOps[vectorOp - 24] << " s2)) discard_fragment();\n";
                    v = "float4(0)";
                    break;
                }
                case 20: case 21: case 22: case 23: // setp_*_push
                {
                    static const char* const kOps[] = { "==", "!=", ">", ">=" };
                    Line() << "p = (s1.w " << kOps[vectorOp - 20] << " 0.0f) && (s2.w " << kOps[vectorOp - 20] << " 0.0f);\n";
                    v = "float4((s1.x " + std::string(kOps[vectorOp - 20]) + " 0.0f && s2.x " + kOps[vectorOp - 20] + " 0.0f) ? 0.0f : s1.x + 1.0f)";
                    break;
                }
                default:
                    warnings << "operación vectorial " << vectorOp << " no soportada; ";
                    v = "float4(0)";
                    break;
                }
                Line() << "float4 vr = " << v << ";\n";

                // Parte escalar: operandos a = src3.(componente 0), b = src3.(componente 1).
                const std::string a = "s3." + std::string(1, kComponents[0]);
                const std::string b = "s3." + std::string(1, kComponents[1]);
                std::string s;
                switch (scalarOp)
                {
                case 0: s = a + " + " + b; break;
                case 1: s = a + " + ps"; break;
                case 2: s = a + " * " + b; break;
                case 3: s = a + " * ps"; break;
                case 4: s = a + " * ps"; break; // muls_prev2: simplificado
                case 5: s = "(" + a + " >= " + b + " ? " + a + " : " + b + ")"; break;
                case 6: s = "(" + a + " < " + b + " ? " + a + " : " + b + ")"; break;
                case 7: s = "(" + a + " == 0.0f ? 1.0f : 0.0f)"; break;
                case 8: s = "(" + a + " > 0.0f ? 1.0f : 0.0f)"; break;
                case 9: s = "(" + a + " >= 0.0f ? 1.0f : 0.0f)"; break;
                case 10: s = "(" + a + " != 0.0f ? 1.0f : 0.0f)"; break;
                case 11: s = "fract(" + a + ")"; break;
                case 12: s = "trunc(" + a + ")"; break;
                case 13: s = "floor(" + a + ")"; break;
                case 14: s = "exp2(" + a + ")"; break;
                case 15: s = "(" + a + " == 0.0f ? -FLT_MAX : log2(" + a + "))"; break;
                case 16: s = "log2(" + a + ")"; break;
                case 17: s = "(" + a + " == 0.0f ? FLT_MAX : 1.0f / " + a + ")"; break;
                case 18: case 19: s = "1.0f / " + a; break;
                case 20: s = "(" + a + " == 0.0f ? FLT_MAX : rsqrt(" + a + "))"; break;
                case 21: case 22: s = "rsqrt(" + a + ")"; break;
                case 23: case 24:
                    Line() << "a0 = clamp(int(floor(" << a << (scalarOp == 23 ? " + 0.5f" : "") << ")), -256, 255);\n";
                    s = "(" + a + " >= " + b + " ? " + a + " : " + b + ")";
                    break;
                case 25: s = a + " - " + b; break;
                case 26: s = a + " - ps"; break;
                case 27: Line() << "p = " << a << " == 0.0f;\n"; s = "(p ? 0.0f : 1.0f)"; break;
                case 28: Line() << "p = " << a << " != 0.0f;\n"; s = "(p ? 0.0f : 1.0f)"; break;
                case 29: Line() << "p = " << a << " > 0.0f;\n"; s = "(p ? 0.0f : 1.0f)"; break;
                case 30: Line() << "p = " << a << " >= 0.0f;\n"; s = "(p ? 0.0f : 1.0f)"; break;
                case 31: Line() << "p = " << a << " == 1.0f;\n"; s = "(p ? 0.0f : (" + a + " == 0.0f ? 1.0f : " + a + "))"; break;
                case 32: Line() << "p = " << a << " - 1.0f <= 0.0f;\n"; s = "(p ? 0.0f : " + a + " - 1.0f)"; break;
                case 33: Line() << "p = false;\n"; s = "FLT_MAX"; break;
                case 34: Line() << "p = " << a << " == 0.0f;\n"; s = "(p ? 0.0f : " + a + ")"; break;
                case 35: case 36: case 37: case 38: case 39:
                {
                    static const char* const kConditions[] = { "== 0.0f", "> 0.0f", ">= 0.0f", "!= 0.0f", "== 1.0f" };
                    if (pixel)
                        Line() << "if (" << a << " " << kConditions[scalarOp - 35] << ") discard_fragment();\n";
                    s = "0.0f";
                    break;
                }
                case 40: s = "sqrt(" + a + ")"; break;
                case 42: case 43: case 44: case 45: case 46: case 47:
                {
                    // Operación con constante y temporal (mulsc, addsc, subsc): la constante es src3 y el
                    // número del temporal sale del código de operación, del bit de selección y de los
                    // bits 2-5 del swizzle (según Xenia). Quedan los bits 0-1 del swizzle para el
                    // componente del temporal y los bits 6-7 para el de la constante.
                    const uint32_t temp = (scalarOp & 1) | (uint32_t(temporary3) << 1) | (swizzle3 & 0x3C);
                    const std::string other = "r[" + std::to_string(temp) + "]." + std::string(1, kComponents[AluComponent(swizzle3, 0)]);
                    Line() << "float sc = c[" << reg3 << "]." << kComponents[AluComponent(swizzle3, 3)] << ";\n";
                    const char* op = scalarOp <= 43 ? " * " : (scalarOp <= 45 ? " + " : " - ");
                    s = std::string("sc") + op + other;
                    break;
                }
                case 48: s = "sin(" + a + ")"; break;
                case 49: s = "cos(" + a + ")"; break;
                case 50: s = "ps"; break;
                default:
                    warnings << "operación escalar " << scalarOp << " no soportada; ";
                    s = "0.0f";
                    break;
                }
                Line() << "float sr = " << s << ";\n";

                const std::string vectorTarget = exportData ? ExportName(vectorDest) : "r[" + std::to_string(vectorDest) + "]";
                const std::string scalarTarget = exportData ? ExportName(vectorDest) : "r[" + std::to_string(scalarDest) + "]";
                WriteMasked(vectorTarget, "vr", vectorMask, vectorClamp);
                if (scalarMask)
                    WriteMasked(scalarTarget, "float4(sr)", scalarMask, scalarClamp);
                Line() << "ps = sr;\n";
                indent--;
                Line() << "}\n";
            }

            // Escribe los componentes de un fetch según su swizzle de destino (3 bits: 0-3, 4 = 0, 5 = 1, 7 = no).
            void WriteFetch(uint32_t dest, uint32_t swizzle)
            {
                for (uint32_t i = 0; i < 4; i++)
                {
                    const uint32_t select = Bits(swizzle, i * 3, 3);
                    if (select == 7)
                        continue;
                    Line() << "r[" << dest << "]." << kComponents[i] << " = "
                           << (select < 4 ? std::string("f.") + kComponents[select] : (select == 5 ? std::string("1.0f") : std::string("0.0f"))) << ";\n";
                }
            }

            void Fetch(const uint32_t* w)
            {
                const uint32_t w0 = w[0], w1 = w[1], w2 = w[2];
                const uint32_t opcode = Bits(w0, 0, 5);
                const uint32_t source = Bits(w0, 5, 6), dest = Bits(w0, 12, 6);
                if (opcode == 0) // vfetch
                {
                    const uint32_t constant = Bits(w0, 20, 5) * 3 + Bits(w0, 25, 2);
                    const char component = kComponents[Bits(w0, 30, 2)];
                    uint32_t stride = Bits(w2, 0, 8) * 4;
                    if (stride == 0)
                        stride = lastStride; // "mini fetch": mismo vértice que la lectura anterior
                    lastStride = stride;
                    const uint32_t offset = Bits(w2, 8, 22) * 4;
                    Line() << "{\n";
                    indent++;
                    Line() << "uint index = uint(int(round(r[" << source << "]." << component << ")));\n";
                    Line() << "uint2 vf = uint2(fc[" << constant * 2 << "], fc[" << constant * 2 + 1 << "]);\n";
                    Line() << "float4 f = fetch_vertex(mem, (vf.x & 0x1FFFFFFCu) + index * " << stride << "u + " << offset
                           << "u, vf.y & 3u, " << Bits(w1, 16, 6) << "u, " << (Bits(w1, 14, 1) ? "true" : "false") << ", "
                           << (Bits(w1, 13, 1) ? "true" : "false") << ");\n";
                    WriteFetch(dest, Bits(w1, 0, 12));
                    indent--;
                    Line() << "}\n";
                    return;
                }
                if (opcode == 1) // tfetch
                {
                    const uint32_t constant = Bits(w0, 20, 5);
                    if (!pixel)
                    {
                        warnings << "lectura de textura en un shader de vértices; ";
                        return;
                    }
                    textures.insert(constant);
                    std::string coordinates = "float2(r[" + std::to_string(source) + "]." + kComponents[Bits(w0, 26, 2)] + ", r[" +
                                              std::to_string(source) + "]." + kComponents[Bits(w0, 28, 2)] + ")";
                    Line() << "{\n";
                    indent++;
                    Line() << "float4 f = fetch_swizzle(t" << constant << ".sample(s" << constant << ", " << coordinates
                           << "), (fc[" << constant * 6 + 3 << "] >> 1) & 0xFFFu);\n";
                    WriteFetch(dest, Bits(w1, 0, 12));
                    indent--;
                    Line() << "}\n";
                    return;
                }
                warnings << "fetch " << opcode << " no soportado; ";
            }

            bool Translate(uint64_t hash, std::string& entry)
            {
                char name[32];
                std::snprintf(name, sizeof(name), "%s_%016llx", pixel ? "ps" : "vs", (unsigned long long)hash);
                entry = name;

                std::ostringstream body;
                std::swap(out, body);

                // Control de flujo: pares de instrucciones de 48 bits al principio.
                uint32_t firstInstruction = uint32_t(code.size() / 3);
                bool finished = false;
                for (uint32_t pair = 0; pair < firstInstruction && pair * 3 + 2 < code.size() && !finished; pair++)
                {
                    const uint32_t d0 = code[pair * 3], d1 = code[pair * 3 + 1], d2 = code[pair * 3 + 2];
                    const uint32_t lows[2] = { d0, (d1 >> 16) | ((d2 & 0xFFFF) << 16) };
                    const uint32_t highs[2] = { d1 & 0xFFFF, d2 >> 16 };
                    for (int k = 0; k < 2 && !finished; k++)
                    {
                        const uint32_t low = lows[k], high = highs[k], opcode = high >> 12;
                        const bool isExec = opcode == 1 || opcode == 2 || (opcode >= 3 && opcode <= 6) || opcode == 13 || opcode == 14;
                        if (opcode == 0 || opcode == 12 || opcode == 15) // NOP, ALLOC, MARK_VS_FETCH_DONE
                            continue;
                        if (!isExec)
                        {
                            warnings << "control de flujo " << opcode << " no soportado; ";
                            continue;
                        }
                        const uint32_t address = Bits(low, 0, 12), count = Bits(low, 12, 3), sequence = Bits(low, 16, 12);
                        if (count)
                            firstInstruction = std::min(firstInstruction, address);

                        bool conditional = false;
                        if (opcode == 3 || opcode == 4 || opcode == 13 || opcode == 14)
                        {
                            Line() << "if (bool_constant(st, " << Bits(high, 2, 8) << ") == " << (Bits(high, 10, 1) ? "true" : "false") << ")\n";
                            conditional = true;
                        }
                        else if (opcode == 5 || opcode == 6)
                        {
                            Line() << "if (p == " << (Bits(high, 10, 1) ? "true" : "false") << ")\n";
                            conditional = true;
                        }
                        Line() << "{\n";
                        indent++;
                        for (uint32_t n = 0; n < count; n++)
                        {
                            const uint32_t start = (address + n) * 3;
                            if (start + 3 > code.size())
                            {
                                warnings << "instrucción fuera del shader; ";
                                break;
                            }
                            if ((sequence >> (n * 2)) & 1)
                                Fetch(&code[start]);
                            else
                                Alu(&code[start]);
                        }
                        indent--;
                        Line() << "}\n";
                        (void)conditional;
                        // EXEC_END termina el shader (salvo si es condicional: entonces solo esa rama).
                        if ((opcode == 2 || opcode == 4 || opcode == 6 || opcode == 14) && !conditional)
                            finished = true;
                    }
                }
                std::swap(out, body);

                // Cabecera y cuerpo de la función.
                if (!pixel)
                {
                    // El shader del juego va en una función aparte (<entrada>_run): las listas de
                    // rectángulos necesitan ejecutarlo para tres vértices y deducir el cuarto.
                    out << "static VSOut " << entry << "_run(uint index, device const uint* mem, constant float4* c,\n"
                        << "    constant uint* fc, constant DrawState& st)\n{\n";
                    out << R"MSL(    if (st.indexSize == 2)
    {
        uint address = st.indexBase + index * 2;
        uint word = load_word(mem, address & ~3u, st.indexEndian);
        index = (address & 2) ? (word >> 16) : (word & 0xFFFF);
    }
    else if (st.indexSize == 4)
    {
        index = load_word(mem, st.indexBase + index * 4, st.indexEndian);
    }
    index += st.indexOffset;

    float4 r[64];
    for (int i = 0; i < 64; i++) r[i] = float4(0);
    r[0].x = float(index);
    float4 o[16];
    for (int i = 0; i < 16; i++) o[i] = float4(0);
    float4 opos = float4(0, 0, 0, 1);
    float4 odiscard = float4(0);
    float ps = 0.0f;
    bool p = false;
    int a0 = 0;
)MSL";
                    out << body.str();
                    out << R"MSL(
    // Transformación de viewport de Xenos (PA_CL_VTE_CNTL) y paso a coordenadas de Metal.
    uint vte = st.vte;
    float w = opos.w;
    float3 v = opos.xyz;
    if (!(vte & 0x100)) v.xy /= w;
    if (!(vte & 0x200)) v.z /= w;
    float3 window;
    window.x = ((vte & 1) ? v.x * st.viewportScale[0] : v.x) + ((vte & 2) ? st.viewportOffset[0] : 0.0f);
    window.y = ((vte & 4) ? v.y * st.viewportScale[1] : v.y) + ((vte & 8) ? st.viewportOffset[1] : 0.0f);
    window.z = ((vte & 16) ? v.z * st.viewportScale[2] : v.z) + ((vte & 32) ? st.viewportOffset[2] : 0.0f);
    float wOut = (vte & 0x100) ? 1.0f : w;
    window.xy += float2(st.windowOffset[0], st.windowOffset[1]); // PA_SC_WINDOW_OFFSET (tiling)
    float2 clip = float2(window.x / st.targetSize[0] * 2.0f - 1.0f, 1.0f - window.y / st.targetSize[1] * 2.0f);

    VSOut result;
    result.position = float4(clip * wOut, window.z * wOut, wOut);
    result.i0 = o[0]; result.i1 = o[1]; result.i2 = o[2]; result.i3 = o[3];
    result.i4 = o[4]; result.i5 = o[5]; result.i6 = o[6]; result.i7 = o[7];
    result.i8 = o[8]; result.i9 = o[9]; result.i10 = o[10]; result.i11 = o[11];
    result.i12 = o[12]; result.i13 = o[13]; result.i14 = o[14]; result.i15 = o[15];
    return result;
}
)MSL";
                    out << "vertex VSOut " << entry << "(uint vid [[vertex_id]], device const uint* mem [[buffer(0)]],\n"
                        << "    constant float4* c [[buffer(1)]], constant uint* fc [[buffer(2)]], constant DrawState& st [[buffer(3)]])\n{\n";
                    out << R"MSL(    // Listas de quads: dos triángulos por quad (0 1 2, 0 2 3).
    if (st.primitive == 13)
    {
        const uint corners[6] = { 0, 1, 2, 0, 2, 3 };
        return )MSL" << entry << R"MSL(_run((vid / 6) * 4 + corners[vid % 6], mem, c, fc, st);
    }
    // Listas de rectángulos: 3 esquinas por rectángulo. El primer triángulo son las tres; el
    // segundo usa la cuarta, P + Q - R, donde R es la esquina en ángulo recto (la opuesta al
    // lado más largo), para la posición y para todos los interpolantes.
    if (st.primitive == 8)
    {
        uint rect = vid / 6, k = vid % 6;
        if (k < 3)
            return )MSL" << entry << R"MSL(_run(rect * 3 + k, mem, c, fc, st);
        VSOut v[3];
        for (uint i = 0; i < 3; i++)
            v[i] = )MSL" << entry << R"MSL(_run(rect * 3 + i, mem, c, fc, st);
        float2 p0 = v[0].position.xy / v[0].position.w, p1 = v[1].position.xy / v[1].position.w,
               p2 = v[2].position.xy / v[2].position.w;
        float d01 = distance_squared(p0, p1), d12 = distance_squared(p1, p2), d02 = distance_squared(p0, p2);
        uint r = (d12 >= d01 && d12 >= d02) ? 0u : ((d02 >= d01) ? 1u : 2u);
        uint a = (r == 0u) ? 1u : 0u, b = (r == 2u) ? 1u : 2u;
        if (k == 3) return v[a];
        if (k == 4) return v[b];
        VSOut result;
        result.position = v[a].position + v[b].position - v[r].position;
        result.i0 = v[a].i0 + v[b].i0 - v[r].i0; result.i1 = v[a].i1 + v[b].i1 - v[r].i1;
        result.i2 = v[a].i2 + v[b].i2 - v[r].i2; result.i3 = v[a].i3 + v[b].i3 - v[r].i3;
        result.i4 = v[a].i4 + v[b].i4 - v[r].i4; result.i5 = v[a].i5 + v[b].i5 - v[r].i5;
        result.i6 = v[a].i6 + v[b].i6 - v[r].i6; result.i7 = v[a].i7 + v[b].i7 - v[r].i7;
        result.i8 = v[a].i8 + v[b].i8 - v[r].i8; result.i9 = v[a].i9 + v[b].i9 - v[r].i9;
        result.i10 = v[a].i10 + v[b].i10 - v[r].i10; result.i11 = v[a].i11 + v[b].i11 - v[r].i11;
        result.i12 = v[a].i12 + v[b].i12 - v[r].i12; result.i13 = v[a].i13 + v[b].i13 - v[r].i13;
        result.i14 = v[a].i14 + v[b].i14 - v[r].i14; result.i15 = v[a].i15 + v[b].i15 - v[r].i15;
        return result;
    }
    return )MSL" << entry << R"MSL(_run(vid, mem, c, fc, st);
}
)MSL";
                }
                else
                {
                    out << "fragment float4 " << entry << "(VSOut in [[stage_in]], device const uint* mem [[buffer(0)]],\n"
                        << "    constant float4* c [[buffer(1)]], constant uint* fc [[buffer(2)]], constant DrawState& st [[buffer(3)]]";
                    for (uint32_t t : textures)
                        out << ",\n    texture2d<float> t" << t << " [[texture(" << t << ")]], sampler s" << t << " [[sampler(" << t << ")]]";
                    out << ")\n{\n";
                    out << R"MSL(    float4 r[64];
    for (int i = 0; i < 64; i++) r[i] = float4(0);
    r[0] = in.i0; r[1] = in.i1; r[2] = in.i2; r[3] = in.i3; r[4] = in.i4; r[5] = in.i5; r[6] = in.i6; r[7] = in.i7;
    r[8] = in.i8; r[9] = in.i9; r[10] = in.i10; r[11] = in.i11; r[12] = in.i12; r[13] = in.i13; r[14] = in.i14; r[15] = in.i15;
    float4 oc[4];
    for (int i = 0; i < 4; i++) oc[i] = float4(0);
    float4 odiscard = float4(0);
    float ps = 0.0f;
    bool p = false;
    int a0 = 0;
)MSL";
                    out << body.str();
                    out << "    return oc[0];\n}\n";
                }
                return true;
            }
        };
    }

    TranslatedShader TranslateShader(bool pixelShader, const std::vector<uint32_t>& code, uint64_t hash)
    {
        TranslatedShader result;
        Translator translator(pixelShader, code);
        std::string entry;
        result.ok = translator.Translate(hash, entry);
        result.entry = entry;
        result.source = std::string(kPrelude) + "\n" + translator.out.str();
        result.warnings = translator.warnings.str();
        result.textures.assign(translator.textures.begin(), translator.textures.end());
        return result;
    }
}
