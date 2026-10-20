#pragma once

#include <cstddef>
#include <cstdint>

// Memoria del invitado: 4 GB contiguos en el anfitrión. Una dirección del juego `a`
// corresponde a `base + a`. Todo lo que hay dentro está en big-endian.
namespace memory
{
    // Regiones del mapa de memoria de la Xbox 360 que usa el runtime.
    constexpr uint32_t kHeapBase = 0x40000000;     // memoria virtual (NtAllocateVirtualMemory)
    constexpr uint32_t kHeapEnd = 0x70000000;
    constexpr uint32_t kKernelBase = 0x70000000;   // estructuras del runtime: pilas, KPCR, variables
    constexpr uint32_t kKernelEnd = 0x80000000;
    constexpr uint32_t kPhysicalBase = 0xA0000000; // memoria física (MmAllocatePhysicalMemoryEx)
    constexpr uint32_t kPhysicalEnd = 0xC0000000;

    uint8_t* Reserve();
    uint8_t* Base();

    // Reserva memoria del invitado dentro de [regionBase, regionEnd); devuelve la dirección
    // del invitado (0 si no queda espacio). La memoria nueva está a cero.
    uint32_t Allocate(uint32_t regionBase, uint32_t regionEnd, uint32_t size, uint32_t alignment = 0x1000);
    // Bytes ya reservados en la región que empieza en regionBase.
    uint32_t Used(uint32_t regionBase);

    inline uint32_t AllocateKernel(uint32_t size, uint32_t alignment = 0x10)
    {
        return Allocate(kKernelBase, kKernelEnd, size, alignment);
    }

    inline void* Translate(uint32_t address) { return Base() + address; }
    inline bool Contains(const void* host)
    {
        const auto* p = static_cast<const uint8_t*>(host);
        return p >= Base() && p < Base() + 0x100000000ull;
    }

    inline void Write32(uint32_t address, uint32_t value)
    {
        *static_cast<uint32_t*>(Translate(address)) = __builtin_bswap32(value);
    }
    inline uint32_t Read32(uint32_t address)
    {
        return __builtin_bswap32(*static_cast<const uint32_t*>(Translate(address)));
    }
    inline void Write16(uint32_t address, uint16_t value)
    {
        *static_cast<uint16_t*>(Translate(address)) = __builtin_bswap16(value);
    }
}
