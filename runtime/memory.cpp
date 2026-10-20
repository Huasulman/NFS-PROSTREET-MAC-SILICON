#include "memory.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <sys/mman.h>

namespace memory
{
    static uint8_t* gBase = nullptr;
    static std::map<uint32_t, uint32_t> gNext; // inicio de región -> siguiente dirección libre

    uint8_t* Reserve()
    {
        // MAP_NORESERVE: las páginas solo ocupan memoria real cuando se tocan.
        void* p = mmap(nullptr, 0x100000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED)
        {
            std::perror("mmap de 4 GB");
            std::exit(1);
        }
        gBase = static_cast<uint8_t*>(p);

        // Los primeros 64 KB quedan sin acceso, para detectar punteros nulos del juego.
        mprotect(gBase, 0x10000, PROT_NONE);
        return gBase;
    }

    uint8_t* Base() { return gBase; }

    uint32_t Used(uint32_t regionBase)
    {
        auto it = gNext.find(regionBase);
        return it == gNext.end() ? 0 : it->second - regionBase;
    }

    uint32_t Allocate(uint32_t regionBase, uint32_t regionEnd, uint32_t size, uint32_t alignment)
    {
        uint32_t& next = gNext.try_emplace(regionBase, regionBase).first->second;
        const uint64_t start = (uint64_t(next) + alignment - 1) & ~uint64_t(alignment - 1);
        if (start + size > regionEnd)
            return 0;
        next = uint32_t(start + size);
        return uint32_t(start);
    }
}
