#pragma once

#include <cstdint>

#include "command_processor.h"

// Renderer en Metal: ejecuta los dibujos y resolves que le pasa el procesador de comandos.
//
// La EDRAM se modela como una textura de Metal en la que se dibuja; un resolve la copia a la
// memoria del juego (en el formato en mosaico de destino), y así el front buffer llega a la
// ventana por gpu/presenter. Los vértices e índices los leen los shaders directamente de la
// memoria física del juego, envuelta como búfer de Metal sin copia.
namespace gpu
{
    struct DrawCall
    {
        const uint32_t* registers;  // archivo de registros de la GPU
        const Shader* vertexShader;
        const Shader* pixelShader;
        uint32_t primitive;         // VGT_DRAW_INITIATOR.prim_type
        uint32_t count;             // número de índices / vértices
        uint32_t indexSize;         // 0 = automáticos, 2 o 4 bytes
        uint32_t indexBase;         // dirección física
        uint32_t indexEndian;
        bool depthOnly;             // RB_MODECONTROL = 5: solo profundidad, sin color
    };

    // Devuelven false si el renderer no está disponible (sin GPU Metal).
    bool RendererDraw(const DrawCall& draw);
    void RendererResolve(const uint32_t* registers);
}
