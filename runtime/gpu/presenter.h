#pragma once

#include <cstdint>
#include <vector>

// Último fotograma presentado por el juego (VdSwap), ya convertido a una imagen normal del Mac:
// píxeles BGRA de 8 bits, fila a fila. La ventana de Metal y la captura a PNG lo leen de aquí.
namespace gpu
{
    struct Frame
    {
        uint32_t width = 0, height = 0;
        uint64_t number = 0;          // número de fotograma (0 = todavía ninguno)
        std::vector<uint32_t> pixels; // BGRA8, width * height
    };

    // Llamado por el procesador de comandos al presentar: lee el front buffer de la memoria
    // del invitado (formato 8_8_8_8 en mosaico de Xenos) y lo deja como último fotograma.
    void PresentFrame(uint32_t frontBufferGuest, uint32_t width, uint32_t height);

    // Copia el último fotograma si es más nuevo que `lastNumber`. Devuelve false si no lo es.
    bool CopyLatestFrame(uint64_t lastNumber, Frame& out);

    // Dirección (en bytes) del píxel (x, y) en una superficie 2D en mosaico de Xenos.
    // `pitch` en píxeles (múltiplo de 32), `log2Bpp` = log2(bytes por píxel).
    // En línea: se llama por cada bloque al quitar el mosaico de las texturas.
    inline uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t log2Bpp)
    {
        // XGAddress2DTiledOffset, como en Xenia (texture_address): bloques de 32x32 píxeles
        // con un patrón interno de micro-mosaicos.
        const uint32_t alignedPitch = (pitch + 31) & ~31u;
        const uint32_t macro = ((y >> 5) * (alignedPitch >> 5)) << (log2Bpp + 7);
        const uint32_t micro = ((y & 6) << 2) << log2Bpp;
        const uint32_t outer = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2Bpp)) + ((y & 1) << 4);

        const uint32_t macroX = (x >> 5) << (log2Bpp + 7);
        const uint32_t microX = (x & 7) << log2Bpp;
        const uint32_t offset = outer + macroX + ((microX & ~0xFu) << 1) + (microX & 0xF);
        return ((offset & ~0x1FFu) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((y & 16) << 7) +
               (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
    }
}
