#pragma once

#include <cstdint>

#include "command_processor.h"
#include "presenter.h"

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
        uint64_t constantsVersion;  // cambia cuando cambia alguna constante de shader
    };

    // Devuelven false si el renderer no está disponible (sin GPU Metal).
    bool RendererDraw(const DrawCall& draw);
    void RendererResolve(const uint32_t* registers);

    // Lee el front buffer de la copia en la GPU de su resolve (píxeles BGRA sin mosaico), al
    // tamaño al que se dibuja (con la escala de resolución). Devuelve false si no hay ninguna:
    // entonces hay que leerlo de la memoria del juego.
    bool RendererReadFrame(uint32_t physical, uint32_t width, uint32_t height, Frame& frame);

    // Escala de resolución (1 = la de la consola, 1.5 = 1920x1080...): todo lo que se dibuja y se
    // copia con resolves va a ese tamaño. Se puede cambiar en cualquier momento; se aplica al
    // presentar el fotograma siguiente. Al empezar: PROSTREET_SCALE (por defecto 1).
    void SetResolutionScale(float scale);
    float ResolutionScale();

    // Antialiasing (MSAA 4x en las superficies que lo piden). Como la escala: se aplica al presentar.
    // Al empezar: activado salvo con PROSTREET_NO_MSAA.
    void SetAntialiasing(bool enabled);
    bool Antialiasing();
}
