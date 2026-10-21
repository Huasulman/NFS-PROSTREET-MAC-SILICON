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
    uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t log2Bpp);
}
