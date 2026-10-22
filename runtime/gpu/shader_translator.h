#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Traductor de shaders de Xenos (microcódigo) a MSL (Metal Shading Language).
//
// El formato del microcódigo sigue a Xenia (ucode.h) y freedreno (instr-a2xx.h), y es el mismo
// que lee tools/xenos_disasm.py. Interfaz del código generado (ver shader_prelude en el .cpp):
//   buffer(0): memoria física del juego (512 MB) como palabras de 32 bits
//   buffer(1): constantes ALU del shader (c0..c255, float4)
//   buffer(2): constantes de fetch (registros 0x4800.., 32 x 6 palabras)
//   buffer(3): DrawState (índices, viewport, booleanas...)
//   texture(N) / sampler(N): textura de la constante de fetch N
namespace gpu
{
    struct TranslatedShader
    {
        bool ok = false;
        std::string source;           // MSL (incluye el preludio común)
        std::string entry;            // nombre de la función
        std::string warnings;         // instrucciones no soportadas, etc.
        std::vector<uint32_t> textures; // constantes de fetch de textura que usa
    };

    TranslatedShader TranslateShader(bool pixelShader, const std::vector<uint32_t>& code, uint64_t hash);

    // Estado de cada dibujo que necesitan los shaders (misma estructura en el preludio MSL).
    struct DrawState
    {
        uint32_t primitive;     // 4 = lista de triángulos, 13 = lista de quads...
        uint32_t indexSize;     // 0 = sin índices (automáticos), 2 = 16 bits, 4 = 32 bits
        uint32_t indexBase;     // dirección física del búfer de índices
        uint32_t indexEndian;
        uint32_t indexOffset;   // VGT_INDX_OFFSET
        uint32_t vte;           // PA_CL_VTE_CNTL
        float viewportScale[4]; // PA_CL_VPORT_{X,Y,Z}SCALE
        float viewportOffset[4];
        float targetSize[2];    // tamaño del destino de dibujo, en píxeles
        uint32_t padding[2];
        uint32_t bools[8];      // constantes booleanas (256 bits)
    };
}
