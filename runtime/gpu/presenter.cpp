#include "presenter.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include "../memory.h"

namespace gpu
{
    namespace
    {
        std::mutex gLock;
        Frame gLatest;

        // Variables de entorno: PROSTREET_DUMP=carpeta guarda un PNG cada PROSTREET_DUMP_EVERY
        // fotogramas (por defecto 60). PROSTREET_LINEAR_FRONTBUFFER=1 lee el front buffer sin mosaico.
        const char* gDumpDirectory = std::getenv("PROSTREET_DUMP");
        const uint64_t gDumpEvery = std::getenv("PROSTREET_DUMP_EVERY") ? std::strtoull(std::getenv("PROSTREET_DUMP_EVERY"), nullptr, 10) : 60;
        const bool gLinear = std::getenv("PROSTREET_LINEAR_FRONTBUFFER") != nullptr;
    }

    // Implementada en platform/window.mm (necesita ImageIO).
    void SavePng(const std::string& path, const Frame& frame);

    uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t log2Bpp)
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

    void PresentFrame(uint32_t frontBufferGuest, uint32_t width, uint32_t height)
    {
        if (frontBufferGuest == 0 || width == 0 || height == 0 || width > 4096 || height > 4096)
            return;

        Frame frame;
        frame.width = width;
        frame.height = height;
        frame.pixels.resize(size_t(width) * height);
        const auto* source = static_cast<const uint8_t*>(memory::Translate(frontBufferGuest));
        for (uint32_t y = 0; y < height; y++)
        {
            for (uint32_t x = 0; x < width; x++)
            {
                const uint32_t offset = gLinear ? (y * width + x) * 4 : TiledOffset(x, y, width, 2);
                // 8_8_8_8 en memoria como A,R,G,B (big-endian): se pasa a BGRA del Mac.
                const uint8_t* p = source + offset;
                frame.pixels[size_t(y) * width + x] = uint32_t(p[3]) | (uint32_t(p[2]) << 8) | (uint32_t(p[1]) << 16) | 0xFF000000u;
            }
        }

        {
            std::lock_guard lock(gLock);
            frame.number = gLatest.number + 1;
            gLatest = std::move(frame);
        }

        if (gDumpDirectory && gLatest.number % gDumpEvery == 1)
        {
            Frame copy;
            CopyLatestFrame(0, copy);
            char name[64];
            std::snprintf(name, sizeof(name), "/frame_%06llu.png", (unsigned long long)copy.number);
            SavePng(std::string(gDumpDirectory) + name, copy);
        }
    }

    bool CopyLatestFrame(uint64_t lastNumber, Frame& out)
    {
        std::lock_guard lock(gLock);
        if (gLatest.number == 0 || gLatest.number == lastNumber)
            return false;
        out = gLatest;
        return true;
    }
}
