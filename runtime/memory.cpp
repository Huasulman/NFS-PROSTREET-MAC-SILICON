#include "memory.h"

#include <algorithm>
#include <atomic>
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

    namespace
    {
        constexpr uint32_t kWatchPageShift = 14; // páginas de 16 KB (Apple Silicon)
        constexpr uint32_t kWatchPages = kPhysicalSize >> kWatchPageShift;
        // 1 = vigilada (de solo lectura y sin escrituras desde entonces).
        std::atomic<uint8_t> gWatched[kWatchPages];
        // Cerrojo de espera activa: lo toma también el manejador de señales (no puede usar un mutex).
        // Quien lo tiene solo cambia protecciones, nunca escribe en la memoria vigilada.
        std::atomic_flag gWatchLock = ATOMIC_FLAG_INIT;

        struct WatchGuard
        {
            WatchGuard() { while (gWatchLock.test_and_set(std::memory_order_acquire)) {} }
            ~WatchGuard() { gWatchLock.clear(std::memory_order_release); }
        };

        void Protect(uint32_t firstPage, uint32_t pages, int protection)
        {
            for (uint32_t view : { kPhysicalBase, kPhysicalMirror })
                mprotect(gBase + view + (size_t(firstPage) << kWatchPageShift), size_t(pages) << kWatchPageShift, protection);
        }
    }

    void Watch(uint32_t physical, uint32_t size)
    {
        if (size == 0 || physical >= kPhysicalSize)
            return;
        const uint32_t first = physical >> kWatchPageShift;
        const uint32_t last = std::min<uint64_t>((uint64_t(physical) + size - 1) >> kWatchPageShift, kWatchPages - 1);
        WatchGuard guard;
        // Por tramos de páginas aún sin vigilar. Se marcan antes de protegerlas: una escritura entre
        // medias no avisa, pero quien vigila lee la memoria después de esto y la ve.
        for (uint32_t page = first; page <= last;)
        {
            if (gWatched[page].load(std::memory_order_relaxed))
            {
                page++;
                continue;
            }
            uint32_t end = page;
            while (end <= last && !gWatched[end].load(std::memory_order_relaxed))
                gWatched[end++].store(1, std::memory_order_relaxed);
            Protect(page, end - page, PROT_READ);
            page = end;
        }
    }

    bool Unchanged(uint32_t physical, uint32_t size)
    {
        if (size == 0 || physical >= kPhysicalSize)
            return false;
        const uint32_t first = physical >> kWatchPageShift;
        const uint32_t last = std::min<uint64_t>((uint64_t(physical) + size - 1) >> kWatchPageShift, kWatchPages - 1);
        for (uint32_t page = first; page <= last; page++)
            if (!gWatched[page].load(std::memory_order_acquire))
                return false;
        return true;
    }

    namespace
    {
        void Release(uint32_t page)
        {
            if (gWatched[page].load(std::memory_order_relaxed))
            {
                gWatched[page].store(0, std::memory_order_release);
                Protect(page, 1, PROT_READ | PROT_WRITE);
            }
        }
    }

    void PrepareHostWrite(uint32_t guest, uint32_t size)
    {
        if (size == 0)
            return;
        for (uint32_t view : { kPhysicalBase, kPhysicalMirror })
        {
            const uint64_t start = std::max<uint64_t>(guest, view), end = std::min<uint64_t>(uint64_t(guest) + size, uint64_t(view) + kPhysicalSize);
            if (start >= end)
                continue;
            WatchGuard guard;
            for (uint64_t page = (start - view) >> kWatchPageShift; page <= (end - 1 - view) >> kWatchPageShift; page++)
                Release(uint32_t(page));
        }
    }

    bool HandleWriteFault(const void* host)
    {
        if (!gBase || !Contains(host))
            return false;
        const uint64_t guest = static_cast<const uint8_t*>(host) - gBase;
        for (uint32_t view : { kPhysicalBase, kPhysicalMirror })
        {
            if (guest < view || guest >= uint64_t(view) + kPhysicalSize)
                continue;
            const uint32_t page = uint32_t((guest - view) >> kWatchPageShift);
            WatchGuard guard;
            // Si otro hilo la abrió justo antes, ya admite escritura: basta con reintentar (la memoria
            // física solo se protege para vigilarla).
            Release(page);
            return true;
        }
        return false;
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
