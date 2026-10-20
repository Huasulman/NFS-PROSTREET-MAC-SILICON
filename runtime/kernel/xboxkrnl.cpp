// Funciones de xboxkrnl.exe implementadas por el runtime. Reemplazan a las versiones débiles
// de runtime/generated/imports.cpp. Convención: argumentos en r3..r10, resultado en r3.

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <strings.h>
#include <fstream>
#include <vector>

#include "imports.h"
#include "xex.h"
#include "../memory.h"

namespace
{
    constexpr uint32_t kStatusSuccess = 0x00000000;
    constexpr uint32_t kStatusNoMemory = 0xC0000017;
    constexpr uint32_t kMemCommit = 0x00001000;
    constexpr uint32_t kMemLargePages = 0x20000000; // páginas de 64 KB
}

// uint32_t RtlImageXexHeaderField(void* xexHeaderBase, uint32_t imageField)
// Devuelve el valor de un campo de la cabecera opcional del XEX: el propio valor para los
// campos pequeños, o la dirección de sus datos para los demás. 0 si no existe.
PPC_FUNC(__imp__RtlImageXexHeaderField)
{
    (void)base;
    ctx.r3.u64 = xex::HeaderField(ctx.r4.u32);
}

// NTSTATUS NtAllocateVirtualMemory(uint32_t* baseAddress, uint32_t* regionSize,
//                                  uint32_t allocationType, uint32_t protect, uint32_t unknown)
PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    (void)base;
    const uint32_t baseAddressPtr = ctx.r3.u32;
    const uint32_t regionSizePtr = ctx.r4.u32;
    const uint32_t type = ctx.r5.u32;

    const uint32_t pageSize = (type & kMemLargePages) ? 0x10000 : 0x1000;
    const uint32_t size = (memory::Read32(regionSizePtr) + pageSize - 1) & ~(pageSize - 1);
    uint32_t address = memory::Read32(baseAddressPtr);

    if (address == 0)
    {
        // Reserva nueva. La memoria del invitado ya existe entera, así que basta con elegir sitio.
        address = memory::Allocate(memory::kHeapBase, memory::kHeapEnd, size, pageSize);
        if (address == 0)
        {
            ctx.r3.u64 = kStatusNoMemory;
            return;
        }
    }
    else
    {
        // Confirmar (MEM_COMMIT) dentro de una reserva previa: no hay nada que hacer.
        address &= ~(pageSize - 1);
    }

    memory::Write32(baseAddressPtr, address);
    memory::Write32(regionSizePtr, size);
    if (std::getenv("PROSTREET_TRACE"))
        std::fprintf(stderr, "[kernel] NtAllocateVirtualMemory 0x%08X + 0x%X (%s)\n", address, size,
            (type & kMemCommit) ? "commit" : "reserve");
    ctx.r3.u64 = kStatusSuccess;
}

// --- Procesos -----------------------------------------------------------------------------

// uint32_t KeGetCurrentProcessType(): 1 = proceso de un título (juego).
PPC_FUNC(__imp__KeGetCurrentProcessType)
{
    (void)base;
    ctx.r3.u64 = 1;
}

// --- TLS dinámico (KeTlsAlloc / KeTlsGetValue / ...) -----------------------------------------
// Los slots de cada hilo están en la memoria del invitado, justo antes de su TLS estática:
// [r13 + 0] apunta a la TLS estática, y los 64 slots ocupan los 0x100 bytes anteriores.

namespace
{
    constexpr uint32_t kTlsSlots = 64;
    constexpr uint32_t kTlsOutOfIndexes = 0xFFFFFFFF;
    uint64_t gTlsUsed = 0; // bit i = slot i reservado

    uint32_t TlsSlotAddress(const PPCContext& ctx, uint32_t index)
    {
        return memory::Read32(ctx.r13.u32) - kTlsSlots * 4 + index * 4;
    }
}

// uint32_t KeTlsAlloc()
PPC_FUNC(__imp__KeTlsAlloc)
{
    (void)base;
    for (uint32_t i = 0; i < kTlsSlots; i++)
    {
        if (!(gTlsUsed & (1ull << i)))
        {
            gTlsUsed |= 1ull << i;
            memory::Write32(TlsSlotAddress(ctx, i), 0);
            ctx.r3.u64 = i;
            return;
        }
    }
    ctx.r3.u64 = kTlsOutOfIndexes;
}

// BOOL KeTlsFree(uint32_t index)
PPC_FUNC(__imp__KeTlsFree)
{
    (void)base;
    const uint32_t index = ctx.r3.u32;
    const bool valid = index < kTlsSlots && (gTlsUsed & (1ull << index));
    if (valid)
        gTlsUsed &= ~(1ull << index);
    ctx.r3.u64 = valid;
}

// uint32_t KeTlsGetValue(uint32_t index)
PPC_FUNC(__imp__KeTlsGetValue)
{
    (void)base;
    const uint32_t index = ctx.r3.u32;
    ctx.r3.u64 = index < kTlsSlots ? memory::Read32(TlsSlotAddress(ctx, index)) : 0;
}

// BOOL KeTlsSetValue(uint32_t index, uint32_t value)
PPC_FUNC(__imp__KeTlsSetValue)
{
    (void)base;
    const uint32_t index = ctx.r3.u32;
    if (index >= kTlsSlots)
    {
        ctx.r3.u64 = 0;
        return;
    }
    memory::Write32(TlsSlotAddress(ctx, index), ctx.r4.u32);
    ctx.r3.u64 = 1;
}

// --- Memoria física y estadísticas -----------------------------------------------------------

namespace
{
    constexpr uint32_t kTotalMemory = 512 * 1024 * 1024;
    constexpr uint32_t kSystemReserved = 32 * 1024 * 1024; // lo que se queda el sistema (aprox.)
    constexpr uint32_t kStatusInvalidParameter = 0xC000000D;
}

// void* MmAllocatePhysicalMemoryEx(uint32_t flags, uint32_t size, uint32_t protect,
//                                  uint32_t minAddress, uint32_t maxAddress, uint32_t alignment)
PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx)
{
    (void)base;
    const uint32_t size = ctx.r4.u32;
    const uint32_t protect = ctx.r5.u32;
    const uint32_t pageSize = (protect & kMemLargePages) ? 0x10000 : 0x1000;
    const uint32_t alignment = std::max(pageSize, ctx.r8.u32);
    const uint32_t rounded = (size + pageSize - 1) & ~(pageSize - 1);

    const uint32_t address = memory::Allocate(memory::kPhysicalBase, memory::kPhysicalEnd, rounded, alignment);
    if (std::getenv("PROSTREET_TRACE"))
        std::fprintf(stderr, "[kernel] MmAllocatePhysicalMemoryEx 0x%08X + 0x%X\n", address, rounded);
    ctx.r3.u64 = address;
}

// NTSTATUS MmQueryStatistics(MM_STATISTICS* stats). Estructura de 0x68 bytes (según Xenia).
PPC_FUNC(__imp__MmQueryStatistics)
{
    (void)base;
    const uint32_t stats = ctx.r3.u32;
    if (memory::Read32(stats) != 0x68)
    {
        ctx.r3.u64 = kStatusInvalidParameter;
        return;
    }

    const uint32_t used = memory::Used(memory::kPhysicalBase) + memory::Used(memory::kHeapBase);
    const uint32_t available = used < kTotalMemory - kSystemReserved ? kTotalMemory - kSystemReserved - used : 0;
    std::memset(memory::Translate(stats + 4), 0, 0x64);
    memory::Write32(stats + 0x04, kTotalMemory / 0x1000);              // TotalPhysicalPages
    memory::Write32(stats + 0x08, kSystemReserved / 0x1000);           // KernelPages
    memory::Write32(stats + 0x0C, available / 0x1000);                 // TitleAvailablePages
    memory::Write32(stats + 0x10, memory::kHeapEnd - memory::kHeapBase); // TitleTotalVirtualMemoryBytes
    memory::Write32(stats + 0x14, memory::Used(memory::kHeapBase));    // TitleReservedVirtualMemoryBytes
    memory::Write32(stats + 0x18, memory::Used(memory::kPhysicalBase) / 0x1000); // TitlePhysicalPages
    memory::Write32(stats + 0x38, available / 0x1000);                 // SystemAvailablePages
    ctx.r3.u64 = kStatusSuccess;
}

// --- Módulos ---------------------------------------------------------------------------------

namespace
{
    constexpr uint32_t kStatusDllNotFound = 0xC0000135;
    constexpr uint32_t kStatusEntrypointNotFound = 0xC0000139;
}

// NTSTATUS XexGetModuleHandle(const char* name, uint32_t* handle)
PPC_FUNC(__imp__XexGetModuleHandle)
{
    (void)base;
    const char* name = static_cast<const char*>(memory::Translate(ctx.r3.u32));
    static const char* const kModules[] = { "xam.xex", "xboxkrnl.exe" };
    for (uint32_t i = 0; i < 2; i++)
    {
        if (strcasecmp(name, kModules[i]) == 0)
        {
            memory::Write32(ctx.r4.u32, 0xF0000000 | i); // handle ficticio; solo sirve para XexGetProcedureAddress
            ctx.r3.u64 = kStatusSuccess;
            return;
        }
    }
    memory::Write32(ctx.r4.u32, 0);
    ctx.r3.u64 = kStatusDllNotFound;
}

// NTSTATUS XexGetProcedureAddress(uint32_t handle, uint32_t ordinal, void** address)
// De momento no se exporta nada dinámicamente: el juego usa estas funciones para lo opcional.
PPC_FUNC(__imp__XexGetProcedureAddress)
{
    (void)base;
    if (std::getenv("PROSTREET_TRACE"))
        std::fprintf(stderr, "[kernel] XexGetProcedureAddress(%s, 0x%X): no disponible\n",
            ctx.r3.u32 == 0xF0000000 ? "xam.xex" : "xboxkrnl.exe", ctx.r4.u32);
    memory::Write32(ctx.r5.u32, 0);
    ctx.r3.u64 = kStatusEntrypointNotFound;
}
