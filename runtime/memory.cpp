#include "memory.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace memory
{
    uint8_t* gBase = nullptr;
    static std::mutex gLock;
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

        // Memoria física de la Xbox (512 MB): la misma memoria se ve en 0xA0000000 (páginas de
        // 64 KB) y en 0xC0000000 (páginas de 16 MB). D3D usa las dos vistas, así que deben ser la
        // misma memoria: se mapea dos veces un bloque compartido.
        // La vista 0xE0000000 no se refleja: en la consola tiene un desplazamiento de 4 KB, que
        // no se puede reproducir con las páginas de 16 KB de Apple Silicon.
        char name[64];
        std::snprintf(name, sizeof(name), "/prostreet-%d", getpid());
        const int shared = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        shm_unlink(name);
        if (shared < 0 || ftruncate(shared, kPhysicalSize) != 0)
        {
            std::perror("memoria física compartida");
            std::exit(1);
        }
        for (uint32_t view : { kPhysicalBase, kPhysicalMirror })
        {
            if (mmap(gBase + view, kPhysicalSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, shared, 0) == MAP_FAILED)
            {
                std::perror("vista de la memoria física");
                std::exit(1);
            }
        }
        close(shared);

        // Los primeros 64 KB quedan sin acceso, para detectar punteros nulos del juego.
        mprotect(gBase, 0x10000, PROT_NONE);
        return gBase;
    }

    uint32_t Used(uint32_t regionBase)
    {
        std::lock_guard lock(gLock);
        auto it = gNext.find(regionBase);
        return it == gNext.end() ? 0 : it->second - regionBase;
    }

    uint32_t Allocate(uint32_t regionBase, uint32_t regionEnd, uint32_t size, uint32_t alignment)
    {
        std::lock_guard lock(gLock);
        uint32_t& next = gNext.try_emplace(regionBase, regionBase).first->second;
        const uint64_t start = (uint64_t(next) + alignment - 1) & ~uint64_t(alignment - 1);
        if (start + size > regionEnd)
            return 0;
        next = uint32_t(start + size);
        return uint32_t(start);
    }
}
