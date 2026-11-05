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
    constexpr uint32_t kPhysicalMirror = 0xC0000000; // la misma memoria física, otra vista
    constexpr uint32_t kPhysicalSize = 0x20000000;   // 512 MB

    uint8_t* Reserve();
    extern uint8_t* gBase;
    inline uint8_t* Base() { return gBase; }

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

    // Vigilancia de escrituras en la memoria física (para saber si una textura ha cambiado sin
    // mirar su memoria): Watch deja de solo lectura las páginas (16 KB) de [physical, physical + size)
    // en las dos vistas; la primera escritura en una de ellas la vuelve a abrir y la marca como
    // cambiada (HandleWriteFault, desde el manejador de SIGBUS/SIGSEGV). Las direcciones son
    // físicas (0 .. 512 MB).
    void Watch(uint32_t physical, uint32_t size);
    // ¿Siguen vigiladas (sin escrituras) todas las páginas del rango?
    bool Unchanged(uint32_t physical, uint32_t size);
    // Antes de que el sistema escriba en la memoria del juego (p. ej. pread): con la página
    // protegida fallaría sin señal. Admite cualquier dirección del invitado.
    void PrepareHostWrite(uint32_t guest, uint32_t size);
    // Para el manejador de señales: si la dirección es de una página vigilada, la abre y devuelve true.
    bool HandleWriteFault(const void* host);
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
    inline uint8_t Read8(uint32_t address)
    {
        return *static_cast<const uint8_t*>(Translate(address));
    }
    inline void Write8(uint32_t address, uint8_t value)
    {
        *static_cast<uint8_t*>(Translate(address)) = value;
    }
}
