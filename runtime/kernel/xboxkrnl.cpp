// Funciones de xboxkrnl.exe implementadas por el runtime. Reemplazan a las versiones débiles
// de runtime/generated/imports.cpp. Convención: argumentos en r3..r10, resultado en r3.

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <string>
#include <strings.h>
#include <fstream>
#include <mutex>
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
    std::mutex gTlsLock;
    uint64_t gTlsUsed = 0; // bit i = slot i reservado (protegido por gTlsLock)

    uint32_t TlsSlotAddress(const PPCContext& ctx, uint32_t index)
    {
        return memory::Read32(ctx.r13.u32) - kTlsSlots * 4 + index * 4;
    }
}

// uint32_t KeTlsAlloc()
PPC_FUNC(__imp__KeTlsAlloc)
{
    (void)base;
    std::lock_guard lock(gTlsLock);
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
    std::lock_guard lock(gTlsLock);
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

// --- Cadenas y excepciones -------------------------------------------------------------------

// void RtlInitAnsiString(ANSI_STRING* destination, const char* source)
// ANSI_STRING: uint16_t Length, uint16_t MaximumLength, char* Buffer.
PPC_FUNC(__imp__RtlInitAnsiString)
{
    (void)base;
    const uint32_t destination = ctx.r3.u32, source = ctx.r4.u32;
    const size_t length = source ? std::strlen(static_cast<const char*>(memory::Translate(source))) : 0;
    memory::Write16(destination + 0, uint16_t(length));
    memory::Write16(destination + 2, uint16_t(source ? length + 1 : 0));
    memory::Write32(destination + 4, source);
}

// void RtlRaiseException(EXCEPTION_RECORD* record)
// El código recompilado no admite excepciones. La única que se trata es el truco de Visual Studio
// para nombrar hilos (0x406D1388), que se ignora tras mostrar el nombre.
PPC_FUNC(__imp__RtlRaiseException)
{
    (void)base;
    constexpr uint32_t kSetThreadName = 0x406D1388;
    const uint32_t record = ctx.r3.u32;
    const uint32_t code = memory::Read32(record);
    if (code == kSetThreadName)
    {
        // ExceptionInformation (+0x14): tipo 0x1000, nombre, id del hilo (-1 = el actual).
        const uint32_t name = memory::Read32(record + 0x18);
        const uint32_t id = memory::Read32(record + 0x1C);
        std::fprintf(stderr, "[kernel] nombre de hilo: %s \"%s\"\n",
            id == 0xFFFFFFFF ? "(actual)" : std::to_string(id).c_str(), static_cast<const char*>(memory::Translate(name)));
        return;
    }
    std::fprintf(stderr, "[kernel] RtlRaiseException 0x%08X en 0x%08X: no soportada, se ignora\n",
        code, memory::Read32(record + 0x0C));
}

// void RtlFillMemoryUlong(void* destination, uint32_t length, uint32_t pattern)
PPC_FUNC(__imp__RtlFillMemoryUlong)
{
    (void)base;
    const uint32_t destination = ctx.r3.u32, length = ctx.r4.u32, pattern = ctx.r5.u32;
    for (uint32_t offset = 0; offset + 4 <= length; offset += 4)
        memory::Write32(destination + offset, pattern);
}

// --- Configuración de la consola -------------------------------------------------------------

uint32_t ConsoleLanguage()
{
    const char* language = std::getenv("PROSTREET_LANGUAGE");
    return language ? std::strtoul(language, nullptr, 10) : 5;
}

// NTSTATUS ExGetXConfigSetting(uint16_t category, uint16_t setting, void* buffer,
//                              uint16_t bufferSize, uint16_t* requiredSize)
// Solo los ajustes de usuario (categoría 3) que pide el juego, con los valores de Xenia.
PPC_FUNC(__imp__ExGetXConfigSetting)
{
    (void)base;
    const uint32_t category = ctx.r3.u32 & 0xFFFF, setting = ctx.r4.u32 & 0xFFFF;
    const uint32_t buffer = ctx.r5.u32, bufferSize = ctx.r6.u32 & 0xFFFF, requiredSize = ctx.r7.u32;

    uint32_t value;
    if (category == 3 && setting == 0x01)       // XCONFIG_USER_TIME_ZONE_BIAS: minutos (UTC - local), los del Mac
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        value = uint32_t(int32_t(-local.tm_gmtoff / 60));
    }
    else if (category == 3 && (setting == 0x02 || setting == 0x03)) // nombres de zona (estándar / verano), 4 caracteres
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        char name[4] = {};
        std::strncpy(name, local.tm_zone ? local.tm_zone : "UTC", sizeof(name));
        value = (uint32_t(uint8_t(name[0])) << 24) | (uint32_t(uint8_t(name[1])) << 16) |
                (uint32_t(uint8_t(name[2])) << 8) | uint8_t(name[3]); // Write32 los deja en orden
    }
    else if (category == 3 && setting >= 0x04 && setting <= 0x07) // fechas y ajustes del horario de verano:
        value = 0;                                                // el desplazamiento ya incluye el actual
    else if (category == 3 && setting == 0x09)  // XCONFIG_USER_LANGUAGE
        value = ConsoleLanguage();
    else if (category == 3 && setting == 0x0A)  // XCONFIG_USER_VIDEO_FLAGS: panorámico
        value = 0x00040000;
    else if (category == 3 && setting == 0x0C)  // XCONFIG_USER_RETAIL_FLAGS
        value = 0x40;
    else
    {
        std::fprintf(stderr, "[kernel] ExGetXConfigSetting(%u, 0x%X): ajuste no soportado\n", category, setting);
        ctx.r3.u64 = 0xC000000D; // STATUS_INVALID_PARAMETER
        return;
    }

    if (requiredSize)
        memory::Write16(requiredSize, 4);
    if (bufferSize < 4)
    {
        ctx.r3.u64 = 0xC0000023; // STATUS_BUFFER_TOO_SMALL
        return;
    }
    if (buffer)
        memory::Write32(buffer, value);
    ctx.r3.u64 = 0;
}

// void RtlTimeToTimeFields(const LARGE_INTEGER* time, TIME_FIELDS* fields)
// time: unidades de 100 ns desde 1601. TIME_FIELDS: año, mes, día, hora, minuto, segundo,
// milisegundo y día de la semana (0 = domingo), cada uno de 16 bits.
PPC_FUNC(__imp__RtlTimeToTimeFields)
{
    (void)base;
    constexpr int64_t kUnixEpochAsFileTime = 116444736000000000LL;
    const int64_t time = int64_t((uint64_t(memory::Read32(ctx.r3.u32)) << 32) | memory::Read32(ctx.r3.u32 + 4));
    const int64_t sinceUnix = time - kUnixEpochAsFileTime;
    const std::time_t seconds = std::time_t(sinceUnix >= 0 ? sinceUnix / 10000000 : (sinceUnix - 9999999) / 10000000);
    std::tm fields{};
    gmtime_r(&seconds, &fields);
    const uint32_t out = ctx.r4.u32;
    memory::Write16(out + 0, uint16_t(fields.tm_year + 1900));
    memory::Write16(out + 2, uint16_t(fields.tm_mon + 1));
    memory::Write16(out + 4, uint16_t(fields.tm_mday));
    memory::Write16(out + 6, uint16_t(fields.tm_hour));
    memory::Write16(out + 8, uint16_t(fields.tm_min));
    memory::Write16(out + 10, uint16_t(fields.tm_sec));
    memory::Write16(out + 12, uint16_t((sinceUnix - int64_t(seconds) * 10000000) / 10000));
    memory::Write16(out + 14, uint16_t(fields.tm_wday));
}

// --- Depuración --------------------------------------------------------------------------------

// uint32_t DbgPrint(const char* format, ...): los mensajes de depuración del juego, a stderr.
// Argumentos enteros y cadenas en r4..r10 y luego en la pila; los decimales, en f1..f8.
PPC_FUNC(__imp__DbgPrint)
{
    (void)base;
    const char* format = static_cast<const char*>(memory::Translate(ctx.r3.u32));
    const uint64_t* integers[] = { &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64, &ctx.r7.u64, &ctx.r8.u64, &ctx.r9.u64, &ctx.r10.u64 };
    const double floats[] = { ctx.f1.f64, ctx.f2.f64, ctx.f3.f64, ctx.f4.f64, ctx.f5.f64, ctx.f6.f64, ctx.f7.f64, ctx.f8.f64 };
    uint32_t nextInteger = 0, nextFloat = 0;
    auto integer = [&]() -> uint64_t {
        const uint32_t n = nextInteger++;
        return n < 7 ? *integers[n] : StackArgument(ctx, n - 7);
    };

    std::string out;
    for (const char* p = format; *p; p++)
    {
        if (*p != '%')
        {
            out += *p;
            continue;
        }
        // Especificador: %[flags][ancho][.precisión][l|h]tipo
        std::string spec = "%";
        p++;
        while (*p && std::strchr("-+ #0123456789.lhIq", *p))
            spec += *p++;
        if (!*p)
            break;
        const char type = *p;
        spec.erase(std::remove_if(spec.begin() + 1, spec.end(), [](char c) { return c == 'l' || c == 'h' || c == 'I' || c == 'q'; }), spec.end());
        char buffer[512];
        switch (type)
        {
        case 's':
        {
            const uint32_t address = uint32_t(integer());
            std::snprintf(buffer, sizeof(buffer), (spec + "s").c_str(), address ? static_cast<const char*>(memory::Translate(address)) : "(null)");
            break;
        }
        case 'f': case 'g': case 'e':
            std::snprintf(buffer, sizeof(buffer), (spec + type).c_str(), nextFloat < 8 ? floats[nextFloat++] : 0.0);
            break;
        case '%':
            std::snprintf(buffer, sizeof(buffer), "%%");
            break;
        default: // d i u x X c p
            std::snprintf(buffer, sizeof(buffer), (spec + (type == 'p' ? 'x' : type)).c_str(), uint32_t(integer()));
            break;
        }
        out += buffer;
    }
    std::fprintf(stderr, "[juego] %s%s", out.c_str(), (!out.empty() && out.back() == '\n') ? "" : "\n");
    ctx.r3.u64 = 0;
}

// --- Sin efecto en el runtime -------------------------------------------------------------
// Llamadas que en la consola configuran algo que aquí no existe (APC, privilegios de sistema,
// notificaciones al cerrar el título) o liberan memoria que el runtime no recupera.

// void KeEnterCriticalRegion(), KeLeaveCriticalRegion(): desactivan las APC del hilo.
PPC_FUNC(__imp__KeEnterCriticalRegion)
{
    (void)base;
}

PPC_FUNC(__imp__KeLeaveCriticalRegion)
{
    (void)base;
}

// void KiApcNormalRoutineNop(...): rutina de APC vacía.
PPC_FUNC(__imp__KiApcNormalRoutineNop)
{
    (void)base;
}

// void ExRegisterTitleTerminateNotification(X_EX_TITLE_TERMINATE_REGISTRATION* reg, uint32_t create)
PPC_FUNC(__imp__ExRegisterTitleTerminateNotification)
{
    (void)base;
}

// uint32_t XexCheckExecutablePrivilege(uint32_t privilege): un título normal no tiene ninguno.
PPC_FUNC(__imp__XexCheckExecutablePrivilege)
{
    (void)base;
    ctx.r3.u64 = 0;
}

// void MmFreePhysicalMemory(uint32_t type, uint32_t address): la memoria no se reutiliza.
PPC_FUNC(__imp__MmFreePhysicalMemory)
{
    (void)base;
}

// NTSTATUS NtFreeVirtualMemory(uint32_t* baseAddress, uint32_t* regionSize, uint32_t freeType, ...)
PPC_FUNC(__imp__NtFreeVirtualMemory)
{
    (void)base;
    ctx.r3.u64 = kStatusSuccess;
}

// uint32_t MmQueryAddressProtect(uint32_t address): toda la memoria del invitado existe y se puede
// leer y escribir (PAGE_READWRITE), salvo la primera página (punteros nulos).
PPC_FUNC(__imp__MmQueryAddressProtect)
{
    (void)base;
    ctx.r3.u64 = ctx.r3.u32 >= 0x10000 ? 0x04 : 0x00;
}

// uint32_t RtlNtStatusToDosError(NTSTATUS status): los códigos que ve el juego (de archivos y
// memoria). Sin equivalente: ERROR_MR_MID_NOT_FOUND, como Windows.
PPC_FUNC(__imp__RtlNtStatusToDosError)
{
    (void)base;
    const uint32_t status = ctx.r3.u32;
    uint32_t error;
    switch (status)
    {
    case 0x00000000: error = 0; break;      // STATUS_SUCCESS
    case 0x00000103: error = 997; break;    // STATUS_PENDING -> ERROR_IO_PENDING
    case 0x80000006: error = 18; break;     // STATUS_NO_MORE_FILES
    case 0xC0000008: error = 6; break;      // STATUS_INVALID_HANDLE
    case 0xC000000D: error = 87; break;     // STATUS_INVALID_PARAMETER
    case 0xC000000F: error = 2; break;      // STATUS_NO_SUCH_FILE
    case 0xC0000011: error = 38; break;     // STATUS_END_OF_FILE -> ERROR_HANDLE_EOF
    case 0xC0000017: error = 8; break;      // STATUS_NO_MEMORY -> ERROR_NOT_ENOUGH_MEMORY
    case 0xC0000022: error = 5; break;      // STATUS_ACCESS_DENIED
    case 0xC0000033: error = 123; break;    // STATUS_OBJECT_NAME_INVALID -> ERROR_INVALID_NAME
    case 0xC0000034: error = 2; break;      // STATUS_OBJECT_NAME_NOT_FOUND -> ERROR_FILE_NOT_FOUND
    case 0xC0000035: error = 183; break;    // STATUS_OBJECT_NAME_COLLISION -> ERROR_ALREADY_EXISTS
    case 0xC000003A: error = 3; break;      // STATUS_OBJECT_PATH_NOT_FOUND -> ERROR_PATH_NOT_FOUND
    case 0xC000007F: error = 112; break;    // STATUS_DISK_FULL
    case 0xC00000BA: error = 5; break;      // STATUS_FILE_IS_A_DIRECTORY -> ERROR_ACCESS_DENIED
    case 0xC0000103: error = 267; break;    // STATUS_NOT_A_DIRECTORY -> ERROR_DIRECTORY
    case 0xC000026E: error = 21; break;     // STATUS_VOLUME_DISMOUNTED -> ERROR_NOT_READY
    default: error = (status & 0xFFFF0000) == 0x80070000 ? (status & 0xFFFF) : 317; break; // HRESULT de Win32 o ERROR_MR_MID_NOT_FOUND
    }
    ctx.r3.u64 = error;
}
