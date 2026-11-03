#include "presenter.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "../memory.h"
#include "renderer.h"

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

    void PresentFrame(uint32_t frontBufferGuest, uint32_t width, uint32_t height)
    {
        if (frontBufferGuest == 0 || width == 0 || height == 0 || width > 4096 || height > 4096)
            return;

        Frame frame;
        const auto* source = static_cast<const uint8_t*>(memory::Translate(frontBufferGuest));
        if (gLinear || !RendererReadFrame(frontBufferGuest, width, height, frame))
        {
            frame.width = width;
            frame.height = height;
            frame.pixels.resize(size_t(width) * height);
            for (uint32_t y = 0; y < height; y++)
            {
                for (uint32_t x = 0; x < width; x++)
                {
                    const uint32_t offset = gLinear ? (y * width + x) * 4 : TiledOffset(x, y, width, 2);
                    // 8_8_8_8 como palabra little-endian 0xAARRGGBB (como lo escribe el resolve, sin
                    // intercambio de bytes): es directamente un píxel BGRA del Mac.
                    uint32_t value;
                    std::memcpy(&value, source + offset, 4);
                    frame.pixels[size_t(y) * width + x] = value | 0xFF000000u;
                }
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
